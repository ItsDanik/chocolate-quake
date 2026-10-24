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
// d_part.c: software driver module for drawing particles


#include "d_local.h"


/*
==============
D_EndParticles
==============
*/
void D_EndParticles(void) {
    // not used by software driver
}


/*
==============
D_StartParticles
==============
*/
void D_StartParticles(void) {
    // after the alias models' spans (d_spanjob.c): they test the z buffer
    D_SpanJobsFinish();
    // not used by software driver
}

/*
==============
D_DrawParticle
==============
*/
// A particle's square: the widths and its colour in registers (a byte store
// could change the globals, so the compiler loaded them again every pixel).
// `pix` is a constant where it is inlined, for cases 1 to 4.
static inline void D_ParticleSquare(i16* pz, byte* pdest, const i32 izi, const byte color,
                                    const i32 pix, i32 count, const i32 zwidth,
                                    const i32 width) {
    for (; count; count--, pz += zwidth, pdest += width) {
        for (i32 i = 0; i < pix; i++) {
            if (pz[i] <= izi) {
                pz[i] = izi;
                pdest[i] = color;
            }
        }
    }
}

void D_DrawParticle(particle_t* pparticle) {
    vec3_t local, transformed;
    float zi;
    byte* pdest;
    i16* pz;
    i32 izi, pix, u, v;

    // transform point
    VectorSubtract(pparticle->org, r_origin, local);

    transformed[0] = DotProduct(local, r_pright);
    transformed[1] = DotProduct(local, r_pup);
    transformed[2] = DotProduct(local, r_ppn);

    if (transformed[2] < PARTICLE_Z_CLIP)
        return;

    // project the point
    // FIXME: preadjust xcenter and ycenter
    zi = 1.0 / transformed[2];
    u = (i32) (xcenter + zi * transformed[0] + 0.5);
    v = (i32) (ycenter - zi * transformed[1] + 0.5);

    if ((v > d_vrectbottom_particle) || (u > d_vrectright_particle) ||
        (v < d_vrecty) || (u < d_vrectx)) {
        return;
    }

    pz = d_pzbuffer + (d_zwidth * v) + u;
    pdest = d_viewbuffer + d_scantable[v] + u;
    izi = (i32) (zi * 0x8000);

    pix = izi >> d_pix_shift;

    if (pix < d_pix_min)
        pix = d_pix_min;
    else if (pix > d_pix_max)
        pix = d_pix_max;

    const byte color = pparticle->color;
    const i32 zwidth = d_zwidth;
    const i32 width = screenwidth;
    switch (pix) {
        case 1:
            D_ParticleSquare(pz, pdest, izi, color, 1, 1 << d_y_aspect_shift, zwidth, width);
            break;
        case 2:
            D_ParticleSquare(pz, pdest, izi, color, 2, 2 << d_y_aspect_shift, zwidth, width);
            break;
        case 3:
            D_ParticleSquare(pz, pdest, izi, color, 3, 3 << d_y_aspect_shift, zwidth, width);
            break;
        case 4:
            D_ParticleSquare(pz, pdest, izi, color, 4, 4 << d_y_aspect_shift, zwidth, width);
            break;
        default:
            D_ParticleSquare(pz, pdest, izi, color, pix, pix << d_y_aspect_shift, zwidth, width);
            break;
    }
}
