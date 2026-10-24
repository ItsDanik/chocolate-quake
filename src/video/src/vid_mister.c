/*
 * Copyright (C) 1996-1997 Id Software, Inc.
 * Copyright (C) Henrique Barateli, <henriquejb194@gmail.com>, et al.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
// vid_mister.c -- the video of the MiSTer build (MISTER_HYBRID), in place of
// vid_window.c, vid_buffers.c, vid_modes.c and vid_menu.c.
//
// The game draws into the window surface of SDL's "mister" driver, 8 bits
// with the palette done by the FPGA core (SDL_MISTER_VIDEO_FORMAT=INDEX8),
// as DOS Quake drew into the VGA's memory. The surface's pixels are the
// library's two frame buffers in turn (SDL_MISTER_DIRECT), so the frame goes
// to the core without a copy: two pages, as Quake had with a VGA's page
// flipping (vid.numpages), each holding the frame before last; the status
// bar and the border, drawn only when they change, are drawn on both.
// MISTER_HYBRID_DIRECT=0 copies the frame instead, with one page. The mode is
// the OSD's Resolution (320x200, 640x200, 320x240, 640x240), read every
// frame.


#include "vid_buffers.h"
#include "vid_menu.h"
#include "vid_modes.h"
#include "vid_window.h"
#include "config.h"
#include "d_local.h"
#include "draw.h"
#include "host.h"
#include "keys.h"
#include "mister_port.h"
#include "sound.h"
#include "sys.h"
#include <SDL.h>

static SDL_Window* window = NULL;
static SDL_Surface* surface = NULL;

static i32 VID_highhunkmark;
static i32 vid_surfcachesize;

static qboolean palette_changed;
static qboolean direct;
static SDL_Color pal[256];


/*
================================================================================

PALETTE

================================================================================
*/

void VID_SetPalette(const byte* palette) {
    for (i32 i = 0; i < 256; i++) {
        // the VGA's 6 bits of each channel
        pal[i].r = palette[i * 3] & ~3;
        pal[i].g = palette[i * 3 + 1] & ~3;
        pal[i].b = palette[i * 3 + 2] & ~3;
        pal[i].a = SDL_ALPHA_OPAQUE;
    }
    palette_changed = true;
}

void VID_ShiftPalette(const byte* palette) {
    VID_SetPalette(palette);
}


/*
================================================================================

BUFFERS

================================================================================
*/

void VID_FreeBuffers(void) {
    if (d_pzbuffer) {
        D_FlushCaches();
        Hunk_FreeToHighMark(VID_highhunkmark);
        d_pzbuffer = NULL;
    }
}

static void VID_AllocBuffers(void) {
    VID_FreeBuffers();

    // the z buffer and the surface cache
    vid_surfcachesize = D_SurfaceCacheForRes(vid.width, vid.height);
    i32 chunk = vid.width * vid.height * sizeof(*d_pzbuffer);
    VID_highhunkmark = Hunk_HighMark();
    d_pzbuffer = Hunk_HighAllocName(chunk + vid_surfcachesize, "video");
    if (!d_pzbuffer) {
        Sys_Error("Not enough memory for video mode\n");
    }
    D_InitCaches((byte*) d_pzbuffer + chunk, vid_surfcachesize);
}

void VID_LockBuffer(void) {
}

void VID_UnlockBuffer(void) {
}


/*
================================================================================

MODES

================================================================================
*/

static void VID_ApplyMode(i32 width, i32 height) {
    vid.width = width;
    vid.height = height;
    // 640 pixels across a 15kHz screen are half as wide as 320: the 2D is
    // drawn twice as wide (draw.c), to look as in 320x200
    vid.width2d = width >= 640 ? width / 2 : width;
    // 240 lines: the view fills the screen, the status bar and the
    // console's lines are drawn over it inside the HUD margins
    vid.hud_over_view = height == 240;
    MiSTer_HudRows(height, &vid.hud_top, &vid.hud_bottom);
    // the pixels' shape on a 4:3 screen, as DOS Quake's 320x200
    vid.aspect = ((float) vid.height / (float) vid.width) * (320.0f / 240.0f);
    vid.recalc_refdef = true;
    vid.numpages = direct ? 2 : 1;
    vid.colormap = host_colormap;

    if (!window) {
        window = SDL_CreateWindow(PACKAGE_STRING, 0, 0, width, height, 0);
        if (!window) {
            Sys_Error("Error creating window for video startup: %s",
                      SDL_GetError());
        }
    } else {
        SDL_SetWindowSize(window, width, height);
    }
    surface = SDL_GetWindowSurface(window);
    if (!surface || surface->pitch != width) {
        Sys_Error("No 8 bit window surface of %dx%d: %s", width, height,
                  SDL_GetError());
    }
    SDL_FillRect(surface, NULL, 0);
    vid.buffer = (byte*) surface->pixels;
    palette_changed = true;

    VID_AllocBuffers();
}

static void VID_SetOSDMode(void) {
    i32 width, height;
    MiSTer_Resolution(&width, &height);
    if (window && (u32) width == vid.width && (u32) height == vid.height) {
        // the HUD margins, as the player changes them
        MiSTer_HudRows(height, &vid.hud_top, &vid.hud_bottom);
        return;
    }
    VID_ApplyMode(width, height);
}

void VID_InitModes(void) {
    VID_SetOSDMode();
}

void VID_UpdateModes(void) {
    VID_SetOSDMode();
}

void VID_SetMode(i32 modenum) {
    // Quake's fall back to mode 0 when memory is short: there is one mode
}

qboolean VID_IsFullscreenMode(void) {
    return true;
}

qboolean VID_IsWindowedMode(void) {
    return false;
}

qboolean VID_WindowedMouse(void) {
    return false;
}

void VID_ToggleMouseGrab(void) {
}


/*
================================================================================

WINDOW

================================================================================
*/

void VID_InitWindow(void) {
    const char* env = getenv("MISTER_HYBRID_DIRECT");

    SDL_SetHint("SDL_MISTER_VIDEO_FORMAT", "INDEX8");
    direct = !env || atoi(env) != 0;
    SDL_SetHint("SDL_MISTER_DIRECT", direct ? "1" : "0");
}

void VID_ShutdownWindow(void) {
    if (window) {
        SDL_DestroyWindow(window);
        window = NULL;
        surface = NULL;
    }
}

void VID_UpdateWindow(vrect_t* rect) {
    if (!surface || !rect) {
        return;
    }
    if (palette_changed) {
        SDL_SetPaletteColors(surface->format->palette, pal, 0, 256);
        palette_changed = false;
    }
    MiSTer_FrameHash(vid.buffer, vid.width * vid.height, d_pzbuffer,
                     vid.width * vid.height * sizeof(*d_pzbuffer));
    MiSTer_Mark(MISTER_T_BLIT);
    SDL_UpdateWindowSurface(window);
    // the next page (SDL_MISTER_DIRECT)
    vid.buffer = (byte*) surface->pixels;
}

void VID_HandlePause(qboolean pause) {
}


/*
================================================================================

VIDEO MENU

================================================================================
*/

void M_Menu_Options_f(void);
void M_DrawPic(i32 x, i32 y, qpic_t* pic);
void M_Print(i32 cx, i32 cy, char* str);
void M_PrintWhite(i32 cx, i32 cy, char* str);

void VID_MenuDraw(void) {
    qpic_t* p = Draw_CachePic("gfx/vidmodes.lmp");
    M_DrawPic((320 - p->width) / 2, 4, p);

    char mode[32];
    sprintf(mode, "%dx%d", vid.width, vid.height);
    M_Print(8 * 8, 40, "Resolution:");
    M_PrintWhite(21 * 8, 40, mode);

    M_Print(4 * 8, 64, "The resolution is set in the");
    M_Print(4 * 8, 72, "core's menu (the OSD), on the");
    M_Print(4 * 8, 80, "line Resolution.");
}

void VID_MenuKey(i32 key) {
    switch (key) {
        case K_ESCAPE:
        case K_BBUTTON:
        case K_ENTER:
        case K_ABUTTON:
            S_LocalSound("misc/menu1.wav");
            M_Menu_Options_f();
            break;
        default:
            break;
    }
}
