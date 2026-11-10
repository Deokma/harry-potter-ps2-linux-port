// Real pad input (docs/PLAN_INPUT.md): the raylib gamepad calls ps2_pad.cpp makes, over SDL2's GameController API.
// SDL is used on the main thread only: ps2portPadInit (from ps2portDisplayInit) and ps2portPadPoll (every displayed vsync).
// The EE thread, which reads the pad in scePadRead, sees one snapshot in atomics.
//   PS2PORT_PAD_DB=FILE      SDL mappings to load first (else PortMaster's Knulli database, only when SDL knows no mapping)
//   PS2PORT_PAD_SWAP_FACE=1|0  swap A with B and X with Y, or not (default: swapped for ANBERNIC-keys, whose mapping follows the labels)
//   PS2PORT_PAD_LOG=1        print every change of the buttons and sticks
// Quit: Menu (guide) + Start, or L1 + R1 + Select + Start, held for a second. SIGTERM / SIGINT (SDL turns them into SDL_QUIT) too.
#include "raylib.h"

extern "C"
{
#include "sdl_min.h"
}

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace
{
    std::atomic<bool> g_available{false};
    std::atomic<uint32_t> g_buttons{0}; // bit n: raylib GamepadButton n
    std::atomic<uint64_t> g_axes{0};    // LX, LY, RX, RY: int16 each, as SDL reports them
    // Sticky presses: a button that went down at a poll is reported to the next two game reads even if it is already up, so a
    // tap shorter than a slow game frame (the Remembrall has 700-900 ms frames) still reaches the game. PS2PORT_PAD_STICKY=0: off.
    std::atomic<uint32_t> g_latched{0}, g_latchedOld{0};
    uint32_t g_prevButtons = 0;

    bool g_ready = false;
    bool g_dbLoaded = false;
    bool g_swapFace = false;
    bsdl_gamecontroller *g_pad = nullptr;
    unsigned g_polls = 0;
    unsigned g_reported = 0; // joystick indices already reported as unmapped (bit mask)

    const char *kKnulliDb = "/userdata/system/.local/share/PortMaster/knulli/gamecontrollerdb.txt";

    const char *kSdlNames[] = {"a", "b", "x", "y", "back", "guide", "start", "l3", "r3", "l1", "r1", "up", "down", "left", "right"};

    double nowS()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool loadDb(const char *path)
    {
        if (!path || access(path, R_OK) != 0 || !sdl.RWFromFile || !sdl.GameControllerAddMappingsFromRW)
            return false;
        bsdl_rwops *rw = sdl.RWFromFile(path, "rb");
        const int n = rw ? sdl.GameControllerAddMappingsFromRW(rw, 1) : -1;
        std::fprintf(stderr, "[pad] %d mappings from %s\n", n, path);
        return n > 0;
    }

    void quit(const char *why)
    {
        std::fprintf(stderr, "[pad] quit: %s\n", why);
        std::fflush(nullptr);
        std::_Exit(0);
    }

    // Opens the first joystick SDL can map. Unmapped ones are reported once, with their GUID (for a mapping line).
    void scan()
    {
        const int n = sdl.NumJoysticks();
        for (int pass = 0; pass < 2 && !g_pad; ++pass)
        {
            for (int i = 0; i < n && !g_pad; ++i)
            {
                if (sdl.IsGameController && sdl.IsGameController(i))
                {
                    g_pad = sdl.GameControllerOpen(i);
                    if (!g_pad)
                        continue;
                    const char *name = sdl.GameControllerName ? sdl.GameControllerName(g_pad) : nullptr;
                    std::fprintf(stderr, "[pad] controller %d: %s\n", i, name ? name : "?");
                    // PortMaster's ANBERNIC-keys mapping follows the labels: the button marked A, on the right, reads as SDL's
                    // a (the user's session, 2026-11-08). Swapped by default so that the bottom button is Cross, as on a DualShock.
                    const char *env = std::getenv("PS2PORT_PAD_SWAP_FACE");
                    g_swapFace = env ? std::atoi(env) != 0 : (name && std::strstr(name, "ANBERNIC"));
                    std::fprintf(stderr, "[pad] face buttons %s\n", g_swapFace ? "by position: bottom Cross, right Circle, left Square, top Triangle"
                                                                                : "as SDL names them: a Cross, b Circle, x Square, y Triangle");
                    if (sdl.GameControllerMapping)
                    {
                        if (char *m = sdl.GameControllerMapping(g_pad))
                        {
                            std::fprintf(stderr, "[pad] mapping: %.400s\n", m);
                            if (sdl.Free)
                                sdl.Free(m);
                        }
                    }
                }
                else if (pass == 1 && i < 32 && !(g_reported & (1u << i)))
                {
                    g_reported |= 1u << i;
                    char guid[64] = "?";
                    if (sdl.JoystickGetDeviceGUID && sdl.JoystickGetGUIDString)
                        sdl.JoystickGetGUIDString(sdl.JoystickGetDeviceGUID(i), guid, sizeof guid);
                    const char *name = sdl.JoystickNameForIndex ? sdl.JoystickNameForIndex(i) : nullptr;
                    std::fprintf(stderr, "[pad] joystick %d without a mapping: %s, GUID %s\n", i, name ? name : "?", guid);
                }
            }
            // Nothing SDL knows: its built-in mappings first, then the firmware's database (PortMaster's, for this device).
            if (!g_pad && pass == 0 && !g_dbLoaded && n > 0)
            {
                g_dbLoaded = true;
                loadDb(kKnulliDb);
            }
        }
        if (g_pad)
            g_available.store(true, std::memory_order_release);
        else if (g_polls == 0)
            std::fprintf(stderr, "[pad] no controller yet (%d joysticks)\n", n);
    }
}

void ps2portPadInit()
{
    if (!sdl.InitSubSystem || !sdl.NumJoysticks || !sdl.GameControllerOpen || !sdl.GameControllerGetButton || !sdl.GameControllerGetAxis)
    {
        std::fprintf(stderr, "[pad] this SDL2 lacks the game controller calls: no pad\n");
        return;
    }
    if (sdl.InitSubSystem(BSDL_INIT_JOYSTICK | BSDL_INIT_GAMECONTROLLER) != 0)
    {
        std::fprintf(stderr, "[pad] SDL_InitSubSystem: %s\n", sdl.GetError());
        return;
    }
    if (const char *db = std::getenv("PS2PORT_PAD_DB"))
        g_dbLoaded = loadDb(db);
    g_ready = true;
    scan();
}

// Main thread, every displayed vsync: SDL's events drained (they also update the controller), the state published.
void ps2portRequestShot(); // display.cpp

void ps2portPadPoll()
{
    if (!sdl.PollEvent)
        return;
    bsdl_event e;
    while (sdl.PollEvent(&e))
        if (e.type == BSDL_QUIT)
            quit("SDL_QUIT (a signal)");
    if (!g_ready)
        return;
    ++g_polls;
    if (g_pad && sdl.GameControllerGetAttached && !sdl.GameControllerGetAttached(g_pad))
    {
        std::fprintf(stderr, "[pad] controller gone\n");
        g_available.store(false, std::memory_order_release);
        if (sdl.GameControllerClose)
            sdl.GameControllerClose(g_pad);
        g_pad = nullptr;
    }
    if (!g_pad)
    {
        if (g_polls % 60 == 0)
            scan();
        if (!g_pad)
            return;
    }

    const bool swap = g_swapFace;
    static const bool log = std::getenv("PS2PORT_PAD_LOG") != nullptr;
    struct Map
    {
        int sdlButton, rayButton;
    };
    static const Map map[] = {
        {BSDL_CONTROLLER_BUTTON_DPAD_UP, GAMEPAD_BUTTON_LEFT_FACE_UP},
        {BSDL_CONTROLLER_BUTTON_DPAD_DOWN, GAMEPAD_BUTTON_LEFT_FACE_DOWN},
        {BSDL_CONTROLLER_BUTTON_DPAD_LEFT, GAMEPAD_BUTTON_LEFT_FACE_LEFT},
        {BSDL_CONTROLLER_BUTTON_DPAD_RIGHT, GAMEPAD_BUTTON_LEFT_FACE_RIGHT},
        // positional: south = Cross, east = Circle, west = Square, north = Triangle
        {BSDL_CONTROLLER_BUTTON_A, GAMEPAD_BUTTON_RIGHT_FACE_DOWN},
        {BSDL_CONTROLLER_BUTTON_B, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT},
        {BSDL_CONTROLLER_BUTTON_X, GAMEPAD_BUTTON_RIGHT_FACE_LEFT},
        {BSDL_CONTROLLER_BUTTON_Y, GAMEPAD_BUTTON_RIGHT_FACE_UP},
        {BSDL_CONTROLLER_BUTTON_LEFTSHOULDER, GAMEPAD_BUTTON_LEFT_TRIGGER_1},
        {BSDL_CONTROLLER_BUTTON_RIGHTSHOULDER, GAMEPAD_BUTTON_RIGHT_TRIGGER_1},
        {BSDL_CONTROLLER_BUTTON_BACK, GAMEPAD_BUTTON_MIDDLE_LEFT},
        {BSDL_CONTROLLER_BUTTON_GUIDE, GAMEPAD_BUTTON_MIDDLE},
        {BSDL_CONTROLLER_BUTTON_START, GAMEPAD_BUTTON_MIDDLE_RIGHT},
        {BSDL_CONTROLLER_BUTTON_LEFTSTICK, GAMEPAD_BUTTON_LEFT_THUMB},
        {BSDL_CONTROLLER_BUTTON_RIGHTSTICK, GAMEPAD_BUTTON_RIGHT_THUMB},
    };
    uint32_t buttons = 0, raw = 0;
    for (const Map &m : map)
    {
        if (!sdl.GameControllerGetButton(g_pad, m.sdlButton))
            continue;
        raw |= 1u << m.sdlButton;
        int b = m.rayButton;
        if (swap)
        {
            if (b == GAMEPAD_BUTTON_RIGHT_FACE_DOWN)
                b = GAMEPAD_BUTTON_RIGHT_FACE_RIGHT;
            else if (b == GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)
                b = GAMEPAD_BUTTON_RIGHT_FACE_DOWN;
            else if (b == GAMEPAD_BUTTON_RIGHT_FACE_LEFT)
                b = GAMEPAD_BUTTON_RIGHT_FACE_UP;
            else if (b == GAMEPAD_BUTTON_RIGHT_FACE_UP)
                b = GAMEPAD_BUTTON_RIGHT_FACE_LEFT;
        }
        buttons |= 1u << b;
    }
    // L2 / R2: trigger axes (a digital trigger reads 0 or 32767)
    const int16_t lt = sdl.GameControllerGetAxis(g_pad, BSDL_CONTROLLER_AXIS_TRIGGERLEFT);
    const int16_t rt = sdl.GameControllerGetAxis(g_pad, BSDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (lt > 16384)
        buttons |= 1u << GAMEPAD_BUTTON_LEFT_TRIGGER_2;
    if (rt > 16384)
        buttons |= 1u << GAMEPAD_BUTTON_RIGHT_TRIGGER_2;
    uint64_t axes = 0;
    int16_t a[4];
    for (int i = 0; i < 4; ++i)
    {
        a[i] = sdl.GameControllerGetAxis(g_pad, BSDL_CONTROLLER_AXIS_LEFTX + i);
        axes |= static_cast<uint64_t>(static_cast<uint16_t>(a[i])) << (16 * i);
    }
    g_axes.store(axes, std::memory_order_relaxed);
    // Screenshot: Menu + L1 (not with Start: Menu + Start quits), once per press. L1 does not reach the game while Menu is down.
    {
        static bool shotHeld = false;
        const uint32_t menu = 1u << GAMEPAD_BUTTON_MIDDLE, l1 = 1u << GAMEPAD_BUTTON_LEFT_TRIGGER_1;
        const bool shotNow = (buttons & menu) && (buttons & l1) && !(buttons & (1u << GAMEPAD_BUTTON_MIDDLE_RIGHT));
        if (shotNow && !shotHeld)
            ps2portRequestShot();
        shotHeld = shotNow;
        if (buttons & menu)
            buttons &= ~l1;
    }
    static const bool sticky = !std::getenv("PS2PORT_PAD_STICKY") || std::atoi(std::getenv("PS2PORT_PAD_STICKY")) != 0;
    if (sticky)
        g_latched.fetch_or(buttons & ~g_prevButtons, std::memory_order_acq_rel);
    g_prevButtons = buttons;
    g_buttons.store(buttons, std::memory_order_release);

    // quit combination, held for a second
    static double held = 0;
    const uint32_t menuStart = (1u << GAMEPAD_BUTTON_MIDDLE) | (1u << GAMEPAD_BUTTON_MIDDLE_RIGHT);
    const uint32_t fourButtons = (1u << GAMEPAD_BUTTON_LEFT_TRIGGER_1) | (1u << GAMEPAD_BUTTON_RIGHT_TRIGGER_1) |
                                 (1u << GAMEPAD_BUTTON_MIDDLE_LEFT) | (1u << GAMEPAD_BUTTON_MIDDLE_RIGHT);
    if ((buttons & menuStart) == menuStart || (buttons & fourButtons) == fourButtons)
    {
        if (held == 0)
            held = nowS();
        else if (nowS() - held > 1.0)
            quit("button combination");
    }
    else
        held = 0;

    if (log)
    {
        static uint32_t lastRaw = ~0u;
        static int lastCoarse[6] = {99, 99, 99, 99, 99, 99};
        int coarse[6] = {a[0] / 8192, a[1] / 8192, a[2] / 8192, a[3] / 8192, lt / 8192, rt / 8192};
        if (raw != lastRaw || std::memcmp(coarse, lastCoarse, sizeof coarse) != 0)
        {
            char names[160] = "";
            for (int i = 0; i < 15; ++i)
                if (raw & (1u << i))
                {
                    std::strncat(names, " ", sizeof names - std::strlen(names) - 1);
                    std::strncat(names, kSdlNames[i], sizeof names - std::strlen(names) - 1);
                }
            std::fprintf(stderr, "[pad] t=%.1f buttons:%s; left %d %d right %d %d triggers %d %d\n", nowS(), names[0] ? names : " none",
                         a[0], a[1], a[2], a[3], lt, rt);
            lastRaw = raw;
            std::memcpy(lastCoarse, coarse, sizeof coarse);
        }
    }
}

bool IsGamepadAvailable(int gamepad)
{
    return gamepad == 0 && g_available.load(std::memory_order_acquire);
}

bool IsGamepadButtonDown(int gamepad, int button)
{
    if (gamepad != 0 || button < 0 || button >= 32)
        return false;
    const uint32_t down = g_buttons.load(std::memory_order_acquire) | g_latched.load(std::memory_order_acquire) |
                          g_latchedOld.load(std::memory_order_acquire);
    return (down >> button) & 1u;
}

// After a game read of port 0 (ps2_pad.cpp): latched presses have now been seen once; they stay for one more read, then go
void ps2portPadConsumed()
{
    g_latchedOld.store(g_latched.exchange(0u, std::memory_order_acq_rel), std::memory_order_release);
}

// -1..1 with a dead zone of 12 % (the sticks rest a little off centre), rescaled so the edge of the zone reads 0
float GetGamepadAxisMovement(int gamepad, int axis)
{
    if (gamepad != 0 || axis < 0 || axis > 3)
        return 0.0f;
    const int16_t raw = static_cast<int16_t>(g_axes.load(std::memory_order_relaxed) >> (16 * axis));
    float v = raw / 32767.0f;
    const float dead = 0.12f;
    const float m = std::fabs(v);
    if (m < dead)
        return 0.0f;
    v = std::copysign((m - dead) / (1.0f - dead), v);
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}
