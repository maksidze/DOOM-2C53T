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
 *  DOOM graphics stuff.
 *  next-hack: and also key handling, don't ask me why they were put in
 *  i_video.c. Key handling and graphics I/O modified according HW...
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "main.h"
#include <stdlib.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <math.h>
#include "main.h"

#include "doomstat.h"
#include "doomdef.h"
#include "doomtype.h"
#include "v_video.h"
#include "r_draw.h"
#include "d_main.h"
#include "d_event.h"
#include "i_video.h"
#include "i_sound.h"
#include "z_zone.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "st_stuff.h"
#include "lprintf.h"
#include "doomdef.h"

#include "global_data.h"
#include "lcd.h"
#include "button_scan.h"

// DOOM renders at 320x200; output is stretched to the 320x240 LCD.
uint8_t doom_framebuffer[SCREENWIDTH * SCREENHEIGHT];

#define NO_PALETTE_CHANGE 255

uint16_t palette[256];

static bool button_is_pressed(button_id_t btn) {
    uint16_t raw = button_scan_get_raw();
    const button_id_t bit_to_button[15] = {
        BTN_POWER, BTN_AUTO, BTN_CH1, BTN_MOVE, BTN_SELECT, BTN_TRIGGER,
        BTN_PRM, BTN_CH2, BTN_SAVE, BTN_MENU, BTN_UP, BTN_DOWN,
        BTN_LEFT, BTN_RIGHT, BTN_OK
    };
    for(int i=0; i<15; i++) {
        if(bit_to_button[i] == btn) return (raw & (1 << i)) != 0;
    }
    return false;
}

void I_StartTic(void)
{
    static uint16_t oldGameKeyState = 0;
    uint16_t gameKeyState = 0;
    if (button_is_pressed(BTN_UP)) gameKeyState |= 1 << KEYD_UP;
    if (button_is_pressed(BTN_DOWN)) gameKeyState |= 1 << KEYD_DOWN;
    if (button_is_pressed(BTN_LEFT)) gameKeyState |= 1 << KEYD_LEFT;
    if (button_is_pressed(BTN_RIGHT)) gameKeyState |= 1 << KEYD_RIGHT;
    if (button_is_pressed(BTN_OK)) gameKeyState |= 1 << KEYD_FIRE;     // F1 = FIRE
    if (button_is_pressed(BTN_MENU)) gameKeyState |= 1 << KEYD_USE;     // F2 = USE
    if (button_is_pressed(BTN_AUTO)) gameKeyState |= 1 << KEYD_SPEED;   // F3 = SPEED/RUN
    if (button_is_pressed(BTN_SAVE)) gameKeyState |= 1 << KEYD_MENU;    // F4 = MENU

    
    if (button_is_pressed(BTN_CH1)) gameKeyState |= 1 << KEYD_CHGWDOWN; // Next weapon
    if (button_is_pressed(BTN_CH2)) gameKeyState |= 1 << KEYD_CHGW;     // Prev weapon

    uint16_t keys_changed = oldGameKeyState ^ gameKeyState;

    event_t ev;
    ev.type = ev_keydown;
    for (int i = 0; i < NUMKEYS; i++)
    {
        if ((1 & (keys_changed >> i)) && (1 & (gameKeyState >> i)))
        {
            ev.data1 = i;
            D_PostEvent(&ev);
        }
    }
    ev.type = ev_keyup;
    for (int i = 0; i < NUMKEYS; i++)
    {
        if ((1 & (keys_changed >> i)) && !(1 & (gameKeyState >> i)))
        {
            ev.data1 = i;
            D_PostEvent(&ev);
        }
    }
    oldGameKeyState = gameKeyState;
}

boolean I_StartDisplay(void)
{
    _g->screens[0].data = doom_framebuffer;
    drawvars.byte_topleft = _g->screens[0].data;
    return true;
}

/////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////
// Palette stuff.
//

static void I_UploadNewPalette(int pal)
{
    static int uploaded_palette = -1;

    /* PLAYPAL contains 14 palettes (0..13). Keep a damaged state value from
     * turning the entire display into an out-of-bounds palette read. */
    if (pal < 0 || pal > 13) {
        pal = 0;
    }
    if (pal == uploaded_palette) {
        return;
    }

    _g->current_palette = palette;
    //
    if (_g->gamma)
    {
   
        for (int i = 0; i < 256; i++)
        {
            uint16_t r = gammatable[_g->gamma][p_wad_immutable_flash_data->palette_lump[pal * 256 * 3 + 3 * i]] >> 3;
            uint16_t g = gammatable[_g->gamma][p_wad_immutable_flash_data->palette_lump[pal * 256 * 3 + 3 * i + 1]] >> 2;
            uint16_t b = gammatable[_g->gamma][p_wad_immutable_flash_data->palette_lump[pal * 256 * 3 + 3 * i + 2]] >> 3;
            uint16_t rgb = (r << (6 + 5)) | (g << 5) | b;
            _g->current_palette[i] = rgb;
        }
    }
    else
    {   // save some cycles for those playing with 0 gamma.
           for (int i = 0; i < 256; i++)
        {
            uint16_t r = p_wad_immutable_flash_data->palette_lump[pal * 256 * 3 + 3 * i] >> 3;
            uint16_t g = p_wad_immutable_flash_data->palette_lump[pal * 256 * 3 + 3 * i + 1] >> 2;
            uint16_t b = p_wad_immutable_flash_data->palette_lump[pal * 256 * 3 + 3 * i + 2] >> 3;
            uint16_t rgb = (r << (6 + 5)) | (g << 5) | b;
            _g->current_palette[i] = rgb;
        }
    }
    uploaded_palette = pal;
}

//////////////////////////////////////////////////////////////////////////////
// Graphics API
//

void I_FinishUpdateBlock(uint8_t numberOfRows)
{
    I_UploadNewPalette(_g->newpal);

    if (numberOfRows > SCREENHEIGHT) {
        numberOfRows = SCREENHEIGHT;
    }

    uint16_t output_rows = (uint16_t)((uint32_t)numberOfRows * LCD_HEIGHT /
                                      SCREENHEIGHT);
    if (output_rows == 0) {
        return;
    }
    lcd_set_window(0, 0, LCD_WIDTH, output_rows);

    for (uint16_t y = 0; y < numberOfRows; y++) {
        uint16_t first_output_row = (uint16_t)((uint32_t)y * LCD_HEIGHT /
                                               SCREENHEIGHT);
        uint16_t next_output_row = (uint16_t)((uint32_t)(y + 1U) * LCD_HEIGHT /
                                              SCREENHEIGHT);
        uint16_t repeats = next_output_row - first_output_row;
        const uint8_t *row = &doom_framebuffer[(uint32_t)y * SCREENWIDTH];

        for (uint16_t repeat = 0; repeat < repeats; repeat++) {
            for (uint16_t x = 0; x < SCREENWIDTH; x++) {
                *LCD_DATA_ADDR = _g->current_palette[row[x]];
            }
        }
    }
}

//
// I_SetPalette
//
void I_SetPalette(int pal)
{
    _g->newpal = pal;
}
