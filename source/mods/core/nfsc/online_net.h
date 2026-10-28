/* online_net.h - the game's own Online mode on our virtual LAN (included by nfsc.c).
 *
 * Only when the page runs the virtual LAN (RECOMP_LAN set, by ?lan= or the start options):
 * otherwise nothing here is installed and the game is unchanged.
 *
 * SSL OFF FOR FESL. EA's FESL SDK 2.9 decides once, when its service hub is created, whether
 * the FESL link (nfs-pc.fesl.ea.com:18210) uses SSL: ConnMgr's set-up (008e0550) copies the
 * TitleParameters and stores  secure = ~options & 1  (008e05d5-008e05e4, byte ConnMgr+0x4ac),
 * and the FESL connect (008df13c) passes that byte to ProtoAries, which hands it to
 * ProtoSSLConnect as iSecure (00977f03-00977f13). Theater and Messenger always connect plain
 * (008df26b, 008df2db push 0). Bit 0 of the options is the SDK's own TitleParameters::
 * SSL_OVERRIDE_OFF (its name is printed by 0093e414). Carbon builds the TitleParameters on
 * the stack at 00834570 with options 0x500 (communicator + game manager) and passes them to
 * the hub's create, 008e0f00 (cdecl: TitleParameters *, 0). A hook before that create sets
 * bit 0, so the SDK itself makes FESL plain TCP and our server needs no SSL 3.0 / RC4.
 *   The write is checked first: the TitleParameters must hold FESL port 18210, options
 * exactly 0x500 and client string "nfs-pc" (as 00834570 writes them); anything else is
 * logged and left alone. Code bytes are never patched (they mean nothing to the recompiled
 * game) and the exe on disk is never touched.
 *   The other way, keeping SSL and skipping only the certificate check, is Luigi Auriemma's
 * finding (fesl.lpatch: ProtoSSL's state forced to 0x15, at 00980537 in this exe); it would
 * still need an SSL 3.0 server, so the SDK's own switch is used instead.
 *
 * THE SDK'S LOG. The SDK formats its debug lines (connection states, every transaction it
 * sends and receives, "->N res fsys ..." etc.) and hands each finished line to
 * DebugImpl::Print (008f3af0, thiscall, the text; RET 4), which passes it to a callback the
 * game set to an empty function. A hook before Print copies the line into our log, so a
 * LAN session shows what the game's online code thinks. The SDK prints Login's password in
 * clear text, so a line is cut where a password (or CD key) value starts: it never reaches
 * the log.
 *
 * AUTOMATIC SIGN-IN. The game's own SIGN IN screen talks about creating an EA account, which
 * means nothing on our server (any name signs in, the password is ignored). The game already
 * signs in without that screen when the alias has "Automatically Sign In" on: its online
 * options object, [[00a8acfc]+8] (vtable 009c9380, the handler of option ids 15000-15025),
 * keeps the account name at +0x10 (char[32], option 15017), the password at +0x30 (char[32],
 * option 15018) and the switch at +0x6b (option 15000, read by query 004b7550); the sign-in
 * flow asks option 15000 before it opens FEPCOnlineLogin.fng (00716ef0) and, when it is on,
 * logs in with the stored name and password. So in LAN mode, once a menu frame (before
 * cFEng::Service 00600c90), those three are set: the player's name (RECOMP_LAN_NAME, which
 * the kit takes from the page's lan.js; letters, digits, '_', '-' and '.', at most 16, as the
 * server keeps them), a fixed dummy password, and the switch on. The object is checked by
 * its vtable first. An alias saved meanwhile keeps the name and the switch; outside LAN mode
 * nothing reads them (the kit's Winsock never connects, and the online menus are hidden).
 */
#include <stdlib.h>

#define NET_HUB_CREATE 0x008e0f00u   /* cdecl (TitleParameters *, int) */
#define NET_SDK_PRINT 0x008f3af0u    /* DebugImpl::Print, thiscall (const char *) */
#define NET_TP_FESL_PORT 0x04u       /* TitleParameters fields, as 008e1090 fills them */
#define NET_TP_OPTIONS 0x10u
#define NET_TP_CLIENT 0x218u         /* char[32] */
#define NET_EXPECT_OPTIONS 0x500u    /* USE_COMMUNICATOR_API | USE_GAMEMANAGER_API */
#define NET_SSL_OVERRIDE_OFF 0x1u

/* Handles of their own rather than slots in nfsc.c's g_hooks, which the other headers share;
 * the hooks stay for the whole run (pop_mod_exit only comes when the process ends). */
static uint32_t g_net_hooks[3];

static void net_ssl_off(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t tp = get_u32(cpu->esp + 4u);
    char client[16] = {0};
    void *p = NULL;
    if (tp && api->guest_ptr(api, tp + NET_TP_CLIENT, sizeof client - 1, &p) == POP_OK && p)
        memcpy(client, p, sizeof client - 1);
    const uint32_t port = tp ? get_u32(tp + NET_TP_FESL_PORT) : 0;
    const uint32_t options = tp ? get_u32(tp + NET_TP_OPTIONS) : 0;
    char line[192];
    if (!tp || port != 18210u || options != NET_EXPECT_OPTIONS || strcmp(client, "nfs-pc") != 0) {
        snprintf(line, sizeof line,
                 "core.nfsc: online: TitleParameters at %08x are not the expected ones (FESL port %u, options "
                 "%#x, client \"%s\"); SSL left on",
                 tp, port, options, client);
        log_line(line);
        return;
    }
    api->guest_write_u32(api, tp + NET_TP_OPTIONS, options | NET_SSL_OVERRIDE_OFF);
    snprintf(line, sizeof line,
             "core.nfsc: online: FESL without SSL (TitleParameters options %#x -> %#x, the SDK's SSL_OVERRIDE_OFF)",
             options, get_u32(tp + NET_TP_OPTIONS));
    log_line(line);
}

/* The SDK prints a transaction a piece at a time ("->N ", "res ", "fsys ", "{", "TXN=Hello",
 * ", sku=America", ... "}\n"), so the pieces are joined here and logged a line at a time. Very
 * long lines (base64 telemetry) are cut. Guest threads take turns (the kit's baton), so one
 * buffer is enough. */
#define NET_LINE_HEAD "fesl-sdk: "
static char g_net_line[704] = NET_LINE_HEAD;
static size_t g_net_len = sizeof NET_LINE_HEAD - 1;
static int g_net_cut;

/* "->N res acct 0x80000002 {TXN=Login, ..., name=x, password=<it>, machineId=, ...}": the line
 * ends where a secret's value starts (a value may itself hold ", " or "}", so nothing after
 * it is kept). The key must start a field, so returnEncryptedInfo= is not one. */
static void net_mask(void) {
    static const char *const keys[] = {"password=", "encryptedInfo=", "cdKey=", "cdkey=", "newPassword="};
    char *cut = NULL;
    for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; ++k)
        for (char *p = strstr(g_net_line, keys[k]); p; p = strstr(p + 1, keys[k]))
            if ((p[-1] == ' ' || p[-1] == '{' || p[-1] == ',') && (!cut || p < cut))
                cut = p + strlen(keys[k]);
    static const char tail[] = "(not logged) ...";
    if (cut) {
        if ((size_t)(cut - g_net_line) + sizeof tail <= sizeof g_net_line)
            memcpy(cut, tail, sizeof tail);
        else
            *cut = 0;
        g_net_len = strlen(g_net_line);
        g_net_cut = 0;
    }
}

static void net_flush(void) {
    if (g_net_len > sizeof NET_LINE_HEAD - 1) {
        /* g_net_len stays within sizeof g_net_line - 4, so "..." and its NUL fit. */
        memcpy(g_net_line + g_net_len, g_net_cut ? "..." : "", g_net_cut ? 4 : 1);
        net_mask();
        log_line(g_net_line);
    }
    g_net_len = sizeof NET_LINE_HEAD - 1;
    g_net_cut = 0;
}

static void net_sdk_print(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t text = get_u32(cpu->esp + 4u);
    void *p = NULL;
    /* As much as fits; a shorter view is tried near the end of guest memory. */
    uint32_t len = 512;
    while (text && len >= 16 && (api->guest_ptr(api, text, len, &p) != POP_OK || !p))
        len /= 2;
    if (!text || !p)
        return;
    const char *s = (const char *)p;
    for (size_t i = 0, n = strnlen(s, len); i < n; ++i) {
        if (s[i] == '\n') {
            net_flush();
        } else if (s[i] != '\r') {
            if (g_net_len < sizeof g_net_line - 4)
                g_net_line[g_net_len++] = s[i];
            else
                g_net_cut = 1;
        }
    }
}

#define NET_OPTIONS_PTR 0x00a8acfcu    /* the online options' owner; options at [owner+8] */
#define NET_OPTIONS_VTABLE 0x009c9380u
#define NET_OPT_NAME 0x10u
#define NET_OPT_PASSWORD 0x30u
#define NET_OPT_AUTO 0x6bu
#define NET_FE_SERVICE 0x00600c90u
#define NET_LOGIN_FIELD 32u
/* Any value: the server never looks at it. Not a secret. */
#define NET_LOGIN_PASSWORD "lan"

static char g_net_name[NET_LOGIN_FIELD];
static int g_net_auto_logged;

/* The name the page uses for this player, as the server will keep it ("" until the page has
 * told the kit). */
static const char *net_player_name(void) {
    if (g_net_name[0])
        return g_net_name; /* read once: the environment is not looked at every frame */
    const char *raw = getenv("RECOMP_LAN_NAME");
    if (!raw || !*raw)
        return "";
    size_t n = 0;
    for (const char *c = raw; *c && n < 16; ++c)
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' ||
            *c == '-' || *c == '.')
            g_net_name[n++] = *c;
    g_net_name[n] = 0;
    if (!n)
        memcpy(g_net_name, "Player", sizeof "Player");
    return g_net_name;
}

static void net_auto_sign_in(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    const uint32_t owner = get_u32(NET_OPTIONS_PTR);
    const uint32_t opts = owner ? get_u32(owner + 8u) : 0;
    if (!opts || get_u32(opts) != NET_OPTIONS_VTABLE)
        return;
    const char *name = net_player_name();
    if (!*name)
        return;
    void *pn = NULL, *pp = NULL;
    if (api->guest_ptr(api, opts + NET_OPT_NAME, NET_LOGIN_FIELD, &pn) != POP_OK || !pn ||
        api->guest_ptr(api, opts + NET_OPT_PASSWORD, NET_LOGIN_FIELD, &pp) != POP_OK || !pp)
        return;
    uint8_t on = 0;
    api->guest_read_u8(api, opts + NET_OPT_AUTO, &on);
    if (on && !strncmp((const char *)pn, name, NET_LOGIN_FIELD) && !strncmp((const char *)pp, NET_LOGIN_PASSWORD, NET_LOGIN_FIELD))
        return;
    memset(pn, 0, NET_LOGIN_FIELD);
    memcpy(pn, name, strlen(name));
    memset(pp, 0, NET_LOGIN_FIELD);
    memcpy(pp, NET_LOGIN_PASSWORD, sizeof NET_LOGIN_PASSWORD);
    api->guest_write_u8(api, opts + NET_OPT_AUTO, 1);
    if (!g_net_auto_logged) {
        g_net_auto_logged = 1;
        char line[128];
        snprintf(line, sizeof line, "core.nfsc: online: signs in automatically as \"%s\"", name);
        log_line(line);
    }
}

static void online_net_install(void) {
    const char *lan = getenv("RECOMP_LAN");
    if (!lan || !*lan || !strcmp(lan, "0"))
        return;
    if (g_api->hook_install(g_api, NET_HUB_CREATE, net_ssl_off, POP_HOOK_BEFORE, NULL, &g_net_hooks[0]) != POP_OK)
        log_line("core.nfsc: online: could not hook the FESL hub's create; FESL keeps SSL");
    if (g_api->hook_install(g_api, NET_SDK_PRINT, net_sdk_print, POP_HOOK_BEFORE, NULL, &g_net_hooks[1]) != POP_OK)
        log_line("core.nfsc: online: the FESL SDK's log is not shown");
    if (g_api->hook_install(g_api, NET_FE_SERVICE, net_auto_sign_in, POP_HOOK_BEFORE, NULL, &g_net_hooks[2]) != POP_OK)
        log_line("core.nfsc: online: automatic sign-in not installed; the game's SIGN IN screen shows");
    log_line("core.nfsc: online: virtual LAN mode, FESL SSL switch, SDK log and automatic sign-in installed");
}
