/* intro.h - skip the start-up movies (included by nfsc.c). Setting skip_intro.
 *
 * The game has its own "skip movies" switch (dword 0x00a9e6d8, the one Extra
 * Options sets): an FE movie object (type 7) that comes up while it is set
 * reports "finished" (0xc3960eb9) instead of playing (005baec2). It skips every
 * movie, so it is only set while the movie the front end has queued (its name
 * at 0x00a97a88, written by 00591780/005916e0 before the movie screen opens)
 * is the EA logo or the street-racing warning; story movies still play. This
 * also takes the start-up EA_Bumper movie out of the way, which on some first
 * frames never started and left the game on a silent black screen
 * (FeBlackScreen.fng + FEAnyMovie.fng, 2026-10-10).
 */
#include <strings.h>
#define SKIP_MOVIES_FLAG 0x00a9e6d8u
#define QUEUED_MOVIE 0x00a97a88u /* char[64] */

static int g_skip_intro = 0;
/* The browser build has no movie decoder (RECOMP_VIDEO is off), so a movie
 * never finishes there and the game waits on its first frame for good: the
 * EA logo at start-up, the story movies in a career. Skip them all. */
#ifdef __EMSCRIPTEN__
static const int g_no_movies = 1;
#else
static const int g_no_movies = 0;
#endif

static int intro_movie(const char *name) {
    static const char *const intro[] = {"EA_Bumper", "EA_Bumper_WS", "PSA_Nikki", "EAHD_Bumper"};
    for (unsigned i = 0; i < sizeof intro / sizeof intro[0]; ++i)
        if (!strcasecmp(name, intro[i]))
            return 1;
    return 0;
}

/* Before cFEng::Service (00600c90): set the switch for the queued movie. */
static void intro_service(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    char name[64] = {0};
    void *p = NULL;
    if (api->guest_ptr(api, QUEUED_MOVIE, sizeof name - 1, &p) == POP_OK && p)
        memcpy(name, p, sizeof name - 1);
    const uint32_t want = g_no_movies || intro_movie(name) ? 1u : 0u;
    if (get_u32(SKIP_MOVIES_FLAG) != want) {
        api->guest_write_u32(api, SKIP_MOVIES_FLAG, want);
        if (want) {
            char line[96];
            snprintf(line, sizeof line, "core.nfsc: skipping the intro movie %s", name);
            log_line(line);
        }
    }
}

static void intro_install(void) {
    g_skip_intro = setting("skip_intro", 0) != 0;
    if ((g_skip_intro || g_no_movies) && install(0x00600c90u, intro_service, POP_HOOK_BEFORE, 17) != POP_OK)
        log_line("core.nfsc: intro skipping unavailable");
}
