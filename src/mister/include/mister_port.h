#ifndef MISTER_PORT_H
#define MISTER_PORT_H

// MiSTer hybrid core: the game runs on the ARM, the FPGA core shows the
// picture at 15kHz and provides audio and input. SDL's "mister" drivers
// (hybrid/sdl2) do most of the work; this is what is specific to the game.
// Everything is a no-op without the core, so the PC build still runs.

#ifdef __cplusplus
extern "C" {
#endif

// Joystick buttons as SDL sees them: the buttons of the core's "J1," list
// (core/ChocolateQuake.sv) in order, then the ones the SDL driver adds. The
// game takes the joystick as an SDL game controller (MiSTer_AddMappings() in
// input/src/in_gamepad.c) and binds its buttons (MiSTer_BindPad()).
enum {
    MISTER_JOY_JUMP,      // A
    MISTER_JOY_SWIM_DOWN, // B
    MISTER_JOY_NEXT,      // X: next weapon
    MISTER_JOY_PREVIOUS,  // Y: previous weapon
    MISTER_JOY_RUN,       // L
    MISTER_JOY_ATTACK,    // R
    MISTER_JOY_SCORES,    // Select
    MISTER_JOY_MENU,      // Start
    MISTER_JOY_CORE_OK,
    MISTER_JOY_CORE_BACK,

    MISTER_JOY_MENU_OK = 28,
    MISTER_JOY_MENU_BACK = 29,
    MISTER_JOY_L3 = 30, // the stick clicks, read from Linux (MH_ReadStickButtons())
    MISTER_JOY_R3 = 31
};

// 1 while the game runs on the core
int MiSTer_Open(void);
// Puts the calling thread on CPU1, next to the core's helper threads, so
// that it works beside the game's on CPU0 (the renderer's span worker,
// renderer/src/d_spanjob.c). 1 if it is there, or without the core (the PC:
// left where it is); 0 if the game may not use CPU1
int MiSTer_PinHelper(void);

// Frame pacing: call once per frame in place of the game's own clock. Sleeps
// until the core has taken the last frame and returns how many fields
// (1/59.64s) have passed since the last call, which is the game's clock. 0
// without the core: use the original timing then.
unsigned MiSTer_WaitFrame(void);
// The next call starts counting anew, after the game stood still (loading)
void MiSTer_ResetFrames(void);
// Game time of that many fields, in seconds
double MiSTer_FieldsToSeconds(unsigned fields);

// Development, MISTER_HYBRID_FPSLOG=1: every 300 fields, the frames shown and
// where the time of a frame went. The engine says what it starts doing
// (MiSTer_Mark()); the log has the milliseconds per frame of each.
enum {
    MISTER_T_OTHER,
    MISTER_T_WAIT,   // for the core to take the frame
    MISTER_T_INPUT,  // the events, the pad
    MISTER_T_SERVER, // the game's logic
    MISTER_T_CLIENT, // the rest of the client
    MISTER_T_RENDER, // the 3D view
    MISTER_T_HUD,    // 2D: status bar, console, menus
    MISTER_T_BLIT,   // the frame to the core
    MISTER_T_SOUND,  // the mix
    MISTER_T_COUNT
};
void MiSTer_Mark(int what);
// Development, MISTER_HYBRID_FRAMEHASH=1: prints a hash of every frame's
// pixels and one of its z buffer, so that a change to the renderer can be
// shown to draw the same pictures (with "+timedemo demo1", which draws the
// same frames every time)
void MiSTer_FrameHash(const unsigned char* pixels, int size, const void* z, int zsize);

// OSD "Resolution": the size of the game's screen, 320 or 640 wide, 200 or
// 240 high. MISTER_HYBRID_MODE=<width>x<height> fixes it for a test.
void MiSTer_Resolution(int* width, int* height);
// The HUD margins of the OSD's CRT Options for a screen of `height` rows: the
// row the top HUD starts at and the row under the bottom HUD. 0 and `height`
// in 200 rows (the HUD lines are greyed out then). Read every frame.
void MiSTer_HudRows(int height, int* top, int* bottom);

// Rumble (the OSD's Rumble: On, Strong, Off): the controller's heavy and
// light motor, 0..255, fading out over `ms`; a jolt that is weaker than the
// one going on is left out. The game's moments:
void MiSTer_Rumble(int strong, int weak, int ms);
// the player was hit (the damage the server reported)
void MiSTer_RumbleHit(int damage);
// the player fired a weapon (how far it kicked the view up, in degrees)
void MiSTer_RumbleShot(float kick);
// the player landed: 1 hard, 2 hard enough to hurt
void MiSTer_RumbleLand(int hard);
// an explosion that far from the player
void MiSTer_RumbleExplosion(float distance);

// The OSD's Cheats page, once a frame: the MISTER_CHEAT_* switches that are
// on, and the triggers selected since the last call (MH_OSDTriggers(): from
// one place only). `level`: 0 while no level of a single player game runs,
// and the triggers are dropped
#define MISTER_CHEAT_GOD      1  // God Mode
#define MISTER_CHEAT_NOCLIP   2  // No Clipping
#define MISTER_CHEAT_NOTARGET 4  // Invisible to Monsters
#define MISTER_CHEAT_SWITCHES 7
#define MISTER_CHEAT_GIVE     8  // the trigger All Weapons and Keys
#define MISTER_CHEAT_QUAD     16 // the trigger Quad Damage
#define MISTER_CHEAT_HEALTH   32 // the trigger Full Health and Armor
#define MISTER_CHEAT_NEXT     64 // the trigger Next Level
int MiSTer_Cheats(int level);

// The controller's default bindings (the core's J1 list and the analog
// triggers), executed after default.cfg
const char* MiSTer_PadBindings(void);

// Before the engine starts: the game of the menu entry the core was loaded
// from (MISTER_HYBRID_SET, ChocolateQuake_<id>: QUAKE, HIPNOTIC, ROGUE), or the one installed, or the
// one the player picks from the list of those (MH_PickGame()), with its logo
// and "Loading <title>..." until the first frame. Adds "-hipnotic" or
// "-rogue" to the arguments for a mission pack. Returns 0 if there is
// nothing to play (said on the screen until the player left the core) or
// the player backed out of the list.
int MiSTer_SelectGame(int* argc, char*** argv);
// Problems the player can fix (missing data), and the engine's errors: shown
// under the core's logo until the player leaves the core, in plain words,
// in 4 lines of 71 characters at most
void MiSTer_ShowError(const char* message);
// Exit code of the process after the player quit: the launcher starts the
// game again (back to the list of MiSTer_SelectGame) or returns to the menu
int MiSTer_ExitCode(void);

#ifdef __cplusplus
}
#endif

#endif
