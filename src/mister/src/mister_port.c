// MiSTer hybrid core support, see mister_port.h

#define _GNU_SOURCE
#include "mister_port.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <mister_hybrid.h>

#define EXIT_TO_MENU 0
#define EXIT_RESTART 42 // danik_hybrid_launch.sh starts the game again
#define EXIT_RELOADED 43 // the same, after the core has settled

static const char* const TITLE = "ChocolateQuake";

// A field of the core's 15kHz modes: 400 x 262 pixels at 6.25MHz
// (hybrid/tools/fakecore.c)
#define FIELD_USEC 16768

int MiSTer_Open(void) {
    return MH_IsOpen();
}

int MiSTer_PinHelper(void) {
    return !MH_IsOpen() || MH_PinThreadToCPU1();
}

static uint64_t NowUS(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}


// OSD options of the game (CONF_STR in core/ChocolateQuake.sv, status bits
// 24..63). The first entry of each is 0, the default.

// "O[25:24],Resolution,320x200,640x200,320x240,640x240;"
void MiSTer_Resolution(int* width, int* height) {
    static int fixed_w = -1, fixed_h;
    int mode;

    if (fixed_w < 0) {
        const char* s = getenv("MISTER_HYBRID_MODE");
        fixed_w = 0;
        if (s && sscanf(s, "%dx%d", &fixed_w, &fixed_h) != 2) {
            fixed_w = 0;
        }
    }
    if (fixed_w == 320 || fixed_w == 640) {
        *width = fixed_w;
        *height = fixed_h == 240 ? 240 : 200;
        return;
    }
    mode = MH_IsOpen() ? MH_OSD_GAME_BITS(MH_OSDStatus(), 24, 2) : 0;
    *width = (mode & 1) ? 640 : 320;
    *height = (mode & 2) ? 240 : 200;
}

// "D0D7P1O[81:77],HUD (Top)" and "D0D8P1O[86:82],HUD (Bottom)" (CRT
// Options), greyed out with mask bit 0 while the game is in 200 lines
// (HudLines()). Quake's top HUD is the console's message lines, its bottom
// HUD the status bar (Danik, 2026-10-24). MISTER_HYBRID_HUD=<top>,<bottom>
// fixes them for a test (the library).
void MiSTer_HudRows(int height, int* top, int* bottom) {
    int t, b;

    *top = 0;
    *bottom = height;
    if (height < 240 || !MH_IsOpen()) {
        return;
    }
    MH_HudMargins(&t, &b);
    *top = t;
    *bottom = height - b;
}

static void HudLines(void) {
    static int mask = -1;
    int width, height, m;

    MiSTer_Resolution(&width, &height);
    m = height == 240 ? 0 : 1;
    if (m != mask) {
        MH_SetMenuMask(m);
    }
    mask = m;
}

// "P2O[28],God Mode", "P2O[29],No Clipping", "P2O[30],Invisible to Monsters"
// and the triggers "P2T[56],All Weapons and Keys" .. "P2T[59],Next Level"
// (core/ChocolateQuake.sv). MISTER_HYBRID_CHEATS=<bits> fixes them for a
// test: the switches stay, the triggers are selected once a level runs.
int MiSTer_Cheats(int level) {
    static int fixed = -2;
    int triggers;

    if (fixed == -2) {
        fixed = getenv("MISTER_HYBRID_CHEATS") ? atoi(getenv("MISTER_HYBRID_CHEATS")) : -1;
    }
    if (fixed >= 0) {
        if (!level) {
            return fixed & MISTER_CHEAT_SWITCHES;
        }
        triggers = fixed & ~MISTER_CHEAT_SWITCHES;
        fixed &= MISTER_CHEAT_SWITCHES;
        return fixed | triggers;
    }
    if (!MH_IsOpen()) {
        return 0;
    }
    triggers = MH_OSDTriggers() & 15;
    return MH_OSD_GAME_BITS(MH_OSDStatus(), 28, 3) | (level ? triggers << 3 : 0);
}


// Rumble: a jolt for each motor, its strength fading out from `start` to
// `end`; RumbleUpdate() tells the controller every frame while one goes on.
// "O[27:26],Rumble,On,Strong,Off;": Strong is twice as strong.
static struct {
    int level;
    uint64_t start, end;
} Jolts[2];
static int Rumbling = 0;

static int RumbleScale(void) {
    static const int scale[4] = {1, 2, 0, 0};
    return MH_IsOpen() ? scale[MH_OSD_GAME_BITS(MH_OSDStatus(), 26, 2)] : 0;
}

static int JoltNow(int motor, uint64_t now) {
    if (now >= Jolts[motor].end) {
        return 0;
    }
    return (int) (Jolts[motor].level * (Jolts[motor].end - now)
                  / (Jolts[motor].end - Jolts[motor].start));
}

void MiSTer_Rumble(int strong, int weak, int ms) {
    int levels[2] = {strong, weak};
    int scale = RumbleScale();
    uint64_t now = NowUS();
    int motor;

    if (!scale || ms <= 0) {
        return;
    }
    for (motor = 0; motor < 2; motor++) {
        int level = levels[motor] * scale;

        if (level > 255) {
            level = 255;
        }
        if (level > JoltNow(motor, now)) {
            Jolts[motor].level = level;
            Jolts[motor].start = now;
            Jolts[motor].end = now + (uint64_t) ms * 1000;
        }
    }
}

void MiSTer_RumbleHit(int damage) {
    if (damage > 0) {
        if (damage > 100) {
            damage = 100;
        }
        MiSTer_Rumble(30 + damage * 3, 60 + damage * 2, 120 + damage * 4);
    }
}

void MiSTer_RumbleShot(float kick) {
    // id1's weapons kick the view up 2 degrees (4 the super shotgun, the
    // rocket launcher's 2 with a heavier shot); a light buzz, more with more
    if (kick >= 3.5f) {
        MiSTer_Rumble(120, 140, 150);
    } else if (kick > 0.5f) {
        MiSTer_Rumble(0, 90, 70);
    }
}

void MiSTer_RumbleLand(int hard) {
    if (hard >= 2) {
        MiSTer_Rumble(200, 120, 250);
    } else if (hard == 1) {
        MiSTer_Rumble(90, 50, 120);
    }
}

void MiSTer_RumbleExplosion(float distance) {
    // a rocket's blast reaches 120 units; felt out to 600
    const float far = 600.0f;

    if (distance < far) {
        int level = (int) (220.0f * (1.0f - distance / far));
        MiSTer_Rumble(level, level / 2, 150 + level);
    }
}

static void RumbleUpdate(void) {
    uint64_t now = NowUS();
    int strong, weak;

    if (!RumbleScale()) {
        Jolts[0].end = Jolts[1].end = 0;
    }
    strong = JoltNow(0, now);
    weak = JoltNow(1, now);
    // once a frame while it goes on (the motors stop by themselves two
    // seconds after the last call), and once more to stop them
    if (strong || weak || Rumbling) {
        MH_Rumble(strong, weak);
    }
    Rumbling = strong || weak;
}


// Frames drawn a field ahead (hybrid/CLAUDE.md, "Frame pacing"): the frame
// for field F is started once the counter is F - 2 and handed over with
// MH_PresentAtField(F). Decided by benchmark (the core's CLAUDE.md).
#define DRAW_AHEAD 1

static uint32_t LastField; // with DRAW_AHEAD the field the last frame is shown at
static int FieldValid = 0;

// Development: see mister_port.h
static int LogEnabled = -1;
static int MarkNow;
static struct timespec MarkTime;
static double MarkSum[MISTER_T_COUNT];
static double MarkFrame[MISTER_T_COUNT]; // of the frame that is being made
static double MarkSlowest[MISTER_T_COUNT]; // of the frame that took longest
static double MarkSlowestWork;

void MiSTer_Mark(int what) {
    struct timespec now;

    if (LogEnabled <= 0) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    MarkFrame[MarkNow] += (now.tv_sec - MarkTime.tv_sec) * 1e3
                          + (now.tv_nsec - MarkTime.tv_nsec) * 1e-6;
    MarkTime = now;
    MarkNow = what;
}

static uint64_t Fnv(uint64_t hash, const unsigned char* data, int size) {
    int i;

    for (i = 0; i < size; i++) {
        hash = (hash ^ data[i]) * 1099511628211ull;
    }
    return hash;
}

void MiSTer_FrameHash(const unsigned char* pixels, int size, const void* z, int zsize) {
    static int enabled = -1;
    static unsigned frame;
    uint64_t hash = 14695981039346656037ull; // FNV-1a

    if (enabled < 0) {
        enabled = getenv("MISTER_HYBRID_FRAMEHASH") && atoi(getenv("MISTER_HYBRID_FRAMEHASH"));
    }
    if (!enabled) {
        return;
    }
    hash = Fnv(hash, pixels, size);
    printf("frame %u %016llx %016llx\n", frame++, (unsigned long long) hash,
           (unsigned long long) Fnv(14695981039346656037ull, z, zsize));
}

// Development, MISTER_HYBRID_PROFILE=<file> with MISTER_HYBRID_FPSLOG=1: where
// the game thread spends its time. A timer on the thread's CPU time (so it
// never wakes a sleep) notes the program counter and the return address at
// every tick of the kernel, from the first log on; the eleventh writes them
// (those of the 50 seconds of the benchmark) to the file as "pc lr" in hex, to
// be looked up in the unstripped binary. "ffffffff <us>" ends a frame that
// took that many microseconds of work.
#define PROFILE_MAX (1 << 18)
static uint32_t (*ProfileSamples)[2];
static volatile int ProfileCount;
static const char* ProfileFile;

static void ProfileTick(int sig, siginfo_t* info, void* context) {
    const ucontext_t* uc = context;
    (void) sig;
    (void) info;
    if (ProfileCount < PROFILE_MAX) {
#ifdef __arm__
        ProfileSamples[ProfileCount][0] = uc->uc_mcontext.arm_pc;
        ProfileSamples[ProfileCount][1] = uc->uc_mcontext.arm_lr;
#else
        (void) uc;
#endif
        ProfileCount++;
    }
}

static void ProfileStart(void) {
    struct sigaction sa;
    struct sigevent sev;
    struct itimerspec its;
    timer_t timer;

    ProfileFile = getenv("MISTER_HYBRID_PROFILE");
    if (!ProfileFile || !*ProfileFile
        || !(ProfileSamples = malloc(PROFILE_MAX * sizeof(*ProfileSamples)))) {
        ProfileFile = NULL;
        return;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ProfileTick;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev._sigev_un._tid = syscall(SYS_gettid);
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timer) != 0) {
        printf("mister: no profiler timer\n");
        ProfileFile = NULL;
        return;
    }
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 1000000; // as often as the kernel's tick allows
    its.it_value = its.it_interval;
    timer_settime(timer, 0, &its, NULL);
}

static void ProfileLog(void) {
    static int logs;
    FILE* f;
    int i, n = ProfileCount;

    if (!ProfileFile) {
        return;
    }
    if (++logs == 1) {
        ProfileCount = 0; // the game's start left out
        return;
    }
    if (logs != 11 || !(f = fopen(ProfileFile, "w"))) {
        return;
    }
    fprintf(f, "# %d logs\n", logs - 1);
    for (i = 0; i < n; i++) {
        fprintf(f, "%08x %08x\n", (unsigned) ProfileSamples[i][0],
                (unsigned) ProfileSamples[i][1]);
    }
    fclose(f);
}

// Every 300 fields (5 seconds): the frames shown, how many of them came 1, 2,
// 3 and more fields after the one before, and the milliseconds per frame
static void FpsLog(unsigned fields) {
    static const char* const names[MISTER_T_COUNT] = {
        "other", "wait", "input", "server", "client", "render", "hud", "blit", "sound"
    };
    static unsigned frames, total, after[4];
    double work = 0;
    int i;

    if (LogEnabled < 0) {
        LogEnabled = getenv("MISTER_HYBRID_FPSLOG") && atoi(getenv("MISTER_HYBRID_FPSLOG"));
        if (LogEnabled) {
            clock_gettime(CLOCK_MONOTONIC, &MarkTime);
            ProfileStart();
        }
    }
    if (!LogEnabled || !fields) {
        return;
    }
    for (i = 0; i < MISTER_T_COUNT; i++) {
        MarkSum[i] += MarkFrame[i];
        if (i != MISTER_T_WAIT) {
            work += MarkFrame[i];
        }
    }
    if (ProfileFile && ProfileCount < PROFILE_MAX) {
        // the end of a frame, with its time in microseconds
        ProfileSamples[ProfileCount][0] = 0xFFFFFFFF;
        ProfileSamples[ProfileCount][1] = (uint32_t) (work * 1000);
        ProfileCount++;
    }
    if (work > MarkSlowestWork) {
        MarkSlowestWork = work;
        memcpy(MarkSlowest, MarkFrame, sizeof(MarkSlowest));
    }
    memset(MarkFrame, 0, sizeof(MarkFrame));
    frames++;
    total += fields;
    after[fields >= 4 ? 3 : fields - 1]++;
    if (total >= 300) {
        printf("mister: %u frames in %u fields (%.1f per second), after 1/2/3/more fields %u/%u/%u/%u, ms:",
               frames, total, frames * 1000000.0 / (total * FIELD_USEC), after[0], after[1],
               after[2], after[3]);
        for (i = 0; i < MISTER_T_COUNT; i++) {
            printf(" %s %.2f", names[i], MarkSum[i] / frames);
            MarkSum[i] = 0;
        }
        printf("\nmister: slowest frame %.1f ms:", MarkSlowestWork);
        for (i = 0; i < MISTER_T_COUNT; i++) {
            if (i != MISTER_T_WAIT && MarkSlowest[i] >= 0.5) {
                printf(" %s %.1f", names[i], MarkSlowest[i]);
            }
        }
        printf("\n");
        fflush(stdout);
        ProfileLog();
        MarkSlowestWork = 0;
        frames = total = 0;
        memset(after, 0, sizeof(after));
    }
}

void MiSTer_ResetFrames(void) {
    FieldValid = 0;
}

unsigned MiSTer_WaitFrame(void) {
    uint32_t field;
    unsigned fields;

    if (!MH_IsOpen()) {
        return 0;
    }
    RumbleUpdate();
    if (!FieldValid) {
        LastField = MH_FieldCounter();
        FieldValid = 1;
    }
    MiSTer_Mark(MISTER_T_WAIT);
    HudLines();
    if (DRAW_AHEAD && !MH_Interlaced()) {
        uint32_t target = LastField + 1;

        MH_WaitField(target - 3); // not before the field two before its own
        field = MH_FieldCounter();
        if ((int32_t) (field + 1 - target) > 0) {
            target = field + 1; // too late for it: the next vblank
        }
        if (!MH_IsOpen()) {
            return 0;
        }
        MH_PresentAtField(target);
        fields = target - LastField;
        LastField = target;
        MiSTer_Mark(MISTER_T_OTHER);
        FpsLog(fields);
        return fields;
    }
    MH_WaitFrame(LastField);
    field = MH_FieldCounter();
    fields = field - LastField; // 0: the core is gone
    LastField = field;
    MiSTer_Mark(MISTER_T_OTHER);
    FpsLog(fields);
    return fields;
}

double MiSTer_FieldsToSeconds(unsigned fields) {
    static int hashing = -1;

    if (hashing < 0) {
        hashing = getenv("MISTER_HYBRID_FRAMEHASH") && atoi(getenv("MISTER_HYBRID_FRAMEHASH"));
    }
    if (hashing) {
        // the same frame times on every run: particles move by them
        fields = 1;
    }
    return fields * (FIELD_USEC / 1e6);
}


const char* MiSTer_PadBindings(void) {
    return "bind ABUTTON +jump\n"
           "bind BBUTTON +movedown\n"
           "bind XBUTTON \"impulse 10\"\n"
           "bind YBUTTON \"impulse 12\"\n"
           "bind LSHOULDER +speed\n"
           "bind RSHOULDER +attack\n"
           "bind LTRIGGER +speed\n"
           "bind RTRIGGER +attack\n"
           "bind RTHUMB centerview\n";
}


// The player came through the list: quitting a game goes back to it
static int PickedFromList = 0;

void MiSTer_ShowError(const char* message) {
    if (MH_Open()) {
        MH_UI_Message(TITLE, message, MH_UI_WAIT | MH_UI_ERROR);
    }
}

// Quake, its shareware and the mission packs, each a menu entry
// (package/games.txt: the same ids and titles; the shareware has none,
// Quake's starts it), the set name ChocolateQuake_<id>. The shareware is
// id1/pak0.pak alone (the full game's pak0.pak is the same file), so with
// pak1.pak there it is not offered on its own. The mission packs need the
// full game. Messages: 4 lines of 71, %s the game's folder.
static const char* const QuakeFiles[] = {"id1/pak0.pak", "id1/pak1.pak", NULL};
static const char* const SharewareFiles[] = {"id1/pak0.pak", NULL};
static const char* const HipnoticFiles[] = {"id1/pak0.pak", "id1/pak1.pak", "hipnotic/pak0.pak", NULL};
static const char* const RogueFiles[] = {"id1/pak0.pak", "id1/pak1.pak", "rogue/pak0.pak", NULL};
static struct mh_game Games[] = {
    {"QUAKE", "Quake", QuakeFiles, NULL,
     "Quake was not found (id1/pak0.pak and id1/pak1.pak). Copy everything in "
     "the folder id1 of Quake (CD, Steam or GOG, or the shareware) into %s/id1/."},
    {"SHAREWARE", "Quake (Shareware)", SharewareFiles, "QUAKE", NULL},
    {"HIPNOTIC", "Quake - Scourge of Armagon", HipnoticFiles, NULL,
     "Scourge of Armagon was not found. It needs the full Quake (its folder id1 "
     "with pak0.pak and pak1.pak) and the mission pack's folder hipnotic, both "
     "in %s/."},
    {"ROGUE", "Quake - Dissolution of Eternity", RogueFiles, NULL,
     "Dissolution of Eternity was not found. It needs the full Quake (its "
     "folder id1 with pak0.pak and pak1.pak) and the mission pack's folder "
     "rogue, both in %s/."},
};
static const char* const GameArgs[] = {NULL, NULL, "-hipnotic", "-rogue"};
#define NUM_GAMES ((int) (sizeof(Games) / sizeof(Games[0])))
#define NO_GAME "No game found (id1/pak0.pak). Copy everything in the folder id1 " \
                "of Quake (CD, Steam or GOG, or the shareware) into %s/id1/."

int MiSTer_SelectGame(int* argc, char*** argv) {
    static char* args[64];
    struct stat st;
    int pick;

    if (!MH_Open()) {
        return 1;
    }
    // the full game has the shareware's episode
    Games[1].files = stat("id1/pak1.pak", &st) == 0 ? MH_NOT_THERE : SharewareFiles;
    // "Checking Game Data...", then the game's logo with "Loading <title>..."
    // come with the pick (MH_GameLogo(), host version 17); the shareware has
    // Quake's logo
    pick = MH_PickGame(Games, NUM_GAMES, NULL, NO_GAME, &PickedFromList);
    if (pick < 0) {
        return 0;
    }
    if (*argc + 4 < (int) (sizeof(args) / sizeof(args[0]))) {
        memcpy(args, *argv, *argc * sizeof(char*));
        // the data is in the current folder (danik_hybrid_launch.sh)
        args[(*argc)++] = "-basedir";
        args[(*argc)++] = ".";
        if (GameArgs[pick]) {
            args[(*argc)++] = (char*) GameArgs[pick];
        }
        // development: MISTER_HYBRID_ARGS=+timedemo,demo1 adds arguments
        // (commas for spaces, which the test script's TEST_ENV cannot pass)
        if (getenv("MISTER_HYBRID_ARGS")) {
            char* more = strdup(getenv("MISTER_HYBRID_ARGS"));
            for (char* a = strtok(more, ","); a && *argc + 1 < (int) (sizeof(args) / sizeof(args[0]));
                 a = strtok(NULL, ",")) {
                args[(*argc)++] = a;
            }
        }
        args[*argc] = NULL;
        *argv = args;
    }
    return 1;
}

int MiSTer_ExitCode(void) {
    if (Rumbling) {
        MH_Rumble(0, 0);
    }
    // The player loaded the core again while the game ran
    if (MH_CoreReloaded()) {
        return EXIT_RELOADED;
    }
    // Not if we are quitting because another core was loaded
    if (PickedFromList && MH_Open()) {
        return EXIT_RESTART;
    }
    return EXIT_TO_MENU;
}
