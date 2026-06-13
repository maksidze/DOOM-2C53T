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
 *   next-hack:  Network game support. Note, we are calling doom_ble functions
 *               directly. We do not provide an abstraction layer.
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#include "doomtype.h"
#include "doomstat.h"
#include "d_net.h"
#include "z_zone.h"

#include "d_main.h"
#include "g_game.h"
#include "m_menu.h"

#include "i_system.h"
#include "i_main.h"
#include "i_video.h"
#include "lprintf.h"

#include "global_data.h"
// removed graphics.h
// removed doom_ble.h
//
//
//
void D_InitNetGame(void)
{
  for (int i = 0; i < MAXPLAYERS; i++)
  {
    _g->playeringame[i] = false;
  }
  _g->playeringame[_g->consoleplayer] = true;
}

void NetUpdate(void)
{
}
void D_BuildNewTiccmds(void)
{
    int newtics = I_GetTime() - _g->lastmadetic;
    _g->lastmadetic += newtics;

    while (newtics--)
    {
        I_StartTic();
        if (_g->maketic - _g->gametic > BACKUPTICS - 2)
            break;

        G_BuildTiccmd(&_g->netcmds[_g->consoleplayer][_g->maketic % BACKUPTICS]);
        _g->maketic++;
    }
}

void TryRunTics(void)
{
#if !STATIC_INTERCEPTS
    // next-hack. Here we are using a big stack, so intercepts can be put here.
    intercept_t stack_intercepts[MAXINTERCEPTS];
    boolean stack_interceptIsALine[MAXINTERCEPTS];
    interceptIsALine = stack_interceptIsALine;
    intercepts = stack_intercepts;
#endif
    int lastSoundTime;
    static int lastEnterTime;
    int runtics;
    int entertime = I_GetTime();
    lastSoundTime = entertime;
    int maxTics = entertime - lastEnterTime;
    lastEnterTime = entertime;
    // Wait for tics to run
    while (1)
    {
#ifdef HAS_NETWORK
        NetUpdate();
#else
        D_BuildNewTiccmds();
#endif

        if (_g->netgame)
        {
          runtics = _g->remotetic - _g->gametic;
          if (maxTics == 0)
          {
            lastEnterTime = I_GetTime();
            maxTics = lastEnterTime - entertime;

          }
          // if for any reason executing all the required ticks would deplete
          // all network tic, then we must reduce the number of tics to run,
          // to prevent that next time we don't have any tics to run, stalling
          // for up to a frame.
          if (maxTics >= runtics && runtics > 2) // was 4
          {
            runtics--;    // next time we will have at least one free
          }
          // do not execute too many tics even if available
          if (runtics > maxTics )
          {
              runtics = maxTics;
          }
        }
        else
        {
          runtics = _g->maketic - _g->gametic;
        }
        if (runtics <= 0)
        {
            // run menu anyway.
            uint32_t timeNow = I_GetTime();
            if (timeNow - lastSoundTime > 1)    // next-hack prevent audio glitches in multiplayer.
            {
              lastSoundTime = timeNow;
              if (!nosfxparm)
                  updateSound();
            }
            if (timeNow - entertime > 4)
            {
                //printf("MT\r\n");
                M_Ticker();
                return;
            }
        }
        else
            break;
    }
    while (runtics-- > 0)
    {

        if (_g->advancedemo)
            D_DoAdvanceDemo();
        M_Ticker();
        G_Ticker();
        _g->gametic++;
    }
#ifdef HAS_NETWORK
    NetUpdate();
#endif
}
