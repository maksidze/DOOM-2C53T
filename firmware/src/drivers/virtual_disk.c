#include "virtual_disk.h"
#include "flash_fs.h"

#define FAT1_BASE_ADDRESS 0x00200000u
#define FAT1_SIZE_BYTES   (14u * 1024u * 1024u)

static bool disk_ready;
static uint32_t disk_block_size;
static uint32_t disk_block_count;

bool virtual_disk_init(void)
{
    const flash_fs_volume_info_t *volume = flash_fs_volume(1);
    disk_ready = volume != 0 && volume->mounted &&
                 volume->base_address == FAT1_BASE_ADDRESS &&
                 volume->size_bytes == FAT1_SIZE_BYTES &&
                 volume->bytes_per_sector != 0 &&
                 volume->total_sectors != 0;
    if (disk_ready) {
        disk_block_size = volume->bytes_per_sector;
        disk_block_count = volume->total_sectors;
    }
    return disk_ready;
}

uint32_t virtual_disk_block_size(void)
{
    return disk_block_size;
}

uint32_t virtual_disk_block_count(void)
{
    return disk_block_count;
}

bool virtual_disk_read(uint32_t offset, void *buffer, uint32_t length)
{
    if (!disk_ready || buffer == 0 || offset > FAT1_SIZE_BYTES ||
        length > FAT1_SIZE_BYTES - offset) {
        return false;
    }
    return flash_fs_raw_read_bytes_direct(FAT1_BASE_ADDRESS + offset,
                                           buffer, length) == FLASH_FS_OK;
}

bool virtual_disk_write(uint32_t offset, const void *buffer, uint32_t length)
{
    if (!disk_ready || buffer == 0 || offset > FAT1_SIZE_BYTES ||
        length > FAT1_SIZE_BYTES - offset) {
        return false;
    }
    return flash_fs_fat1_write_bytes_direct(offset, buffer, length) ==
           FLASH_FS_OK;
}
