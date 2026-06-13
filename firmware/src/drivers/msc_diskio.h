#ifndef MSC_DISKIO_H
#define MSC_DISKIO_H

#include "usb_std.h"

uint8_t *get_inquiry(uint8_t lun);
usb_sts_type msc_disk_read(uint8_t lun, uint64_t addr, uint8_t *buffer,
                           uint32_t len);
usb_sts_type msc_disk_write(uint8_t lun, uint64_t addr, uint8_t *buffer,
                            uint32_t len);
usb_sts_type msc_disk_capacity(uint8_t lun, uint32_t *block_count,
                               uint32_t *block_size);

#endif
