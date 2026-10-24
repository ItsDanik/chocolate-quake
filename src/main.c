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


#include "host.h"
#include "sys.h"
#include <SDL_main.h>

#ifdef MISTER_HYBRID
#include "mister_port.h"
#endif


int main(int argc, char* argv[]) {
#ifdef MISTER_HYBRID
    // the game of the menu entry, or the player's pick; the purple screen
    // shows its logo until the first frame
    if (!MiSTer_SelectGame(&argc, &argv)) {
        return MiSTer_ExitCode();
    }
#endif
    printf("Host_Init\n");
    quakeparms_t* parms = Sys_Init(argc, argv);
    Host_Init(parms);

    double old_time = Sys_FloatTime();
    while (true) {
#ifdef MISTER_HYBRID
        // one frame per field, the game's clock the core's field counter
        if (MiSTer_Open()) {
            unsigned fields = MiSTer_WaitFrame();
            if (!fields) {
                Sys_Quit(); // the core is gone
            }
            Host_Frame((float) MiSTer_FieldsToSeconds(fields));
            old_time = Sys_FloatTime();
            continue;
        }
#endif
        double new_time = Sys_FloatTime();
        double dt = new_time - old_time;
        Host_Frame((float) dt);
        old_time = new_time;
    }
}
