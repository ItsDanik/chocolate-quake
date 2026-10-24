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
// d_polyset.c: routines for drawing sets of polygons sharing the same
// texture (used for Alias models)


#include "d_local.h"
#include <math.h>


// TODO: put in span spilling to shrink list size
// !!! if this is changed, it must be changed in d_polysa.s too !!!
#define DPS_MAXSPANS MAXHEIGHT + 1
// 1 extra for spanpackage that marks end

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct {
    void* pdest;
    i16* pz;
    i32 count;
    byte* ptex;
    i32 sfrac, tfrac, light, zi;
} spanpackage_t;

typedef struct {
    i32 isflattop;
    i32 numleftedges;
    i32* pleftedgevert0;
    i32* pleftedgevert1;
    i32* pleftedgevert2;
    i32 numrightedges;
    i32* prightedgevert0;
    i32* prightedgevert1;
    i32* prightedgevert2;
} edgetable;

i32 r_p0[6], r_p1[6], r_p2[6];

byte* d_pcolormap;

i32 d_aflatcolor;
i32 d_xdenom;

edgetable* pedgetable;

edgetable edgetables[12] = {
    {0, 1, r_p0, r_p2, NULL, 2, r_p0, r_p1, r_p2},
    {0, 2, r_p1, r_p0, r_p2, 1, r_p1, r_p2, NULL},
    {1, 1, r_p0, r_p2, NULL, 1, r_p1, r_p2, NULL},
    {0, 1, r_p1, r_p0, NULL, 2, r_p1, r_p2, r_p0},
    {0, 2, r_p0, r_p2, r_p1, 1, r_p0, r_p1, NULL},
    {0, 1, r_p2, r_p1, NULL, 1, r_p2, r_p0, NULL},
    {0, 1, r_p2, r_p1, NULL, 2, r_p2, r_p0, r_p1},
    {0, 2, r_p2, r_p1, r_p0, 1, r_p2, r_p0, NULL},
    {0, 1, r_p1, r_p0, NULL, 1, r_p1, r_p2, NULL},
    {1, 1, r_p2, r_p1, NULL, 1, r_p0, r_p1, NULL},
    {1, 1, r_p1, r_p0, NULL, 1, r_p2, r_p0, NULL},
    {0, 1, r_p0, r_p2, NULL, 1, r_p0, r_p1, NULL},
};

// FIXME: some of these can become statics
i32 a_sstepxfrac, a_tstepxfrac, r_lstepx, a_ststepxwhole;
i32 r_sstepx, r_tstepx, r_lstepy, r_sstepy, r_tstepy;
i32 r_zistepx, r_zistepy;
i32 d_aspancount, d_countextrastep;

spanpackage_t* a_spans;
spanpackage_t* d_pedgespanpackage;
static i32 ystart;
byte *d_pdest, *d_ptex;
i16* d_pz;
i32 d_sfrac, d_tfrac, d_light, d_zi;
i32 d_ptexextrastep, d_sfracextrastep;
i32 d_tfracextrastep, d_lightextrastep, d_pdestextrastep;
i32 d_lightbasestep, d_pdestbasestep, d_ptexbasestep;
i32 d_sfracbasestep, d_tfracbasestep;
i32 d_ziextrastep, d_zibasestep;
i32 d_pzextrastep, d_pzbasestep;

typedef struct {
    i32 quotient;
    i32 remainder;
} adivtab_t;

static adivtab_t adivtab[32 * 32] = {
#include "adivtab.h"
};

byte* skintable[MAX_LBM_HEIGHT];
i32 skinwidth;
byte* skinstart;

void D_PolysetDrawSpans8(spanpackage_t* pspanpackage);
void D_PolysetCalcGradients(i32 skinwidth);
void D_DrawSubdiv(void);
void D_DrawNonSubdiv(void);
void D_PolysetRecursiveTriangle(i32* p1, i32* p2, i32* p3);
void D_PolysetSetEdgeTable(void);
void D_RasterizeAliasPolySmooth(void);
void D_PolysetScanLeftEdge(i32 height);
static void D_PolysetSpans(spanpackage_t* pspanpackage, qboolean last);
static spanpackage_t* D_AliasReserve(i32 n);
static qboolean alias_jobs; // this model's spans are queued (D_PolysetSpans())


/*
================
D_PolysetDraw
================
*/
void D_PolysetDraw(void) {
    spanpackage_t spans[DPS_MAXSPANS + 1 +
                        ((CACHE_SIZE - 1) / sizeof(spanpackage_t)) + 1];
    // one extra because of cache line pretouching

    a_spans = (spanpackage_t*) (((intptr_t) &spans[0] + CACHE_SIZE - 1) &
                                ~(CACHE_SIZE - 1));

    alias_jobs = !r_affinetridesc.drawtype && D_SpanJobsOn();
    if (r_affinetridesc.drawtype) {
        // drawn here: after the queued spans
        D_SpanJobsFinish();
        D_DrawSubdiv();
    } else {
        D_DrawNonSubdiv();
    }
}


/*
================
D_PolysetDrawFinalVerts
================
*/
void D_PolysetDrawFinalVerts(finalvert_t* fv, i32 numverts) {
    i32 i, z;
    i16* zbuf;

    D_SpanJobsFinish(); // after the queued spans

    for (i = 0; i < numverts; i++, fv++) {
        // valid triangle coordinates for filling can include the bottom and
        // right clip edges, due to the fill rule; these shouldn't be drawn
        if ((fv->v[0] < r_refdef.vrectright) &&
            (fv->v[1] < r_refdef.vrectbottom)) {
            z = fv->v[5] >> 16;
            zbuf = zspantable[fv->v[1]] + fv->v[0];
            if (z >= *zbuf) {
                i32 pix;

                *zbuf = z;
                pix = skintable[fv->v[3] >> 16][fv->v[2] >> 16];
                pix = ((byte*) acolormap)[pix + (fv->v[4] & 0xFF00)];
                d_viewbuffer[d_scantable[fv->v[1]] + fv->v[0]] = pix;
            }
        }
    }
}


/*
================
D_DrawSubdiv
================
*/
void D_DrawSubdiv(void) {
    mtriangle_t* ptri;
    finalvert_t *pfv, *index0, *index1, *index2;
    i32 i;
    i32 lnumtriangles;

    pfv = r_affinetridesc.pfinalverts;
    ptri = r_affinetridesc.ptriangles;
    lnumtriangles = r_affinetridesc.numtriangles;

    for (i = 0; i < lnumtriangles; i++) {
        index0 = pfv + ptri[i].vertindex[0];
        index1 = pfv + ptri[i].vertindex[1];
        index2 = pfv + ptri[i].vertindex[2];

        if (((index0->v[1] - index1->v[1]) * (index0->v[0] - index2->v[0]) -
             (index0->v[0] - index1->v[0]) * (index0->v[1] - index2->v[1])) >=
            0) {
            continue;
        }

        d_pcolormap = &((byte*) acolormap)[index0->v[4] & 0xFF00];

        if (ptri[i].facesfront) {
            D_PolysetRecursiveTriangle(index0->v, index1->v, index2->v);
        } else {
            i32 s0, s1, s2;

            s0 = index0->v[2];
            s1 = index1->v[2];
            s2 = index2->v[2];

            if (index0->flags & ALIAS_ONSEAM)
                index0->v[2] += r_affinetridesc.seamfixupX16;
            if (index1->flags & ALIAS_ONSEAM)
                index1->v[2] += r_affinetridesc.seamfixupX16;
            if (index2->flags & ALIAS_ONSEAM)
                index2->v[2] += r_affinetridesc.seamfixupX16;

            D_PolysetRecursiveTriangle(index0->v, index1->v, index2->v);

            index0->v[2] = s0;
            index1->v[2] = s1;
            index2->v[2] = s2;
        }
    }
}


/*
================
D_DrawNonSubdiv
================
*/
void D_DrawNonSubdiv(void) {
    mtriangle_t* ptri;
    finalvert_t *pfv, *index0, *index1, *index2;
    i32 i;
    i32 lnumtriangles;

    pfv = r_affinetridesc.pfinalverts;
    ptri = r_affinetridesc.ptriangles;
    lnumtriangles = r_affinetridesc.numtriangles;

    for (i = 0; i < lnumtriangles; i++, ptri++) {
        index0 = pfv + ptri->vertindex[0];
        index1 = pfv + ptri->vertindex[1];
        index2 = pfv + ptri->vertindex[2];

        d_xdenom =
            (index0->v[1] - index1->v[1]) * (index0->v[0] - index2->v[0]) -
            (index0->v[0] - index1->v[0]) * (index0->v[1] - index2->v[1]);

        if (d_xdenom >= 0) {
            continue;
        }

        r_p0[0] = index0->v[0]; // u
        r_p0[1] = index0->v[1]; // v
        r_p0[2] = index0->v[2]; // s
        r_p0[3] = index0->v[3]; // t
        r_p0[4] = index0->v[4]; // light
        r_p0[5] = index0->v[5]; // iz

        r_p1[0] = index1->v[0];
        r_p1[1] = index1->v[1];
        r_p1[2] = index1->v[2];
        r_p1[3] = index1->v[3];
        r_p1[4] = index1->v[4];
        r_p1[5] = index1->v[5];

        r_p2[0] = index2->v[0];
        r_p2[1] = index2->v[1];
        r_p2[2] = index2->v[2];
        r_p2[3] = index2->v[3];
        r_p2[4] = index2->v[4];
        r_p2[5] = index2->v[5];

        if (!ptri->facesfront) {
            if (index0->flags & ALIAS_ONSEAM)
                r_p0[2] += r_affinetridesc.seamfixupX16;
            if (index1->flags & ALIAS_ONSEAM)
                r_p1[2] += r_affinetridesc.seamfixupX16;
            if (index2->flags & ALIAS_ONSEAM)
                r_p2[2] += r_affinetridesc.seamfixupX16;
        }

        D_PolysetSetEdgeTable();
        D_RasterizeAliasPolySmooth();
    }
}


/*
================
D_PolysetRecursiveTriangle
================
*/
void D_PolysetRecursiveTriangle(i32* lp1, i32* lp2, i32* lp3) {
    i32* temp;
    i32 d;
    i32 new[6];
    i32 z;
    i16* zbuf;

    d = lp2[0] - lp1[0];
    if (d < -1 || d > 1)
        goto split;
    d = lp2[1] - lp1[1];
    if (d < -1 || d > 1)
        goto split;

    d = lp3[0] - lp2[0];
    if (d < -1 || d > 1)
        goto split2;
    d = lp3[1] - lp2[1];
    if (d < -1 || d > 1)
        goto split2;

    d = lp1[0] - lp3[0];
    if (d < -1 || d > 1)
        goto split3;
    d = lp1[1] - lp3[1];
    if (d < -1 || d > 1) {
    split3:
        temp = lp1;
        lp1 = lp3;
        lp3 = lp2;
        lp2 = temp;

        goto split;
    }

    return; // entire tri is filled

split2:
    temp = lp1;
    lp1 = lp2;
    lp2 = lp3;
    lp3 = temp;

split:
    // split this edge
    new[0] = (lp1[0] + lp2[0]) >> 1;
    new[1] = (lp1[1] + lp2[1]) >> 1;
    new[2] = (lp1[2] + lp2[2]) >> 1;
    new[3] = (lp1[3] + lp2[3]) >> 1;
    new[5] = (lp1[5] + lp2[5]) >> 1;

    // draw the point if splitting a leading edge
    if (lp2[1] > lp1[1])
        goto nodraw;
    if ((lp2[1] == lp1[1]) && (lp2[0] < lp1[0]))
        goto nodraw;


    z = new[5] >> 16;
    zbuf = zspantable[new[1]] + new[0];
    if (z >= *zbuf) {
        i32 pix;

        *zbuf = z;
        pix = d_pcolormap[skintable[new[3] >> 16][new[2] >> 16]];
        d_viewbuffer[d_scantable[new[1]] + new[0]] = pix;
    }

nodraw:
    // recursively continue
    D_PolysetRecursiveTriangle(lp3, lp1, new);
    D_PolysetRecursiveTriangle(lp3, new, lp2);
}

/*
================
D_PolysetUpdateTables
================
*/
void D_PolysetUpdateTables(void) {
    i32 i;
    byte* s;

    if (r_affinetridesc.skinwidth != skinwidth ||
        r_affinetridesc.pskin != skinstart) {
        skinwidth = r_affinetridesc.skinwidth;
        skinstart = r_affinetridesc.pskin;
        s = skinstart;
        for (i = 0; i < MAX_LBM_HEIGHT; i++, s += skinwidth)
            skintable[i] = s;
    }
}

/*
===================
D_PolysetScanLeftEdge
====================
*/
void D_PolysetScanLeftEdge(i32 height) {

    do {
        d_pedgespanpackage->pdest = d_pdest;
        d_pedgespanpackage->pz = d_pz;
        d_pedgespanpackage->count = d_aspancount;
        d_pedgespanpackage->ptex = d_ptex;

        d_pedgespanpackage->sfrac = d_sfrac;
        d_pedgespanpackage->tfrac = d_tfrac;

        // FIXME: need to clamp l, s, t, at both ends?
        d_pedgespanpackage->light = d_light;
        d_pedgespanpackage->zi = d_zi;

        d_pedgespanpackage++;

        errorterm += erroradjustup;
        if (errorterm >= 0) {
            d_pdest += d_pdestextrastep;
            d_pz += d_pzextrastep;
            d_aspancount += d_countextrastep;
            d_ptex += d_ptexextrastep;
            d_sfrac += d_sfracextrastep;
            d_ptex += d_sfrac >> 16;

            d_sfrac &= 0xFFFF;
            d_tfrac += d_tfracextrastep;
            if (d_tfrac & 0x10000) {
                d_ptex += r_affinetridesc.skinwidth;
                d_tfrac &= 0xFFFF;
            }
            d_light += d_lightextrastep;
            d_zi += d_ziextrastep;
            errorterm -= erroradjustdown;
        } else {
            d_pdest += d_pdestbasestep;
            d_pz += d_pzbasestep;
            d_aspancount += ubasestep;
            d_ptex += d_ptexbasestep;
            d_sfrac += d_sfracbasestep;
            d_ptex += d_sfrac >> 16;
            d_sfrac &= 0xFFFF;
            d_tfrac += d_tfracbasestep;
            if (d_tfrac & 0x10000) {
                d_ptex += r_affinetridesc.skinwidth;
                d_tfrac &= 0xFFFF;
            }
            d_light += d_lightbasestep;
            d_zi += d_zibasestep;
        }
    } while (--height);
}

/*
===================
D_PolysetSetUpForLineScan
====================
*/
void D_PolysetSetUpForLineScan(fixed8_t startvertu, fixed8_t startvertv,
                               fixed8_t endvertu, fixed8_t endvertv) {
    double dm, dn;
    i32 tm, tn;
    adivtab_t* ptemp;

    // TODO: implement x86 version

    errorterm = -1;

    tm = endvertu - startvertu;
    tn = endvertv - startvertv;

    if (((tm <= 16) && (tm >= -15)) && ((tn <= 16) && (tn >= -15))) {
        ptemp = &adivtab[((tm + 15) << 5) + (tn + 15)];
        ubasestep = ptemp->quotient;
        erroradjustup = ptemp->remainder;
        erroradjustdown = tn;
    } else {
        dm = (double) tm;
        dn = (double) tn;

        FloorDivMod(dm, dn, &ubasestep, &erroradjustup);

        erroradjustdown = dn;
    }
}

/*
================
D_PolysetCalcGradients
================
*/
void D_PolysetCalcGradients(i32 skinwidth) {
    float xstepdenominv, ystepdenominv, t0, t1;
    float p01_minus_p21, p11_minus_p21, p00_minus_p20, p10_minus_p20;

    p00_minus_p20 = r_p0[0] - r_p2[0];
    p01_minus_p21 = r_p0[1] - r_p2[1];
    p10_minus_p20 = r_p1[0] - r_p2[0];
    p11_minus_p21 = r_p1[1] - r_p2[1];

    xstepdenominv = 1.0 / (float) d_xdenom;

    ystepdenominv = -xstepdenominv;

    // ceil () for light so positive steps are exaggerated, negative steps
    // diminished,  pushing us away from underflow toward overflow. Underflow is
    // very visible, overflow is very unlikely, because of ambient lighting
    t0 = r_p0[4] - r_p2[4];
    t1 = r_p1[4] - r_p2[4];
    r_lstepx =
        (i32) ceil((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv);
    r_lstepy =
        (i32) ceil((t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv);

    t0 = r_p0[2] - r_p2[2];
    t1 = r_p1[2] - r_p2[2];
    r_sstepx =
        (i32) ((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv);
    r_sstepy =
        (i32) ((t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv);

    t0 = r_p0[3] - r_p2[3];
    t1 = r_p1[3] - r_p2[3];
    r_tstepx =
        (i32) ((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv);
    r_tstepy =
        (i32) ((t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv);

    t0 = r_p0[5] - r_p2[5];
    t1 = r_p1[5] - r_p2[5];
    r_zistepx =
        (i32) ((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv);
    r_zistepy =
        (i32) ((t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv);

    a_sstepxfrac = r_sstepx & 0xFFFF;
    a_tstepxfrac = r_tstepx & 0xFFFF;

    a_ststepxwhole = skinwidth * (r_tstepx >> 16) + (r_sstepx >> 16);
}

#if 0
byte gelmap[256];
void InitGel(byte* palette) {
    i32 i;
    i32 r;

    for (i = 0; i < 256; i++) {
        //		r = (palette[i*3]>>4);
        r = (palette[i * 3] + palette[i * 3 + 1] + palette[i * 3 + 2])
            / (16 * 3);
        gelmap[i] = /* 64 */ 0 + r;
    }
}
#endif

/*
================
D_PolysetDrawSpans8
================
*/
// Alias model spans on two CPUs (d_spanjob.c): each call of the span drawer
// becomes a record with its packages (a row each) in a ring. The game thread
// draws its even rows at once; the odd ones are drawn from the queue, in its
// order, one record at a time, by the worker or (while it waits for them)
// the game thread. Each pixel is in one row, so the pixels and the z buffer
// are written in id's order. Whatever else draws pixels or reads the z
// buffer (the subdivided triangles, sprites, particles, the 2D) waits for
// them first, D_SpanJobsFinish().
#define ALIAS_RING 16384 // span packages
#define ALIAS_RECS 4096

typedef struct {
    spanpackage_t* spans;
    i32 count; // packages
    i32 row0;  // the screen row of the first
    const byte* colormap;
    i32 zistepx, lstepx, ststepxwhole, sstepxfrac, tstepxfrac, skinwidth;
    i32 adjustup, adjustdown, countextrastep, basestep, error, aspancount;
    i32 release; // the ring's packages before this are free once it is drawn
} d_aliasjob_t;

static spanpackage_t alias_ring[ALIAS_RING];
static d_aliasjob_t alias_recs[ALIAS_RECS];
static i32 alias_queued; // records queued: written by the game thread only
static i32 alias_next;   // the next record to draw, under alias_lock
static i32 alias_done;   // records drawn
static i32 alias_free;   // ring position the drawn records have released
static i32 alias_lock;
static i32 alias_head, alias_start; // the game thread's: ring positions
static i32 alias_top;               // the triangle's top row

// The steps of D_PolysetDrawSpans8, from the globals
static void D_PolysetJobSetup(d_aliasjob_t* job, spanpackage_t* pspanpackage) {
    i32 count = 1;

    // as many packages as id's loop would draw: up to the end marker
    while (pspanpackage[count].count != -999999) {
        count++;
    }
    job->spans = pspanpackage;
    job->count = count;
    job->row0 = alias_top + (i32) (pspanpackage - a_spans);
    job->colormap = (const byte*) acolormap;
    job->zistepx = r_zistepx;
    job->lstepx = r_lstepx;
    job->ststepxwhole = a_ststepxwhole;
    job->sstepxfrac = a_sstepxfrac;
    job->tstepxfrac = a_tstepxfrac;
    job->skinwidth = r_affinetridesc.skinwidth;
    job->adjustup = erroradjustup;
    job->adjustdown = erroradjustdown;
    job->countextrastep = d_countextrastep;
    job->basestep = ubasestep;
    job->error = errorterm;
    job->aspancount = d_aspancount;
}

// id's loop with everything in registers: a byte store could change the
// globals, so the compiler loaded every step again for every pixel (the same
// pixels as id's code, faster on the MiSTer's ARM). Returns the right edge's
// error term and count at the end. `parity`: the rows drawn, 0 even, 1 odd,
// -1 all.
static void D_PolysetJobDraw(const d_aliasjob_t* job, const i32 parity, i32* errorp,
                             i32* aspancountp) {
    const byte* const colormap = job->colormap;
    const i32 zistepx = job->zistepx;
    const i32 lstepx = job->lstepx;
    const i32 ststepxwhole = job->ststepxwhole;
    const i32 sstepxfrac = job->sstepxfrac;
    const i32 tstepxfrac = job->tstepxfrac;
    const i32 skinwidth = job->skinwidth;
    const i32 adjustup = job->adjustup;
    const i32 adjustdown = job->adjustdown;
    const i32 countextrastep = job->countextrastep;
    const i32 basestep = job->basestep;
    const spanpackage_t* pspanpackage = job->spans;
    const spanpackage_t* const end = pspanpackage + job->count;
    i32 error = job->error;
    i32 aspancount = job->aspancount;
    i32 row = job->row0;

    do {
        i32 lcount = aspancount - pspanpackage->count;

        error += adjustup;
        if (error >= 0) {
            aspancount += countextrastep;
            error -= adjustdown;
        } else {
            aspancount += basestep;
        }

        if (lcount && (parity < 0 || ((row ^ parity) & 1) == 0)) {
            byte* lpdest = pspanpackage->pdest;
            byte* lptex = pspanpackage->ptex;
            i16* lpz = pspanpackage->pz;
            i32 lsfrac = pspanpackage->sfrac;
            i32 ltfrac = pspanpackage->tfrac;
            i32 llight = pspanpackage->light;
            i32 lzi = pspanpackage->zi;

            do {
                if ((lzi >> 16) >= *lpz) {
                    *lpdest = colormap[*lptex + (llight & 0xFF00)];
                    *lpz = lzi >> 16;
                }
                lpdest++;
                lzi += zistepx;
                lpz++;
                llight += lstepx;
                lptex += ststepxwhole;
                lsfrac += sstepxfrac;
                lptex += lsfrac >> 16;
                lsfrac &= 0xFFFF;
                ltfrac += tstepxfrac;
                if (ltfrac & 0x10000) {
                    lptex += skinwidth;
                    ltfrac &= 0xFFFF;
                }
            } while (--lcount);
        }

        pspanpackage++;
        row++;
    } while (pspanpackage != end);

    *errorp = error;
    *aspancountp = aspancount;
}

void D_PolysetDrawSpans8(spanpackage_t* pspanpackage) {
    d_aliasjob_t job;

    D_PolysetJobSetup(&job, pspanpackage);
    D_PolysetJobDraw(&job, -1, &errorterm, &d_aspancount);
}

qboolean D_AliasTakeJob(void) {
    i32 n, error, aspancount;

    if (__atomic_load_n(&alias_next, __ATOMIC_RELAXED) >=
        __atomic_load_n(&alias_queued, __ATOMIC_ACQUIRE)) {
        return false;
    }
    if (__atomic_exchange_n(&alias_lock, 1, __ATOMIC_ACQUIRE)) {
        return false; // the other thread draws one
    }
    n = alias_next;
    if (n >= __atomic_load_n(&alias_queued, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&alias_lock, 0, __ATOMIC_RELEASE);
        return false;
    }
    D_PolysetJobDraw(&alias_recs[n % ALIAS_RECS], 1, &error, &aspancount);
    alias_next = n + 1;
    __atomic_store_n(&alias_free, alias_recs[n % ALIAS_RECS].release, __ATOMIC_RELEASE);
    __atomic_store_n(&alias_done, n + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&alias_lock, 0, __ATOMIC_RELEASE);
    return true;
}

qboolean D_AliasJobsPending(void) {
    return __atomic_load_n(&alias_done, __ATOMIC_ACQUIRE) != alias_queued;
}

void D_AliasJobsReset(void) {
    alias_queued = alias_next = alias_done = 0;
    alias_free = alias_head = alias_start = 0;
}

// Room in the ring for a triangle's packages, waiting (and drawing) while
// the records before still read it
static spanpackage_t* D_AliasReserve(i32 n) {
    i32 pos = alias_head;

    if (pos % ALIAS_RING + n > ALIAS_RING) {
        pos += ALIAS_RING - pos % ALIAS_RING; // the rest of the ring is left out
    }
    while (pos + n - __atomic_load_n(&alias_free, __ATOMIC_ACQUIRE) > ALIAS_RING) {
        D_AliasTakeJob();
    }
    alias_start = pos;
    alias_head = pos + n;
    return &alias_ring[pos % ALIAS_RING];
}

// D_PolysetDrawSpans8, or its record queued; `last`: the triangle's last
static void D_PolysetSpans(spanpackage_t* pspanpackage, qboolean last) {
    d_aliasjob_t job;

    if (!alias_jobs) {
        D_PolysetDrawSpans8(pspanpackage);
        return;
    }
    i32 error, aspancount;

    D_PolysetJobSetup(&job, pspanpackage);
    job.release = last ? alias_head : alias_start;
    D_PolysetJobDraw(&job, 0, &error, &aspancount);
    while (alias_queued - __atomic_load_n(&alias_done, __ATOMIC_ACQUIRE) >= ALIAS_RECS) {
        D_AliasTakeJob();
    }
    alias_recs[alias_queued % ALIAS_RECS] = job;
    __atomic_store_n(&alias_queued, alias_queued + 1, __ATOMIC_RELEASE);
    D_JobsOpen();
}

/*
================
D_PolysetFillSpans8
================
*/
void D_PolysetFillSpans8(spanpackage_t* pspanpackage) {
    i32 color;

    // FIXME: do z buffering

    color = d_aflatcolor++;

    while (1) {
        i32 lcount;
        byte* lpdest;

        lcount = pspanpackage->count;

        if (lcount == -1)
            return;

        if (lcount) {
            lpdest = pspanpackage->pdest;

            do {
                *lpdest++ = color;
            } while (--lcount);
        }

        pspanpackage++;
    }
}

/*
================
D_RasterizeAliasPolySmooth
================
*/
void D_RasterizeAliasPolySmooth(void) {
    i32 initialleftheight, initialrightheight;
    i32 *plefttop, *prighttop, *pleftbottom, *prightbottom;
    i32 working_lstepx, originalcount;

    plefttop = pedgetable->pleftedgevert0;
    prighttop = pedgetable->prightedgevert0;

    pleftbottom = pedgetable->pleftedgevert1;
    prightbottom = pedgetable->prightedgevert1;

    initialleftheight = pleftbottom[1] - plefttop[1];
    initialrightheight = prightbottom[1] - prighttop[1];

    if (alias_jobs) {
        // the triangle's rows and the end marker, in the ring
        i32 rows = initialrightheight;
        if (pedgetable->numrightedges == 2) {
            rows += pedgetable->prightedgevert2[1] - prightbottom[1];
        }
        i32 left = initialleftheight;
        if (pedgetable->numleftedges == 2) {
            left += pedgetable->pleftedgevert2[1] - pleftbottom[1];
        }
        a_spans = D_AliasReserve((rows > left ? rows : left) + 2);
        alias_top = plefttop[1];
    }

    //
    // set the s, t, and light gradients, which are consistent across the triangle
    // because being a triangle, things are affine
    //
    D_PolysetCalcGradients(r_affinetridesc.skinwidth);

    //
    // rasterize the polygon
    //

    //
    // scan out the top (and possibly only) part of the left edge
    //
    d_pedgespanpackage = a_spans;

    ystart = plefttop[1];
    d_aspancount = plefttop[0] - prighttop[0];

    d_ptex = (byte*) r_affinetridesc.pskin + (plefttop[2] >> 16) +
             (plefttop[3] >> 16) * r_affinetridesc.skinwidth;
    d_sfrac = plefttop[2] & 0xFFFF;
    d_tfrac = plefttop[3] & 0xFFFF;
    d_light = plefttop[4];
    d_zi = plefttop[5];

    d_pdest = (byte*) d_viewbuffer + ystart * screenwidth + plefttop[0];
    d_pz = d_pzbuffer + ystart * d_zwidth + plefttop[0];

    if (initialleftheight == 1) {
        d_pedgespanpackage->pdest = d_pdest;
        d_pedgespanpackage->pz = d_pz;
        d_pedgespanpackage->count = d_aspancount;
        d_pedgespanpackage->ptex = d_ptex;

        d_pedgespanpackage->sfrac = d_sfrac;
        d_pedgespanpackage->tfrac = d_tfrac;

        // FIXME: need to clamp l, s, t, at both ends?
        d_pedgespanpackage->light = d_light;
        d_pedgespanpackage->zi = d_zi;

        d_pedgespanpackage++;
    } else {
        D_PolysetSetUpForLineScan(plefttop[0], plefttop[1], pleftbottom[0],
                                  pleftbottom[1]);

        d_pzbasestep = d_zwidth + ubasestep;
        d_pzextrastep = d_pzbasestep + 1;

        d_pdestbasestep = screenwidth + ubasestep;
        d_pdestextrastep = d_pdestbasestep + 1;

        // TODO: can reuse partial expressions here

        // for negative steps in x along left edge, bias toward overflow rather than
        // underflow (sort of turning the floor () we did in the gradient calcs into
        // ceil (), but plus a little bit)
        if (ubasestep < 0)
            working_lstepx = r_lstepx - 1;
        else
            working_lstepx = r_lstepx;

        d_countextrastep = ubasestep + 1;
        d_ptexbasestep = ((r_sstepy + r_sstepx * ubasestep) >> 16) +
                         ((r_tstepy + r_tstepx * ubasestep) >> 16) *
                             r_affinetridesc.skinwidth;
        d_sfracbasestep = (r_sstepy + r_sstepx * ubasestep) & 0xFFFF;
        d_tfracbasestep = (r_tstepy + r_tstepx * ubasestep) & 0xFFFF;
        d_lightbasestep = r_lstepy + working_lstepx * ubasestep;
        d_zibasestep = r_zistepy + r_zistepx * ubasestep;

        d_ptexextrastep = ((r_sstepy + r_sstepx * d_countextrastep) >> 16) +
                          ((r_tstepy + r_tstepx * d_countextrastep) >> 16) *
                              r_affinetridesc.skinwidth;
        d_sfracextrastep = (r_sstepy + r_sstepx * d_countextrastep) & 0xFFFF;
        d_tfracextrastep = (r_tstepy + r_tstepx * d_countextrastep) & 0xFFFF;
        d_lightextrastep = d_lightbasestep + working_lstepx;
        d_ziextrastep = d_zibasestep + r_zistepx;

        D_PolysetScanLeftEdge(initialleftheight);
    }

    //
    // scan out the bottom part of the left edge, if it exists
    //
    if (pedgetable->numleftedges == 2) {
        i32 height;

        plefttop = pleftbottom;
        pleftbottom = pedgetable->pleftedgevert2;

        height = pleftbottom[1] - plefttop[1];

        // TODO: make this a function; modularize this function in general

        ystart = plefttop[1];
        d_aspancount = plefttop[0] - prighttop[0];
        d_ptex = (byte*) r_affinetridesc.pskin + (plefttop[2] >> 16) +
                 (plefttop[3] >> 16) * r_affinetridesc.skinwidth;
        d_sfrac = 0;
        d_tfrac = 0;
        d_light = plefttop[4];
        d_zi = plefttop[5];

        d_pdest = (byte*) d_viewbuffer + ystart * screenwidth + plefttop[0];
        d_pz = d_pzbuffer + ystart * d_zwidth + plefttop[0];

        if (height == 1) {
            d_pedgespanpackage->pdest = d_pdest;
            d_pedgespanpackage->pz = d_pz;
            d_pedgespanpackage->count = d_aspancount;
            d_pedgespanpackage->ptex = d_ptex;

            d_pedgespanpackage->sfrac = d_sfrac;
            d_pedgespanpackage->tfrac = d_tfrac;

            // FIXME: need to clamp l, s, t, at both ends?
            d_pedgespanpackage->light = d_light;
            d_pedgespanpackage->zi = d_zi;

            d_pedgespanpackage++;
        } else {
            D_PolysetSetUpForLineScan(plefttop[0], plefttop[1], pleftbottom[0],
                                      pleftbottom[1]);

            d_pdestbasestep = screenwidth + ubasestep;
            d_pdestextrastep = d_pdestbasestep + 1;

            d_pzbasestep = d_zwidth + ubasestep;
            d_pzextrastep = d_pzbasestep + 1;

            if (ubasestep < 0)
                working_lstepx = r_lstepx - 1;
            else
                working_lstepx = r_lstepx;

            d_countextrastep = ubasestep + 1;
            d_ptexbasestep = ((r_sstepy + r_sstepx * ubasestep) >> 16) +
                             ((r_tstepy + r_tstepx * ubasestep) >> 16) *
                                 r_affinetridesc.skinwidth;
            d_sfracbasestep = (r_sstepy + r_sstepx * ubasestep) & 0xFFFF;
            d_tfracbasestep = (r_tstepy + r_tstepx * ubasestep) & 0xFFFF;
            d_lightbasestep = r_lstepy + working_lstepx * ubasestep;
            d_zibasestep = r_zistepy + r_zistepx * ubasestep;

            d_ptexextrastep = ((r_sstepy + r_sstepx * d_countextrastep) >> 16) +
                              ((r_tstepy + r_tstepx * d_countextrastep) >> 16) *
                                  r_affinetridesc.skinwidth;
            d_sfracextrastep =
                (r_sstepy + r_sstepx * d_countextrastep) & 0xFFFF;
            d_tfracextrastep =
                (r_tstepy + r_tstepx * d_countextrastep) & 0xFFFF;
            d_lightextrastep = d_lightbasestep + working_lstepx;
            d_ziextrastep = d_zibasestep + r_zistepx;

            D_PolysetScanLeftEdge(height);
        }
    }

    // scan out the top (and possibly only) part of the right edge, updating the
    // count field
    d_pedgespanpackage = a_spans;

    D_PolysetSetUpForLineScan(prighttop[0], prighttop[1], prightbottom[0],
                              prightbottom[1]);
    d_aspancount = 0;
    d_countextrastep = ubasestep + 1;
    originalcount = a_spans[initialrightheight].count;
    a_spans[initialrightheight].count = -999999; // mark end of the spanpackages
    D_PolysetSpans(a_spans, pedgetable->numrightedges != 2);

    // scan out the bottom part of the right edge, if it exists
    if (pedgetable->numrightedges == 2) {
        i32 height;
        spanpackage_t* pstart;

        pstart = a_spans + initialrightheight;
        pstart->count = originalcount;

        d_aspancount = prightbottom[0] - prighttop[0];

        prighttop = prightbottom;
        prightbottom = pedgetable->prightedgevert2;

        height = prightbottom[1] - prighttop[1];

        D_PolysetSetUpForLineScan(prighttop[0], prighttop[1], prightbottom[0],
                                  prightbottom[1]);

        d_countextrastep = ubasestep + 1;
        a_spans[initialrightheight + height].count = -999999;
        // mark end of the spanpackages
        D_PolysetSpans(pstart, true);
    }
}


/*
================
D_PolysetSetEdgeTable
================
*/
void D_PolysetSetEdgeTable(void) {
    i32 edgetableindex;

    edgetableindex = 0; // assume the vertices are already in
                        //  top to bottom order

    //
    // determine which edges are right & left, and the order in which
    // to rasterize them
    //
    if (r_p0[1] >= r_p1[1]) {
        if (r_p0[1] == r_p1[1]) {
            if (r_p0[1] < r_p2[1])
                pedgetable = &edgetables[2];
            else
                pedgetable = &edgetables[5];

            return;
        } else {
            edgetableindex = 1;
        }
    }

    if (r_p0[1] == r_p2[1]) {
        if (edgetableindex)
            pedgetable = &edgetables[8];
        else
            pedgetable = &edgetables[9];

        return;
    } else if (r_p1[1] == r_p2[1]) {
        if (edgetableindex)
            pedgetable = &edgetables[10];
        else
            pedgetable = &edgetables[11];

        return;
    }

    if (r_p0[1] > r_p2[1])
        edgetableindex += 2;

    if (r_p1[1] > r_p2[1])
        edgetableindex += 4;

    pedgetable = &edgetables[edgetableindex];
}


#if 0

void D_PolysetRecursiveDrawLine(i32* lp1, i32* lp2) {
    i32 d;
    i32 new[6];
    i32 ofs;

    d = lp2[0] - lp1[0];
    if (d < -1 || d > 1)
        goto split;
    d = lp2[1] - lp1[1];
    if (d < -1 || d > 1)
        goto split;

    return; // line is completed

split:
    // split this edge
    new[0] = (lp1[0] + lp2[0]) >> 1;
    new[1] = (lp1[1] + lp2[1]) >> 1;
    new[5] = (lp1[5] + lp2[5]) >> 1;
    new[2] = (lp1[2] + lp2[2]) >> 1;
    new[3] = (lp1[3] + lp2[3]) >> 1;
    new[4] = (lp1[4] + lp2[4]) >> 1;

    // draw the point
    ofs = d_scantable[new[1]] + new[0];
    if (new[5] > d_pzbuffer[ofs]) {
        i32 pix;

        d_pzbuffer[ofs] = new[5];
        pix = skintable[new[3] >> 16][new[2] >> 16];
        //		pix = ((byte *)acolormap)[pix + (new[4] & 0xFF00)];
        d_viewbuffer[ofs] = pix;
    }

    // recursively continue
    D_PolysetRecursiveDrawLine(lp1, new);
    D_PolysetRecursiveDrawLine(new, lp2);
}

void D_PolysetRecursiveTriangle2(i32* lp1, i32* lp2, i32* lp3) {
    i32 d;
    i32 new[4];

    d = lp2[0] - lp1[0];
    if (d < -1 || d > 1)
        goto split;
    d = lp2[1] - lp1[1];
    if (d < -1 || d > 1)
        goto split;
    return;

split:
    // split this edge
    new[0] = (lp1[0] + lp2[0]) >> 1;
    new[1] = (lp1[1] + lp2[1]) >> 1;
    new[5] = (lp1[5] + lp2[5]) >> 1;
    new[2] = (lp1[2] + lp2[2]) >> 1;
    new[3] = (lp1[3] + lp2[3]) >> 1;
    new[4] = (lp1[4] + lp2[4]) >> 1;

    D_PolysetRecursiveDrawLine(new, lp3);

    // recursively continue
    D_PolysetRecursiveTriangle(lp1, new, lp3);
    D_PolysetRecursiveTriangle(new, lp2, lp3);
}

#endif
