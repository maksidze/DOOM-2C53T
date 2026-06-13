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
 * DESCRIPTION:
 *  System interface for sound.
 *  next-hack: added support for pwm and DAC sound.
 *
 *-----------------------------------------------------------------------------
 */

#include <stdint.h>
#include <string.h>

#include "i_sound.h"
#include "s_sound.h"
#include "w_wad.h"
#include "extMemory.h"
#include "dac_output.h"
#include "lprintf.h"

#define DOOM_AUDIO_RATE       11025U
#define DOOM_AUDIO_CHANNELS   8
#define DOOM_AUDIO_DELAY      128U
#define DMX_DATA_SOUND_OFFSET 8U
#define AUDIO_BUFFER_MASK     (DAC_BUFFER_SIZE - 1U)
#define AUDIO_READ_CHUNK      512U
#define DAC_MID_PAIR          ((2048U << 16) | 2048U)

_Static_assert((DAC_BUFFER_SIZE & AUDIO_BUFFER_MASK) == 0,
               "DAC buffer size must be a power of two");

typedef struct {
    uint16_t last_buffer_index;
    uint32_t offset;
    uint8_t sfx_index;
    uint8_t volume;
} doom_sound_channel_t;

static doom_sound_channel_t sound_channels[DOOM_AUDIO_CHANNELS];
static uint8_t sample_scratch[AUDIO_READ_CHUNK];
static uint8_t sound_ready;
static uint8_t sound_start_logs;
static uint8_t sound_dma_log;
static uint8_t sound_mix_logs;

void I_UpdateSoundParams(int channel, int volume, int separation)
{
    (void)separation;

    if (channel < 0 || channel >= DOOM_AUDIO_CHANNELS) {
        return;
    }
    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    sound_channels[channel].volume = (uint8_t)volume;
}

int I_StartSound(int id, int channel, int vol, int sep)
{
    if (!sound_ready || channel < 0 || channel >= DOOM_AUDIO_CHANNELS ||
        id <= 0 || id >= NUMSFX) {
        return -1;
    }

    I_UpdateSoundParams(channel, vol, sep);
    sound_channels[channel].last_buffer_index = UINT16_MAX;
    sound_channels[channel].offset = 0;
    sound_channels[channel].sfx_index = (uint8_t)id;
    if (sound_start_logs < 8U) {
        const soundLumpData_t *lump =
            &p_wad_immutable_flash_data->soundLumps[id];
        lprintf(LO_INFO,
                "AUDIO START id:%d ch:%d vol:%d addr:%08lX len:%lu inc:%u\r\n",
                id, channel, vol, (unsigned long)lump->lumpAddress,
                (unsigned long)lump->length, lump->increment);
        ++sound_start_logs;
    }
    return channel;
}

void I_StopSound(int channel)
{
    if (channel < 0 || channel >= DOOM_AUDIO_CHANNELS) {
        return;
    }
    sound_channels[channel].sfx_index = 0;
    sound_channels[channel].volume = 0;
}

void I_InitSound(void)
{
    uint32_t *output = dac_output_get_buffer();

    memset(sound_channels, 0, sizeof(sound_channels));
    sound_start_logs = 0;
    sound_dma_log = 0;
    sound_mix_logs = 0;
    for (unsigned int i = 0; i < DAC_BUFFER_SIZE; ++i) {
        output[i] = DAC_MID_PAIR;
    }
    dac_output_start(DOOM_AUDIO_RATE);
    sound_ready = 1;
    lprintf(LO_INFO, "I_InitSound: DAC audio at %lu Hz\r\n",
            (unsigned long)DOOM_AUDIO_RATE);
}

void updateSound(void)
{
    if (!sound_ready || !p_wad_immutable_flash_data ||
        !p_wad_immutable_flash_data->soundLumps) {
        return;
    }

    uint32_t *output = dac_output_get_buffer();
    uint32_t current = dac_output_get_position();
    uint32_t start = (current + DOOM_AUDIO_DELAY) & AUDIO_BUFFER_MASK;
    uint32_t fill_count = (current - 1U - start) & AUDIO_BUFFER_MASK;

    if (!sound_dma_log && current != 0U) {
        lprintf(LO_INFO, "AUDIO DMA moving pos:%lu fill:%lu\r\n",
                (unsigned long)current, (unsigned long)fill_count);
        sound_dma_log = 1;
    }

    for (uint32_t n = 0; n < fill_count; ++n) {
        output[(start + n) & AUDIO_BUFFER_MASK] = DAC_MID_PAIR;
    }

    for (int ch = 0; ch < DOOM_AUDIO_CHANNELS; ++ch) {
        doom_sound_channel_t *channel = &sound_channels[ch];
        if (!channel->sfx_index || !channel->volume) {
            continue;
        }

        if (channel->last_buffer_index != UINT16_MAX) {
            channel->offset +=
                (start - channel->last_buffer_index) & AUDIO_BUFFER_MASK;
        }
        channel->last_buffer_index = (uint16_t)start;

        soundLumpData_t *lump =
            &p_wad_immutable_flash_data->soundLumps[channel->sfx_index];
        uint32_t increment = lump->increment ? (uint32_t)lump->increment : 1U;
        uint32_t total_samples = ((uint32_t)lump->length + increment - 1U) / increment;
        if (!lump->lumpAddress || channel->offset >= total_samples) {
            I_StopSound(ch);
            continue;
        }

        uint32_t samples_left = total_samples - channel->offset;
        uint32_t samples_to_mix = samples_left < fill_count ? samples_left : fill_count;
        uint32_t mixed = 0;
        uint16_t mixed_min = 4095U;
        uint16_t mixed_max = 0U;

        while (mixed < samples_to_mix) {
            uint32_t chunk_samples = samples_to_mix - mixed;
            uint32_t max_chunk_samples = AUDIO_READ_CHUNK / increment;
            if (chunk_samples > max_chunk_samples) {
                chunk_samples = max_chunk_samples;
            }

            uint32_t source_address = (uint32_t)lump->lumpAddress +
                DMX_DATA_SOUND_OFFSET + (channel->offset + mixed) * increment;
            extMemSetCurrentAddress(source_address);
            extMemGetDataFromCurrentAddress(sample_scratch,
                                            chunk_samples * increment);

            for (uint32_t n = 0; n < chunk_samples; ++n) {
                uint32_t dst = (start + mixed + n) & AUDIO_BUFFER_MASK;
                int32_t sample = (int32_t)sample_scratch[n * increment] - 128;
                int32_t value = (int32_t)(output[dst] & 0xFFFU) +
                    ((sample * channel->volume) >> 6);
                if (value < 0) value = 0;
                if (value > 4095) value = 4095;
                output[dst] = ((uint32_t)value << 16) | (uint32_t)value;
                if ((uint16_t)value < mixed_min) mixed_min = (uint16_t)value;
                if ((uint16_t)value > mixed_max) mixed_max = (uint16_t)value;
            }
            mixed += chunk_samples;
        }
        if (mixed && sound_mix_logs < 8U) {
            lprintf(LO_INFO,
                    "AUDIO MIX id:%u samples:%lu range:%u..%u off:%lu/%lu\r\n",
                    channel->sfx_index, (unsigned long)mixed,
                    mixed_min, mixed_max, (unsigned long)channel->offset,
                    (unsigned long)total_samples);
            ++sound_mix_logs;
        }
    }
}

void I_InitMusic(void) { }
void I_PlaySong(int handle, int looping) { }
void I_PauseSong(int handle) { }
void I_ResumeSong(int handle) { }
void I_StopSong(int handle) { }
void I_SetMusicVolume(int volume) { }
