#include "usb_msc.h"
#include "virtual_disk.h"
#include "msc_diskio.h"
#include "at32f403a_407.h"
#include "usbd_core.h"
#include "usbd_int.h"
#include "msc_class.h"
#include "msc_desc.h"
#include "msc_bot_scsi.h"
#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

static usbd_core_type usb_core_dev;
static bool usb_initialized;
static volatile usb_msc_diag_t usb_diag;

extern msc_type msc_struct;

/* Exported by the AT32 MSC middleware. Bit 7 is the SCSI write-protect flag. */
extern uint8_t mode_sense6_data[8];
extern uint8_t mode_sense10_data[8];

static uint8_t inquiry[36] = {
    0x00, 0x80, 0x00, 0x01, 31, 0, 0, 0,
    'O','P','E','N','S','C','P',' ',
    '2','C','5','3','T',' ','F','A','T','1',' ','D','I','S','K',' ',
    '1','.','0','0'
};

uint8_t *get_inquiry(uint8_t lun)
{
    usb_diag.inquiry_count++;
    return lun == 0 ? inquiry : 0;
}

usb_sts_type msc_disk_read(uint8_t lun, uint64_t addr, uint8_t *buffer,
                           uint32_t len)
{
    usb_diag.read_count++;
    usb_diag.last_address = (uint32_t)addr;
    usb_diag.last_length = len;
    if (lun != 0 || addr > UINT32_MAX ||
        !virtual_disk_read((uint32_t)addr, buffer, len)) {
        usb_diag.read_errors++;
        return USB_FAIL;
    }
    return USB_OK;
}

usb_sts_type msc_disk_write(uint8_t lun, uint64_t addr, uint8_t *buffer,
                            uint32_t len)
{
    usb_diag.write_count++;
    usb_diag.last_address = (uint32_t)addr;
    usb_diag.last_length = len;
    if (lun != 0 || addr > UINT32_MAX ||
        !virtual_disk_write((uint32_t)addr, buffer, len)) {
        usb_diag.write_errors++;
        return USB_FAIL;
    }
    return USB_OK;
}

usb_sts_type msc_disk_capacity(uint8_t lun, uint32_t *block_count,
                               uint32_t *block_size)
{
    usb_diag.capacity_count++;
    if (lun != 0) return USB_FAIL;
    *block_count = virtual_disk_block_count();
    *block_size = virtual_disk_block_size();
    if (*block_count == 0 || *block_size == 0) return USB_FAIL;
    return USB_OK;
}

void usb_delay_ms(uint32_t ms)
{
    volatile uint32_t count;
    while (ms--) {
        count = system_core_clock / 10000u;
        while (count--) __asm volatile("nop");
    }
}

void usb_delay_us(uint32_t us)
{
    volatile uint32_t count = (system_core_clock / 1000000u) * us;
    while (count--) __asm volatile("nop");
}

void USBFS_L_CAN1_RX0_IRQHandler(void)
{
    usb_diag.irq_count++;
    if (!usb_initialized || usb_core_dev.usb_reg == 0) {
        return;
    }
    usbd_irq_handler(&usb_core_dev);
}

bool usb_msc_init(void)
{
#ifdef EMULATOR_BUILD
    return false;
#else
    usb_initialized = false;
    memset((void *)&usb_diag, 0, sizeof(usb_diag));
    usb_diag.disk_ready = virtual_disk_init();
    if (!usb_diag.disk_ready) return false;

    mode_sense6_data[2] = 0x00;
    mode_sense6_data[0] = 3;
    mode_sense10_data[3] = 0x00;

    crm_clock_source_enable(CRM_CLOCK_SOURCE_HICK, TRUE);
    while(crm_flag_get(CRM_HICK_STABLE_FLAG) != SET) {}
    crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_HICK);
    
    crm_periph_clock_enable(CRM_ACC_PERIPH_CLOCK, TRUE);
    acc_write_c1(7980);
    acc_write_c2(8000);
    acc_write_c3(8020);
    acc_calibration_mode_enable(ACC_CAL_HICKTRIM, TRUE);
    
    crm_periph_clock_enable(CRM_USB_PERIPH_CLOCK, TRUE);
    crm_periph_reset(CRM_USB_PERIPH_RESET, TRUE);
    crm_periph_reset(CRM_USB_PERIPH_RESET, FALSE);

    usbd_core_init(&usb_core_dev, USB, &msc_class_handler, &msc_desc_handler, 0);

    /* The stock bootloader also uses USB. Force a visible detach so the host
     * discards its old device state before this guest enumerates as MSC. */
    usbd_disconnect(&usb_core_dev);
    usb_delay_ms(300);

    usb_initialized = true;
    NVIC_ClearPendingIRQ(USBFS_L_CAN1_RX0_IRQn);
    nvic_irq_enable(USBFS_L_CAN1_RX0_IRQn, 6, 0);
    usbd_connect(&usb_core_dev);
    return true;
#endif
}

bool usb_msc_connected(void)
{
#ifdef EMULATOR_BUILD
    return false;
#else
    return usbd_connect_state_get(&usb_core_dev) == USB_CONN_STATE_CONFIGURED;
#endif
}

uint8_t usb_msc_state(void)
{
#ifdef EMULATOR_BUILD
    return 0;
#else
    if (!usb_initialized) return 0;
    return (uint8_t)usbd_connect_state_get(&usb_core_dev);
#endif
}

void usb_msc_get_diag(usb_msc_diag_t *diag)
{
    if (!diag) return;

    taskENTER_CRITICAL();
    memcpy(diag, (const void *)&usb_diag, sizeof(*diag));
    if (usb_initialized) {
        diag->usb_state = (uint8_t)usbd_connect_state_get(&usb_core_dev);
        diag->msc_state = msc_struct.msc_state;
        diag->bot_status = msc_struct.bot_status;
        diag->last_opcode = msc_struct.cbw_struct.CBWCB[0];
        diag->cbw_flags = msc_struct.cbw_struct.bmCBWFlags;
        diag->transfer_length = msc_struct.cbw_struct.dCBWDataTransferLength;
        diag->residue = msc_struct.csw_struct.dCSWDataResidue;
        diag->csw_status = (uint8_t)msc_struct.csw_struct.bCSWStatus;
    }
    taskEXIT_CRITICAL();
}
