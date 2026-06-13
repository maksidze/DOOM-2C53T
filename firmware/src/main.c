/*
 * OpenScope 2C53T - DOOM Port Minimal Base
 *
 * Target: Artery AT32F403A (ARM Cortex-M4F @ 240MHz)
 * Display: ST7789V 320x240 via EXMC/XMC
 * RTOS: FreeRTOS
 *
 * This minimal firmware initializes the hardware and FreeRTOS,
 * and provides a clean canvas for porting DOOM.
 */

#include "at32f403a_407.h"

/* AT32 clock config */
extern void system_clock_config(void);
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* Core drivers */
#include "lcd.h"
#include "button_scan.h"
#include "dac_output.h"
#include "flash_fs.h"
#include "battery.h"
#include "dfu_boot.h"
#include "watchdog.h"
#include "usb_msc.h"
#include "font.h"
#include "w_wad.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

bool g_skip_doom = false;
static bool g_wad_found;
static uint32_t g_iwad_addr;
static uint32_t g_iwad_size;
static uint16_t g_eopb0;
static uint32_t g_wad_fat_offset;
static uint32_t g_wad_data_offset;
static uint32_t g_wad_cluster_size;
static uint16_t g_wad_first_cluster;

typedef struct {
    uint8_t name[11];
    uint8_t attributes;
    uint8_t reserved[14];
    uint16_t first_cluster;
    uint32_t size;
} fat_dir_entry_t;

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static bool find_doom_wad(uint32_t *flash_address, uint32_t *file_size)
{
    static const uint8_t doom_name[11] = {
        'D', 'O', 'O', 'M', '1', ' ', ' ', ' ', 'W', 'A', 'D'
    };
    const uint32_t volume_base = 0x00200000U;
    uint8_t boot[512];

    if (flash_fs_raw_read_bytes_direct(volume_base, boot, sizeof(boot)) != FLASH_FS_OK ||
        boot[510] != 0x55 || boot[511] != 0xAA) {
        return false;
    }

    uint16_t bytes_per_sector = read_le16(&boot[11]);
    uint8_t sectors_per_cluster = boot[13];
    uint16_t reserved_sectors = read_le16(&boot[14]);
    uint8_t fat_count = boot[16];
    uint16_t root_entries = read_le16(&boot[17]);
    uint16_t sectors_per_fat = read_le16(&boot[22]);
    if (bytes_per_sector == 0 || sectors_per_cluster == 0 ||
        fat_count == 0 || root_entries == 0 || sectors_per_fat == 0) {
        return false;
    }

    uint32_t root_start_sector = reserved_sectors +
                                 (uint32_t)fat_count * sectors_per_fat;
    uint32_t root_sectors = ((uint32_t)root_entries * 32U +
                             bytes_per_sector - 1U) / bytes_per_sector;
    uint32_t data_start_sector = root_start_sector + root_sectors;

    for (uint32_t i = 0; i < root_entries; i++) {
        fat_dir_entry_t entry;
        uint32_t address = volume_base + root_start_sector * bytes_per_sector +
                           i * sizeof(entry);
        if (flash_fs_raw_read_bytes_direct(address, &entry, sizeof(entry)) != FLASH_FS_OK) {
            return false;
        }
        if (entry.name[0] == 0x00) {
            break;
        }
        if (entry.name[0] == 0xE5 || entry.attributes == 0x0F ||
            (entry.attributes & 0x18) != 0) {
            continue;
        }
        if (memcmp(entry.name, doom_name, sizeof(doom_name)) == 0 &&
            entry.first_cluster >= 2 && entry.size >= 12) {
            uint32_t first_sector = data_start_sector +
                                    (uint32_t)(entry.first_cluster - 2) *
                                    sectors_per_cluster;
            uint32_t wad_address = volume_base + first_sector * bytes_per_sector;
            char magic[4];
            if (flash_fs_raw_read_bytes_direct(wad_address, magic, sizeof(magic)) == FLASH_FS_OK &&
                ((memcmp(magic, "IWAD", 4) == 0) ||
                 (memcmp(magic, "PWAD", 4) == 0))) {
                *flash_address = wad_address;
                *file_size = entry.size;
                g_wad_fat_offset = (uint32_t)reserved_sectors * bytes_per_sector;
                g_wad_data_offset = data_start_sector * bytes_per_sector;
                g_wad_cluster_size = (uint32_t)sectors_per_cluster * bytes_per_sector;
                g_wad_first_cluster = entry.first_cluster;
                return true;
            }
        }
        if ((i & 0x1F) == 0) {
            watchdog_feed();
        }
    }
    return false;
}

static void show_wad_not_found(void)
{
    lcd_clear(COLOR_BLACK);
    lcd_draw_string(28, 64, "DOOM1.WAD NOT FOUND", COLOR_RED, COLOR_BLACK);
    lcd_draw_string(28, 104, "File is missing from disk", COLOR_WHITE, COLOR_BLACK);
    lcd_draw_string(28, 136, "Restart after adding the file", COLOR_WHITE, COLOR_BLACK);
    for (;;) {
        watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void show_sram_mode_error(void)
{
    char value[32];
    snprintf(value, sizeof(value), "EOPB0 = 0x%02X", g_eopb0 & 0xFFU);
    lcd_clear(COLOR_BLACK);
    lcd_draw_string(24, 56, "224KB SRAM REQUIRED", COLOR_RED, COLOR_BLACK);
    lcd_draw_string(24, 96, value, COLOR_YELLOW, COLOR_BLACK);
    lcd_draw_string(24, 132, "Required value: 0xFE", COLOR_WHITE, COLOR_BLACK);
    for (;;) {
        watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void show_flash_diag(void)
{
    char line1[48];
    char line2[64];
    snprintf(line1, sizeof(line1), "P:%lu A:%08lX",
             doom_flash_diag.phase, doom_flash_diag.address);
    snprintf(line2, sizeof(line2), "W:%08lX S:%08lX C:%08lX",
             doom_flash_diag.word, doom_flash_diag.status,
             doom_flash_diag.control);
    lcd_clear(COLOR_BLACK);
    lcd_draw_string(16, 38, "FLASH DIAG", COLOR_RED, COLOR_BLACK);
    lcd_draw_string(16, 78, line1, COLOR_YELLOW, COLOR_BLACK);
    lcd_draw_string(16, 112, line2, COLOR_WHITE, COLOR_BLACK);
    for (;;) {
        watchdog_feed();
        delay_ms(50);
    }
}

/* FreeRTOS handles */
static TaskHandle_t  xDisplayTaskHandle = NULL;
static TaskHandle_t  xInputTaskHandle   = NULL;
static QueueHandle_t xInputQueue        = NULL;

/* ═══════════════════════════════════════════════════════════════════
 * Simple delay
 * ═══════════════════════════════════════════════════════════════════ */

void delay_ms(uint32_t ms)
{
    volatile uint32_t count;
    while (ms--) {
        count = system_core_clock / 10000;
        while (count--) {
            __asm volatile("nop");
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * FreeRTOS Tasks
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Display / Main Task
 *
 * Currently a stub that clears the screen and draws test text.
 * The DOOM game loop can eventually run here.
 */
extern void D_DoomMain(void);

#include "doom_iwad.h"
extern void I_Error(const char *error, ...);

static void vDisplayTask(void *pvParameters)
{
    (void)pvParameters;

    /* Feed watchdog immediately — we're alive */
    watchdog_feed();

#ifndef EMULATOR_BUILD
    if ((g_eopb0 & 0xFFU) != 0xFEU) {
        show_sram_mode_error();
    }
#endif

    if (g_skip_doom) {
        lcd_clear(COLOR_BLUE);
        lcd_draw_string(16, 24, "USB-MODE", COLOR_WHITE, COLOR_BLUE);
        lcd_draw_string(16, 64, "DOOM startup skipped", COLOR_YELLOW, COLOR_BLUE);
        lcd_draw_string(16, 96, "USB STORAGE ACTIVE", COLOR_WHITE, COLOR_BLUE);
        lcd_draw_string(16, 136, "Connect USB-C to update WAD", COLOR_WHITE, COLOR_BLUE);
        for (;;) {
            watchdog_feed();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    lcd_clear(COLOR_BLACK);
    watchdog_feed();  /* lcd_clear can be slow, feed again */

    font_draw_string(12, 12, "LOOKING FOR DOOM1.WAD...", COLOR_WHITE, COLOR_BLACK, &font_medium);
    vTaskDelay(pdMS_TO_TICKS(10));

    if (!g_wad_found) {
        show_wad_not_found();
    }

    /* Keep DOOM's virtual WAD pointers and map them to this SPI offset. */
    doom_iwad_configure_fat12(0x00200000U, g_wad_fat_offset,
                              g_wad_data_offset, g_wad_cluster_size,
                              g_wad_first_cluster, g_iwad_size);
    if (!doom_iwad_validate()) {
        I_Error("DOOM1.WAD invalid or PLAYPAL missing");
    }

    font_draw_string(12, 40, "BOOTING DOOM...", COLOR_WHITE, COLOR_BLACK, &font_medium);
    vTaskDelay(pdMS_TO_TICKS(500));

    // Call into the DOOM engine
    D_DoomMain();

    // Should never reach here
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/*
 * Input Task
 *
 * button_scan_init() is called HERE, after the scheduler starts, so the
 * TMR3 ISR never fires before xQueueSendFromISR() is safe to call.
 * Calling button_scan_init() in main() before vTaskStartScheduler() would
 * immediately trigger configASSERT because the scheduler is suspended.
 */
static void vInputTask(void *pvParameters)
{
    (void)pvParameters;

    /* Start button scanner now that scheduler is running */
    button_scan_init(xInputQueue);

    button_id_t pressed;
    TickType_t power_press_time = 0;
    bool power_hold_active = false;

    const uint16_t power_mask = 0x0001U;
    const uint16_t menu_mask = 0x0200U;

    for (;;) {
        /* Block until a button event arrives */
        if (xQueueReceive(xInputQueue, &pressed, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (pressed == BTN_POWER) {
                power_press_time = xTaskGetTickCount();
                power_hold_active = true;
            }
        }

        /* Check if USB was plugged in during runtime */
        battery_update();
        if (battery_is_charging()) {
            NVIC_SystemReset();
        }

        /* MENU + POWER enters the updater; POWER alone shuts down. */
        uint16_t raw_btns = button_scan_get_raw();
        if (power_hold_active && (raw_btns & power_mask)) {
            if (raw_btns & menu_mask) {
                dfu_request_reboot();
            }

            if ((xTaskGetTickCount() - power_press_time) >= pdMS_TO_TICKS(3000)) {
                /* Stop DOOM from repainting over the shutdown message. */
                vTaskSuspend(xDisplayTaskHandle);
                watchdog_feed();
                lcd_clear(COLOR_BLACK);
                font_draw_string(82, 106, "POWERING OFF", COLOR_WHITE,
                                 COLOR_BLACK, &font_medium);
                vTaskDelay(pdMS_TO_TICKS(250));

                /* PC9 is the external power latch: LOW removes power. */
                GPIOC->clr = (1U << 9);

                /* Keep the watchdog quiet if external power drops slowly. */
                for (;;) {
                    watchdog_feed();
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
        } else {
            power_hold_active = false;
        }
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void)
{
#ifdef EMULATOR_BUILD
    system_core_clock = 240000000;
    *(volatile uint32_t *)0x40021014 = 0x00000114;
    *(volatile uint32_t *)0x40021018 = 0x0000FFFD;
    *(volatile uint32_t *)0x4002101C = 0x3FFFFFFF;
#else
    /* Reset_Handler already installed this image's MSP. Never change MSP from
     * inside main(): doing so invalidates the current C stack frame. */
    __disable_irq();

    /* Make data bus faults precise while diagnosing guest SRAM writes.
     * Without this, Cortex-M4 reports IMPRECISERR after the failing store and
     * BFAR contains no valid address. */
    SCnSCB->ACTLR |= (1UL << 1); /* DISDEFWBUF */
    __DSB();
    __ISB();

    /* Power hold — PC9 HIGH to keep device on (MUST be first!) */
    crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
    GPIOC->cfghr = (GPIOC->cfghr & ~(0xF << 4)) | (0x3 << 4); /* PC9 push-pull 50MHz */
    GPIOC->scr = (1 << 9);  /* PC9 HIGH */

    /* Set VTOR to our vector table.
     * GUEST_BUILD: app linked at 0x08007000 (runs under stock FNIRSI bootloader)
     * Normal build: app linked at 0x08004000 (runs under our HID bootloader)
     * Wrong VTOR → SVC/PendSV jump into wrong table → instant HardFault */
#ifdef GUEST_BUILD
    SCB->VTOR = FLASH_BASE | 0x7000;  /* 0x08007000 — guest base */
#else
    SCB->VTOR = FLASH_BASE | 0x4000;  /* 0x08004000 — HID bootloader app */
#endif

    /* IMPORTANT: The Chinese stock bootloader jumps to the guest app without
     * disabling its peripherals or interrupts (e.g., USB, TMR3, USART2).
     * We MUST disable all interrupts in the NVIC and clear any pending
     * interrupts before they fire and jump to our uninitialized handlers. */
    
    __DSB();
    __ISB();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

    NVIC->ICER[0] = 0xFFFFFFFF;
    NVIC->ICER[1] = 0xFFFFFFFF;
    NVIC->ICER[2] = 0xFFFFFFFF;
    NVIC->ICPR[0] = 0xFFFFFFFF;
    NVIC->ICPR[1] = 0xFFFFFFFF;
    NVIC->ICPR[2] = 0xFFFFFFFF;


    /* Check DFU */
    dfu_check_magic();

    /* Configure system clock */
    system_clock_config();

    /* FreeRTOS strictly requires NVIC Priority Group 4 for Cortex-M */
    nvic_priority_group_config(NVIC_PRIORITY_GROUP_4);
    __enable_irq();

    /* Enable peripheral clocks */
    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOE_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_XMC_PERIPH_CLOCK, TRUE);

    /* PB8 = LCD backlight ON */
    GPIOB->cfghr = (GPIOB->cfghr & ~(0xF << 0)) | (0x3 << 0); /* PB8 push-pull 50MHz */
    GPIOB->scr = (1 << 8);
#endif

    /* Initialize battery ADC */
    battery_adc_init();

    /* Check USB connection to skip DOOM and enter storage mode */
    if (battery_is_charging()) {
        g_skip_doom = true;
    }

    /* Initialize LCD */
    {
        #define _GPIO_CFG(base, pin, mode, cnf) do { \
            volatile uint32_t *r = (pin < 8) ? \
                (volatile uint32_t *)(base + 0x00) : \
                (volatile uint32_t *)(base + 0x04); \
            uint8_t p = (pin < 8) ? pin : (pin - 8); \
            uint32_t v = *r; \
            v &= ~(0xFU << (p * 4)); \
            v |= (((mode) | ((cnf) << 2)) << (p * 4)); \
            *r = v; \
        } while(0)

        uint8_t pd_pins[] = {0,1,4,5,7,8,9,10,11,12,14,15};
        for (int i = 0; i < 12; i++) _GPIO_CFG(0x40011400, pd_pins[i], 3, 2);
        for (int i = 7; i <= 15; i++) _GPIO_CFG(0x40011800, i, 3, 2);

        /* EXMC config */
        *(volatile uint32_t *)0xA0000000 = 0x00005010;
        *(volatile uint32_t *)0xA0000004 = 0x02020424;
        *(volatile uint32_t *)0xA0000104 = 0x00000202;
        *(volatile uint32_t *)0xA0000000 |= 0x0001;
        delay_ms(50);

        /* LCD init */
        lcd_write_cmd(0x01);  /* Software reset */
        delay_ms(200);
        lcd_write_cmd(0x11);  /* Sleep out */
        delay_ms(200);
        lcd_write_cmd(0x36);  /* MADCTL — landscape, flipped */
        delay_ms(1);
        lcd_write_data8(0xA0);
        delay_ms(10);
        lcd_write_cmd(0x3A);  /* Pixel format 16-bit */
        delay_ms(1);
        lcd_write_data8(0x55);
        delay_ms(10);
        lcd_write_cmd(0x29);  /* Display on */
        delay_ms(50);
        #undef _GPIO_CFG
    }

    if (doom_flash_diag.magic == DOOM_FLASH_DIAG_MAGIC) {
        show_flash_diag();
    }

#ifndef EMULATOR_BUILD
    /* Initialize DAC for audio */
    dac_output_init();
#endif

    /* Initialize SPI Flash FS */
    (void)flash_fs_init();

#ifndef EMULATOR_BUILD
    /* DOOM's framebuffer and zone allocator occupy addresses above the
     * default 96KB SRAM window. EOPB0=0xFE enables the full 224KB. */
    g_eopb0 = USD->eopb0;
#endif

    /* Probe the root directory before FreeRTOS starts. The no-WAD path then
     * performs no SPI access from a task stack. */
    if (!g_skip_doom) {
        g_wad_found = find_doom_wad(&g_iwad_addr, &g_iwad_size);
    }

    /* USB storage is only needed in explicit PRM recovery mode. Starting it
     * during normal boot can service a stale USB IRQ inherited from the
     * bootloader before the device core is ready. */
    if (g_skip_doom) {
        (void)usb_msc_init();
    } else {
#ifndef EMULATOR_BUILD
        nvic_irq_disable(USBFS_L_CAN1_RX0_IRQn);
        NVIC_ClearPendingIRQ(USBFS_L_CAN1_RX0_IRQn);
        crm_periph_reset(CRM_USB_PERIPH_RESET, TRUE);
        crm_periph_reset(CRM_USB_PERIPH_RESET, FALSE);
#endif
    }

    /* Start IWDG — 3 second timeout. Must be fed via watchdog_feed() in
     * the display task loop. If the display task stalls, MCU resets. */
#if !defined(EMULATOR_BUILD) && !defined(GUEST_BUILD)
    watchdog_init();
#endif

    /* Create queues */
    xInputQueue = xQueueCreate(15, sizeof(button_id_t));
    /* button_scan_init() is called inside vInputTask after scheduler starts */

    /* Create tasks */
    xTaskCreate(vDisplayTask, "display", 4096, NULL, 1, &xDisplayTaskHandle);
    xTaskCreate(vInputTask,   "key",     512,  NULL, 4, &xInputTaskHandle);

    /* Validate boot — clears 3-strike counter so bootloader won't enter safe mode */
    boot_validate();

    vTaskStartScheduler();

    for (;;) {}
}

/* ═══════════════════════════════════════════════════════════════════
 * FreeRTOS Hooks & Fault Display
 * ═══════════════════════════════════════════════════════════════════ */

/* fault_display() is defined in watchdog.c */

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    taskDISABLE_INTERRUPTS();
    fault_display("STACK OVF", pcTaskName ? pcTaskName : "?");
    for (;;) {}
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    fault_display("MALLOC FAIL", "heap exhausted");
    for (;;) {}
}

#ifdef EMULATOR_BUILD
unsigned int system_core_clock = 240000000;
void SystemInit(void) {}
void system_clock_config(void) {}
#endif
