// gamepad_sdl.h - the first physical controller, through SDL_Gamepad: opened
// on hot-plug, read into the shared virtual pad as kPadSourceController, and
// rumbled on request. Called only from the SDL host's thread (main.cpp and
// controls_host.cpp). SDL's standard mapping names DualSense, Xbox and MFi
// buttons alike, so there are no per-controller tables here - except in the
// browser, where SDL's one default mapping leaves out a PlayStation pad's
// touchpad, or misplaces every button when the browser has no layout for it
// (gamepad_sdl.cpp web_sony_mapping).
// Design: docs/superpowers/specs/2026-09-17-touch-controls-design.md, 7.2.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

namespace controls {

// SDL_EVENT_GAMEPAD_ADDED opens the pad when none is open;
// SDL_EVENT_GAMEPAD_REMOVED of the open one zeroes its source (the game sees
// every release), marks it disconnected, closes it and opens any other pad
// still attached. Button and axis events of the open pad update the source
// at once, so a press and release inside one pump both reach the edge queue.
void gamepad_handle_event(const SDL_Event &e);
// Once per pump, after the events: re-reads the open pad's whole state into
// the source (a no-op when nothing changed).
void gamepad_poll();
bool gamepad_connected();
// Called on every publish: while it returns true the game is given a centred pad with nothing
// held (the settings page owns the pad). Null, the default, never suppresses.
void gamepad_set_suppressed_query(bool (*suppressed)());
// The game does not see this button until it is released (a press the host used, e.g. the page toggle).
void gamepad_mask_until_release(int button);
// The page's key for a pad event - D-pad and left stick as arrows, South as Enter, East as
// Escape, the shoulders as Page Up/Down - or 0. *down is the edge. A stick that moves straight
// from one side to the other reports the old side's release first.
int gamepad_menu_key(const SDL_Event &e, bool *down);
// SDL_RumbleGamepad on the open pad; false when there is none or SDL refused.
bool gamepad_rumble(uint16_t low, uint16_t high, uint32_t ms);

} // namespace controls
