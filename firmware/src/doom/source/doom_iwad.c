/**
 *  Doom Port to Silicon Labs EFR32xG24 devices and MGM240 modules
 *  by Nicola Wrachien (next-hack in the comments).
 *
 *  This port is based on the excellent doomhack's GBA Doom Port,
 *  with Kippykip additions.
 *  
 *  Several data structures and functions have been optimized 
 *  to fit in only 256kB RAM (GBA has 384 kB RAM).
 *  Z-Depth Light has been restored with almost no RAM consumption!
 *  Added BLE-based multiplayer.
 *  Added OPL2-based music.
 *  Restored screen melt effect!
 *  Tons of speed optimizations have been done, and the game now
 *  runs extremely fast, despite the much higher 3D resolution with
 *  respect to GBA.
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *  Copyright (C) 2021-2023 Nicola Wrachien (next-hack in the comments)
 *  on the EFR32xG24 and MGM240 port.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 *  DESCRIPTION:
 *  wad pointers.
 *
 */
#pragma GCC optimize ("-O0")
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "w_wad.h"
#include "doom_iwad.h"
#include "flash_fs.h"
#include "watchdog.h"
unsigned char *doom_iwad = (unsigned char*) WAD_ADDRESS;
unsigned int *p_doom_iwad_len = (unsigned int*) (WAD_ADDRESS - 4);
unsigned int doom_iwad_flash_offset;
unsigned int doom_iwad_size;

static bool fat12_mapped;
static uint32_t fat12_volume_base;
static uint32_t fat12_fat_offset;
static uint32_t fat12_data_offset;
static uint32_t fat12_cluster_size;
static uint16_t fat12_first_cluster;

#define DOOM_IWAD_MAX_EXTENTS 128U
typedef struct {
    uint32_t logical_cluster;
    uint16_t physical_cluster;
    uint16_t cluster_count;
} doom_iwad_extent_t;

static doom_iwad_extent_t fat12_extents[DOOM_IWAD_MAX_EXTENTS];
static uint32_t fat12_extent_count;

static bool fat12_next_cluster(uint16_t cluster, uint16_t *next)
{
    uint8_t entry[2];
    uint32_t offset = cluster + (cluster >> 1);

    if (flash_fs_raw_read_bytes_direct(fat12_volume_base + fat12_fat_offset + offset,
                                       entry, sizeof(entry)) != FLASH_FS_OK) {
        return false;
    }

    uint16_t value = (uint16_t)entry[0] | ((uint16_t)entry[1] << 8);
    value = (cluster & 1U) ? (value >> 4) : (value & 0x0FFFU);
    if (value < 2U || value >= 0x0FF0U) {
        return false;
    }

    *next = value;
    return true;
}

void doom_iwad_configure(unsigned int flash_offset, unsigned int size)
{
    doom_iwad = (unsigned char*) WAD_ADDRESS;
    p_doom_iwad_len = (unsigned int*) (WAD_ADDRESS - 4);
    doom_iwad_flash_offset = flash_offset;
    doom_iwad_size = size;
    fat12_mapped = false;
}

void doom_iwad_configure_fat12(unsigned int volume_base,
                               unsigned int fat_offset,
                               unsigned int data_offset,
                               unsigned int cluster_size,
                               unsigned int first_cluster,
                               unsigned int size)
{
    doom_iwad_configure(0, size);
    fat12_volume_base = volume_base;
    fat12_fat_offset = fat_offset;
    fat12_data_offset = data_offset;
    fat12_cluster_size = cluster_size;
    fat12_first_cluster = (uint16_t)first_cluster;
    fat12_extent_count = 0;
    fat12_mapped = cluster_size != 0 && first_cluster >= 2 && first_cluster < 0x0FF0U;

    uint32_t cluster_count = (size + cluster_size - 1U) / cluster_size;
    uint16_t cluster = fat12_first_cluster;
    for (uint32_t index = 0; fat12_mapped && index < cluster_count; index++) {
        doom_iwad_extent_t *extent = fat12_extent_count == 0 ? NULL :
                                     &fat12_extents[fat12_extent_count - 1U];
        if (extent != NULL &&
            cluster == (uint16_t)(extent->physical_cluster + extent->cluster_count)) {
            extent->cluster_count++;
        } else {
            if (fat12_extent_count >= DOOM_IWAD_MAX_EXTENTS) {
                fat12_mapped = false;
                break;
            }
            extent = &fat12_extents[fat12_extent_count++];
            extent->logical_cluster = index;
            extent->physical_cluster = cluster;
            extent->cluster_count = 1;
        }

        if (index + 1U < cluster_count && !fat12_next_cluster(cluster, &cluster)) {
            fat12_mapped = false;
        }
        if ((index & 0x3FU) == 0x3FU) {
            watchdog_feed();
        }
    }
}

bool doom_iwad_read(unsigned int offset, void *dest, unsigned int length)
{
    uint8_t *out = (uint8_t *)dest;
    if (dest == NULL || offset > doom_iwad_size || length > doom_iwad_size - offset) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    if (!fat12_mapped) {
        return flash_fs_raw_read_bytes_direct(doom_iwad_flash_offset + offset,
                                              dest, length) == FLASH_FS_OK;
    }

    while (length != 0) {
        uint32_t target_index = offset / fat12_cluster_size;
        const doom_iwad_extent_t *extent = NULL;
        for (uint32_t i = 0; i < fat12_extent_count; i++) {
            uint32_t extent_end = fat12_extents[i].logical_cluster +
                                  fat12_extents[i].cluster_count;
            if (target_index >= fat12_extents[i].logical_cluster &&
                target_index < extent_end) {
                extent = &fat12_extents[i];
                break;
            }
        }
        if (extent == NULL) {
            return false;
        }

        uint16_t cluster = (uint16_t)(extent->physical_cluster +
                           target_index - extent->logical_cluster);
        uint32_t within = offset % fat12_cluster_size;
        uint32_t chunk = fat12_cluster_size - within;
        if (chunk > length) {
            chunk = length;
        }
        uint32_t address = fat12_volume_base + fat12_data_offset +
                           (uint32_t)(cluster - 2U) * fat12_cluster_size + within;
        if (flash_fs_raw_read_bytes_direct(address, out, chunk) != FLASH_FS_OK) {
            return false;
        }

        offset += chunk;
        out += chunk;
        length -= chunk;
    }
    return true;
}

bool doom_iwad_validate(void)
{
    wadinfo_t header;
    if (!doom_iwad_read(0, &header, sizeof(header)) ||
        memcmp(header.identification, "IWAD", 4) != 0 ||
        header.numlumps <= 0 || header.infotableofs < 0) {
        return false;
    }

    uint32_t directory_size = (uint32_t)header.numlumps * sizeof(filelump_t);
    if ((uint32_t)header.infotableofs > doom_iwad_size ||
        directory_size > doom_iwad_size - (uint32_t)header.infotableofs) {
        return false;
    }

    for (int i = 0; i < header.numlumps; i++) {
        filelump_t lump;
        uint32_t offset = (uint32_t)header.infotableofs +
                          (uint32_t)i * sizeof(lump);
        if (!doom_iwad_read(offset, &lump, sizeof(lump))) {
            return false;
        }
        if (memcmp(lump.name, "PLAYPAL", 7) == 0 && lump.name[7] == '\0') {
            return true;
        }
    }
    return false;
}
