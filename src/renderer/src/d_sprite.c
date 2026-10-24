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
// d_sprite.c: software top-level rasterization driver module for drawing
// sprites


#include "d_local.h"
#include <math.h>


static i32 sprite_height;
static i32 minindex, maxindex;
static sspan_t* sprite_spans;


/*
=====================
D_SpriteDrawSpans
=====================
*/
// The same pixels as id's code, with the globals in registers and the last
// piece's step by multiplication, as D_DrawSpans8 (d_scan.c)
void D_SpriteDrawSpans(sspan_t* pspan) {
    const byte* const pbase = cacheblock;
    const i32 cw = cachewidth;
    const i32 width = screenwidth;
    const i32 zwidth = d_zwidth;
    byte* const viewbuffer = (byte*) d_viewbuffer;
    i16* const zbuffer = d_pzbuffer;
    const fixed16_t sadj = sadjust, tadj = tadjust;
    const fixed16_t bbs = bbextents, bbt = bbextentt;
    const float sdivzorigin = d_sdivzorigin, tdivzorigin = d_tdivzorigin;
    const float ziorigin = d_ziorigin;
    const float sdivzstepu = d_sdivzstepu, tdivzstepu = d_tdivzstepu;
    const float zistepu = d_zistepu;
    const float sdivzstepv = d_sdivzstepv, tdivzstepv = d_tdivzstepv;
    const float zistepv = d_zistepv;
    const float sdivz8stepu = sdivzstepu * 8;
    const float tdivz8stepu = tdivzstepu * 8;
    const float zi8stepu = zistepu * 8;
    // we count on FP exceptions being turned off to avoid range problems
    const i32 izistep = (i32) (zistepu * 0x8000 * 0x10000);
    fixed16_t sstep = 0, tstep = 0;

    do {
        i32 count = pspan->count;

        if (count > 0) {
            byte* pdest = viewbuffer + (width * pspan->v) + pspan->u;
            i16* pz = zbuffer + (zwidth * pspan->v) + pspan->u;

            // calculate the initial s/z, t/z, 1/z, s, and t and clamp
            const float du = (float) pspan->u;
            const float dv = (float) pspan->v;
            float sdivz = sdivzorigin + dv * sdivzstepv + du * sdivzstepu;
            float tdivz = tdivzorigin + dv * tdivzstepv + du * tdivzstepu;
            float zi = ziorigin + dv * zistepv + du * zistepu;
            float z = (float) 0x10000 / zi; // prescale to 16.16 fixed-point
            // we count on FP exceptions being turned off to avoid range problems
            i32 izi = (i32) (zi * 0x8000 * 0x10000);

            fixed16_t s = (i32) (sdivz * z) + sadj;
            if (s > bbs)
                s = bbs;
            else if (s < 0)
                s = 0;
            fixed16_t t = (i32) (tdivz * z) + tadj;
            if (t > bbt)
                t = bbt;
            else if (t < 0)
                t = 0;

            do {
                // calculate s and t at the far end of the span
                i32 spancount = count >= 8 ? 8 : count;
                fixed16_t snext, tnext;

                count -= spancount;
                if (count) {
                    sdivz += sdivz8stepu;
                    tdivz += tdivz8stepu;
                    zi += zi8stepu;
                    z = (float) 0x10000 / zi;
                    snext = D_SpanClamp(sdivz * z, sadj, bbs);
                    tnext = D_SpanClamp(tdivz * z, tadj, bbt);
                    sstep = (snext - s) >> 3;
                    tstep = (tnext - t) >> 3;
                } else {
                    // at the last pixel of the span, the steps biased low so
                    // as not to run off the texture
                    const float spancountminus1 = (float) (spancount - 1);
                    sdivz += sdivzstepu * spancountminus1;
                    tdivz += tdivzstepu * spancountminus1;
                    zi += zistepu * spancountminus1;
                    z = (float) 0x10000 / zi;
                    snext = D_SpanClamp(sdivz * z, sadj, bbs);
                    tnext = D_SpanClamp(tdivz * z, tadj, bbt);
                    if (spancount > 1) {
                        sstep = D_SpanStep(snext - s, spancount - 1);
                        tstep = D_SpanStep(tnext - t, spancount - 1);
                    }
                }

                do {
                    const byte btemp = pbase[(s >> 16) + (t >> 16) * cw];
                    if (btemp != 255 && *pz <= (izi >> 16)) {
                        *pz = izi >> 16;
                        *pdest = btemp;
                    }
                    izi += izistep;
                    pdest++;
                    pz++;
                    s += sstep;
                    t += tstep;
                } while (--spancount > 0);

                s = snext;
                t = tnext;
            } while (count > 0);
        }

        pspan++;
    } while (pspan->count != DS_SPAN_LIST_END);
}

/*
=====================
D_SpriteScanLeftEdge
=====================
*/
void D_SpriteScanLeftEdge(void) {
    i32 i, v, itop, ibottom, lmaxindex;
    emitpoint_t *pvert, *pnext;
    sspan_t* pspan;
    float du, dv, vtop, vbottom, slope;
    fixed16_t u, u_step;

    pspan = sprite_spans;
    i = minindex;
    if (i == 0)
        i = r_spritedesc.nump;

    lmaxindex = maxindex;
    if (lmaxindex == 0)
        lmaxindex = r_spritedesc.nump;

    vtop = ceil(r_spritedesc.pverts[i].v);

    do {
        pvert = &r_spritedesc.pverts[i];
        pnext = pvert - 1;

        vbottom = ceil(pnext->v);

        if (vtop < vbottom) {
            du = pnext->u - pvert->u;
            dv = pnext->v - pvert->v;
            slope = du / dv;
            u_step = (i32) (slope * 0x10000);
            // adjust u to ceil the integer portion
            u = (i32) ((pvert->u + (slope * (vtop - pvert->v))) * 0x10000) +
                (0x10000 - 1);
            itop = (i32) vtop;
            ibottom = (i32) vbottom;

            for (v = itop; v < ibottom; v++) {
                pspan->u = u >> 16;
                pspan->v = v;
                u += u_step;
                pspan++;
            }
        }

        vtop = vbottom;

        i--;
        if (i == 0)
            i = r_spritedesc.nump;

    } while (i != lmaxindex);
}


/*
=====================
D_SpriteScanRightEdge
=====================
*/
void D_SpriteScanRightEdge(void) {
    i32 i, v, itop, ibottom;
    emitpoint_t *pvert, *pnext;
    sspan_t* pspan;
    float du, dv, vtop, vbottom, slope, uvert, unext, vvert, vnext;
    fixed16_t u, u_step;

    pspan = sprite_spans;
    i = minindex;

    vvert = r_spritedesc.pverts[i].v;
    if (vvert < r_refdef.fvrecty_adj)
        vvert = r_refdef.fvrecty_adj;
    if (vvert > r_refdef.fvrectbottom_adj)
        vvert = r_refdef.fvrectbottom_adj;

    vtop = ceil(vvert);

    do {
        pvert = &r_spritedesc.pverts[i];
        pnext = pvert + 1;

        vnext = pnext->v;
        if (vnext < r_refdef.fvrecty_adj)
            vnext = r_refdef.fvrecty_adj;
        if (vnext > r_refdef.fvrectbottom_adj)
            vnext = r_refdef.fvrectbottom_adj;

        vbottom = ceil(vnext);

        if (vtop < vbottom) {
            uvert = pvert->u;
            if (uvert < r_refdef.fvrectx_adj)
                uvert = r_refdef.fvrectx_adj;
            if (uvert > r_refdef.fvrectright_adj)
                uvert = r_refdef.fvrectright_adj;

            unext = pnext->u;
            if (unext < r_refdef.fvrectx_adj)
                unext = r_refdef.fvrectx_adj;
            if (unext > r_refdef.fvrectright_adj)
                unext = r_refdef.fvrectright_adj;

            du = unext - uvert;
            dv = vnext - vvert;
            slope = du / dv;
            u_step = (i32) (slope * 0x10000);
            // adjust u to ceil the integer portion
            u = (i32) ((uvert + (slope * (vtop - vvert))) * 0x10000) +
                (0x10000 - 1);
            itop = (i32) vtop;
            ibottom = (i32) vbottom;

            for (v = itop; v < ibottom; v++) {
                pspan->count = (u >> 16) - pspan->u;
                u += u_step;
                pspan++;
            }
        }

        vtop = vbottom;
        vvert = vnext;

        i++;
        if (i == r_spritedesc.nump)
            i = 0;

    } while (i != maxindex);

    pspan->count = DS_SPAN_LIST_END; // mark the end of the span list
}


/*
=====================
D_SpriteCalculateGradients
=====================
*/
void D_SpriteCalculateGradients(void) {
    vec3_t p_normal, p_saxis, p_taxis, p_temp1;
    float distinv;

    TransformVector(r_spritedesc.vpn, p_normal);
    TransformVector(r_spritedesc.vright, p_saxis);
    TransformVector(r_spritedesc.vup, p_taxis);
    VectorInverse(p_taxis);

    distinv = 1.0 / (-DotProduct(modelorg, r_spritedesc.vpn));

    d_sdivzstepu = p_saxis[0] * xscaleinv;
    d_tdivzstepu = p_taxis[0] * xscaleinv;

    d_sdivzstepv = -p_saxis[1] * yscaleinv;
    d_tdivzstepv = -p_taxis[1] * yscaleinv;

    d_zistepu = p_normal[0] * xscaleinv * distinv;
    d_zistepv = -p_normal[1] * yscaleinv * distinv;

    d_sdivzorigin =
        p_saxis[2] - xcenter * d_sdivzstepu - ycenter * d_sdivzstepv;
    d_tdivzorigin =
        p_taxis[2] - xcenter * d_tdivzstepu - ycenter * d_tdivzstepv;
    d_ziorigin =
        p_normal[2] * distinv - xcenter * d_zistepu - ycenter * d_zistepv;

    TransformVector(modelorg, p_temp1);

    sadjust = ((fixed16_t) (DotProduct(p_temp1, p_saxis) * 0x10000 + 0.5)) -
              (-(cachewidth >> 1) << 16);
    tadjust = ((fixed16_t) (DotProduct(p_temp1, p_taxis) * 0x10000 + 0.5)) -
              (-(sprite_height >> 1) << 16);

    // -1 (-epsilon) so we never wander off the edge of the texture
    bbextents = (cachewidth << 16) - 1;
    bbextentt = (sprite_height << 16) - 1;
}


/*
=====================
D_DrawSprite
=====================
*/
void D_DrawSprite(void) {
    // after the alias models' spans (d_spanjob.c): in their order
    D_SpanJobsFinish();
    i32 i, nump;
    float ymin, ymax;
    emitpoint_t* pverts;
    sspan_t spans[MAXHEIGHT + 1];

    sprite_spans = spans;

    // find the top and bottom vertices, and make sure there's at least one scan to
    // draw
    ymin = 999999.9;
    ymax = -999999.9;
    pverts = r_spritedesc.pverts;

    for (i = 0; i < r_spritedesc.nump; i++) {
        if (pverts->v < ymin) {
            ymin = pverts->v;
            minindex = i;
        }

        if (pverts->v > ymax) {
            ymax = pverts->v;
            maxindex = i;
        }

        pverts++;
    }

    ymin = ceil(ymin);
    ymax = ceil(ymax);

    if (ymin >= ymax)
        return; // doesn't cross any scans at all

    cachewidth = r_spritedesc.pspriteframe->width;
    sprite_height = r_spritedesc.pspriteframe->height;
    cacheblock = (byte*) &r_spritedesc.pspriteframe->pixels[0];

    // copy the first vertex to the last vertex, so we don't have to deal with
    // wrapping
    nump = r_spritedesc.nump;
    pverts = r_spritedesc.pverts;
    pverts[nump] = pverts[0];

    D_SpriteCalculateGradients();
    D_SpriteScanLeftEdge();
    D_SpriteScanRightEdge();
    D_SpriteDrawSpans(sprite_spans);
}
