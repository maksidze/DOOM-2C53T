#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t irq_count;
    uint32_t inquiry_count;
    uint32_t capacity_count;
    uint32_t read_count;
    uint32_t write_count;
    uint32_t read_errors;
    uint32_t write_errors;
    uint32_t last_address;
    uint32_t last_length;
    uint32_t transfer_length;
    uint32_t residue;
    uint8_t usb_state;
    uint8_t msc_state;
    uint8_t bot_status;
    uint8_t last_opcode;
    uint8_t cbw_flags;
    uint8_t csw_status;
    bool disk_ready;
} usb_msc_diag_t;

bool usb_msc_init(void);
bool usb_msc_connected(void);
uint8_t usb_msc_state(void);
void usb_msc_get_diag(usb_msc_diag_t *diag);

#endif
