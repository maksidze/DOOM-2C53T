#ifndef VIRTUAL_DISK_H
#define VIRTUAL_DISK_H

#include <stdint.h>
#include <stdbool.h>

bool virtual_disk_init(void);
bool virtual_disk_read(uint32_t offset, void *buffer, uint32_t length);
bool virtual_disk_write(uint32_t offset, const void *buffer, uint32_t length);
uint32_t virtual_disk_block_size(void);
uint32_t virtual_disk_block_count(void);

#endif
