// Bounded physical output diagnostic, using the same pinned SDL as the app.
// No game state, profile, system preferences or device firmware is changed.
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <functional>
#include "../controls/vpad.h"
#include "../controls/haptics.h"

static void pump_for(Uint32 ms) {
    const Uint64 end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        SDL_UpdateGamepads();
        SDL_Delay(10);
    }
}

// Observe the controller's own accelerometer while it rests untouched. This
// measures motion, not perceived strength, and external motion can confound it.
static void sample_motion(SDL_Gamepad *pad, const char *phase, Uint32 ms,
                          const std::function<void()> &tick = {}) {
    double mean[3] = {}, m2[3] = {};
    unsigned samples = 0;
    const Uint64 end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        if (tick)
            tick();
        SDL_PumpEvents();
        SDL_UpdateGamepads();
        float v[3];
        if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, v, 3)) {
            ++samples;
            for (int a = 0; a < 3; ++a) {
                double delta = v[a] - mean[a];
                mean[a] += delta / samples;
                m2[a] += delta * (v[a] - mean[a]);
            }
        }
        SDL_Delay(1);
    }
    printf("accel %s: samples=%u motion_rms=%.6f m/s^2 mean=(%.4f,%.4f,%.4f)\n", phase, samples,
           samples > 1 ? sqrt((m2[0] + m2[1] + m2[2]) / (samples - 1)) : 0, mean[0], mean[1],
           mean[2]);
}

int main(int argc, char **argv) {
    bool rumble = argc == 2 && !strcmp(argv[1], "--rumble");
    bool impact = argc == 2 && !strcmp(argv[1], "--impact");
    if (argc > 1 && !rumble && !impact) {
        fprintf(stderr, "Usage: gamepad_device_probe [--rumble|--impact]\n");
        return 2;
    }
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    printf("SDL %d, revision %s, audio %s\n", SDL_GetVersion(), SDL_GetRevision(),
           SDL_GetCurrentAudioDriver());
    pump_for(500);
    int count = 0;
    SDL_AudioDeviceID *outputs = SDL_GetAudioPlaybackDevices(&count);
    for (int i = 0; i < count; ++i) {
        SDL_AudioSpec spec = {};
        int frames = 0;
        if (SDL_GetAudioDeviceFormat(outputs[i], &spec, &frames))
            printf("audio: %s: %d Hz, %d channels, %d frames\n", SDL_GetAudioDeviceName(outputs[i]),
                   spec.freq, spec.channels, frames);
    }
    SDL_free(outputs);
    SDL_JoystickID *pads = SDL_GetGamepads(&count);
    printf("controllers: %d\n", count);
    bool failed = (rumble || impact) && count == 0;
    for (int i = 0; i < count; ++i) {
        SDL_Gamepad *pad = SDL_OpenGamepad(pads[i]);
        if (!pad) {
            failed = true;
            continue;
        }
        const char *path = SDL_GetGamepadPath(pad);
        const auto props = SDL_GetGamepadProperties(pad);
        bool supported = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        printf("pad: %s vendor=%04x product=%04x firmware=%04x transport=%d path=%s rumble=%d "
               "trigger_rumble=%d\n",
               SDL_GetGamepadName(pad), SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad),
               SDL_GetGamepadFirmwareVersion(pad), int(SDL_GetGamepadConnectionState(pad)),
               path ? path : "", supported,
               SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false));
        if (rumble || impact) {
            const bool accel = SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) &&
                               SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
            printf("accelerometer: %s, rate %.1f Hz\n", accel ? "enabled" : "unavailable",
                   accel ? SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_ACCEL) : 0);
            if (accel) {
                pump_for(200);
                sample_motion(pad, "baseline", 500);
            }
            if (impact) {
                // Replay the measured PC bump envelope through the same finite
                // pulse queue and router used by the app. This verifies output,
                // not that the game generated a particular collision event.
                controls::Vpad vpad;
                controls::RumbleRouter router(
                    {[&](uint16_t low, uint16_t high, uint32_t ms) {
                         bool ok = SDL_RumbleGamepad(pad, low, high, ms);
                         printf("impact output: %u/%u duration=%ums accepted=%d\n", low, high, ms,
                                ok);
                         failed |= !ok;
                     },
                     {}});
                const auto tick = [&] {
                    const auto now = SDL_GetTicksNS();
                    vpad.expire_rumble_pulses(now);
                    uint16_t low, high;
                    vpad.rumble(&low, &high);
                    router.update(vpad.rumble_serial(), low, high, controls::RumbleSink::Controller,
                                  now);
                };
                failed |= !vpad.rumble_pulse(1, 18748, 18748, 150, SDL_GetTicksNS());
                sample_motion(pad, "impact-150ms", 150, tick);
                sample_motion(pad, "automatic-stop", 600, tick);
                uint16_t low, high;
                vpad.rumble(&low, &high);
                failed |= low != 0 || high != 0;
                router.stop();
            }
            // Separate motors, 50% strength, half a second each. Stop between
            // commands and before closing; API success is not tactile proof.
            for (int motor = 0; rumble && motor < 2; ++motor) {
                bool ok = SDL_RumbleGamepad(pad, motor ? 0 : 32768, motor ? 32768 : 0, 500);
                printf("motor %d, 500ms 50%%: %s (%s)\n", motor, ok ? "accepted" : "FAILED",
                       ok ? "" : SDL_GetError());
                failed |= !ok;
                if (accel)
                    sample_motion(pad, motor ? "high-motor" : "low-motor", 500);
                else
                    pump_for(500);
                pump_for(100);
                bool stopped = SDL_RumbleGamepad(pad, 0, 0, 0);
                printf("stop: %s\n", stopped ? "accepted" : "FAILED");
                failed |= !stopped;
                pump_for(300);
            }
            if (accel) {
                sample_motion(pad, "stopped", 500);
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, false);
            }
        }
        if (rumble || impact)
            SDL_RumbleGamepad(pad, 0, 0, 0);
        SDL_CloseGamepad(pad);
    }
    SDL_free(pads);
    SDL_Quit();
    return failed ? 1 : 0;
}
