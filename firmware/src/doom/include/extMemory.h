#ifndef SRC_EXTMEMORY_H_
#define SRC_EXTMEMORY_H_

#include <stdint.h>
#include <string.h>
#include "i_memory.h"
#include "doom_iwad.h"
#include "flash_fs.h"
#include "watchdog.h"

extern uint32_t currentSpiAddress;

static inline void extMemSetCurrentAddress(uint32_t address)
{
    currentSpiAddress = address;
}

static inline void* extMemGetDataFromCurrentAddress(void *dest, unsigned int length)
{
    if (currentSpiAddress == (uint32_t)p_doom_iwad_len) {
        if (length == 4) {
            memcpy(dest, &doom_iwad_size, 4);
        } else {
            memset(dest, 0, length);
        }
    } else if (currentSpiAddress >= WAD_ADDRESS &&
               currentSpiAddress - WAD_ADDRESS < doom_iwad_size) {
        uint32_t offset = currentSpiAddress - WAD_ADDRESS;
        if (!doom_iwad_read(offset, dest, length)) {
            memset(dest, 0, length);
        }
    } else if (currentSpiAddress >= FLASH_PTR_BASE &&
               currentSpiAddress < FLASH_PTR_BASE + 0x00100000U) {
        memcpy(dest, (const void *)currentSpiAddress, length);
    } else {
        flash_fs_raw_read_bytes_direct(currentSpiAddress, dest, length);
    }
    currentSpiAddress += length;
    watchdog_feed();
    return dest;
}

static inline short extMemFlashGetShortFromAddress(const void *addr)
{
    short val = 0;
    extMemSetCurrentAddress((uint32_t)addr);
    extMemGetDataFromCurrentAddress(&val, sizeof(val));
    return val;
}

static inline uint8_t extMemGetByteFromAddress(const void *addr)
{
    uint8_t val = 0;
    extMemSetCurrentAddress((uint32_t)addr);
    extMemGetDataFromCurrentAddress(&val, sizeof(val));
    return val;
}

#define extMemGetSize() (1024 * 1024)

static inline void extMemWrite(uint32_t addr, const void *src, uint32_t size) {
    // Stub for now
}

#define EXT_MEMORY_HEADER_SIZE 0
#define EXT_MEMORY_READ_ALIGN_SIZE 0

static inline void* extMemStartAsynchDataRead(uint32_t addr, void *dest, uint32_t length) {
    extMemSetCurrentAddress(addr);
    return extMemGetDataFromCurrentAddress(dest, length);
}

static inline void extMemWaitAsynchDataRead(void) {
}

#endif /* SRC_EXTMEMORY_H_ */
