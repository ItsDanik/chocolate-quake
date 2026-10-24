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
// d_scan.c
//
// Portable C scan-level rasterization code, all pixel depths.


#include "d_local.h"
#include "r_local.h"
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif


byte *r_turb_pbase, *r_turb_pdest;
fixed16_t r_turb_s, r_turb_t, r_turb_sstep, r_turb_tstep;
i32* r_turb_turb;
i32 r_turb_spancount;

void D_DrawTurbulent8Span(void);


/*
=============
D_WarpScreen

// this performs a slight compression of the screen at the same time as
// the sine warp, to keep the edges from wrapping
=============
*/
void D_WarpScreen(void) {
    i32 w, h;
    i32 u, v;
    byte* dest;
    i32* turb;
    i32* col;
    byte** row;
    byte* rowptr[MAXHEIGHT + (AMP2 * 2)];
    i32 column[MAXWIDTH + (AMP2 * 2)];
    float wratio, hratio;

    w = r_refdef.vrect.width;
    h = r_refdef.vrect.height;

    wratio = w / (float) scr_vrect.width;
    hratio = h / (float) scr_vrect.height;

    for (v = 0; v < scr_vrect.height + AMP2 * 2; v++) {
        rowptr[v] =
            d_viewbuffer + (r_refdef.vrect.y * screenwidth) +
            (screenwidth * (i32) ((float) v * hratio * h / (h + AMP2 * 2)));
    }

    for (u = 0; u < scr_vrect.width + AMP2 * 2; u++) {
        column[u] =
            r_refdef.vrect.x + (i32) ((float) u * wratio * w / (w + AMP2 * 2));
    }

    turb = intsintable + ((i32) (cl.time * SPEED) & (CYCLE - 1));
    dest = vid.buffer + scr_vrect.y * vid.width + scr_vrect.x;

    for (v = 0; v < scr_vrect.height; v++, dest += vid.width) {
        col = &column[turb[v]];
        row = &rowptr[v];

        for (u = 0; u < scr_vrect.width; u += 4) {
            dest[u + 0] = row[turb[u + 0]][col[u + 0]];
            dest[u + 1] = row[turb[u + 1]][col[u + 1]];
            dest[u + 2] = row[turb[u + 2]][col[u + 2]];
            dest[u + 3] = row[turb[u + 3]][col[u + 3]];
        }
    }
}

/*
=============
D_DrawTurbulent8Span
=============
*/
void D_DrawTurbulent8Span(void) {
    i32 sturb, tturb;

    do {
        sturb =
            ((r_turb_s + r_turb_turb[(r_turb_t >> 16) & (CYCLE - 1)]) >> 16) &
            63;
        tturb =
            ((r_turb_t + r_turb_turb[(r_turb_s >> 16) & (CYCLE - 1)]) >> 16) &
            63;
        *r_turb_pdest++ = *(r_turb_pbase + (tturb << 6) + sturb);
        r_turb_s += r_turb_sstep;
        r_turb_t += r_turb_tstep;
    } while (--r_turb_spancount > 0);
}

/*
=============
Turbulent8
=============
*/
void Turbulent8(espan_t* pspan) {
    i32 count;
    fixed16_t snext, tnext;
    float sdivz, tdivz, zi, z, du, dv, spancountminus1;
    float sdivz16stepu, tdivz16stepu, zi16stepu;

    r_turb_turb = sintable + ((i32) (cl.time * SPEED) & (CYCLE - 1));

    r_turb_sstep = 0; // keep compiler happy
    r_turb_tstep = 0; // ditto

    r_turb_pbase = (byte*) cacheblock;

    sdivz16stepu = d_sdivzstepu * 16;
    tdivz16stepu = d_tdivzstepu * 16;
    zi16stepu = d_zistepu * 16;

    do {
        r_turb_pdest = (byte*) ((byte*) d_viewbuffer + (screenwidth * pspan->v) + pspan->u);

        count = pspan->count;

        // calculate the initial s/z, t/z, 1/z, s, and t and clamp
        du = (float) pspan->u;
        dv = (float) pspan->v;

        sdivz = d_sdivzorigin + dv * d_sdivzstepv + du * d_sdivzstepu;
        tdivz = d_tdivzorigin + dv * d_tdivzstepv + du * d_tdivzstepu;
        zi = d_ziorigin + dv * d_zistepv + du * d_zistepu;
        z = (float) 0x10000 / zi; // prescale to 16.16 fixed-point

        r_turb_s = (i32) (sdivz * z) + sadjust;
        if (r_turb_s > bbextents)
            r_turb_s = bbextents;
        else if (r_turb_s < 0)
            r_turb_s = 0;

        r_turb_t = (i32) (tdivz * z) + tadjust;
        if (r_turb_t > bbextentt)
            r_turb_t = bbextentt;
        else if (r_turb_t < 0)
            r_turb_t = 0;

        do {
            // calculate s and t at the far end of the span
            if (count >= 16)
                r_turb_spancount = 16;
            else
                r_turb_spancount = count;

            count -= r_turb_spancount;

            if (count) {
                // calculate s/z, t/z, zi->fixed s and t at far end of span,
                // calculate s and t steps across span by shifting
                sdivz += sdivz16stepu;
                tdivz += tdivz16stepu;
                zi += zi16stepu;
                z = (float) 0x10000 / zi; // prescale to 16.16 fixed-point

                snext = (i32) (sdivz * z) + sadjust;
                if (snext > bbextents)
                    snext = bbextents;
                else if (snext < 16)
                    snext = 16; // prevent round-off error on <0 steps from
                        //  from causing overstepping & running off the
                        //  edge of the texture

                tnext = (i32) (tdivz * z) + tadjust;
                if (tnext > bbextentt)
                    tnext = bbextentt;
                else if (tnext < 16)
                    tnext = 16; // guard against round-off error on <0 steps

                r_turb_sstep = (snext - r_turb_s) >> 4;
                r_turb_tstep = (tnext - r_turb_t) >> 4;
            } else {
                // calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
                // can't step off polygon), clamp, calculate s and t steps across
                // span by division, biasing steps low so we don't run off the
                // texture
                spancountminus1 = (float) (r_turb_spancount - 1);
                sdivz += d_sdivzstepu * spancountminus1;
                tdivz += d_tdivzstepu * spancountminus1;
                zi += d_zistepu * spancountminus1;
                z = (float) 0x10000 / zi; // prescale to 16.16 fixed-point
                snext = (i32) (sdivz * z) + sadjust;
                if (snext > bbextents)
                    snext = bbextents;
                else if (snext < 16)
                    snext = 16; // prevent round-off error on <0 steps from
                        //  from causing overstepping & running off the
                        //  edge of the texture

                tnext = (i32) (tdivz * z) + tadjust;
                if (tnext > bbextentt)
                    tnext = bbextentt;
                else if (tnext < 16)
                    tnext = 16; // guard against round-off error on <0 steps

                if (r_turb_spancount > 1) {
                    r_turb_sstep = (snext - r_turb_s) / (r_turb_spancount - 1);
                    r_turb_tstep = (tnext - r_turb_t) / (r_turb_spancount - 1);
                }
            }

            r_turb_s = r_turb_s & ((CYCLE << 16) - 1);
            r_turb_t = r_turb_t & ((CYCLE << 16) - 1);

            D_DrawTurbulent8Span();

            r_turb_s = snext;
            r_turb_t = tnext;

        } while (count > 0);

    } while ((pspan = pspan->pnext) != NULL);
}

/*
=============
D_DrawSpans8

The same pixels as id's C version, faster on the MiSTer's ARM: the globals in
registers (a byte store could change them, so the compiler loaded cachewidth
again for every pixel), the divide for the next 8 pixels started before
these are drawn (the floating point unit works on it meanwhile, as DOS
Quake's assembly did with the FPU), and the step of a span's last piece taken
by multiplication (the Cortex-A9 has no integer division).
=============
*/

void D_DrawSpans8(espan_t* pspan) {
    d_spanjob_t job;

    job.spans = pspan;
    job.cache = NULL;
    job.cacheblock = (const byte*) cacheblock;
    job.cachewidth = cachewidth;
    job.sdivzorigin = d_sdivzorigin;
    job.tdivzorigin = d_tdivzorigin;
    job.ziorigin = d_ziorigin;
    job.sdivzstepu = d_sdivzstepu;
    job.tdivzstepu = d_tdivzstepu;
    job.zistepu = d_zistepu;
    job.sdivzstepv = d_sdivzstepv;
    job.tdivzstepv = d_tdivzstepv;
    job.zistepv = d_zistepv;
    job.sadjust = sadjust;
    job.tadjust = tadjust;
    job.bbextents = bbextents;
    job.bbextentt = bbextentt;
    D_DrawSpans8Job(&job);
}

// D_DrawSpans8 with its inputs in a job, so that it can run on another CPU
// (d_spanjob.c). The view buffer and its width do not change while spans
// are drawn.
void D_DrawSpans8Job(const d_spanjob_t* job) {
    espan_t* pspan = job->spans;
    const byte* const pbase = job->cacheblock;
    const i32 cw = job->cachewidth;
    const i32 width = screenwidth;
    byte* const viewbuffer = (byte*) d_viewbuffer;
    const fixed16_t sadj = job->sadjust, tadj = job->tadjust;
    const fixed16_t bbs = job->bbextents, bbt = job->bbextentt;
    const float sdivzorigin = job->sdivzorigin, tdivzorigin = job->tdivzorigin;
    const float ziorigin = job->ziorigin;
    const float sdivzstepu = job->sdivzstepu, tdivzstepu = job->tdivzstepu;
    const float zistepu = job->zistepu;
    const float sdivzstepv = job->sdivzstepv, tdivzstepv = job->tdivzstepv;
    const float zistepv = job->zistepv;
    const float sdivz8stepu = sdivzstepu * 8;
    const float tdivz8stepu = tdivzstepu * 8;
    const float zi8stepu = zistepu * 8;

    do {
        byte* pdest = viewbuffer + (width * pspan->v) + pspan->u;
        i32 count = pspan->count;

        // calculate the initial s/z, t/z, 1/z, s, and t and clamp
        float du = (float) pspan->u;
        float dv = (float) pspan->v;
        float sdivz = sdivzorigin + dv * sdivzstepv + du * sdivzstepu;
        float tdivz = tdivzorigin + dv * tdivzstepv + du * tdivzstepu;
        float zi = ziorigin + dv * zistepv + du * zistepu;
        float z = (float) 0x10000 / zi; // prescale to 16.16 fixed-point

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

        // 1/z at the far end of the first piece: 8 pixels on, or the last
        // pixel of a shorter span
        i32 spancount = count >= 8 ? 8 : count;
        count -= spancount;
        if (count) {
            sdivz += sdivz8stepu;
            tdivz += tdivz8stepu;
            zi += zi8stepu;
        } else {
            float spancountminus1 = (float) (spancount - 1);
            sdivz += sdivzstepu * spancountminus1;
            tdivz += tdivzstepu * spancountminus1;
            zi += zistepu * spancountminus1;
        }
        z = (float) 0x10000 / zi;

        for (;;) {
            fixed16_t snext = D_SpanClamp(sdivz * z, sadj, bbs);
            fixed16_t tnext = D_SpanClamp(tdivz * z, tadj, bbt);
            fixed16_t sstep, tstep;
            if (count) {
                sstep = (snext - s) >> 3;
                tstep = (tnext - t) >> 3;
            } else {
                // biased low so as not to run off the texture
                sstep = spancount > 1 ? D_SpanStep(snext - s, spancount - 1) : 0;
                tstep = spancount > 1 ? D_SpanStep(tnext - t, spancount - 1) : 0;
            }

            // the next piece's far end, divided while these pixels are drawn
            i32 nextcount = 0;
            if (count) {
                nextcount = count >= 8 ? 8 : count;
                count -= nextcount;
                if (count) {
                    sdivz += sdivz8stepu;
                    tdivz += tdivz8stepu;
                    zi += zi8stepu;
                } else {
                    float spancountminus1 = (float) (nextcount - 1);
                    sdivz += sdivzstepu * spancountminus1;
                    tdivz += tdivzstepu * spancountminus1;
                    zi += zistepu * spancountminus1;
                }
                z = (float) 0x10000 / zi;
#if defined(__arm__)
                // here, not after the pixels where it is used
                __asm__("" : "+w"(z));
#endif
            }

            if (spancount == 8) {
                pdest[0] = pbase[(s >> 16) + (t >> 16) * cw];
                s += sstep;
                t += tstep;
                pdest[1] = pbase[(s >> 16) + (t >> 16) * cw];
                s += sstep;
                t += tstep;
                pdest[2] = pbase[(s >> 16) + (t >> 16) * cw];
                s += sstep;
                t += tstep;
                pdest[3] = pbase[(s >> 16) + (t >> 16) * cw];
                s += sstep;
                t += tstep;
                pdest[4] = pbase[(s >> 16) + (t >> 16) * cw];
                s += sstep;
                t += tstep;
                pdest[5] = pbase[(s >> 16) + (t >> 16) * cw];
                s += sstep;
                t += tstep;
                pdest[6] = pbase[(s >> 16) + (t >> 16) * cw];
                s += sstep;
                t += tstep;
                pdest[7] = pbase[(s >> 16) + (t >> 16) * cw];
                pdest += 8;
            } else {
                do {
                    *pdest++ = pbase[(s >> 16) + (t >> 16) * cw];
                    s += sstep;
                    t += tstep;
                } while (--spancount > 0);
            }

            if (!nextcount) {
                break;
            }
            s = snext;
            t = tnext;
            spancount = nextcount;
        }
    } while ((pspan = pspan->pnext) != NULL);
}

/*
=============
D_DrawZSpans
=============
*/
void D_DrawZSpans(espan_t* pspan) {
    D_DrawZSpansAt(pspan, d_ziorigin, d_zistepu, d_zistepv);
}

// D_DrawZSpans with the gradients as arguments (d_spanjob.c)
void D_DrawZSpansAt(espan_t* pspan, float ziorigin, float zistepu, float zistepv) {
    i32 count, doublecount, izistep;
    i32 izi;
    i16* pdest;
    u32 ltemp;
    double zi;
    float du, dv;

    // FIXME: check for clamping/range problems
    // we count on FP exceptions being turned off to avoid range problems
    izistep = (i32) (zistepu * 0x8000 * 0x10000);

    do {
        pdest = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;

        count = pspan->count;

        // calculate the initial 1/z
        du = (float) pspan->u;
        dv = (float) pspan->v;

        zi = ziorigin + dv * zistepv + du * zistepu;
        // we count on FP exceptions being turned off to avoid range problems
        izi = (i32) (zi * 0x8000 * 0x10000);

        if ((intptr_t) pdest & 0x02) {
            *pdest++ = (i16) (izi >> 16);
            izi += izistep;
            count--;
        }

#if defined(__ARM_NEON)
        // eight at a time on the MiSTer's NEON: the same values, as the
        // high halves of izi, izi + izistep, ...
        if (count >= 8) {
            const u32 ustep = (u32) izistep;
            const int32x4_t step8 = vdupq_n_s32((i32) (ustep * 8));
            int32x4_t lo = {izi, (i32) ((u32) izi + ustep), (i32) ((u32) izi + ustep * 2),
                            (i32) ((u32) izi + ustep * 3)};
            int32x4_t hi = vaddq_s32(lo, vdupq_n_s32((i32) (ustep * 4)));
            do {
                vst1q_s16(pdest, vcombine_s16(vshrn_n_s32(lo, 16), vshrn_n_s32(hi, 16)));
                lo = vaddq_s32(lo, step8);
                hi = vaddq_s32(hi, step8);
                pdest += 8;
                count -= 8;
            } while (count >= 8);
            izi = vgetq_lane_s32(lo, 0);
        }
#endif
        if ((doublecount = count >> 1) > 0) {
            do {
                ltemp = izi >> 16;
                izi += izistep;
                ltemp |= izi & 0xFFFF0000;
                izi += izistep;
                *(i32*) pdest = ltemp;
                pdest += 2;
            } while (--doublecount > 0);
        }

        if (count & 1)
            *pdest = (i16) (izi >> 16);

    } while ((pspan = pspan->pnext) != NULL);
}
