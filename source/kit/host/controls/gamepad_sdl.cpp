// gamepad_sdl.cpp - see gamepad_sdl.h.
#include "gamepad_sdl.h"

#include "vpad.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
EM_JS_DEPS(recomp_gamepad, "$UTF8ToString");
#endif

namespace controls {

namespace {

SDL_Gamepad *g_pad = nullptr;
SDL_JoystickID g_id = 0;
bool g_buttons[kSdlPadButtonCount] = {};
int16_t g_axes[kSdlAxisCount] = {};

bool (*g_suppressed)() = nullptr;
bool g_masked[kSdlPadButtonCount] =
    {}; // pressed for the host (the page toggle); hidden until released
bool g_was_suppressed = false;

// While something of the host's owns the pad (the settings page, or the web page's own menu),
// the game sees a centred pad with nothing held, so a press that drives the page does not also
// drive the car. The real state is still tracked, and the next poll after the page closes hands
// it back - except buttons still held then (the press that closed the page), which the game
// sees only after they are released.
void publish() {
    if (g_suppressed && g_suppressed()) {
        static const bool none[kSdlPadButtonCount] = {};
        static const int16_t centred[kSdlAxisCount] = {};
        g_was_suppressed = true;
        vpad().set_source(kPadSourceController, pad_from_sdl(none, centred));
        return;
    }
    if (g_was_suppressed) {
        g_was_suppressed = false;
        for (int i = 0; i < kSdlPadButtonCount; ++i)
            if (g_buttons[i])
                g_masked[i] = true;
    }
    bool shown[kSdlPadButtonCount];
    for (int i = 0; i < kSdlPadButtonCount; ++i) {
        if (!g_buttons[i])
            g_masked[i] = false; // released, however the release was learnt
        shown[i] = g_buttons[i] && !g_masked[i];
    }
    vpad().set_source(kPadSourceController, pad_from_sdl(shown, g_axes));
}

// Reads every mapped button and axis from SDL's current state.
void read_all() {
    for (int i = 0; i < kSdlPadButtonCount; ++i)
        g_buttons[i] = SDL_GetGamepadButton(g_pad, SDL_GamepadButton(i));
    for (int i = 0; i < kSdlAxisCount; ++i)
        g_axes[i] = SDL_GetGamepadAxis(g_pad, SDL_GamepadAxis(i));
}

#ifdef __EMSCRIPTEN__
// PlayStation pads in the browser. SDL's Emscripten driver gives every pad one default mapping,
// written for the W3C "standard" layout. Chrome uses that layout for the DualShock 4 and the
// DualSense / DualSense Edge on macOS and Windows (Chromium's MapperDualSense), with the
// touchpad click as an 18th button that the default leaves out. A browser with no layout for
// the pad (mapping "") hands over HID order instead: square, cross, circle, triangle, L1, R1,
// L2, R2, create, options, L3, R3, PS, touchpad; axes X, Y, Z (right X), Rx (L2), Ry (R2),
// Rz (right Y), hat on axis 9 - which SDL turns into a hat only on Windows. The default
// mapping would put gas on the right stick and cross on square there. So a Sony pad gets a
// mapping of its own, for this pad's GUID, before it is opened. SDL's indices for the
// standard layout skip the digital triggers (6, 7) and the d-pad (12-15, made a hat), so
// SDL button 10 is W3C 16 (PS) and 11 is W3C 17 (touchpad); the triggers are axes 4 and 5.
// Returns which layout the pad came in, for the log and the page.
const char *web_sony_mapping(SDL_JoystickID id) {
    if (SDL_GetJoystickVendorForID(id) != 0x054c)
        return "";
    const Uint16 product = SDL_GetJoystickProductForID(id);
    // DualShock 4 (05c4, 09cc, 0ba0 its USB adapter), DualSense (0ce6), DualSense Edge (0df2).
    if (product != 0x05c4 && product != 0x09cc && product != 0x0ba0 && product != 0x0ce6 &&
        product != 0x0df2)
        return "";
    static const char kStandard[] =
        "a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,leftstick:b8,"
        "rightstick:b9,guide:b10,touchpad:b11,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
        "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,";
    static const char kRaw[] =
        "a:b1,b:b2,x:b0,y:b3,leftshoulder:b4,rightshoulder:b5,back:b8,start:b9,leftstick:b10,"
        "rightstick:b11,guide:b12,touchpad:b13,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
        "leftx:a0,lefty:a1,rightx:a2,righty:a5,lefttrigger:a3,righttrigger:a4,";
    const SDL_GUID guid = SDL_GetJoystickGUIDForID(id);
    // SDL_sysjoystick.c (emscripten) adds 0x80 to the GUID's last byte for mapping "standard".
    const bool standard = (guid.data[15] & 0x80) != 0;
    char text[33] = "";
    SDL_GUIDToString(guid, text, sizeof text);
    char mapping[512];
    snprintf(mapping, sizeof mapping, "%s,%s,%s", text,
             product == 0x0ce6 || product == 0x0df2 ? "PS5 Controller" : "PS4 Controller",
             standard ? kStandard : kRaw);
    if (SDL_AddGamepadMapping(mapping) < 0)
        fprintf(stderr, "[controls] PlayStation layout not added: %s\n", SDL_GetError());
    return standard ? "standard" : "raw";
}
#endif

void open_pad(SDL_JoystickID id) {
    // HyperX Cloud headsets can expose media controls as a joystick. They
    // must not occupy the first-controller slot ahead of a real gamepad.
    const char *device_name = SDL_GetJoystickNameForID(id);
    if (device_name && SDL_strcasestr(device_name, "HyperX Cloud"))
        return;
#ifdef __EMSCRIPTEN__
    const char *layout = web_sony_mapping(id);
#endif
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    if (!pad) {
        fprintf(stderr, "[controls] could not open gamepad %u: %s\n", unsigned(id), SDL_GetError());
        return;
    }
    g_pad = pad;
    g_id = id;
    const char *name = SDL_GetGamepadName(pad);
    const SDL_JoystickConnectionState connection = SDL_GetGamepadConnectionState(pad);
    const char *transport = connection == SDL_JOYSTICK_CONNECTION_WIRELESS ? "wireless"
                            : connection == SDL_JOYSTICK_CONNECTION_WIRED  ? "wired"
                                                                           : "unknown";
    fprintf(stderr, "[controls] gamepad connected: %s (%s, instance %u)\n",
            name ? name : "(unnamed)", transport, unsigned(id));
#ifdef __EMSCRIPTEN__
    {
        // The browser's Gamepad API serves every pad here. The page's menu shows what the game
        // uses (web/menu.js's controller check): globalThis.recompGameController.
        const char *type = SDL_GetGamepadStringForType(SDL_GetGamepadType(pad));
        const char *how = *layout ? layout : "browser default";
        fprintf(stderr, "[controls] gamepad type %s, layout %s%s\n", type ? type : "unknown", how,
                *layout ? " (PlayStation mapping added)" : "");
        // (No commas outside parentheses in the JS: the macro would split its argument there.)
        MAIN_THREAD_EM_ASM(
            {
                var pad = {};
                pad['name'] = UTF8ToString($0);
                pad['deviceName'] = UTF8ToString($3);
                pad['type'] = UTF8ToString($1);
                pad['layout'] = UTF8ToString($2);
                globalThis['recompGameController'] = pad;
                globalThis.dispatchEvent(new Event('recomp-controller'));
            },
            name ? name : "", type ? type : "", how, device_name ? device_name : "");
    }
#else
    // Which SDL driver serves it decides how rumble reaches the motors (HIDAPI
    // writes reports itself; MFi goes through Apple's GameController haptics).
    // The GUID's driver byte says which (SDL_joystick.c: 'h' HIDAPI, 'm' MFi,
    // 'v' virtual; otherwise the platform's IOKit driver).
    {
        char guid[33] = "";
        const SDL_GUID g = SDL_GetGamepadGUIDForID(id);
        SDL_GUIDToString(g, guid, sizeof guid);
        const char *backend = g.data[14] == 'h'   ? "HIDAPI"
                              : g.data[14] == 'm' ? "MFi/GameController"
                              : g.data[14] == 'v' ? "virtual"
                                                  : "IOKit";
        const char *path = SDL_GetGamepadPath(pad);
        fprintf(stderr, "[controls] gamepad backend: %s (guid %s%s%s)\n", backend, guid,
                path ? ", path " : "", path ? path : "");
    }
#endif
    const SDL_PropertiesID properties = SDL_GetGamepadProperties(pad);
    const bool rumble =
        SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    fprintf(stderr, "[controls] gamepad rumble capability: %s; trigger rumble: %s\n",
            rumble ? "yes" : "no",
            SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false)
                ? "yes"
                : "no");
    // An explicitly requested diagnostic pulse separates physical motor output
    // from guest force-feedback delivery. Never synthesize gameplay feedback.
    // Run at most once per process, even if a transport reconnects repeatedly.
    static bool tested_rumble = false;
    const char *test_rumble = getenv("RECOMP_GAMEPAD_RUMBLE_TEST");
    if (!tested_rumble && test_rumble && test_rumble[0] == '1' && test_rumble[1] == '\0') {
        tested_rumble = true;
        const bool ok = SDL_RumbleGamepad(pad, 16384, 16384, 250);
        fprintf(stderr, "[controls] diagnostic 250ms rumble pulse: %s%s%s\n",
                ok ? "sent" : "failed", ok ? "" : ": ", ok ? "" : SDL_GetError());
    }
    read_all();
    publish();
    vpad().set_controller_connected(true);
}

// Opens the first attached pad, if any, when none is open. `skip` is an
// instance SDL may still list although it is going away (the one whose
// SDL_EVENT_GAMEPAD_REMOVED is being handled); 0 skips nothing.
void open_any(SDL_JoystickID skip) {
    if (g_pad)
        return;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!ids)
        return;
    for (int i = 0; i < count && !g_pad; ++i)
        if (ids[i] != skip)
            open_pad(ids[i]);
    SDL_free(ids);
}

void close_pad() {
    // Zero first, so a mapped binding or a buffered reader sees every release
    // before the pad is reported gone.
    for (bool &b : g_buttons)
        b = false;
    for (int16_t &a : g_axes)
        a = 0;
    publish();
    vpad().set_controller_connected(false);
    // Best effort: a pad closed while still attached (another pad takes over,
    // or shutdown) must not keep running a motor it was last told to run.
    if (SDL_GamepadConnected(g_pad))
        SDL_RumbleGamepad(g_pad, 0, 0, 0);
    SDL_CloseGamepad(g_pad);
    g_pad = nullptr;
    g_id = 0;
    fprintf(stderr, "[controls] gamepad disconnected\n");
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_EM_ASM({
        globalThis['recompGameController'] = null;
        globalThis.dispatchEvent(new Event('recomp-controller'));
    });
#endif
}

} // namespace

void gamepad_handle_event(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!g_pad)
            open_pad(e.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (g_pad && e.gdevice.which == g_id) {
            const SDL_JoystickID gone = g_id;
            close_pad();
            open_any(gone);
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (g_pad && e.gbutton.which == g_id && e.gbutton.button < kSdlPadButtonCount) {
            g_buttons[e.gbutton.button] = e.gbutton.down;
            if (!e.gbutton.down)
                g_masked[e.gbutton.button] = false;
            publish();
        }
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (g_pad && e.gaxis.which == g_id && e.gaxis.axis < kSdlAxisCount) {
            g_axes[e.gaxis.axis] = e.gaxis.value;
            publish();
        }
        break;
    default:
        break;
    }
}

void gamepad_poll() {
    // A transport change can detach an existing handle while another event
    // consumer drains its removal event. Do not keep polling a dead handle.
    if (g_pad && !SDL_GamepadConnected(g_pad))
        close_pad();
    // A pad whose SDL_EVENT_GAMEPAD_ADDED went somewhere else (the launcher
    // drains its own events) never reaches gamepad_handle_event, so while
    // nothing is open, look for one every kScanIntervalMs.
    if (!g_pad) {
        constexpr uint64_t kScanIntervalMs = 2000;
        static uint64_t next_scan_ms = 0;
        const uint64_t now_ms = SDL_GetTicks();
        if (now_ms >= next_scan_ms) {
            next_scan_ms = now_ms + kScanIntervalMs;
            open_any(0);
        }
    }
    if (!g_pad)
        return;
    read_all();
    publish();
}

void gamepad_mask_until_release(int button) {
    if (button >= 0 && button < kSdlPadButtonCount)
        g_masked[button] = true;
}

void gamepad_set_suppressed_query(bool (*suppressed)()) {
    g_suppressed = suppressed;
}

int gamepad_menu_key(const SDL_Event &e, bool *down) {
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        if (g_pad && e.gbutton.which != g_id)
            return 0;
        *down = e.gbutton.down;
        switch (e.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            return 0xc8;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            return 0xd0;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            return 0xcb;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            return 0xcd;
        case SDL_GAMEPAD_BUTTON_SOUTH:
            return 0x1c; // Enter
        case SDL_GAMEPAD_BUTTON_EAST:
            return 0x01; // Escape
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            return 0xc9; // Page Up: previous page
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            return 0xd1; // Page Down: next page
        default:
            return 0;
        }
    }
    if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
        (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
        if (g_pad && e.gaxis.which != g_id)
            return 0;
        // The left stick as a D-pad, with hysteresis so a stick resting near the edge does not chatter.
        static int held[2] = {0, 0}; // -1, 0, +1 per axis
        const int a = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? 0 : 1;
        const int v = e.gaxis.value;
        const int dir = v > 20000 ? 1 : v < -20000 ? -1 : (v > -12000 && v < 12000) ? 0 : held[a];
        if (dir == held[a])
            return 0;
        const int was = held[a];
        // Straight from one side to the other: release the old side first; the next motion
        // event presses the new one. Every press the page saw gets its release.
        held[a] = was && dir ? 0 : dir;
        static const int keys[2][2] = {{0xcb, 0xcd}, {0xc8, 0xd0}};
        if (!was) {
            *down = true;
            return keys[a][dir > 0];
        }
        *down = false;
        return keys[a][was > 0];
    }
    return 0;
}

bool gamepad_connected() {
    return g_pad != nullptr;
}

bool gamepad_rumble(uint16_t low, uint16_t high, uint32_t ms) {
    return g_pad && SDL_RumbleGamepad(g_pad, low, high, ms);
}

} // namespace controls
