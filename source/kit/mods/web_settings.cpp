// web_settings.cpp - the settings, for a menu the web page draws itself.
//
// On the web the game's in-game menu is HTML (a game's web/menu.js). This file
// lets that page show and change the same settings the F10 page does, stored
// the same way (mod-settings.json, through mods_settings_set / mods_display_set)
// and applied the same way: a "live" setting changes at once because its owner
// reads the value every frame, a "restart" one is saved for the next start.
//
// THE BLOCK. A small struct in the game's memory, which the page reads and
// writes through the shared wasm memory (as a game's own mods hand theirs to the
// page). Its address goes to the page as globalThis.recompSettingsBlock
// ({memory, ptr}) with a "recomp-settings" event. 32-bit little-endian fields:
//   +0  magic 0x31425352 ("RSB1")   +4  version 1
//   +8  generation: odd while the list below is being rewritten, then even again
//   +12 address of the list, UTF-8 JSON   +16 its length in bytes
//   +20 requests the page has written (it adds 1 after writing one; Atomics)
//   +24 requests the game has handled (the list is new when this moves)
//   +28 status of the last request: 0 done, else the PopModStatus it failed with
//   +64 8 request slots of 192 bytes, slot = request number % 8, each a
//       NUL-terminated "<mod id>/<key>=<value>" (host.display/<key> for the
//       display rows) or "" to only rewrite the list. The page writes a slot only
//       while fewer than 8 of its requests are unhandled.
// The list: {"settings":[{id, label, group, kind "bool"|"int", value, min, max,
// step, def, choices: [...], apply, help}, ...]} with the display rows first
// (group "display"), then every mod setting in the order the mods declared them.
//
// THREADS. Requests are handled on the thread that booted the game (display
// rows refuse any other, and the profile's files are written there, never on
// the browser's main thread), at the scheduler checkpoint that also runs the
// F10 page's deferred changes: one atomic load when the page asked nothing.
#include "settings_menu.h"
#include "display_settings.h"
#include "game_config.h"
#include "mods_internal.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#ifndef RECOMP_SETTINGS_FILTERING_KEY
#define RECOMP_SETTINGS_FILTERING_KEY "texture_filtering"
#endif

namespace {

constexpr int kSlots = 8, kSlotBytes = 192;
struct Block {
    uint32_t magic, version;
    int32_t generation;
    uint32_t json, json_len;
    int32_t asked, served, status;
    int32_t spare[8];
    char slot[kSlots][kSlotBytes];
};
static_assert(sizeof(Block) == 64 + kSlots * kSlotBytes, "the page reads these offsets");

Block g_block{0x31425352u, 1u, 0, 0, 0, 0, 0, 0, {}, {}};
std::string g_lists[2]; // the list the page may be reading stays while the next is written
int g_list = 0;
int32_t g_served = 0;
bool g_shown = false;

// The display rows, in DisplayRow order (display_settings.cpp's keys).
const char *const kDisplayKeys[DISPLAY_ROW_COUNT] = {
    "rendering", "ui_scale", "wide_view", "window", "classic_mode",
    "frame_limit", "performance_overlay", "hd_textures", RECOMP_SETTINGS_FILTERING_KEY};

void add_string(std::string &out, const char *s) {
    out += '"';
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
        if (*p == '"' || *p == '\\') {
            out += '\\';
            out += char(*p);
        } else if (*p < 0x20) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", *p);
            out += esc;
        } else
            out += char(*p);
    }
    out += '"';
}

// "a|b|c" -> ["a","b","c"]; "" -> [].
void add_choices(std::string &out, const char *choices) {
    out += '[';
    if (choices && *choices) {
        const char *p = choices;
        for (bool first = true;; first = false) {
            const char *end = strchr(p, '|');
            const std::string one = end ? std::string(p, end) : std::string(p);
            if (!first)
                out += ',';
            add_string(out, one.c_str());
            if (!end)
                break;
            p = end + 1;
        }
    }
    out += ']';
}

void add_row(std::string &out, bool &first, const char *mod_id, const char *key, const char *label,
             const char *group, bool is_bool, int64_t value, int64_t mn, int64_t mx, int64_t step, int64_t def,
             const char *choices, const char *apply, const char *help) {
    out += first ? "\n" : ",\n";
    first = false;
    out += "{\"id\":";
    add_string(out, (std::string(mod_id) + "/" + key).c_str());
    out += ",\"label\":";
    add_string(out, label);
    out += ",\"group\":";
    add_string(out, group);
    out += is_bool ? ",\"kind\":\"bool\"" : ",\"kind\":\"int\"";
    char nums[160];
    snprintf(nums, sizeof nums, ",\"value\":%lld,\"min\":%lld,\"max\":%lld,\"step\":%lld,\"def\":%lld",
             (long long)value, (long long)mn, (long long)mx, (long long)step, (long long)def);
    out += nums;
    out += ",\"choices\":";
    add_choices(out, choices);
    out += ",\"apply\":";
    add_string(out, apply);
    out += ",\"help\":";
    add_string(out, help);
    out += '}';
}

// The display rows the F10 page shows, with its words (settings_menu.cpp line_for).
void add_display_rows(std::string &out, bool &first) {
    struct Row {
        DisplayRow row;
        const char *label, *choices, *help;
    };
    static const Row rows[] = {
        {DISPLAY_WINDOW, "Window", "Windowed|Borderless|Full screen",
         "Window, borderless full screen or full screen."},
        {DISPLAY_FPS, "Frame limit", "Original|40 FPS|60 FPS|120 FPS (experimental)",
         "The presented frame limit (60 is the default). The game itself keeps running at 60 Hz. 120 FPS is "
         "experimental: the original game was authored around 60 Hz, and above 60 FPS some cinematics, vehicle "
         "poses or animations may render incorrectly."},
        {DISPLAY_OVERLAY, "Performance overlay", "Off|Counters|Graph",
         "Frame counters or a frame-time graph in a corner (F11 cycles it too)."},
        {DISPLAY_FILTERING, "World filtering", "Game's own|Trilinear|4x anisotropic|8x anisotropic|16x anisotropic",
         "Sharper road, wall and terrain textures at an angle. \"Game's own\" keeps the filtering the game asks "
         "for; 4x-16x apply that anisotropy to every world texture, Trilinear turns it off. Text, the HUD, "
         "movies, shadows and lighting are not affected."},
    };
    for (const Row &r : rows) {
        if (!mods_display_row_applies(r.row))
            continue;
        const int count = 1 + int(std::count(r.choices, r.choices + strlen(r.choices), '|'));
        // The default is the one the row was declared with (display_settings.cpp mods_display_init).
        int64_t def = 0;
        for (uint32_t i = 0; i < mods_settings_entry_count(); ++i) {
            uint32_t owner = 1;
            const char *m = nullptr, *k = nullptr;
            if (mods_settings_entry(i, &owner, &m, &k, nullptr, nullptr, nullptr, nullptr, nullptr) &&
                owner == MODS_OWNER_RUNTIME && m && k && !strcmp(m, "host.display") && !strcmp(k, kDisplayKeys[r.row])) {
                mods_settings_default(i, &def);
                break;
            }
        }
        add_row(out, first, "host.display", kDisplayKeys[r.row], r.label, "display", false, mods_display_value(r.row),
                0, count - 1, 1, std::clamp<int64_t>(def, 0, count - 1), r.choices, "live", r.help);
    }
}

void write_list() {
    std::string &out = g_lists[g_list ^= 1];
    out = "{\"settings\":[";
    bool first = true;
    add_display_rows(out, first);
    for (uint32_t i = 0; i < mods_settings_entry_count(); ++i) {
        uint32_t owner = 0;
        const char *mod_id = nullptr, *key = nullptr, *label = nullptr;
        int32_t kind = 0;
        int64_t value = 0, mn = 0, mx = 0, step = 1, def = 0;
        const char *group = "", *help = "", *apply = "", *choices = "", *unit = "";
        if (!mods_settings_entry(i, &owner, &mod_id, &key, &label, &kind, &value, &mn, &mx) ||
            owner == MODS_OWNER_RUNTIME || !mod_id || !key)
            continue;
        mods_settings_presentation(i, &step, &unit);
        mods_settings_meta(i, &group, &help, &apply, &choices);
        mods_settings_default(i, &def);
        add_row(out, first, mod_id, key, label, group, kind == POP_SETTING_BOOL, value, mn, mx, step < 1 ? 1 : step,
                def, choices, apply, help);
    }
    out += "\n]}\n";
    __atomic_add_fetch(&g_block.generation, 1, __ATOMIC_SEQ_CST); // odd: being changed
    g_block.json = uint32_t(uintptr_t(out.data()));
    g_block.json_len = uint32_t(out.size());
    __atomic_add_fetch(&g_block.generation, 1, __ATOMIC_SEQ_CST); // even: ready
}

// One request: "<mod id>/<key>=<value>". Same checks as the F10 page.
PopModStatus handle(const char *text) {
    if (!*text)
        return POP_OK; // the page asked for the list only
    const char *eq = strrchr(text, '='), *slash = strchr(text, '/');
    if (!eq || !slash || slash > eq)
        return POP_E_INVAL;
    const std::string mod_id(text, slash), key(slash + 1, eq);
    char *end = nullptr;
    const long long want = strtoll(eq + 1, &end, 10);
    if (end == eq + 1 || *end)
        return POP_E_INVAL;
    if (mod_id == "host.display") {
        for (int r = 0; r < DISPLAY_ROW_COUNT; ++r)
            if (key == kDisplayKeys[r] && mods_display_row_applies(DisplayRow(r)))
                return mods_display_set(DisplayRow(r), int(want));
        return POP_E_NOTFOUND;
    }
    for (uint32_t i = 0; i < mods_settings_entry_count(); ++i) {
        uint32_t owner = 0;
        const char *m = nullptr, *k = nullptr;
        int64_t mn = 0, mx = 0;
        if (!mods_settings_entry(i, &owner, &m, &k, nullptr, nullptr, nullptr, &mn, &mx) ||
            owner == MODS_OWNER_RUNTIME || !m || !k || mod_id != m || key != k)
            continue;
        return mods_settings_set(owner, k, std::clamp<int64_t>(want, mn, mx));
    }
    return POP_E_NOTFOUND;
}

} // namespace

void mods_web_settings_poll() {
    if (!g_shown) {
        if (!mods_host_on_main_thread())
            return;
        g_shown = true;
        write_list();
        // No commas in the JS below: the macro would split its argument there (as mp.h writes it).
        MAIN_THREAD_ASYNC_EM_ASM({
            var block = {};
            block.memory = wasmMemory;
            block.ptr = $0;
            globalThis.recompSettingsBlock = block;
            globalThis.dispatchEvent(new Event("recomp-settings"));
        }, (uint32_t)(uintptr_t)&g_block);
        return;
    }
    if (__atomic_load_n(&g_block.asked, __ATOMIC_ACQUIRE) == g_served || !mods_host_on_main_thread())
        return;
    const int32_t asked = __atomic_load_n(&g_block.asked, __ATOMIC_ACQUIRE);
    int32_t status = 0;
    for (; g_served != asked; ++g_served) {
        char text[kSlotBytes];
        memcpy(text, g_block.slot[uint32_t(g_served) % kSlots], kSlotBytes);
        text[kSlotBytes - 1] = 0;
        const PopModStatus st = handle(text);
        if (st != POP_OK) {
            status = int32_t(st);
            printf("mods: the page's menu could not set \"%s\" (status %d)\n", text, int(st));
        } else if (*text) {
            printf("mods: the page's menu set %s\n", text);
        }
    }
    fflush(stdout);
    write_list();
    __atomic_store_n(&g_block.status, status, __ATOMIC_RELEASE);
    __atomic_store_n(&g_block.served, g_served, __ATOMIC_RELEASE);
}

// F10, or the controller's settings chord, while the page draws the menu: the page's menu.
void mods_web_settings_menu_key() {
    MAIN_THREAD_ASYNC_EM_ASM({ globalThis.dispatchEvent(new Event("recomp-menu")); });
}

#else

void mods_web_settings_poll() {}
void mods_web_settings_menu_key() {}

#endif
