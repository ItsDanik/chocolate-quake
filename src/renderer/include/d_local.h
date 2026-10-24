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
// d_local.h:  private rasterization driver defs

#ifndef D_LOCAL_H
#define D_LOCAL_H

#include "quakedef.h"
#include "r_shared.h"
#include "cvar.h"
#include "model.h"
#include "vid.h"

//
// TODO: fine-tune this; it's based on providing some overage even if there
// is a 2k-wide scan, with subdivision every 8, for 256 spans of 12 bytes each
//
#define SCANBUFFERPAD 0x1000

#define R_SKY_SMASK 0x007F0000
#define R_SKY_TMASK 0x007F0000

#define DS_SPAN_LIST_END -128

#define SURFCACHE_SIZE_AT_320X200 600 * 1024

typedef struct surfcache_s {
    struct surfcache_s* next;
    struct surfcache_s** owner; // NULL is an empty chunk of memory
    i32 lightadj[MAXLIGHTMAPS]; // checked for strobe flush
    i32 dlight;
    i32 size; // including header
    u32 width;
    u32 height; // DEBUG only needed for debug
    float mipscale;
    struct texture_s* texture; // checked for animating textures
    i32 jobs; // span jobs that still read it (d_spanjob.c)
    byte data[4];              // width*height elements
} surfcache_t;

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct sspan_s {
    i32 u, v, count;
} sspan_t;

extern cvar_t d_subdiv16;

extern float scale_for_mip;

extern qboolean d_roverwrapped;
extern surfcache_t* sc_rover;
extern surfcache_t* d_initial_rover;

extern float d_sdivzstepu, d_tdivzstepu, d_zistepu;
extern float d_sdivzstepv, d_tdivzstepv, d_zistepv;
extern float d_sdivzorigin, d_tdivzorigin, d_ziorigin;

extern fixed16_t sadjust, tadjust;
extern fixed16_t bbextents, bbextentt;


void D_DrawSpans8(espan_t* pspans);
void D_DrawSpans16(espan_t* pspans);
void D_DrawZSpans(espan_t* pspans);
void Turbulent8(espan_t* pspan);
void D_SpriteDrawSpans(sspan_t* pspan);

void D_DrawSkyScans8(espan_t* pspan);
void D_DrawSkyScans16(espan_t* pspan);

void R_ShowSubDiv(void);
surfcache_t* D_CacheSurface(msurface_t* surface, i32 miplevel);
surfcache_t* D_CacheSurfaceFind(msurface_t* surface, i32 miplevel, qboolean* build);

extern i32 D_MipLevelForScale(float scale);

extern i16* d_pzbuffer;
extern u32 d_zrowbytes, d_zwidth;

extern i32* d_pscantable;
extern i32 d_scantable[MAXHEIGHT];

extern i32 d_vrectx, d_vrecty, d_vrectright_particle, d_vrectbottom_particle;

extern i32 d_y_aspect_shift, d_pix_min, d_pix_max, d_pix_shift;

extern pixel_t* d_viewbuffer;

extern i16* zspantable[MAXHEIGHT];

extern i32 d_minmip;
extern float d_scalemip[3];

extern void (*d_drawspans)(espan_t* pspan);

// A surface's spans with everything D_DrawSpans8 and D_DrawZSpans take from
// globals, so that they can be drawn on another CPU (d_spanjob.c); and the
// surface cache block to be built first, if `build`
typedef struct {
    qboolean build;
    drawsurf_t drawsurf;
    espan_t* spans;
    surfcache_t* cache;
    const byte* cacheblock;
    i32 cachewidth;
    float sdivzorigin, tdivzorigin, ziorigin;
    float sdivzstepu, tdivzstepu, zistepu;
    float sdivzstepv, tdivzstepv, zistepv;
    fixed16_t sadjust, tadjust, bbextents, bbextentt;
} d_spanjob_t;

// The span drawers' arithmetic (d_scan.c, d_sprite.c)
// x / d, truncated, for d 1..7 and |x| < 2^29 (s and t are less than 2^25):
// the high word of |x| * (2^32 / d + 1), exact over that range
static const u32 span_recip[8] = {
    0, 0, 0x80000001, 0x55555556, 0x40000001, 0x33333334, 0x2AAAAAAB, 0x24924925
};

static inline fixed16_t D_SpanStep(fixed16_t x, i32 d) {
    if (d == 1) {
        return x;
    }
    u32 ax = x < 0 ? (u32) -x : (u32) x;
    fixed16_t q = (fixed16_t) (((u64) ax * span_recip[d]) >> 32);
    return x < 0 ? -q : q;
}

// The far end's s or t of a piece, clamped as id's code does
static inline fixed16_t D_SpanClamp(float v, fixed16_t adjust, fixed16_t extent) {
    fixed16_t r = (i32) v + adjust;
    if (r > extent)
        return extent;
    if (r < 8)
        return 8; // round-off on steps below 0 must not run off the texture
    return r;
}

void D_DrawSpans8Job(const d_spanjob_t* job);
void D_DrawZSpansAt(espan_t* pspan, float ziorigin, float zistepu, float zistepv);

// The textured surfaces of D_DrawSurfaces() are drawn by the game thread and
// a worker on the other CPU together: D_SpanJobQueue() hands one over (or
// draws it at once without the worker), D_SpanJobsFinish() returns when all
// are drawn. A surface cache block a job reads is neither rebuilt nor freed
// before then: D_SpanJobsRelease() finishes them first if it is.
qboolean D_SpanJobsOn(void);
void D_SpanJobQueue(const d_spanjob_t* job);
void D_SpanJobsFinish(void);
void D_SpanJobsRelease(surfcache_t* cache);
// The spans of alias models go the same way, in order (d_polyse.c);
// D_SpanJobsFinish() waits for them too
void D_JobsOpen(void);
qboolean D_AliasTakeJob(void);
qboolean D_AliasJobsPending(void);
void D_AliasJobsReset(void);

#endif
