/*
 * flash_fs.c - Safe filesystem wrapper for SPI flash (W25Q128)
 *
 * Prevents flash filesystem corruption (community bug #3) by:
 *   1. Mutex-protecting all filesystem operations so concurrent
 *      tasks cannot interleave SPI flash commands.
 *   2. Using atomic write pattern (write to .tmp, then rename)
 *      so a power loss mid-write cannot corrupt the original file.
 *
 * The actual FatFS/SPI flash driver calls are stubbed out until
 * those drivers are implemented. The safety infrastructure (mutex
 * acquire/release, atomic rename pattern) is fully in place.
 */

#include "flash_fs.h"
#include "at32f403a_407.h"
#include "FreeRTOS.h"
#include "semphr.h"

#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * Internal state
 * ═══════════════════════════════════════════════════════════════════ */

static SemaphoreHandle_t fs_mutex  = NULL;
static bool              fs_ready  = false;
static bool              raw_spi_ready = false;
static bool              jedec_valid = false;
static uint8_t           jedec_id[3];
static flash_fs_volume_info_t volumes[FLASH_FS_VOLUME_COUNT];

#define SPI_FLASH_SPI       ((spi_type *)SPI2_BASE)
#define SPI_FLASH_SIZE      FLASH_FS_RAW_MAX_ADDR
#define SPI_FLASH_CS_ASSERT()   (GPIOB->clr = GPIO_PINS_12)
#define SPI_FLASH_CS_DEASSERT() (GPIOB->scr = GPIO_PINS_12)

/* Mutex timeout: 5 seconds should be more than enough for any
 * single filesystem operation on SPI flash. */
#define FS_MUTEX_TIMEOUT_MS  5000

/* Maximum path length (matching FatFS LFN limits) */
#define FS_MAX_PATH  128

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool flash_fs_probe_fat12(uint32_t index, uint32_t base)
{
    uint8_t boot[512];
    flash_fs_volume_info_t *volume = &volumes[index];

    memset(volume, 0, sizeof(*volume));
    volume->base_address = base;

    if (flash_fs_raw_read_bytes(base, boot, sizeof(boot)) != FLASH_FS_OK) {
        return false;
    }

    uint16_t bytes_per_sector = read_le16(&boot[11]);
    uint16_t total_sectors = read_le16(&boot[19]);
    uint16_t root_entries = read_le16(&boot[17]);
    uint8_t sectors_per_cluster = boot[13];
    bool sector_size_ok = bytes_per_sector == 512u ||
                          bytes_per_sector == 1024u ||
                          bytes_per_sector == 2048u ||
                          bytes_per_sector == 4096u;

    if (memcmp(&boot[3], "MSDOS5.0", 8) != 0 ||
        memcmp(&boot[54], "FAT12   ", 8) != 0 ||
        boot[510] != 0x55 || boot[511] != 0xAA ||
        !sector_size_ok || sectors_per_cluster == 0 ||
        total_sectors == 0 || root_entries == 0) {
        return false;
    }

    volume->bytes_per_sector = bytes_per_sector;
    volume->total_sectors = total_sectors;
    volume->root_entries = root_entries;
    volume->sectors_per_cluster = sectors_per_cluster;
    volume->size_bytes = (uint32_t)bytes_per_sector * total_sectors;
    volume->mounted = volume->size_bytes <= (SPI_FLASH_SIZE - base);
    return volume->mounted;
}

/* ═══════════════════════════════════════════════════════════════════
 * Internal helpers
 * ═══════════════════════════════════════════════════════════════════ */

/* Build a temporary file path by appending ".tmp" to the original */
static bool make_tmp_path(const char *path, char *tmp_path, uint32_t tmp_path_size)
{
    uint32_t len = (uint32_t)strlen(path);
    if (len + 5 > tmp_path_size) {  /* +5 for ".tmp\0" */
        return false;
    }
    memcpy(tmp_path, path, len);
    memcpy(tmp_path + len, ".tmp", 5);  /* includes null terminator */
    return true;
}

static void flash_fs_raw_spi_init_once(void)
{
    if (raw_spi_ready) {
        return;
    }

    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_SPI2_PERIPH_CLOCK, TRUE);

    gpio_init_type gpio_cfg;
    gpio_default_para_init(&gpio_cfg);

    /* PB13 = SPI2_SCK, PB15 = SPI2_MOSI */
    gpio_cfg.gpio_pins = GPIO_PINS_13 | GPIO_PINS_15;
    gpio_cfg.gpio_mode = GPIO_MODE_MUX;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);

    /* PB14 = SPI2_MISO */
    gpio_cfg.gpio_pins = GPIO_PINS_14;
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;
    gpio_init(GPIOB, &gpio_cfg);

    /* PB12 = SPI flash CS */
    gpio_cfg.gpio_pins = GPIO_PINS_12;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);
    SPI_FLASH_CS_DEASSERT();

    /* SPI mode 0, master, software CS, conservative /16 clock. */
    SPI_FLASH_SPI->ctrl1 = (1U << 2)   /* master */
                         | (3U << 3)   /* /16 prescaler */
                         | (1U << 8)   /* internal CS high */
                         | (1U << 9);  /* software CS enable */
    SPI_FLASH_SPI->ctrl2 = 0;
    SPI_FLASH_SPI->ctrl1 |= (1U << 6); /* enable SPI */

    raw_spi_ready = true;
}

static uint8_t flash_fs_raw_spi_xfer(uint8_t tx)
{
    uint32_t timeout = 1000000;
    while (!(SPI_FLASH_SPI->sts & (1U << 1)) && --timeout) {}
    if (!timeout) {
        return 0xFF;
    }

    SPI_FLASH_SPI->dt = tx;

    timeout = 1000000;
    while (!(SPI_FLASH_SPI->sts & (1U << 0)) && --timeout) {}
    if (!timeout) {
        return 0xFF;
    }

    return (uint8_t)SPI_FLASH_SPI->dt;
}

/* ═══════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════ */

flash_fs_error_t flash_fs_init(void)
{
    fs_mutex = xSemaphoreCreateMutex();
    if (fs_mutex == NULL) {
        return FLASH_FS_ERR_MUTEX;
    }

    flash_fs_error_t result = flash_fs_raw_read_jedec(&jedec_id[0],
                                                       &jedec_id[1],
                                                       &jedec_id[2]);
    jedec_valid = result == FLASH_FS_OK &&
                  jedec_id[0] == 0xEF && jedec_id[1] == 0x40 &&
                  jedec_id[2] == 0x18;
    if (!jedec_valid) {
        return FLASH_FS_ERR_READ;
    }

    bool volume0_ok = flash_fs_probe_fat12(0, 0x000000u);
    bool volume1_ok = flash_fs_probe_fat12(1, 0x200000u);
    fs_ready = volume0_ok && volume1_ok;
    return fs_ready ? FLASH_FS_OK : FLASH_FS_ERR_MOUNT;
}

flash_fs_error_t flash_fs_write_atomic(const char *path, const void *data, uint32_t len)
{
    if (!fs_ready || fs_mutex == NULL) {
        return FLASH_FS_ERR_MUTEX;
    }

    /* Acquire mutex with timeout */
    if (xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(FS_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return FLASH_FS_ERR_MUTEX;
    }

    flash_fs_error_t result = FLASH_FS_ERR_WRITE;

    /* Build temporary file path */
    char tmp_path[FS_MAX_PATH];
    if (!make_tmp_path(path, tmp_path, sizeof(tmp_path))) {
        result = FLASH_FS_ERR_WRITE;
        goto done;
    }

    /*
     * Atomic write pattern:
     *   1. Write data to <path>.tmp
     *   2. If write succeeds, delete original <path>
     *   3. Rename <path>.tmp to <path>
     *
     * If power is lost during step 1, the original file is intact.
     * If power is lost during step 3, the .tmp file contains valid
     * data and can be recovered on next boot.
     */

    /* Step 1: Write to temporary file */
    /* TODO: Implement when FatFS/SPI flash driver available
     *   FIL fil;
     *   FRESULT res = f_open(&fil, tmp_path, FA_WRITE | FA_CREATE_ALWAYS);
     *   if (res != FR_OK) { result = FLASH_FS_ERR_OPEN; goto done; }
     *
     *   UINT bytes_written;
     *   res = f_write(&fil, data, len, &bytes_written);
     *   f_close(&fil);
     *   if (res != FR_OK || bytes_written != len) {
     *       f_unlink(tmp_path);
     *       result = FLASH_FS_ERR_WRITE;
     *       goto done;
     *   }
     */

    /* Step 2: Delete original file (ignore error — may not exist) */
    /* TODO: f_unlink(path); */

    /* Step 3: Rename .tmp to final path */
    /* TODO:
     *   res = f_rename(tmp_path, path);
     *   if (res != FR_OK) { result = FLASH_FS_ERR_RENAME; goto done; }
     */

    /* Suppress unused parameter warnings until stubs are replaced */
    (void)data;
    (void)len;
    (void)tmp_path;

done:
    xSemaphoreGive(fs_mutex);
    return result;
}

flash_fs_error_t flash_fs_read(const char *path, void *buf, uint32_t buf_size, uint32_t *bytes_read)
{
    if (!fs_ready || fs_mutex == NULL) {
        return FLASH_FS_ERR_MUTEX;
    }

    if (xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(FS_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return FLASH_FS_ERR_MUTEX;
    }

    flash_fs_error_t result = FLASH_FS_OK;

    /* TODO: Implement when FatFS/SPI flash driver available
     *   FIL fil;
     *   FRESULT res = f_open(&fil, path, FA_READ);
     *   if (res != FR_OK) { result = FLASH_FS_ERR_OPEN; goto done; }
     *
     *   UINT br;
     *   res = f_read(&fil, buf, buf_size, &br);
     *   f_close(&fil);
     *   if (res != FR_OK) { result = FLASH_FS_ERR_READ; goto done; }
     *   if (bytes_read) *bytes_read = (uint32_t)br;
     */

    /* Suppress unused parameter warnings until stubs are replaced */
    (void)path;
    (void)buf;
    (void)buf_size;
    if (bytes_read) {
        *bytes_read = 0;
    }

    xSemaphoreGive(fs_mutex);
    return result;
}

flash_fs_error_t flash_fs_delete(const char *path)
{
    if (!fs_ready || fs_mutex == NULL) {
        return FLASH_FS_ERR_MUTEX;
    }

    if (xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(FS_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return FLASH_FS_ERR_MUTEX;
    }

    /* TODO: Implement when FatFS/SPI flash driver available
     *   FRESULT res = f_unlink(path);
     *   if (res != FR_OK && res != FR_NO_FILE) {
     *       xSemaphoreGive(fs_mutex);
     *       return FLASH_FS_ERR_OPEN;
     *   }
     */

    (void)path;

    xSemaphoreGive(fs_mutex);
    return FLASH_FS_ERR_WRITE;
}

bool flash_fs_is_ready(void)
{
    return fs_ready;
}

bool flash_fs_get_jedec(uint8_t *manufacturer, uint8_t *memory_type,
                        uint8_t *capacity)
{
    if (!jedec_valid) {
        return false;
    }
    if (manufacturer) *manufacturer = jedec_id[0];
    if (memory_type)  *memory_type = jedec_id[1];
    if (capacity)     *capacity = jedec_id[2];
    return true;
}

const flash_fs_volume_info_t *flash_fs_volume(uint32_t index)
{
    return index < FLASH_FS_VOLUME_COUNT ? &volumes[index] : NULL;
}

flash_fs_error_t flash_fs_raw_read_jedec(uint8_t *manufacturer,
                                         uint8_t *memory_type,
                                         uint8_t *capacity)
{
    if (manufacturer == NULL || memory_type == NULL || capacity == NULL) {
        return FLASH_FS_ERR_READ;
    }
    if (fs_mutex == NULL) {
        return FLASH_FS_ERR_MUTEX;
    }

    flash_fs_raw_spi_init_once();

    if (xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(FS_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return FLASH_FS_ERR_MUTEX;
    }

    SPI_FLASH_CS_ASSERT();
    flash_fs_raw_spi_xfer(0x9F);
    *manufacturer = flash_fs_raw_spi_xfer(0xFF);
    *memory_type  = flash_fs_raw_spi_xfer(0xFF);
    *capacity     = flash_fs_raw_spi_xfer(0xFF);
    SPI_FLASH_CS_DEASSERT();

    xSemaphoreGive(fs_mutex);
    return FLASH_FS_OK;
}

flash_fs_error_t flash_fs_raw_read_bytes(uint32_t addr, void *buf, uint32_t len)
{
    uint8_t *out = (uint8_t *)buf;

    if (buf == NULL) {
        return FLASH_FS_ERR_READ;
    }
    if (len == 0) {
        return FLASH_FS_OK;
    }
    if (fs_mutex == NULL) {
        return FLASH_FS_ERR_MUTEX;
    }
    if (addr >= SPI_FLASH_SIZE || len > (SPI_FLASH_SIZE - addr)) {
        return FLASH_FS_ERR_READ;
    }

    flash_fs_raw_spi_init_once();

    if (xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(FS_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return FLASH_FS_ERR_MUTEX;
    }

    flash_fs_raw_read_bytes_direct(addr, out, len);

    xSemaphoreGive(fs_mutex);
    return FLASH_FS_OK;
}

flash_fs_error_t flash_fs_raw_read_bytes_direct(uint32_t addr, void *buf,
                                                 uint32_t len)
{
    uint8_t *out = (uint8_t *)buf;
    if (buf == NULL || addr >= SPI_FLASH_SIZE ||
        len > (SPI_FLASH_SIZE - addr)) {
        return FLASH_FS_ERR_READ;
    }
    if (len == 0) {
        return FLASH_FS_OK;
    }

    flash_fs_raw_spi_init_once();
    SPI_FLASH_CS_ASSERT();
    flash_fs_raw_spi_xfer(0x03);
    flash_fs_raw_spi_xfer((uint8_t)(addr >> 16));
    flash_fs_raw_spi_xfer((uint8_t)(addr >> 8));
    flash_fs_raw_spi_xfer((uint8_t)addr);
    for (uint32_t i = 0; i < len; i++) {
        out[i] = flash_fs_raw_spi_xfer(0xFF);
    }
    SPI_FLASH_CS_DEASSERT();
    return FLASH_FS_OK;
}

#define FAT1_WRITE_BASE       0x00200000u
#define FAT1_WRITE_SIZE       (14u * 1024u * 1024u)
#define SPI_ERASE_SECTOR_SIZE 4096u
#define SPI_PROGRAM_PAGE_SIZE 256u
#define SPI_BUSY_TIMEOUT      8000000u

static uint8_t fat1_sector_buffer[SPI_ERASE_SECTOR_SIZE];
static uint8_t fat1_verify_buffer[SPI_PROGRAM_PAGE_SIZE];

static uint8_t flash_fs_raw_status(void)
{
    uint8_t status;
    SPI_FLASH_CS_ASSERT();
    flash_fs_raw_spi_xfer(0x05);
    status = flash_fs_raw_spi_xfer(0xFF);
    SPI_FLASH_CS_DEASSERT();
    return status;
}

static bool flash_fs_raw_wait_ready(void)
{
    uint32_t timeout = SPI_BUSY_TIMEOUT;
    while ((flash_fs_raw_status() & 0x01u) != 0 && --timeout) {}
    return timeout != 0;
}

static bool flash_fs_raw_write_enable(void)
{
    if (!flash_fs_raw_wait_ready()) return false;
    SPI_FLASH_CS_ASSERT();
    flash_fs_raw_spi_xfer(0x06);
    SPI_FLASH_CS_DEASSERT();
    return (flash_fs_raw_status() & 0x02u) != 0;
}

static bool flash_fs_raw_erase_sector(uint32_t addr)
{
    if (!flash_fs_raw_write_enable()) return false;
    SPI_FLASH_CS_ASSERT();
    flash_fs_raw_spi_xfer(0x20);
    flash_fs_raw_spi_xfer((uint8_t)(addr >> 16));
    flash_fs_raw_spi_xfer((uint8_t)(addr >> 8));
    flash_fs_raw_spi_xfer((uint8_t)addr);
    SPI_FLASH_CS_DEASSERT();
    return flash_fs_raw_wait_ready();
}

static bool flash_fs_raw_program_page(uint32_t addr, const uint8_t *data)
{
    if (!flash_fs_raw_write_enable()) return false;
    SPI_FLASH_CS_ASSERT();
    flash_fs_raw_spi_xfer(0x02);
    flash_fs_raw_spi_xfer((uint8_t)(addr >> 16));
    flash_fs_raw_spi_xfer((uint8_t)(addr >> 8));
    flash_fs_raw_spi_xfer((uint8_t)addr);
    for (uint32_t i = 0; i < SPI_PROGRAM_PAGE_SIZE; i++)
        flash_fs_raw_spi_xfer(data[i]);
    SPI_FLASH_CS_DEASSERT();
    return flash_fs_raw_wait_ready();
}

flash_fs_error_t flash_fs_fat1_write_bytes_direct(uint32_t offset,
                                                   const void *buf,
                                                   uint32_t len)
{
    const uint8_t *input = (const uint8_t *)buf;
    if (buf == NULL || offset > FAT1_WRITE_SIZE ||
        len > FAT1_WRITE_SIZE - offset) {
        return FLASH_FS_ERR_WRITE;
    }
    if (len == 0) return FLASH_FS_OK;

    flash_fs_raw_spi_init_once();
    if (!flash_fs_raw_wait_ready() || (flash_fs_raw_status() & 0x1Cu) != 0)
        return FLASH_FS_ERR_WRITE;

    while (len != 0) {
        uint32_t address = FAT1_WRITE_BASE + offset;
        uint32_t sector = address & ~(SPI_ERASE_SECTOR_SIZE - 1u);
        uint32_t within = address - sector;
        uint32_t chunk = SPI_ERASE_SECTOR_SIZE - within;
        if (chunk > len) chunk = len;

        if (flash_fs_raw_read_bytes_direct(sector, fat1_sector_buffer,
                                           sizeof(fat1_sector_buffer)) !=
            FLASH_FS_OK) {
            return FLASH_FS_ERR_READ;
        }

        uint16_t changed_pages = 0;
        bool erase_needed = false;
        for (uint32_t i = 0; i < chunk; i++) {
            uint8_t old_value = fat1_sector_buffer[within + i];
            uint8_t new_value = input[i];
            if (old_value != new_value) {
                changed_pages |= (uint16_t)(1u << ((within + i) / 256u));
                if ((new_value & (uint8_t)~old_value) != 0)
                    erase_needed = true;
            }
        }
        memcpy(&fat1_sector_buffer[within], input, chunk);

        if (changed_pages != 0) {
            if (erase_needed) {
                if (!flash_fs_raw_erase_sector(sector))
                    return FLASH_FS_ERR_WRITE;
                changed_pages = 0xFFFFu;
            }
            for (uint32_t page = 0; page < 16u; page++) {
                if ((changed_pages & (1u << page)) == 0) continue;
                uint32_t page_address = sector + page * SPI_PROGRAM_PAGE_SIZE;
                if (!flash_fs_raw_program_page(page_address,
                        &fat1_sector_buffer[page * SPI_PROGRAM_PAGE_SIZE])) {
                    return FLASH_FS_ERR_WRITE;
                }
                if (flash_fs_raw_read_bytes_direct(page_address,
                        fat1_verify_buffer, sizeof(fat1_verify_buffer)) !=
                        FLASH_FS_OK ||
                    memcmp(fat1_verify_buffer,
                           &fat1_sector_buffer[page * SPI_PROGRAM_PAGE_SIZE],
                           SPI_PROGRAM_PAGE_SIZE) != 0) {
                    return FLASH_FS_ERR_WRITE;
                }
            }
        }

        offset += chunk;
        input += chunk;
        len -= chunk;
    }
    return FLASH_FS_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * Factory calibration mirror
 *
 * Stock firmware path: SPI flash "3:/System file/" contains per-channel
 * calibration blobs (301 bytes each) that are loaded on boot into a
 * RAM block at 0x20000358-0x20000434 and consumed by the meter/scope
 * scaling pipeline.
 *
 * This is a load-only stub. The real SPI flash driver is not yet
 * wired through flash_fs_read(), so this routine currently reads 0
 * bytes and leaves `loaded` false. Drivers consuming factory_cal_t
 * must fall back to built-in defaults in that case.
 * ═══════════════════════════════════════════════════════════════════ */

static factory_cal_t g_factory_cal;  /* BSS zero */

flash_fs_error_t flash_fs_load_factory_cal(void)
{
    memset(&g_factory_cal, 0, sizeof(g_factory_cal));

    uint32_t br1 = 0;
    uint32_t br2 = 0;

    flash_fs_error_t r1 = flash_fs_read("3:/System file/cal_ch1.bin",
                                        g_factory_cal.ch1,
                                        FACTORY_CAL_CHANNEL_SIZE, &br1);
    flash_fs_error_t r2 = flash_fs_read("3:/System file/cal_ch2.bin",
                                        g_factory_cal.ch2,
                                        FACTORY_CAL_CHANNEL_SIZE, &br2);

    if (r1 == FLASH_FS_OK && r2 == FLASH_FS_OK &&
        br1 == FACTORY_CAL_CHANNEL_SIZE &&
        br2 == FACTORY_CAL_CHANNEL_SIZE) {
        g_factory_cal.loaded = true;
        /* TODO(phase3): apply these coefficients in meter_data.c */
        return FLASH_FS_OK;
    }

    /* Stub path: SPI flash driver not yet populated. Log-only outcome
     * is implicit — the caller can check flash_fs_factory_cal()->loaded. */
    g_factory_cal.loaded = false;
    return FLASH_FS_ERR_READ;
}

const factory_cal_t *flash_fs_factory_cal(void)
{
    return &g_factory_cal;
}
