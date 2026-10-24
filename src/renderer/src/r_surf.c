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
// r_surf.c: surface-related refresh code


#include "r_local.h"
#include "sys.h"
#include <math.h>


// R_DrawSurface()'s state is each thread's own: the span worker builds
// surfaces too (d_spanjob.c)
__thread drawsurf_t r_drawsurf;

static __thread i32 lightleft, sourcesstep, blocksize, sourcetstep;
static __thread i32 lightdelta, lightdeltastep;
static __thread i32 lightright, lightleftstep, lightrightstep, blockdivshift;
static __thread u32 blockdivmask;
static __thread void* prowdestbase;
static __thread byte* pbasesource;
static __thread i32 surfrowbytes; // used by ASM files
static __thread u32* r_lightptr;
static __thread i32 r_stepback;
static __thread i32 r_lightwidth;
static __thread i32 r_numhblocks, r_numvblocks;
static __thread byte *r_source, *r_sourcemax;

void R_DrawSurfaceBlock8_mip0(void);
void R_DrawSurfaceBlock8_mip1(void);
void R_DrawSurfaceBlock8_mip2(void);
void R_DrawSurfaceBlock8_mip3(void);

static void (*surfmiptable[4])(void) = {
    R_DrawSurfaceBlock8_mip0,
    R_DrawSurfaceBlock8_mip1,
    R_DrawSurfaceBlock8_mip2,
    R_DrawSurfaceBlock8_mip3
};

static __thread u32 blocklights[18 * 18];


/*
===============
R_AddDynamicLights
===============
*/
void R_AddDynamicLights(void) {
    msurface_t* surf;
    i32 lnum;
    i32 sd, td;
    float dist, rad, minlight;
    vec3_t impact, local;
    i32 s, t;
    i32 i;
    i32 smax, tmax;
    mtexinfo_t* tex;

    surf = r_drawsurf.surf;
    smax = (surf->extents[0] >> 4) + 1;
    tmax = (surf->extents[1] >> 4) + 1;
    tex = surf->texinfo;

    for (lnum = 0; lnum < MAX_DLIGHTS; lnum++) {
        if (!(surf->dlightbits & (1 << lnum)))
            continue; // not lit by this light

        rad = cl_dlights[lnum].radius;
        dist = DotProduct(cl_dlights[lnum].origin, surf->plane->normal) -
               surf->plane->dist;
        rad -= fabs(dist);
        minlight = cl_dlights[lnum].minlight;
        if (rad < minlight)
            continue;
        minlight = rad - minlight;

        for (i = 0; i < 3; i++) {
            impact[i] =
                cl_dlights[lnum].origin[i] - surf->plane->normal[i] * dist;
        }

        local[0] = DotProduct(impact, tex->vecs[0]) + tex->vecs[0][3];
        local[1] = DotProduct(impact, tex->vecs[1]) + tex->vecs[1][3];

        local[0] -= surf->texturemins[0];
        local[1] -= surf->texturemins[1];

        for (t = 0; t < tmax; t++) {
            td = local[1] - t * 16;
            if (td < 0)
                td = -td;
            for (s = 0; s < smax; s++) {
                sd = local[0] - s * 16;
                if (sd < 0)
                    sd = -sd;
                if (sd > td)
                    dist = sd + (td >> 1);
                else
                    dist = td + (sd >> 1);
                if (dist < minlight)
                    blocklights[t * smax + s] += (rad - dist) * 256;
            }
        }
    }
}

/*
===============
R_BuildLightMap

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
void R_BuildLightMap(void) {
    i32 smax, tmax;
    i32 t;
    i32 i, size;
    byte* lightmap;
    u32 scale;
    i32 maps;
    msurface_t* surf;

    surf = r_drawsurf.surf;

    smax = (surf->extents[0] >> 4) + 1;
    tmax = (surf->extents[1] >> 4) + 1;
    size = smax * tmax;
    lightmap = surf->samples;

    if (r_fullbright.value || !cl.worldmodel->lightdata) {
        for (i = 0; i < size; i++)
            blocklights[i] = 0;
        return;
    }

    // clear to ambient
    for (i = 0; i < size; i++)
        blocklights[i] = r_refdef.ambientlight << 8;


    // add all the lightmaps
    if (lightmap)
        for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255;
             maps++) {
            scale = r_drawsurf.lightadj[maps]; // 8.8 fraction
            for (i = 0; i < size; i++)
                blocklights[i] += lightmap[i] * scale;
            lightmap += size; // skip to next lightmap
        }

    // add all the dynamic lights
    if (surf->dlightframe == r_framecount)
        R_AddDynamicLights();

    // bound, invert, and shift
    for (i = 0; i < size; i++) {
        t = (255 * 256 - (i32) blocklights[i]) >> (8 - VID_CBITS);

        if (t < (1 << 6))
            t = (1 << 6);

        blocklights[i] = t;
    }
}


/*
===============
R_TextureAnimation

Returns the proper texture for a given time and base texture
===============
*/
texture_t* R_TextureAnimation(texture_t* base) {
    i32 reletive;
    i32 count;

    if (currententity->frame) {
        if (base->alternate_anims)
            base = base->alternate_anims;
    }

    if (!base->anim_total)
        return base;

    reletive = (i32) (cl.time * 10) % base->anim_total;

    count = 0;
    while (base->anim_min > reletive || base->anim_max <= reletive) {
        base = base->anim_next;
        if (!base)
            Sys_Error("R_TextureAnimation: broken cycle");
        if (++count > 100)
            Sys_Error("R_TextureAnimation: infinite cycle");
    }

    return base;
}


/*
===============
R_DrawSurface
===============
*/
void R_DrawSurface(void) {
    byte* basetptr;
    i32 smax, tmax, twidth;
    i32 u;
    i32 soffset, basetoffset, texwidth;
    i32 horzblockstep;
    byte* pcolumndest;
    void (*pblockdrawer)(void);
    texture_t* mt;

    // calculate the lightings
    R_BuildLightMap();

    surfrowbytes = r_drawsurf.rowbytes;

    mt = r_drawsurf.texture;

    r_source = (byte*) mt + mt->offsets[r_drawsurf.surfmip];

    // the fractional light values should range from 0 to (VID_GRADES - 1) << 16
    // from a source range of 0 - 255

    texwidth = mt->width >> r_drawsurf.surfmip;

    blocksize = 16 >> r_drawsurf.surfmip;
    blockdivshift = 4 - r_drawsurf.surfmip;
    blockdivmask = (1 << blockdivshift) - 1;

    r_lightwidth = (r_drawsurf.surf->extents[0] >> 4) + 1;

    r_numhblocks = r_drawsurf.surfwidth >> blockdivshift;
    r_numvblocks = r_drawsurf.surfheight >> blockdivshift;

    //==============================

    pblockdrawer = surfmiptable[r_drawsurf.surfmip];
    // TODO: only needs to be set when there is a display settings change
    horzblockstep = blocksize;

    smax = mt->width >> r_drawsurf.surfmip;
    twidth = texwidth;
    tmax = mt->height >> r_drawsurf.surfmip;
    sourcetstep = texwidth;
    r_stepback = tmax * twidth;

    r_sourcemax = r_source + (tmax * smax);

    soffset = r_drawsurf.surf->texturemins[0];
    basetoffset = r_drawsurf.surf->texturemins[1];

    // << 16 components are to guarantee positive values for %
    soffset = ((soffset >> r_drawsurf.surfmip) + (smax << 16)) % smax;
    basetptr = &r_source[(
        (((basetoffset >> r_drawsurf.surfmip) + (tmax << 16)) % tmax) *
        twidth)];

    pcolumndest = r_drawsurf.surfdat;

    for (u = 0; u < r_numhblocks; u++) {
        r_lightptr = blocklights + u;

        prowdestbase = pcolumndest;

        pbasesource = basetptr + soffset;

        (*pblockdrawer)();

        soffset = soffset + blocksize;
        if (soffset >= smax)
            soffset = 0;

        pcolumndest += horzblockstep;
    }
}

//=============================================================================

/*
================
R_DrawLitBlock8

A block of a surface, lit: 16x16 texels at mip level 0, 8x8 at 1, 4x4 at 2,
2x2 at 3 (1 << shift). The same pixels as id's four functions, faster on the
MiSTer's ARM: with the globals in registers (a byte store could change them,
so the compiler loaded vid.colormap again for every texel). The light steps
are worked out in the same types, the unsigned ones of r_lightptr among them.
================
*/
static inline void R_DrawLitBlock8(const i32 shift) {
    const i32 size = 1 << shift;
    const byte* const colormap = (const byte*) vid.colormap;
    const u32* lightptr = r_lightptr;
    const i32 lightwidth = r_lightwidth;
    const i32 tstep = sourcetstep;
    const i32 rowbytes = surfrowbytes;
    const byte* const sourcemax = r_sourcemax;
    const i32 stepback = r_stepback;
    const i32 numvblocks = r_numvblocks;
    const byte* psource = pbasesource;
    byte* prowdest = prowdestbase;

    for (i32 v = 0; v < numvblocks; v++) {
        i32 left = lightptr[0];
        i32 right = lightptr[1];
        lightptr += lightwidth;
        const i32 leftstep = (lightptr[0] - left) >> shift;
        const i32 rightstep = (lightptr[1] - right) >> shift;

        for (i32 i = 0; i < size; i++) {
            const i32 lightstep = (left - right) >> shift;
            i32 light = right;
            for (i32 b = size - 1; b >= 0; b--) {
                byte pix = psource[b];
                prowdest[b] = colormap[(light & 0xFF00) + pix];
                light += lightstep;
            }
            psource += tstep;
            right += rightstep;
            left += leftstep;
            prowdest += rowbytes;
        }

        if (psource >= sourcemax)
            psource -= stepback;
    }
}

void R_DrawSurfaceBlock8_mip0(void) {
    R_DrawLitBlock8(4);
}

void R_DrawSurfaceBlock8_mip1(void) {
    R_DrawLitBlock8(3);
}

void R_DrawSurfaceBlock8_mip2(void) {
    R_DrawLitBlock8(2);
}

void R_DrawSurfaceBlock8_mip3(void) {
    R_DrawLitBlock8(1);
}

//============================================================================

/*
================
R_GenTurbTile
================
*/
void R_GenTurbTile(pixel_t* pbasetex, void* pdest) {
    i32* turb;
    i32 i, j, s, t;
    byte* pd;

    turb = sintable + ((i32) (cl.time * SPEED) & (CYCLE - 1));
    pd = (byte*) pdest;

    for (i = 0; i < TILE_SIZE; i++) {
        for (j = 0; j < TILE_SIZE; j++) {
            s = (((j << 16) + turb[i & (CYCLE - 1)]) >> 16) & 63;
            t = (((i << 16) + turb[j & (CYCLE - 1)]) >> 16) & 63;
            *pd++ = *(pbasetex + (t << 6) + s);
        }
    }
}


/*
================
R_GenTile
================
*/
void R_GenTile(msurface_t* psurf, void* pdest) {
    if (psurf->flags & SURF_DRAWTURB) {
        R_GenTurbTile((pixel_t*) ((byte*) psurf->texinfo->texture +
                                  psurf->texinfo->texture->offsets[0]),
                      pdest);
    } else if (psurf->flags & SURF_DRAWSKY) {
        R_GenSkyTile(pdest);
    } else {
        Sys_Error("Unknown tile type");
    }
}
