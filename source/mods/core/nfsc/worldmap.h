/* worldmap.h - other players on the game's big world map (included by nfsc.c after mp.h).
 *
 * THE MAP'S OWN ICONS. The world map (WorldMapMain.fng; the WorldMap screen is
 * *0x00a977f0 while it exists, its package name at +0xc) shows events, shops,
 * safehouses, the player's arrow and cops as MapItems kept in a list at
 * WorldMap + 0x64 (bTList: next +0, prev +4; an item's node is at item + 4).
 * A MapItem never draws anything itself: it moves one of the image objects the
 * package already holds (MMICON_SPRINT_n, SAFEHOUSEINDICATOR_n, MMICON_COPCAR_n,
 * the player's arrow 0xdd9ef5ff...). Built by 0059dd90 (thiscall: type, image,
 * const float at[2], degrees, a5, a6; RET 0x18): +0 methods (plain: 009d13cc,
 * 7 of them: delete, SetPosition, SetScale, Update, Show, Hide, ResetSize),
 * +0xc the image, +0x10/+0x14 where it is on the unzoomed map, +0x18/+0x1c the
 * image's own size, +0x20 type (1 the player, 4 a cop, 8 a helicopter, an
 * event's filter bit otherwise), +0x24/+0x28 a6/a5 (an event's key). The
 * constructor shows the image only if the map's filter lets that type show
 * (0x4a0490 query 0x13b7), sets its centre and rotation.
 * - Where (AddPlayerCar 005acb50): a car's position in render space (z, -x of
 *   the physics position, as 005d89b0 takes it) goes through 0056e770 (cdecl:
 *   float *x, float *y, WorldMap + 0x44, WorldMap + 0x4c), which applies the
 *   track's map calibration (track record 0x110 bytes, 007990c0 by the number
 *   at 0x00a7a1a8; +0xac/+0xb0 origin, +0xb4 width) and then the TRACK_MAP
 *   image's top-left (+0x44) and size (+0x4c). Heading: 0046dfd0 (cdecl:
 *   -forward x, forward z) gives a binary angle, 65536 to the turn, which
 *   becomes degrees.
 * - Pan and zoom: once a frame while the map shows, the world map state's tick
 *   calls 005c9500 (thiscall, plain RET), which scales every visible item's
 *   +0x10/+0x14 about the view's centre, then calls the item's SetPosition,
 *   SetScale and Update. So an item only needs its unzoomed place kept fresh.
 * - The cop arrows: AddCops 005acc70 takes MMICON_COPCAR_0, 1, 2... in turn, one
 *   per pursuing car (Minimap_Icon_Car.tga, the same art as the player's arrow;
 *   CopMapItem's Update flashes it red and blue). The package has ten.
 * - Lifetime: 005b78d0 empties the list (hide, reset size, delete each), and
 *   the map refills it when the quick list takes or gives back focus
 *   (005cdbf0/005c3880) or the filter changes; the screen's destructor goes too.
 *   The map's setup (005cd650) adds the player's arrow only when it is opened
 *   from the game world (the pause menu's World Map, FEManager pause reason 5):
 *   not on the front end's maps (FEManager 005792a0 says 3 or 4: the career
 *   map from the menus, Quick Race's track choice), not while the quick list
 *   has focus.
 * - The cursor (WorldMap + 0x28) snaps onto items, ours too; Select on one of
 *   type 1 does nothing, as on the player's own arrow (tested).
 * There is no spare icon type, so a friend is a plain MapItem on a cop arrow
 * nobody uses (from MMICON_COPCAR_9 down, so the cops' own from 0 up are never
 * taken), type 1 like the player's own, coloured with its crew's colour
 * (FEObject::SetColor 005f35a0). Its methods are a private copy of the plain
 * ones (the way mp.h copies RBRemote's), which is how this file knows its own
 * items in the list again after the map has emptied and refilled it.
 *
 * WHEN. A BEFORE hook on 005c9500 walks the list each frame: while the
 * player's own arrow is there, each player the page lists (WM_FRIENDS at most,
 * the page's order: crew-mates first, then the nearest) gets an item, kept at
 * their latest place and heading: their car's newest pose when mp.h has them in
 * a slot (smooth), otherwise where the page last heard they were (the relay's
 * map feed, once a second, so players far beyond the 8 cars show too). Anyone
 * the page stops listing (gone, not driving, or no longer among the WM_FRIENDS),
 * or the player's arrow going away, removes theirs. So friends show wherever the
 * game shows the player: the free-roam world map, from near or far, and not the
 * menus' career map.
 * NAME. The package's debug text (0x39f35f28, "Event ID", which the map hides
 * when it opens) is the name label: after 005c9500, the friend whose arrow
 * the map cursor (WorldMap + 0x28) is on gets their name above it.
 *
 * THE PAGE. web/worldmap.js writes who is who into a small block of its own
 * (WmShared below, published as globalThis.carbonWorldMapBlock {memory, ptr}
 * with a "carbon-worldmap" event): each listed player's relay id, crew colour,
 * name and last known place and heading, under a sequence number (odd while
 * written, as in mp.h). The game counts the map's updates there and says how
 * many friends it shows. Version 2 ("WMP2", 2026-10-25): 10 players with places;
 * version 1 had 8 without (places only from mp.h's slots). The magic differs so
 * a page script of the other version leaves the block alone.
 *
 * TEST (RECOMP_WORLDMAP_TEST=1 in the address): the M key opens the world map
 * while driving (FEManager::HandlePauseRequest 005b02b0, reason 5), as the
 * career pause menu would; a Quick Race has no map of its own otherwise, and a
 * test profile with no career cannot reach free roam. Checked that way
 * (2026-10-24, three bots and a fourth driving on its own): arrows at the
 * right places near and 2 km away, the same rotation as the player's own
 * arrow, crew colours, names, live movement while paused, a player leaving,
 * the quick list, closing and reopening.
 */

#define WM_MAP 0x00a977f0u          /* WorldMap *, 0 while the map is closed */
#define WM_UPDATE 0x005c9500u       /* WorldMap: place every item for this frame's pan and zoom */
#define WM_TO_MAP 0x0056e770u       /* cdecl (float *x, float *y, const float origin[2], const float size[2]) */
#define WM_ITEM_INIT 0x0059dd90u    /* MapItem::MapItem, thiscall (type, image, at[2], degrees, a5, a6), RET 0x18 */
#define WM_ITEM_METHODS 0x009d13ccu /* the plain MapItem's 7 methods */
#define WM_ITEM_DELETE 0x00582b10u  /* its deleting destructor, thiscall (1), RET 4 */
#define WM_ANGLE 0x0046dfd0u        /* cdecl (float, float) -> AX: a binary angle */
#define WM_ROTATE 0x005705a0u       /* cdecl (FEObject *, float degrees) */
#define WM_RESIZE 0x00570a90u       /* cdecl (FEObject *, float w, float h) */
#define WM_COLOR 0x005f35a0u        /* FEObject::SetColor, thiscall (const int bgra[4], bool), RET 8 */
#define WM_PRINTF 0x005910e0u       /* cdecl (FEString *, fmt, ...) */
#define WM_MALLOC 0x006a1560u       /* the game's own: the map frees its items */
#define WM_FE_MANAGER 0x00a97a7cu
#define WM_PAUSE_REQUEST 0x005b02b0u /* thiscall (reason, a, b), RET 0xc; 5 opens the world map */
#define WM_PLAYER_ICON 0xdd9ef5ffu
#define WM_LABEL 0x39f35f28u
#define WM_ARROWS 10                /* MMICON_COPCAR_0..9 */
#define WM_FRIENDS WM_ARROWS        /* players the page lists, at most one arrow each */
#define WM_ITEM_SIZE 0x38u          /* a plain item is 0x30; +0x30 degrees, +0x34 colour set (ours) */
#define WM_TYPE_PLAYER 1u
#define WM_NEUTRAL 0xffdbe4eeu      /* no crew (the radar's light grey) */
#define WM_REACH 14.0f              /* map units: how near the cursor must be to an arrow for its name */

typedef struct WmFriend { /* written by the page */
    uint32_t id;    /* relay id, as in mp.h's slots; 0 = empty */
    uint32_t color; /* 0xAARRGGBB, the crew's colour; 0 = none */
    char name[24];  /* Latin-1, NUL-terminated */
    float x, z;     /* physics space, metres: where the page last heard they were; NaN = not known */
    float fx, fz;   /* the way their nose points (unit vector, physics x and z) */
} WmFriend;

typedef struct WmShared {
    uint32_t magic, version;
    uint32_t seq;     /* the page's part (friends), odd while it writes */
    uint32_t updates; /* the game: world map frames so far (it counts while the map is open) */
    WmFriend friends[WM_FRIENDS];
    uint32_t shown;   /* the game: friends on the map at its last frame */
    uint32_t hover;   /* the game: whose name the label shows, 0 none */
    uint32_t spare[2];
} WmShared;
#define WM_MAGIC 0x32504d57u /* "WMP2" */
/* The page reads these by offset (web/worldmap.js). */
_Static_assert(offsetof(WmShared, friends) == 0x10 && sizeof(WmFriend) == 0x30 && offsetof(WmFriend, x) == 0x20 &&
                   offsetof(WmShared, shown) == 0x1f0 && sizeof(WmShared) == 0x200,
               "WmShared layout");

static uint32_t g_wm_block, g_wm_buf, g_wm_methods, g_wm_fmt;
static uint32_t g_wm_hooks[4];
static WmFriend g_wm_people[WM_FRIENDS]; /* the page's part, last whole copy */
static struct { uint32_t id; MpPose pose; } g_wm_last[MP_SLOTS];
static uint32_t g_wm_label_for; /* whose name the label holds */
static uint32_t g_wm_now;       /* the WorldMap of this frame's update: an AFTER hook's ECX is not it */
static int g_wm_test, g_wm_open_asked;
static uint32_t (*g_wm_paint)(uint32_t id, uint32_t argb); /* pursuit.h: a police player's colour (red/blue like the game's cops) */

static WmShared *wm_shared(void) {
    void *p = NULL;
    return g_wm_block && g_api->guest_ptr(g_api, g_wm_block, sizeof(WmShared), &p) == POP_OK ? p : NULL;
}
static uint32_t wm_call(uint32_t addr, uint32_t ecx, const uint32_t *args, uint32_t n) {
    uint32_t eax = 0;
    return g_api->guest_call(g_api, addr, ecx, args, n, &eax) == POP_OK ? eax : 0;
}
static uint32_t wm_find(uint32_t pkg, uint32_t hash) {
    uint32_t args[2] = {pkg, hash};
    return pkg ? wm_call(FE_FIND_OBJECT, 0, args, 2) : 0;
}

/* The page's names and colours, if it is not writing them just now. */
static void wm_read_people(WmShared *sh) {
    uint32_t seq = __atomic_load_n(&sh->seq, __ATOMIC_ACQUIRE);
    if (seq & 1)
        return;
    WmFriend copy[WM_FRIENDS];
    memcpy(copy, sh->friends, sizeof copy);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&sh->seq, __ATOMIC_RELAXED) != seq)
        return;
    for (int i = 0; i < WM_FRIENDS; ++i)
        copy[i].name[sizeof copy[i].name - 1] = 0;
    memcpy(g_wm_people, copy, sizeof copy);
}
static const WmFriend *wm_person(uint32_t id) {
    for (int i = 0; i < WM_FRIENDS; ++i)
        if (g_wm_people[i].id == id)
            return &g_wm_people[i];
    return NULL;
}

/* Slot i's player and newest pose, if they are driving. A slot the page is
 * writing just now keeps the pose read last time. */
static uint32_t wm_slot(MpShared *mp, int i, MpPose *out) {
    MpSlot *s = &mp->slots[i];
    uint32_t seq = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    if (!(seq & 1)) {
        uint32_t id = s->id, mode = s->mode, count = s->count;
        MpPose pose;
        memset(&pose, 0, sizeof pose);
        if (count)
            pose = s->ring[(count - 1) % MP_RING].pose;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s->seq, __ATOMIC_RELAXED) == seq) {
            g_wm_last[i].id = 0;
            if (id && mode == 1 && count && mp_pose_ok(&pose)) {
                g_wm_last[i].id = id;
                g_wm_last[i].pose = pose;
            }
        }
    }
    *out = g_wm_last[i].pose;
    return g_wm_last[i].id;
}

/* A car's place (physics x, z) and the way its nose points (unit fx, fz). */
typedef struct WmWhere {
    float x, z, fx, fz;
} WmWhere;

/* A pose's place and heading: the nose is the body's +z axis turned by the rotation (x, y, z, w). */
static WmWhere wm_of_pose(const MpPose *p) {
    float qx = p->q[0], qy = p->q[1], qz = p->q[2], qw = p->q[3];
    WmWhere w = {p->p[0], p->p[2], 2.0f * (qx * qz + qw * qy), 1.0f - 2.0f * (qx * qx + qy * qy)};
    return w;
}

/* Where a physics-space place goes on the unzoomed map, and which way it points,
 * worked out by the game's own functions as AddPlayerCar does. */
static int wm_place(uint32_t map, const WmWhere *p, float *x, float *y, float *degrees) {
    float *buf = NULL;
    if (g_api->guest_ptr(g_api, g_wm_buf, 8, (void **)&buf) != POP_OK || !buf)
        return 0;
    buf[0] = p->z;
    buf[1] = -p->x;
    uint32_t to_map[4] = {g_wm_buf, g_wm_buf + 4, map + 0x44, map + 0x4c};
    if (g_api->guest_call(g_api, WM_TO_MAP, 0, to_map, 4, NULL) != POP_OK)
        return 0;
    *x = buf[0];
    *y = buf[1];
    uint32_t angle[2] = {float_bits(-p->fx), float_bits(p->fz)};
    *degrees = (float)(wm_call(WM_ANGLE, 0, angle, 2) & 0xffffu) * (360.0f / 65536.0f);
    return isfinite(*x) && isfinite(*y);
}

static void wm_turn(uint32_t item, uint32_t image, float degrees) {
    float was = bits_float(get_u32(item + 0x30));
    if (fabsf(was - degrees) < 0.5f)
        return;
    uint32_t args[2] = {image, float_bits(degrees)};
    wm_call(WM_ROTATE, 0, args, 2);
    g_api->guest_write_u32(g_api, item + 0x30, float_bits(degrees));
}

static void wm_paint(uint32_t item, uint32_t image, uint32_t argb) {
    if (get_u32(item + 0x34) == argb)
        return;
    int32_t *c = NULL;
    if (g_api->guest_ptr(g_api, g_wm_buf + 8, 16, (void **)&c) != POP_OK || !c)
        return;
    c[0] = (int32_t)(argb & 0xff); /* FEColor: blue, green, red, alpha */
    c[1] = (int32_t)(argb >> 8 & 0xff);
    c[2] = (int32_t)(argb >> 16 & 0xff);
    c[3] = (int32_t)(argb >> 24);
    uint32_t args[2] = {g_wm_buf + 8, 0};
    wm_call(WM_COLOR, image, args, 2);
    g_api->guest_write_u32(g_api, item + 0x34, argb);
}

static void wm_link(uint32_t map, uint32_t item) {
    uint32_t head = map + 0x64, tail = get_u32(map + 0x68), node = item + 4;
    g_api->guest_write_u32(g_api, tail, node);
    g_api->guest_write_u32(g_api, head + 4, node);
    g_api->guest_write_u32(g_api, node + 4, tail);
    g_api->guest_write_u32(g_api, node, head);
}

/* Takes one of ours off the map, as the map's own emptying does. */
static void wm_drop(uint32_t item) {
    uint32_t node = item + 4, next = get_u32(node), prev = get_u32(node + 4), image = get_u32(item + 0xc);
    g_api->guest_write_u32(g_api, prev, next);
    g_api->guest_write_u32(g_api, next + 4, prev);
    if (image) {
        fe_set_hidden(image, 1);
        uint32_t size[3] = {image, get_u32(item + 0x18), get_u32(item + 0x1c)};
        wm_call(WM_RESIZE, 0, size, 3);
    }
    uint32_t one = 1;
    wm_call(WM_ITEM_DELETE, item, &one, 1);
}

static uint32_t wm_new(uint32_t map, uint32_t image, uint32_t id, float x, float y, float degrees) {
    uint32_t size = WM_ITEM_SIZE, item = wm_call(WM_MALLOC, 0, &size, 1);
    float *buf = NULL;
    if (!item || g_api->guest_ptr(g_api, g_wm_buf, 8, (void **)&buf) != POP_OK || !buf)
        return 0;
    buf[0] = x;
    buf[1] = y;
    uint32_t args[6] = {WM_TYPE_PLAYER, image, g_wm_buf, float_bits(degrees), 0, id};
    wm_call(WM_ITEM_INIT, item, args, 6);
    g_api->guest_write_u32(g_api, item, g_wm_methods);
    g_api->guest_write_u32(g_api, item + 0x30, float_bits(degrees));
    g_api->guest_write_u32(g_api, item + 0x34, 0);
    wm_link(map, item);
    return item;
}

/* BEFORE 005c9500: the friends' items for this frame. */
static void wm_update(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    uint32_t map = cpu->ecx;
    MpShared *mp = mp_shared();
    WmShared *sh = wm_shared();
    g_wm_now = 0;
    if (!map || map != get_u32(WM_MAP) || !mp || !sh)
        return;
    g_wm_now = map;
    __atomic_store_n(&sh->updates, sh->updates + 1, __ATOMIC_RELEASE);
    wm_read_people(sh);
    uint32_t pkg = get_u32(map + 0xc), player = wm_find(pkg, WM_PLAYER_ICON);

    /* What is in the list: ours, and every image in use. */
    enum { MAX = 512 }; /* the career map holds a few hundred */
    uint32_t ours[MAX], ids[MAX], used[MAX];
    int n_ours = 0, n_used = 0, has_player = 0;
    uint32_t head = map + 0x64, node = get_u32(head);
    for (int guard = 0; node && node != head && guard < 512; node = get_u32(node), ++guard) {
        uint32_t item = node - 4, image = get_u32(item + 0xc);
        if (get_u32(item) == g_wm_methods) {
            if (n_ours < MAX) {
                ids[n_ours] = get_u32(item + 0x24);
                ours[n_ours++] = item;
            }
        } else if (image && image == player) {
            has_player = 1;
        }
        if (image && n_used < MAX)
            used[n_used++] = image;
    }

    /* Who should be there: the page's list, in its order (the first get the free arrows), each
     * at their car's newest pose when mp.h has them in a slot, else at the place the page heard. */
    uint32_t in_slot[MP_SLOTS];
    MpPose slot_pose[MP_SLOTS];
    for (int i = 0; i < MP_SLOTS; ++i)
        in_slot[i] = wm_slot(mp, i, &slot_pose[i]);
    uint32_t want[WM_FRIENDS] = {0};
    WmWhere where[WM_FRIENDS];
    for (int f = 0; f < WM_FRIENDS && has_player; ++f) {
        const WmFriend *who = &g_wm_people[f];
        if (!who->id)
            continue;
        int i = 0;
        while (i < MP_SLOTS && in_slot[i] != who->id)
            ++i;
        if (i < MP_SLOTS) {
            where[f] = wm_of_pose(&slot_pose[i]);
        } else if (isfinite(who->x) && isfinite(who->z) && fabsf(who->x) < 1e6f && fabsf(who->z) < 1e6f) {
            WmWhere w = {who->x, who->z, isfinite(who->fx) ? who->fx : 0.0f, isfinite(who->fz) ? who->fz : 1.0f};
            where[f] = w;
        } else {
            continue;
        }
        want[f] = who->id;
    }

    /* Ours that should go. */
    for (int k = 0; k < n_ours; ++k) {
        int keep = 0;
        for (int i = 0; i < WM_FRIENDS && !keep; ++i)
            keep = want[i] && want[i] == ids[k];
        for (int j = 0; j < k && keep; ++j)
            keep = ids[j] != ids[k]; /* one item a player */
        if (!keep) {
            wm_drop(ours[k]);
            ours[k] = 0;
        }
    }

    int shown = 0;
    for (int i = 0; i < WM_FRIENDS; ++i) {
        if (!want[i])
            continue;
        float x, y, degrees;
        if (!wm_place(map, &where[i], &x, &y, &degrees))
            continue;
        const WmFriend *who = wm_person(want[i]);
        uint32_t argb = who && who->color ? who->color | 0xff000000u : WM_NEUTRAL;
        uint32_t item = 0;
        for (int k = 0; k < n_ours && !item; ++k)
            if (ours[k] && ids[k] == want[i])
                item = ours[k];
        if (!item) {
            /* A free arrow, from the last one down: the cops take theirs from 0 up. */
            uint32_t image = 0;
            for (int a = WM_ARROWS - 1; a >= 0 && !image; --a) {
                char name[24];
                snprintf(name, sizeof name, "MMICON_COPCAR_%d", a);
                uint32_t candidate = wm_find(pkg, fe_hash(name)), taken = 0;
                for (int u = 0; u < n_used && !taken; ++u)
                    taken = used[u] == candidate;
                if (candidate && !taken)
                    image = candidate;
            }
            if (!image)
                continue; /* every arrow is in use: the cops keep theirs */
            item = wm_new(map, image, want[i], x, y, degrees);
            if (!item)
                continue;
            if (n_used < MAX)
                used[n_used++] = image;
            char line[96];
            snprintf(line, sizeof line, "core.nfsc: world map: player %u shown (%.0f, %.0f)", want[i], x, y);
            log_line(line);
        }
        uint32_t image = get_u32(item + 0xc);
        g_api->guest_write_u32(g_api, item + 0x10, float_bits(x));
        g_api->guest_write_u32(g_api, item + 0x14, float_bits(y));
        wm_turn(item, image, degrees);
        if (g_wm_paint) argb = g_wm_paint(want[i], argb);
        wm_paint(item, image, argb);
        ++shown;
    }
    __atomic_store_n(&sh->shown, (uint32_t)shown, __ATOMIC_RELEASE);
}

/* AFTER 005c9500: the name of the friend under the map's cursor, above their arrow.
 * The cursor (a group in the icons group, like the arrows, so GetCenter gives
 * both in the same space) still picks what is under it while the game hides
 * it, as it does when the map is moved with the keys: so does the label. */
static void wm_label(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    uint32_t map = g_wm_now;
    WmShared *sh = wm_shared();
    g_wm_now = 0;
    if (!map || map != get_u32(WM_MAP) || !sh)
        return;
    uint32_t pkg = get_u32(map + 0xc), label = wm_find(pkg, WM_LABEL), cursor = get_u32(map + 0x28);
    float *buf = NULL;
    if (!label || g_api->guest_ptr(g_api, g_wm_buf, 0x40, (void **)&buf) != POP_OK || !buf)
        return;
    uint32_t best = 0, best_id = 0;
    float best_d = WM_REACH * WM_REACH, bx = 0, by = 0;
    if (cursor) {
        uint32_t at[3] = {cursor, g_wm_buf + 0x18, g_wm_buf + 0x1c};
        wm_call(FE_GET_CENTER, 0, at, 3);
        float cx = buf[6], cy = buf[7];
        uint32_t head = map + 0x64, node = get_u32(head);
        for (int guard = 0; node && node != head && guard < 512; node = get_u32(node), ++guard) {
            uint32_t item = node - 4, image = get_u32(item + 0xc);
            if (get_u32(item) != g_wm_methods || !image || (get_u32(image + 0x1c) & 1u))
                continue;
            uint32_t where[3] = {image, g_wm_buf + 0x20, g_wm_buf + 0x24};
            wm_call(FE_GET_CENTER, 0, where, 3);
            float dx = buf[8] - cx, dy = buf[9] - cy, d = dx * dx + dy * dy;
            if (d < best_d) {
                best_d = d;
                best = item;
                best_id = get_u32(item + 0x24);
                /* Above the arrow as drawn now: its size follows the zoom
                 * (FEObject data + 0x3c, as SetScale leaves it). */
                uint32_t data = get_u32(image + 0x2c);
                float h = data ? bits_float(get_u32(data + 0x3c)) : 20.0f;
                bx = buf[8];
                by = buf[9] - (isfinite(h) && h > 0.0f && h < 200.0f ? h : 20.0f) * 0.5f - 12.0f;
            }
        }
    }
    const WmFriend *who = best ? wm_person(best_id) : NULL;
    if (!who || !who->name[0]) {
        if (g_wm_label_for)
            fe_set_hidden(label, 1);
        g_wm_label_for = 0;
        __atomic_store_n(&sh->hover, 0, __ATOMIC_RELEASE);
        return;
    }
    /* A map that has just opened hid the label: its text is the game's again. */
    if (g_wm_label_for != best_id || (get_u32(label + 0x1c) & 1u)) {
        char *text = NULL;
        if (g_api->guest_ptr(g_api, g_wm_buf + 0x28, sizeof who->name, (void **)&text) != POP_OK || !text)
            return;
        memcpy(text, who->name, sizeof who->name);
        uint32_t print[3] = {label, g_wm_fmt, g_wm_buf + 0x28};
        wm_call(WM_PRINTF, 0, print, 3);
        g_wm_label_for = best_id;
    }
    uint32_t place[3] = {label, float_bits(bx), float_bits(by)};
    wm_call(FE_SET_CENTER, 0, place, 3);
    fe_set_hidden(label, 0);
    __atomic_store_n(&sh->hover, best_id, __ATOMIC_RELEASE);
}

/* Test only: M opens the world map while driving. */
static int32_t wm_key(const PopModApi *api, int32_t dik, int32_t vk, int32_t down, void *user) {
    (void)api; (void)vk; (void)user;
    if (down && dik == 0x32) /* DIK_M */
        g_wm_open_asked = 1;
    return 0;
}
static void wm_service(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    if (!g_wm_open_asked)
        return;
    g_wm_open_asked = 0;
    MpShared *mp = mp_shared();
    uint32_t manager = get_u32(WM_FE_MANAGER);
    if (!manager || get_u32(WM_MAP) || !mp || mp->state != 1)
        return;
    uint32_t args[3] = {5, 0, 0};
    wm_call(WM_PAUSE_REQUEST, manager, args, 3);
    log_line("core.nfsc: world map test: opened");
}

static void worldmap_install(void) {
    if (!g_mp_block || g_api->guest_alloc(g_api, 0x80, &g_wm_buf) != POP_OK ||
        g_api->guest_alloc(g_api, sizeof(WmShared), &g_wm_block) != POP_OK ||
        g_api->guest_alloc(g_api, 7 * 4, &g_wm_methods) != POP_OK || g_api->guest_alloc(g_api, 4, &g_wm_fmt) != POP_OK) {
        log_line("core.nfsc: friends on the world map unavailable (no guest memory)");
        return;
    }
    WmShared *sh = wm_shared();
    char *fmt = NULL;
    if (!sh || g_api->guest_ptr(g_api, g_wm_fmt, 3, (void **)&fmt) != POP_OK || !fmt) {
        g_wm_block = 0;
        return;
    }
    memcpy(fmt, "%s", 3);
    for (uint32_t i = 0; i < 7; ++i)
        g_api->guest_write_u32(g_api, g_wm_methods + i * 4, get_u32(WM_ITEM_METHODS + i * 4));
    memset(sh, 0, sizeof *sh);
    sh->magic = WM_MAGIC;
    sh->version = 2;
    if (g_api->hook_install(g_api, WM_UPDATE, wm_update, POP_HOOK_BEFORE, NULL, &g_wm_hooks[0]) != POP_OK) {
        log_line("core.nfsc: friends on the world map unavailable");
        return;
    }
    if (g_api->hook_install(g_api, WM_UPDATE, wm_label, POP_HOOK_AFTER, NULL, &g_wm_hooks[1]) != POP_OK)
        log_line("core.nfsc: world map name label unavailable");
    const char *test = getenv("RECOMP_WORLDMAP_TEST");
    g_wm_test = test && *test ? atoi(test) : 0;
    if (g_wm_test) {
        uint32_t id;
        g_api->on_key(g_api, wm_key, NULL, &id);
        g_api->hook_install(g_api, 0x00600c90u, wm_service, POP_HOOK_BEFORE, NULL, &g_wm_hooks[2]);
        log_line("core.nfsc: world map test: M opens the world map");
    }
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({
        var block = {};
        block.memory = wasmMemory;
        block.ptr = $0;
        globalThis.carbonWorldMapBlock = block;
        globalThis.dispatchEvent(new Event("carbon-worldmap"));
    }, (uint32_t)(uintptr_t)sh);
#endif
}
