// winsock.cpp - ws2_32 (and netapi32!Netbios) answered with Windows
// semantics over the kit's virtual LAN, so a game's own online code can talk
// to a server from inside the browser.
//
// A page cannot open sockets, so every socket a game makes lives here: a
// socket table with Windows' rules (error codes, a non-blocking connect that
// reports later, select, overlapped receives with WSA events), and every
// packet travels as a frame through the LAN mailbox (lan_mailbox.h) to the
// page, which carries it over one WebSocket to the server's switch
// (plans/lan-protocol.md). The switch gives this browser a virtual address
// 10.77.x.y; everything a host's IP stack decides is decided here: ports, UDP
// delivery and "port unreachable", TCP as frames of our own (SYN, SYNACK,
// RST, DATA, FIN: the WebSocket is reliable and ordered, so nothing is ever
// retransmitted), ICMP echo (answered here after a fixed round trip, so a
// game's latency checks pass) and names (a table; no DNS on the wire).
//
// Only when the page asks for it. RECOMP_LAN=1, which the player page sets
// when it starts lan.js, turns this on. Without it every call does exactly
// what the kit did before this file existed (the logging-only stub returning
// 0, and Netbios' NRC_ENVNOTDEF), so normal play cannot change. Names the kit
// never answered (getaddrinfo, WSASend, ...) are only registered in LAN mode,
// so GetProcAddress keeps failing for them otherwise, as it did.
//
// Threads. Guest threads call in holding the scheduler baton. The socket
// table is under g_net_m, which the network thread takes too. That thread
// drains the RX ring, runs the timers and completes pending overlapped
// receives: it writes the data into the guest's buffers and the OVERLAPPED
// as Windows' kernel does behind the program's back, and then signals their
// events with guest_event_signal_from_host, which the next thread to enter the
// scheduler applies. g_net_m is never held while waiting or while taking the
// scheduler's lock, so a guest thread that waits is descheduled the kit's way
// (guest_wait_objects, guest_sleep_ms) and holds nothing meanwhile.
//
// The rules (Windows bind semantics, ephemeral ports, broadcast loopback,
// unreachable notices, TCP over frames and its flow control, the IPv4 header
// in front of a raw socket's ICMP) are ported from nfsu2-web's
// runtime/winsock.c and runtime/web/net_lan.c (Copyright (c) 2026 th0mk;
// Portions copyright (c) 2026 Talha Müderrisoğlu; MIT, see NOTICE).
// Overlapped I/O and WSA events are new here: NFSU2's DirtySock polls, while
// Carbon's posts WSARecvFrom and sleeps in WSAWaitForMultipleEvents.
#include "imports.h"
#include "lan_mailbox.h"
#include "memory.h"
#include "win32.h"
#include "../platform/os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Windows numbers
// ---------------------------------------------------------------------------
constexpr uint32_t SOCKET_ERROR_ = 0xffffffffu;
enum : uint32_t {
    WSA_INVALID_HANDLE_ = 6,
    WSA_NOT_ENOUGH_MEMORY_ = 8,
    WSA_INVALID_PARAMETER_ = 87,
    WSA_OPERATION_ABORTED_ = 995,
    WSA_IO_INCOMPLETE_ = 996,
    WSA_IO_PENDING_ = 997,
    WSAEINTR_ = 10004,
    WSAEACCES_ = 10013,
    WSAEFAULT_ = 10014,
    WSAEINVAL_ = 10022,
    WSAEMFILE_ = 10024,
    WSAEWOULDBLOCK_ = 10035,
    WSAEALREADY_ = 10037,
    WSAENOTSOCK_ = 10038,
    WSAEMSGSIZE_ = 10040,
    WSAENOPROTOOPT_ = 10042,
    WSAEPROTONOSUPPORT_ = 10043,
    WSAESOCKTNOSUPPORT_ = 10044,
    WSAEOPNOTSUPP_ = 10045,
    WSAEAFNOSUPPORT_ = 10047,
    WSAEADDRINUSE_ = 10048,
    WSAEADDRNOTAVAIL_ = 10049,
    WSAENETUNREACH_ = 10051,
    WSAECONNRESET_ = 10054,
    WSAENOBUFS_ = 10055,
    WSAEISCONN_ = 10056,
    WSAENOTCONN_ = 10057,
    WSAESHUTDOWN_ = 10058,
    WSAETIMEDOUT_ = 10060,
    WSAECONNREFUSED_ = 10061,
    WSAEHOSTUNREACH_ = 10065,
    WSAVERNOTSUPPORTED_ = 10092,
    WSANOTINITIALISED_ = 10093,
    WSATYPE_NOT_FOUND_ = 10109,
    WSAHOST_NOT_FOUND_ = 11001,
    WSANO_DATA_ = 11004,
};
constexpr uint32_t STATUS_PENDING_ = 0x103;
constexpr uint32_t WAIT_FAILED_ = 0xffffffffu;
constexpr uint32_t INFINITE_ = 0xffffffffu;
constexpr uint32_t MSG_OOB_ = 1, MSG_PEEK_ = 2;
constexpr uint32_t FIONBIO_ = 0x8004667Eu, FIONREAD_ = 0x4004667Fu, SIOCATMARK_ = 0x40047307u;
constexpr uint32_t SIO_ROUTING_INTERFACE_QUERY_ = 0xC8000014u, SIO_UDP_CONNRESET_ = 0x9800000Cu;

// The kit's own limits.
constexpr int NSOCK = 1024;
constexpr uint32_t SOCK_BASE = 0x1000; // SOCKET = SOCK_BASE + 4 * index, index >= 1
constexpr size_t LOCAL_CAP = 1u << 20;   // a local stream's receive buffer before its writer waits
constexpr size_t REMOTE_CAP = 16u << 20; // a remote stream's receive buffer before it is reset
constexpr size_t DGQ_MAX = 1024;         // datagrams queued per socket
constexpr size_t DGQ_BYTES = 1u << 20;
constexpr double CONNECT_TMO_MS = 3000.0; // no answer to a SYN
constexpr uint32_t TCP_CHUNK = lan::MAX_PAYLOAD - lan::LAN_HDR;
constexpr uint32_t UDP_MAX = lan::MAX_PAYLOAD - lan::LAN_HDR;
// TX slots stream data never takes, so a FIN after a send that filled the
// ring (and SYNACKs and RSTs meanwhile) still fit.
constexpr uint32_t TX_RESERVE = 4;
// How long a blocking call sleeps between looks when nothing signals it.
constexpr uint32_t WAIT_SLICE_MS = 20;

double now_ms() {
    return (double)os_monotonic_ns() / 1e6;
}
uint32_t ld(const uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
void st(uint32_t *p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}
void notify_word(uint32_t *p) {
#ifdef __EMSCRIPTEN__
    emscripten_futex_wake(p, 0x7fffffff);
#else
    (void)p; // natively the "page" is a test thread that polls
#endif
}
std::string ipstr(uint32_t h) {
    char b[20];
    snprintf(b, sizeof b, "%u.%u.%u.%u", h >> 24, (h >> 16) & 255, (h >> 8) & 255, h & 255);
    return b;
}
bool is_loop(uint32_t h) {
    return (h >> 24) == 127;
}
uint32_t rand32() {
    static uint64_t s = 0;
    if (!s)
        s = os_monotonic_ns() ^ ((uint64_t)(uintptr_t)&s << 16) ^ 0x9e3779b97f4a7c15ull;
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return (uint32_t)(s >> 16);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
int g_lan_test = -1;
bool lan_on() {
    static const bool on = [] {
        const char *v = recomp_env("LAN");
        return v && *v && strcmp(v, "0") != 0;
    }();
    return g_lan_test >= 0 ? g_lan_test != 0 : on;
}

// Names (spec section 1): no DNS on the wire. RECOMP_LAN_HOSTS replaces the
// table: "pattern=ip;pattern=ip", a leading * matching any start.
struct NameRule {
    std::string pat; // lower case, without the *
    bool suffix;
    uint32_t ip;
};
bool parse_ip(const char *s, uint32_t *out) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    *out = a << 24 | b << 16 | c << 8 | d;
    return true;
}
const std::vector<NameRule> &name_rules() {
    static const std::vector<NameRule> rules = [] {
        const char *env = recomp_env("LAN_HOSTS");
        std::string text = env && *env ? env : "*.ea.com=10.77.0.1;*madserver.net=10.77.0.1";
        std::vector<NameRule> out;
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find(';', at);
            if (end == std::string::npos)
                end = text.size();
            std::string item = text.substr(at, end - at);
            at = end + 1;
            size_t eq = item.find('=');
            uint32_t ip = 0;
            if (eq == std::string::npos || !parse_ip(item.c_str() + eq + 1, &ip)) {
                if (!item.empty())
                    LOGW("lan: RECOMP_LAN_HOSTS entry \"%s\" is not pattern=a.b.c.d: ignored", item.c_str());
                continue;
            }
            NameRule r;
            r.pat = item.substr(0, eq);
            r.suffix = !r.pat.empty() && r.pat[0] == '*';
            if (r.suffix)
                r.pat.erase(0, 1);
            for (char &ch : r.pat)
                ch = (char)tolower((unsigned char)ch);
            r.ip = ip;
            out.push_back(r);
        }
        return out;
    }();
    return rules;
}
bool name_table(const std::string &name, uint32_t *ip) {
    std::string n = name;
    for (char &ch : n)
        ch = (char)tolower((unsigned char)ch);
    for (const NameRule &r : name_rules()) {
        bool hit = r.suffix ? n.size() >= r.pat.size() && n.compare(n.size() - r.pat.size(), r.pat.size(), r.pat) == 0
                            : n == r.pat;
        if (hit) {
            *ip = r.ip;
            return true;
        }
    }
    return false;
}
uint32_t ping_ms() {
    static const uint32_t ms = [] {
        const char *v = recomp_env("LAN_PING_MS");
        return v && *v ? (uint32_t)strtoul(v, nullptr, 10) : 20u;
    }();
    return ms;
}

// ---------------------------------------------------------------------------
// Per-thread last error. Every guest thread runs on a host thread of its own,
// so a thread-local is exactly Windows' per-thread slot. It is mirrored into
// the kit's GetLastError value, which Windows shares with WSAGetLastError.
// ---------------------------------------------------------------------------
thread_local uint32_t t_wsa_error = 0;
void set_err(uint32_t e) {
    t_wsa_error = e;
    set_last_error(e);
}

// ---------------------------------------------------------------------------
// State, all under g_net_m.
// ---------------------------------------------------------------------------
enum SockState { ST_NEW, ST_BOUND, ST_LISTEN, ST_CONNECTING, ST_CONNECTED, ST_FAILED };
enum ConnState { C_SYN_SENT = 1, C_EST, C_REFUSED, C_FAILED };

struct Conn {
    uint32_t cid = 0;
    uint32_t lip = 0, rip = 0; // host order
    uint16_t lport = 0, rport = 0;
    int state = 0;
    bool local = false; // both ends are sockets of this kit
    Conn *peer = nullptr;
    bool rx_fin = false, rx_rst = false, tx_fin = false, rst_sent = false;
    std::vector<uint8_t> rb;
    size_t roff = 0;
    int sock = -1;     // the socket that owns it; -1 while it waits to be accepted
    int listener = -1; // the listener whose accept queue holds it
    double deadline = 0;
    uint32_t fail_err = WSAETIMEDOUT_;
    size_t rlen() const {
        return rb.size() - roff;
    }
};

struct Dgram {
    uint32_t ip;
    uint16_t port;
    bool notice; // "port unreachable" for the datagram we sent to ip:port
    std::vector<uint8_t> data;
};

// A posted overlapped receive.
struct Pending {
    uint32_t ov = 0, ev = 0;
    std::vector<std::pair<uint32_t, uint32_t>> bufs; // (address, length), captured at the call
    uint32_t from = 0, fromlen = 0;
    bool peek = false;
};

struct Sent {
    uint32_t ip = 0;
    uint16_t port = 0;
    uint32_t seq = 0;
};

struct Sock {
    bool used = false;
    int type = 0; // 1 stream, 2 datagram, 3 raw (ICMP)
    bool nb = false;
    int state = ST_NEW;
    bool bound = false;
    uint32_t laddr = 0; // host order, 0 = any
    uint16_t lport = 0;
    bool has_peer = false; // TCP: connected or connecting to; UDP: the default destination
    uint32_t raddr = 0;
    uint16_t rport = 0;
    bool so_reuse = false, so_broadcast = false, so_keepalive = false, so_dontroute = false,
         so_oobinline = false, nodelay = false;
    bool linger_on = false;
    uint16_t linger_s = 0;
    int32_t sndbuf = 65536, rcvbuf = 65536;
    uint32_t sndtimeo = 0, rcvtimeo = 0;
    int ip_ttl = 128;
    bool connreset = true; // SIO_UDP_CONNRESET: unreachable notices fail the next receive
    bool shut_rd = false, shut_wr = false;
    uint32_t pending_err = 0; // SO_ERROR
    Conn *conn = nullptr;
    int backlog = 0;
    std::deque<Conn *> accq;
    std::deque<Dgram> q;
    size_t qbytes = 0;
    Sent sent[8];
    std::deque<Pending> pend;
};

struct OvDone {
    uint32_t err, bytes, flags;
    int sock; // whose: forgotten when that socket slot is used again
};

// An ICMP echo answer on its way back.
struct Echo {
    double due;
    uint32_t from;
    std::vector<uint8_t> msg;
};

// A TCP control frame that found the TX ring full: sent before any later frame.
struct CtlFrame {
    uint32_t dst;
    uint16_t sport, dport;
    uint8_t op;
    uint32_t cid;
};

std::mutex g_net_m;
lan::Mailbox *g_mb = nullptr;
bool g_thread_started = false;
uint32_t g_vip = 0, g_mask = 0; // host order; the last address the page gave
bool g_was_online = false;
uint32_t g_gen_seen = 0;
int g_startups = 0;
Sock g_s[NSOCK];
int g_s_next = 1;
std::vector<Conn *> g_conns;
std::map<uint32_t, OvDone> g_done; // completed overlapped receives, by OVERLAPPED address
std::deque<CtlFrame> g_ctlq;
std::vector<Echo> g_echoes;
uint32_t g_sent_seq = 0;
std::vector<uint32_t> g_sig; // events to signal once g_net_m is released
bool g_changed = false;
uint32_t g_change_ev = 0; // signalled when anything changes while a blocking call waits
std::atomic<int> g_waiters{0};
bool g_waited_unattached = false;

struct Stats {
    uint64_t tx_frames, rx_frames, tx_full, tx_offline, udp_out, udp_in, udp_noport, udp_outside, udp_qfull,
        tcp_out, tcp_in, tcp_refused, tcp_unreach, tcp_timeout, tcp_rst_in, tcp_rst_out, tcp_reset_link,
        tcp_bytes_out, tcp_bytes_in, icmp_out, echo_answered, auto_held, bad, ov_posted, ov_completed;
} S;

void signal_event(uint32_t h) {
    // The baton holder owns the handle table and may set the event itself; any
    // other thread queues the signal for whoever holds it next.
    if (sched_is_guest_thread())
        win32_signal_event(h, false);
    else
        guest_event_signal_from_host(h);
}

// g_net_m for one scope. Events whose operations completed meanwhile are
// signalled after the lock is down, so nothing ever takes the scheduler's
// lock while holding this one.
struct NetLock {
    NetLock() {
        g_net_m.lock();
    }
    ~NetLock() {
        std::vector<uint32_t> sig;
        sig.swap(g_sig);
        bool wake = g_changed && g_change_ev && g_waiters.load() > 0;
        g_changed = false;
        g_net_m.unlock();
        if (wake)
            sig.push_back(g_change_ev);
        for (uint32_t h : sig)
            signal_event(h);
    }
    NetLock(const NetLock &) = delete;
    NetLock &operator=(const NetLock &) = delete;
};
void changed() {
    g_changed = true;
}

bool online_locked() {
    return g_mb && ld(&g_mb->page_state) == lan::PAGE_ONLINE && g_vip && ld(&g_mb->vip) == g_vip;
}
bool own_ip(uint32_t h) {
    return g_vip && h == g_vip;
}
bool in_subnet(uint32_t h) {
    return g_mask && g_vip && (h & g_mask) == (g_vip & g_mask);
}
bool is_bcast(uint32_t h) {
    return h == lan::BROADCAST || (g_mask && g_vip && h == ((g_vip & g_mask) | ~g_mask));
}

// ---------------------------------------------------------------------------
// The mailbox: TX
// ---------------------------------------------------------------------------
lan::Slot *tx_slot(uint32_t i) {
    return (lan::Slot *)((uint8_t *)g_mb + g_mb->tx_off + (i % lan::TX_SLOTS) * lan::SLOT_BYTES);
}
lan::Slot *rx_slot(uint32_t i) {
    return (lan::Slot *)((uint8_t *)g_mb + g_mb->rx_off + (i % lan::RX_SLOTS) * lan::SLOT_BYTES);
}
uint32_t tx_free() {
    return g_mb ? lan::TX_SLOTS - (g_mb->tx_head - ld(&g_mb->tx_tail)) : 0;
}
bool tx_raw(uint32_t kind, uint32_t dst, uint16_t sport, uint16_t dport, uint8_t op, uint8_t ttl, uint32_t cid,
            const uint8_t *data, uint32_t n) {
    if (!online_locked()) {
        ++S.tx_offline;
        return false;
    }
    if (n > UDP_MAX) {
        ++S.bad;
        return false;
    }
    uint32_t h = g_mb->tx_head;
    if (h - ld(&g_mb->tx_tail) >= lan::TX_SLOTS) {
        ++S.tx_full;
        return false;
    }
    lan::Slot *s = tx_slot(h);
    s->len = lan::LAN_HDR + n;
    s->kind = kind;
    s->src_ip = g_vip;
    s->dst_ip = dst;
    s->src_port = sport;
    s->dst_port = dport;
    s->pad = 0;
    s->t_ms = (double)os_wall_time_us() / 1000.0;
    s->data[0] = op;
    s->data[1] = ttl;
    s->data[2] = s->data[3] = 0;
    s->data[4] = (uint8_t)cid;
    s->data[5] = (uint8_t)(cid >> 8);
    s->data[6] = (uint8_t)(cid >> 16);
    s->data[7] = (uint8_t)(cid >> 24);
    if (n)
        memcpy(s->data + lan::LAN_HDR, data, n);
    st(&g_mb->tx_head, h + 1);
    __atomic_add_fetch(&g_mb->tx_bell, 1, __ATOMIC_SEQ_CST);
    notify_word(&g_mb->tx_bell);
    ++S.tx_frames;
    return true;
}
void ctl_flush() {
    while (!g_ctlq.empty() && online_locked() && tx_free()) {
        const CtlFrame q = g_ctlq.front();
        g_ctlq.pop_front();
        tx_raw(lan::KIND_TCP, q.dst, q.sport, q.dport, q.op, 64, q.cid, nullptr, 0);
    }
}
// Every frame but a control frame goes after the waiting control frames.
bool tx(uint32_t kind, uint32_t dst, uint16_t sport, uint16_t dport, uint8_t op, uint8_t ttl, uint32_t cid,
        const uint8_t *data, uint32_t n) {
    if (!g_ctlq.empty()) {
        ctl_flush();
        if (!g_ctlq.empty() && online_locked()) {
            ++S.tx_full;
            return false;
        }
    }
    return tx_raw(kind, dst, sport, dport, op, ttl, cid, data, n);
}
// A TCP control frame (SYNACK, RST, FIN): now, or queued while the ring is full.
void tx_ctl(uint32_t dst, uint16_t sport, uint16_t dport, uint8_t op, uint32_t cid) {
    ctl_flush();
    if (!online_locked()) {
        ++S.tx_offline;
        return;
    }
    if (g_ctlq.empty() && tx_free()) {
        tx_raw(lan::KIND_TCP, dst, sport, dport, op, 64, cid, nullptr, 0);
        return;
    }
    if (g_ctlq.size() >= 1024) {
        log_once("lan ctlq", "lan: 1024 TCP control frames wait for the TX ring: one dropped");
        return;
    }
    ++S.tx_full;
    g_ctlq.push_back({dst, sport, dport, op, cid});
}
void tcp_ctl(Conn *k, uint8_t op) {
    tx_ctl(k->rip, k->lport, k->rport, op, k->cid);
}

// Answers this "host" gives by itself (a port unreachable, a RST to a segment
// of no connection) cost the frame's size plus 64 bytes; at most 4 KB a second
// go to one sender and 32 KB to all, with a 2 s burst, so stray frames from
// someone else on the switch cannot make this browser spend its share of the
// switch answering them. nfsu2-web's rule.
struct AutoBucket {
    double t = 0, v = 0;
    bool set = false;
};
AutoBucket g_auto_src[256], g_auto_all;
bool auto_has(AutoBucket *b, double rate, double n, double now) {
    double cap = 2.0 * rate;
    if (!b->set) {
        b->set = true;
        b->v = cap;
    } else if (now > b->t) {
        b->v = std::min(cap, b->v + (now - b->t) / 1000.0 * rate);
    }
    b->t = now;
    return b->v >= n;
}
bool auto_ok(uint32_t src, uint32_t len) {
    double now = now_ms(), n = len + 64.0;
    AutoBucket *b = &g_auto_src[src & 255];
    bool ok_src = auto_has(b, 4096, n, now), ok_all = auto_has(&g_auto_all, 32768, n, now);
    if (!ok_src || !ok_all) {
        ++S.auto_held;
        return false;
    }
    b->v -= n;
    g_auto_all.v -= n;
    return true;
}

// ---------------------------------------------------------------------------
// Sockets, ports, connections
// ---------------------------------------------------------------------------
uint32_t shandle(const Sock *s) {
    return SOCK_BASE + 4u * (uint32_t)(s - g_s);
}
int sindex(uint32_t h) {
    if (h < SOCK_BASE + 4 || (h - SOCK_BASE) & 3)
        return -1;
    uint32_t i = (h - SOCK_BASE) >> 2;
    return i < (uint32_t)NSOCK && g_s[i].used ? (int)i : -1;
}
Sock *sget(uint32_t h) {
    int i = sindex(h);
    return i >= 0 ? &g_s[i] : nullptr;
}
Sock *salloc() {
    for (int k = 0; k < NSOCK - 1; ++k) {
        int i = g_s_next + k;
        if (i >= NSOCK)
            i -= NSOCK - 1;
        if (g_s[i].used)
            continue;
        for (auto it = g_done.begin(); it != g_done.end();)
            it = it->second.sock == i ? g_done.erase(it) : std::next(it);
        g_s[i] = Sock();
        g_s[i].used = true;
        g_s_next = i + 1 >= NSOCK ? 1 : i + 1; // round robin: a closed handle is not reused at once
        return &g_s[i];
    }
    return nullptr;
}

bool port_used(bool tcp, uint16_t port, uint32_t ip, const Sock *except, bool *reuse_all) {
    bool used = false;
    *reuse_all = true;
    for (int i = 1; i < NSOCK; ++i) {
        const Sock &o = g_s[i];
        if (!o.used || &o == except || !o.bound || o.type == 3 || (o.type == 1) != tcp || o.lport != port)
            continue;
        if (o.laddr == ip || !o.laddr || !ip) {
            used = true;
            if (!o.so_reuse)
                *reuse_all = false;
        }
    }
    return used;
}
uint16_t ephemeral(bool tcp) {
    for (int k = 0; k < 20000; ++k) {
        uint16_t p = (uint16_t)(49152u + rand32() % (65535u - 49152u));
        bool ra;
        if (!port_used(tcp, p, 0, nullptr, &ra))
            return p;
    }
    return 0;
}

Conn *conn_new(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport, uint32_t cid, int state) {
    Conn *k = new Conn();
    k->lip = lip;
    k->lport = lport;
    k->rip = rip;
    k->rport = rport;
    k->cid = cid;
    k->state = state;
    g_conns.push_back(k);
    return k;
}
void conn_free(Conn *k) {
    if (k->peer)
        k->peer->peer = nullptr;
    g_conns.erase(std::remove(g_conns.begin(), g_conns.end(), k), g_conns.end());
    delete k;
}
Conn *conn_find(uint32_t rip, uint16_t rport, uint16_t lport, uint32_t cid) {
    for (Conn *k : g_conns)
        if (!k->local && k->rip == rip && k->rport == rport && k->lport == lport && k->cid == cid)
            return k;
    return nullptr;
}
bool rb_put(Conn *k, const uint8_t *p, size_t n, size_t cap) {
    if (!n)
        return true;
    if (k->rlen() + n > cap)
        return false;
    if (k->roff && k->roff > k->rb.size() / 2) {
        k->rb.erase(k->rb.begin(), k->rb.begin() + (ptrdiff_t)k->roff);
        k->roff = 0;
    }
    k->rb.insert(k->rb.end(), p, p + n);
    return true;
}
void complete_pending(int si);
void kick_conn(Conn *k) {
    changed();
    if (k->sock >= 0)
        complete_pending(k->sock);
}
void got_rst(Conn *k) {
    k->rx_rst = true; // a reset discards what was not read
    k->rb.clear();
    k->roff = 0;
    if (k->state == C_SYN_SENT) {
        k->state = C_REFUSED;
        k->fail_err = WSAECONNREFUSED_;
    }
    kick_conn(k);
}
// Our side resets the connection (abortive close, overflow, link loss).
void send_rst(Conn *k) {
    if (k->rst_sent || k->rx_rst)
        return;
    k->rst_sent = true;
    if (k->local) {
        if (k->peer)
            got_rst(k->peer);
    } else if (k->state == C_EST || k->state == C_SYN_SENT) {
        tcp_ctl(k, lan::TCP_RST);
        ++S.tcp_rst_out;
    }
}
void send_fin(Conn *k) {
    if (k->tx_fin || k->rst_sent || k->rx_rst || k->state != C_EST)
        return;
    k->tx_fin = true;
    if (k->local) {
        if (k->peer) {
            k->peer->rx_fin = true;
            kick_conn(k->peer);
        }
    } else {
        tcp_ctl(k, lan::TCP_FIN);
    }
}
// Every connection to another host on the switch (or to one) is reset: the
// frames of a lost switch connection are gone, or that host left.
int reset_remote(uint32_t only_rip, const char *why) {
    int n = 0;
    for (Conn *k : std::vector<Conn *>(g_conns)) {
        if (k->local || k->rx_rst || (only_rip && k->rip != only_rip))
            continue;
        if (k->state == C_SYN_SENT) {
            k->state = C_FAILED;
            k->fail_err = WSAEHOSTUNREACH_;
            kick_conn(k);
            ++n;
            continue;
        }
        if (k->state != C_EST)
            continue;
        got_rst(k);
        ++n;
    }
    if (n) {
        S.tcp_reset_link += (uint64_t)n;
        LOGW("lan: %d TCP connection(s) reset: %s", n, why);
    }
    return n;
}
int find_listener(uint32_t dst, uint16_t port) {
    for (int i = 1; i < NSOCK; ++i) {
        const Sock &o = g_s[i];
        if (o.used && o.type == 1 && o.state == ST_LISTEN && o.lport == port &&
            (!o.laddr || o.laddr == dst || (is_loop(dst) && is_loop(o.laddr))))
            return i;
    }
    return -1;
}
void acc_push(int li, Conn *k) {
    k->listener = li;
    g_s[li].accq.push_back(k);
    changed();
}

// ---------------------------------------------------------------------------
// Datagrams
// ---------------------------------------------------------------------------
void dg_push(int si, uint32_t src, uint16_t sport, bool notice, const uint8_t *p, size_t n) {
    Sock &v = g_s[si];
    if (v.q.size() >= DGQ_MAX || v.qbytes + n > DGQ_BYTES) {
        ++S.udp_qfull;
        return;
    }
    Dgram d;
    d.ip = src;
    d.port = sport;
    d.notice = notice;
    d.data.assign(p, p + n);
    v.q.push_back(std::move(d));
    v.qbytes += n;
    changed();
    complete_pending(si);
}
// The sockets of this kit that take a datagram to dst:dport (vlanhub's rule):
// a broadcast goes to every binder of the port, the sender too; a unicast to
// the binder of exactly that address, else a wildcard binder.
int udp_deliver(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *p, size_t n, bool bc) {
    int first = -1, exact = -1, k = 0;
    for (int i = 1; i < NSOCK; ++i) {
        const Sock &v = g_s[i];
        if (!v.used || v.type != 2 || !v.bound || v.lport != dport)
            continue;
        if (!(!v.laddr || v.laddr == dst || (bc && own_ip(v.laddr)) || (is_loop(dst) && is_loop(v.laddr))))
            continue;
        if (bc) {
            dg_push(i, src, sport, false, p, n);
            ++k;
            continue;
        }
        if (first < 0)
            first = i;
        if (exact < 0 && v.laddr == dst)
            exact = i;
    }
    if (!bc && (exact >= 0 || first >= 0)) {
        dg_push(exact >= 0 ? exact : first, src, sport, false, p, n);
        k = 1;
    }
    return k;
}
// An ICMP message goes to every raw socket, as Windows' raw ICMP sockets see
// all of them.
void icmp_deliver(uint32_t src, const uint8_t *p, size_t n) {
    for (int i = 1; i < NSOCK; ++i)
        if (g_s[i].used && g_s[i].type == 3)
            dg_push(i, src, 0, false, p, n);
}
void icmp_cksum(uint8_t *m, size_t n) {
    m[2] = m[3] = 0;
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += (uint32_t)(m[i] << 8 | m[i + 1]);
    if (n & 1)
        sum += (uint32_t)m[n - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    sum = ~sum & 0xffff;
    m[2] = (uint8_t)(sum >> 8);
    m[3] = (uint8_t)sum;
}
// "Port unreachable" from src: our datagram from dport to src:sport found no
// socket there. The socket that sent it gets the notice: the latest sender to
// that address, else one connected to it, else the first binder of the port.
void unreach_deliver(uint32_t src, uint16_t sport, uint16_t dport) {
    int sender = -1, conn = -1, first = -1;
    uint32_t best = 0;
    for (int i = 1; i < NSOCK; ++i) {
        const Sock &v = g_s[i];
        if (!v.used || v.type != 2 || !v.bound || v.lport != dport)
            continue;
        for (const Sent &s : v.sent)
            if (s.seq && s.ip == src && s.port == sport && (sender < 0 || (int32_t)(s.seq - best) > 0)) {
                sender = i;
                best = s.seq;
            }
        if (conn < 0 && v.has_peer && v.raddr == src && v.rport == sport)
            conn = i;
        if (first < 0)
            first = i;
    }
    int to = sender >= 0 ? sender : conn >= 0 ? conn : first;
    if (to >= 0)
        dg_push(to, src, sport, true, nullptr, 0);
}
void sent_note(Sock &v, uint32_t ip, uint16_t port) {
    int k = -1;
    for (int j = 0; j < 8 && k < 0; ++j)
        if (v.sent[j].seq && v.sent[j].ip == ip && v.sent[j].port == port)
            k = j;
    for (int j = 0; j < 8 && k < 0; ++j)
        if (!v.sent[j].seq)
            k = j;
    if (k < 0) {
        k = 0;
        for (int j = 1; j < 8; ++j)
            if ((int32_t)(v.sent[j].seq - v.sent[k].seq) < 0)
                k = j;
    }
    if (!++g_sent_seq)
        ++g_sent_seq;
    v.sent[k] = {ip, port, g_sent_seq};
}

// ---------------------------------------------------------------------------
// Receiving into guest buffers: the one path recv, recvfrom, WSARecv(From)
// and a completing overlapped receive share.
// ---------------------------------------------------------------------------
struct RecvResult {
    uint32_t err = 0, bytes = 0, flags = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
};
using Bufs = std::vector<std::pair<uint32_t, uint32_t>>;

uint32_t bufs_total(const Bufs &b) {
    uint64_t t = 0;
    for (auto &x : b)
        t += x.second;
    return t > 0x7fffffffu ? 0x7fffffffu : (uint32_t)t;
}
// Scatters n bytes into the buffers; returns what fitted.
uint32_t scatter(const Bufs &b, const uint8_t *p, uint32_t n) {
    uint32_t done = 0;
    for (auto &x : b) {
        if (done >= n)
            break;
        uint32_t m = std::min(x.second, n - done);
        if (m && gm_valid(x.first, m))
            memcpy(g_mem + x.first, p + done, m);
        done += m;
    }
    return done;
}
// Windows' raw sockets deliver the IPv4 header in front of the ICMP message.
std::vector<uint8_t> with_ip_header(uint32_t src, const std::vector<uint8_t> &msg) {
    std::vector<uint8_t> out(20 + msg.size());
    uint8_t *ih = out.data();
    size_t tot = out.size();
    ih[0] = 0x45;
    ih[2] = (uint8_t)(tot >> 8);
    ih[3] = (uint8_t)tot;
    ih[8] = 64;
    ih[9] = 1;
    uint32_t dst = g_vip ? g_vip : 0x7f000001u;
    for (int i = 0; i < 4; ++i) {
        ih[12 + i] = (uint8_t)(src >> (24 - 8 * i));
        ih[16 + i] = (uint8_t)(dst >> (24 - 8 * i));
    }
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2)
        sum += (uint32_t)(ih[i] << 8 | ih[i + 1]);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    sum = ~sum & 0xffff;
    ih[10] = (uint8_t)(sum >> 8);
    ih[11] = (uint8_t)sum;
    if (!msg.empty())
        memcpy(ih + 20, msg.data(), msg.size());
    return out;
}
// True when the receive completed (with data, an end or an error); false when
// it would block.
bool try_recv(Sock &s, const Bufs &bufs, bool peek, RecvResult *r) {
    *r = RecvResult();
    uint32_t cap = bufs_total(bufs);
    if (s.type == 1) {
        Conn *k = s.conn;
        if (!k) {
            r->err = WSAENOTCONN_;
            return true;
        }
        r->ip = k->rip;
        r->port = k->rport;
        if (k->rlen()) {
            uint32_t m = (uint32_t)std::min<size_t>(cap, k->rlen());
            r->bytes = scatter(bufs, k->rb.data() + k->roff, m);
            if (!peek) {
                k->roff += r->bytes;
                if (!k->rlen()) {
                    k->rb.clear();
                    k->roff = 0;
                }
                if (k->local)
                    changed(); // room for a local writer
            }
            return true;
        }
        if (k->rx_rst) {
            r->err = WSAECONNRESET_;
            return true;
        }
        if (k->rx_fin || (k->local && !k->peer))
            return true; // the other side closed: 0 bytes
        if (k->state != C_EST) {
            r->err = WSAENOTCONN_;
            return true;
        }
        return false;
    }
    while (!s.q.empty()) {
        Dgram &d = s.q.front();
        if (d.notice) {
            // A connected socket hears only of its own peer, as a kernel
            // matches an ICMP error to a connected socket.
            bool ours = s.connreset && (!s.has_peer || (d.ip == s.raddr && d.port == s.rport));
            uint32_t ip = d.ip;
            uint16_t port = d.port;
            s.q.pop_front();
            if (ours) {
                r->err = WSAECONNRESET_;
                r->ip = ip;
                r->port = port;
                return true;
            }
            continue;
        }
        r->ip = d.ip;
        r->port = d.port;
        std::vector<uint8_t> raw;
        const std::vector<uint8_t> *data = &d.data;
        if (s.type == 3) {
            raw = with_ip_header(d.ip, d.data);
            data = &raw;
        }
        uint32_t n = (uint32_t)data->size();
        r->bytes = scatter(bufs, data->data(), std::min(n, cap));
        if (n > cap)
            r->err = WSAEMSGSIZE_; // the head is copied, the rest of the datagram is lost
        if (!peek) {
            s.qbytes -= d.data.size();
            s.q.pop_front();
        }
        return true;
    }
    return false;
}

void write_sockaddr(uint32_t p, uint32_t ip, uint16_t port) {
    wr16(p, 2);
    wr8(p + 2, (uint8_t)(port >> 8));
    wr8(p + 3, (uint8_t)port);
    wr32(p + 4, __builtin_bswap32(ip));
    wr32(p + 8, 0);
    wr32(p + 12, 0);
}
uint32_t ntstatus_of(uint32_t err) {
    switch (err) {
    case 0:
        return 0;
    case WSAEMSGSIZE_:
        return 0x80000005u; // STATUS_BUFFER_OVERFLOW
    case WSAECONNRESET_:
        return 0xC000020Du; // STATUS_CONNECTION_RESET
    case WSA_OPERATION_ABORTED_:
        return 0xC0000120u; // STATUS_CANCELLED
    default:
        return 0xC0000001u; // STATUS_UNSUCCESSFUL
    }
}
// An overlapped receive is done: the OVERLAPPED says so, its result is kept
// for WSAGetOverlappedResult, and its event is signalled.
void finish_pending(int si, const Pending &p, const RecvResult &r) {
    if (p.from && p.fromlen && gm_valid(p.fromlen, 4) && (int32_t)rd32(p.fromlen) >= 16 && gm_valid(p.from, 16)) {
        write_sockaddr(p.from, r.ip, r.port);
        wr32(p.fromlen, 16);
    }
    if (gm_valid(p.ov, 20)) {
        wr32(p.ov + 0, ntstatus_of(r.err));
        wr32(p.ov + 4, r.bytes);
    }
    g_done[p.ov] = {r.err, r.bytes, r.flags, si};
    ++S.ov_completed;
    if (p.ev)
        g_sig.push_back(p.ev);
}
void complete_pending(int si) {
    Sock &s = g_s[si];
    while (!s.pend.empty()) {
        RecvResult r;
        if (!try_recv(s, s.pend.front().bufs, s.pend.front().peek, &r))
            break;
        Pending p = s.pend.front();
        s.pend.pop_front();
        finish_pending(si, p, r);
    }
}
// closesocket and WSACleanup cancel what is still posted.
void abort_pending(Sock &s) {
    while (!s.pend.empty()) {
        Pending p = s.pend.front();
        s.pend.pop_front();
        RecvResult r;
        r.err = WSA_OPERATION_ABORTED_;
        finish_pending((int)(&s - g_s), p, r);
    }
}

// ---------------------------------------------------------------------------
// Received frames
// ---------------------------------------------------------------------------
void rst_back(const lan::Slot *sl, uint32_t cid) {
    if (auto_ok(sl->src_ip, lan::LAN_HDR))
        tx_ctl(sl->src_ip, sl->dst_port, sl->src_port, lan::TCP_RST, cid);
}
void rx_frame(const lan::Slot *sl) {
    uint32_t n = sl->len;
    if (n < lan::LAN_HDR || n > lan::MAX_PAYLOAD) {
        ++S.bad;
        return;
    }
    const uint8_t *d = sl->data;
    uint8_t op = d[0];
    uint32_t cid = (uint32_t)d[4] | (uint32_t)d[5] << 8 | (uint32_t)d[6] << 16 | (uint32_t)d[7] << 24;
    const uint8_t *p = d + lan::LAN_HDR;
    size_t pn = n - lan::LAN_HDR;
    uint32_t src = sl->src_ip, dst = sl->dst_ip;
    uint16_t sport = sl->src_port, dport = sl->dst_port;
    bool bc = is_bcast(dst);
    if (sl->kind == lan::KIND_TCP && op == lan::TCP_GONE) {
        reset_remote(src, "that host left the switch");
        return;
    }
    if (!bc && dst != g_vip) { // the switch routes by address: not ours
        ++S.bad;
        return;
    }
    ++S.rx_frames;
    if (sl->kind == lan::KIND_UDP) {
        if (op == lan::UDP_UNREACH) {
            unreach_deliver(src, sport, dport);
            return;
        }
        ++S.udp_in;
        if (!udp_deliver(src, sport, bc ? lan::BROADCAST : g_vip, dport, p, pn, bc) && !bc) {
            ++S.udp_noport;
            if (auto_ok(src, lan::LAN_HDR))
                tx(lan::KIND_UDP, src, dport, sport, lan::UDP_UNREACH, 64, 0, nullptr, 0);
        }
        return;
    }
    if (sl->kind != lan::KIND_TCP || bc) {
        ++S.bad;
        return;
    }
    Conn *k = conn_find(src, sport, dport, cid);
    switch (op) {
    case lan::TCP_SYN: {
        if (k)
            break; // a duplicate
        int li = find_listener(g_vip, dport);
        if (li < 0 || (int)g_s[li].accq.size() >= g_s[li].backlog + 4) {
            ++S.tcp_refused;
            rst_back(sl, cid);
            LOGW("lan: tcp %s:%u -> :%u refused (%s)", ipstr(src).c_str(), sport, dport,
                 li >= 0 ? "backlog full" : "no listener");
            break;
        }
        k = conn_new(g_vip, dport, src, sport, cid, C_EST);
        acc_push(li, k);
        ++S.tcp_in;
        tx_ctl(src, dport, sport, lan::TCP_SYNACK, cid);
        LOGW("lan: tcp %s:%u -> %s:%u accepted", ipstr(src).c_str(), sport, ipstr(g_vip).c_str(), dport);
        break;
    }
    case lan::TCP_SYNACK:
        if (k && k->state == C_SYN_SENT) {
            k->state = C_EST;
            kick_conn(k);
        } else if (!k || k->state != C_EST) {
            rst_back(sl, cid);
        }
        break;
    case lan::TCP_RST:
        if (k) {
            ++S.tcp_rst_in;
            if (k->state == C_SYN_SENT)
                ++S.tcp_refused;
            got_rst(k);
        }
        break;
    case lan::TCP_DATA:
        if (!k || k->state != C_EST || k->rx_rst) {
            if (!k || !k->rst_sent)
                rst_back(sl, cid);
            break;
        }
        if (k->rx_fin)
            break;
        if (!rb_put(k, p, pn, REMOTE_CAP)) {
            LOGW("lan: tcp from %s:%u: %zu bytes unread, the connection is reset", ipstr(src).c_str(), sport,
                 k->rlen());
            send_rst(k);
            got_rst(k);
            break;
        }
        S.tcp_bytes_in += pn;
        kick_conn(k);
        break;
    case lan::TCP_FIN:
        if (k) {
            k->rx_fin = true;
            kick_conn(k);
        }
        break;
    default:
        ++S.bad;
        break;
    }
}

// ---------------------------------------------------------------------------
// One pass: the page's link, the received frames, the timers.
// ---------------------------------------------------------------------------
void link_check() {
    uint32_t ps = ld(&g_mb->page_state), gen = ld(&g_mb->generation);
    bool up = ps == lan::PAGE_ONLINE;
    // The page wrote the player's name before it first set page_state. Mods read it from the
    // environment (an explicit RECOMP_LAN_NAME, from the page's address, wins); once only,
    // long before any game code looks.
    static bool name_seen = false;
    if (!name_seen && ps != lan::PAGE_NONE) {
        name_seen = true;
        char name[sizeof g_mb->player_name];
        memcpy(name, (const void *)g_mb->player_name, sizeof name);
        name[sizeof name - 1] = 0;
        const char *given = getenv("RECOMP_LAN_NAME");
        if (name[0] && !(given && *given))
            setenv("RECOMP_LAN_NAME", name, 1);
    }
    if (up) {
        uint32_t vip = ld(&g_mb->vip), mask = ld(&g_mb->netmask);
        if (vip && (vip != g_vip || mask != g_mask)) {
            if (g_vip && vip != g_vip)
                LOGW("lan: the switch gave this browser a new address: %s (was %s)", ipstr(vip).c_str(),
                     ipstr(g_vip).c_str());
            g_vip = vip;
            g_mask = mask;
        }
    }
    if (g_was_online && (!up || gen != g_gen_seen)) {
        g_ctlq.clear(); // frames for the old link: its connections are reset
        reset_remote(0, up ? "the page reconnected to the switch (frames were lost meanwhile)"
                           : "the page lost the switch");
    }
    if (up && (!g_was_online || gen != g_gen_seen))
        LOGW("lan: online: this browser is %s on the server's switch (link %u)", ipstr(g_vip).c_str(), gen);
    else if (!up && g_was_online)
        LOGW("lan: offline: the page is reconnecting to the server's switch");
    if (up != g_was_online || gen != g_gen_seen)
        changed();
    g_was_online = up;
    if (up)
        g_gen_seen = gen;
}
void timers() {
    double now = now_ms();
    // ICMP echo answers that are due.
    if (!g_echoes.empty()) {
        std::vector<Echo> due;
        for (auto it = g_echoes.begin(); it != g_echoes.end();) {
            if (it->due <= now) {
                due.push_back(std::move(*it));
                it = g_echoes.erase(it);
            } else {
                ++it;
            }
        }
        for (Echo &e : due) {
            ++S.echo_answered;
            icmp_deliver(e.from, e.msg.data(), e.msg.size());
        }
    }
    // Connects nobody answered.
    for (Conn *k : g_conns)
        if (k->state == C_SYN_SENT && k->deadline && now >= k->deadline) {
            k->state = C_FAILED;
            ++S.tcp_timeout;
            LOGW("lan: tcp connect to %s:%u: no answer in %.0f s (%s)", ipstr(k->rip).c_str(), k->rport,
                 CONNECT_TMO_MS / 1000.0, k->fail_err == WSAEHOSTUNREACH_ ? "WSAEHOSTUNREACH" : "WSAETIMEDOUT");
            kick_conn(k);
        }
}
double next_timer_ms() {
    double t = 0;
    for (const Echo &e : g_echoes)
        if (!t || e.due < t)
            t = e.due;
    for (Conn *k : g_conns)
        if (k->state == C_SYN_SENT && k->deadline && (!t || k->deadline < t))
            t = k->deadline;
    return t;
}
// Caller holds g_net_m.
void pump() {
    if (!g_mb)
        return;
    link_check();
    if (!g_ctlq.empty())
        ctl_flush();
    uint32_t tail = ld(&g_mb->rx_tail);
    for (int k = 0; k < 512 && tail != ld(&g_mb->rx_head); ++k) {
        rx_frame(rx_slot(tail));
        ++tail;
        st(&g_mb->rx_tail, tail);
    }
    timers();
}

void stats_line(const char *when) {
    LOGW("lan %s: frames out %llu in %llu; udp out %llu in %llu, no port %llu, outside the LAN %llu, queue full "
         "%llu; tcp out %llu in %llu, refused %llu, unreachable %llu, timed out %llu, resets in %llu out %llu, "
         "link resets %llu, bytes out %llu in %llu; icmp out %llu, echo answered %llu; overlapped posted %llu "
         "completed %llu; TX ring full %llu, offline %llu, auto answers held %llu, bad %llu; page: RX full %u",
         when, (unsigned long long)S.tx_frames, (unsigned long long)S.rx_frames, (unsigned long long)S.udp_out,
         (unsigned long long)S.udp_in, (unsigned long long)S.udp_noport, (unsigned long long)S.udp_outside,
         (unsigned long long)S.udp_qfull, (unsigned long long)S.tcp_out, (unsigned long long)S.tcp_in,
         (unsigned long long)S.tcp_refused, (unsigned long long)S.tcp_unreach, (unsigned long long)S.tcp_timeout,
         (unsigned long long)S.tcp_rst_in, (unsigned long long)S.tcp_rst_out, (unsigned long long)S.tcp_reset_link,
         (unsigned long long)S.tcp_bytes_out, (unsigned long long)S.tcp_bytes_in, (unsigned long long)S.icmp_out,
         (unsigned long long)S.echo_answered, (unsigned long long)S.ov_posted, (unsigned long long)S.ov_completed,
         (unsigned long long)S.tx_full, (unsigned long long)S.tx_offline, (unsigned long long)S.auto_held,
         (unsigned long long)S.bad, g_mb ? ld(&g_mb->rx_full_drop) : 0u);
}

// The network thread: wakes on the page's RX bell, or for the next timer.
void net_thread_main() {
    double next_stats = now_ms() + 60000.0;
    for (;;) {
        uint32_t bell = ld(&g_mb->rx_bell);
        double wait = 50.0;
        {
            NetLock l;
            double t = next_timer_ms();
            if (t)
                wait = std::max(1.0, std::min(wait, t - now_ms()));
            if (!g_ctlq.empty())
                wait = std::min(wait, 2.0); // the page frees TX slots without a bell
        }
        if (ld(&g_mb->rx_head) == ld(&g_mb->rx_tail)) {
#ifdef __EMSCRIPTEN__
            emscripten_futex_wait(&g_mb->rx_bell, bell, wait);
#else
            // Natively the page is a test thread; it rings no futex.
            (void)bell;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
        }
        __atomic_add_fetch(&g_mb->kit_alive, 1, __ATOMIC_RELAXED);
        NetLock l;
        pump();
        double now = now_ms();
        if (now >= next_stats) {
            next_stats = now + 60000.0;
            if (g_startups)
                stats_line("stats");
        }
    }
}

// The mailbox, made once and kept: the page holds on to its address.
void mailbox_once() {
    if (g_mb)
        return;
    void *mem = nullptr;
    if (posix_memalign(&mem, 64, lan::MAILBOX_BYTES) != 0 || !mem) {
        LOGW("lan: no memory for the LAN mailbox");
        return;
    }
    memset(mem, 0, lan::MAILBOX_BYTES);
    lan::Mailbox *m = (lan::Mailbox *)mem;
    m->magic = lan::MAGIC;
    m->version = lan::VERSION;
    m->hdr_bytes = lan::HDR_BYTES;
    m->slot_bytes = lan::SLOT_BYTES;
    m->tx_slots = lan::TX_SLOTS;
    m->rx_slots = lan::RX_SLOTS;
    m->tx_off = lan::HDR_BYTES;
    m->rx_off = lan::HDR_BYTES + lan::TX_SLOTS * lan::SLOT_BYTES;
    g_mb = m;
#ifdef __EMSCRIPTEN__
    // As mods/core/nfsc/mp.h publishes the free-roam block: the page's lan.js
    // hands memory and address to its worker. No commas outside a string in
    // the JavaScript, or the macro splits it.
    MAIN_THREAD_ASYNC_EM_ASM({
        var box = {};
        box.memory = wasmMemory;
        box.ptr = $0;
        globalThis.recompLanMailbox = box;
        globalThis.dispatchEvent(new Event("recomp-lan"));
    }, (uint32_t)(uintptr_t)m);
#endif
    LOGW("lan: virtual LAN mailbox at %p (%u bytes)", (void *)m, lan::MAILBOX_BYTES);
}
void set_want(uint32_t on) {
    if (!g_mb || ld(&g_mb->want) == on)
        return;
    st(&g_mb->want, on);
    __atomic_add_fetch(&g_mb->tx_bell, 1, __ATOMIC_SEQ_CST);
    notify_word(&g_mb->tx_bell);
}

// ---------------------------------------------------------------------------
// Waiting, the kit's way. g_net_m is never held here.
// ---------------------------------------------------------------------------
// Call before looking at the state a wait depends on: a change after the look
// then leaves the event set and the wait returns at once.
void wait_prepare() {
    if (g_change_ev)
        win32_reset_event(g_change_ev);
}
// Up to ms (one slice at most) for something on the LAN to change.
void wait_change(uint32_t ms) {
    ms = std::max(1u, std::min(ms, WAIT_SLICE_MS));
    if (!g_change_ev) {
        guest_sleep_ms(ms);
        return;
    }
    g_waiters.fetch_add(1);
    guest_wait_objects(&g_change_ev, 1, false, ms);
    g_waiters.fetch_sub(1);
}
double deadline_of(uint32_t ms) {
    return ms ? now_ms() + ms : 0.0;
}
// Milliseconds to wait now toward an absolute deadline (0 = none).
uint32_t remaining(double deadline) {
    if (!deadline)
        return WAIT_SLICE_MS;
    double r = deadline - now_ms();
    return r <= 0 ? 0 : (uint32_t)std::ceil(r);
}

// ---------------------------------------------------------------------------
// The calls. Every shim first answers as the kit always did when LAN mode is
// off.
// ---------------------------------------------------------------------------
void legacy(X86 *c, const char *name) {
    char key[64];
    snprintf(key, sizeof key, "WS2_32.dll!%s", name);
    log_once(key, "unimplemented import %s: returning 0", key);
    set_eax(c, 0);
}
#define LAN_OR_LEGACY(name)                                                                                        \
    do {                                                                                                           \
        if (!lan_on()) {                                                                                           \
            legacy(c, name);                                                                                       \
            return;                                                                                                \
        }                                                                                                          \
    } while (0)
#define FAIL(e)                                                                                                    \
    do {                                                                                                           \
        set_err(e);                                                                                                \
        set_eax(c, SOCKET_ERROR_);                                                                                 \
        return;                                                                                                    \
    } while (0)
#define NEED_STARTUP()                                                                                             \
    do {                                                                                                           \
        if (!g_startups)                                                                                           \
            FAIL(WSANOTINITIALISED_);                                                                              \
    } while (0)

// Guest SOCKADDR_IN: u16 family, u16 port (big-endian), u32 address (network
// order), 8 bytes the stack ignores (DirtySock keeps a time stamp there).
uint32_t sa_in(uint32_t p, uint32_t len, uint32_t *ip, uint16_t *port) {
    if (!p || (int32_t)len < 16 || !gm_valid(p, 16))
        return WSAEFAULT_;
    if (rd16(p) != 2)
        return WSAEAFNOSUPPORT_;
    *port = (uint16_t)(rd8(p + 2) << 8 | rd8(p + 3));
    *ip = __builtin_bswap32(rd32(p + 4));
    return 0;
}
// An optional out-address with its length word.
uint32_t sa_out(uint32_t p, uint32_t plen, uint32_t ip, uint16_t port) {
    if (!p)
        return 0;
    if (!plen || !gm_valid(plen, 4) || (int32_t)rd32(plen) < 16 || !gm_valid(p, 16))
        return WSAEFAULT_;
    write_sockaddr(p, ip, port);
    wr32(plen, 16);
    return 0;
}

// The address this host uses toward dst: a host's routing answer.
uint32_t route_from(uint32_t dst) {
    if (is_loop(dst))
        return 0x7f000001u;
    return g_vip;
}

uint32_t implicit_bind(Sock &s) {
    if (s.bound)
        return 0;
    if (s.type == 3) { // a raw socket has no port
        s.bound = true;
        return 0;
    }
    uint16_t p = ephemeral(s.type == 1);
    if (!p)
        return WSAENOBUFS_;
    s.bound = true;
    s.laddr = 0;
    s.lport = p;
    if (s.state == ST_NEW)
        s.state = ST_BOUND;
    return 0;
}

// The outcome of a TCP connect, as select, SO_ERROR and getpeername see it.
void conn_update(Sock &s) {
    if (s.type != 1 || s.state != ST_CONNECTING || !s.conn)
        return;
    Conn *k = s.conn;
    if (k->state == C_EST) {
        s.state = ST_CONNECTED;
    } else if (k->state == C_REFUSED || k->state == C_FAILED) {
        s.state = ST_FAILED;
        s.pending_err = k->state == C_REFUSED ? WSAECONNREFUSED_ : k->fail_err;
    }
}

void sclose(Sock &s, bool abortive) {
    Conn *k = s.conn;
    if (k) {
        if (abortive || k->state == C_SYN_SENT)
            send_rst(k);
        else
            send_fin(k);
        conn_free(k);
        s.conn = nullptr;
    }
    while (!s.accq.empty()) { // connections nobody accepted are reset
        Conn *q = s.accq.front();
        s.accq.pop_front();
        send_rst(q);
        conn_free(q);
    }
    abort_pending(s);
    s = Sock();
    changed();
}

void w_WSAStartup(X86 *c) {
    LAN_OR_LEGACY("WSAStartup");
    uint32_t req = arg(c, 0) & 0xffff, p = arg(c, 1);
    uint8_t maj = (uint8_t)req, min = (uint8_t)(req >> 8);
    if (!maj) {
        set_eax(c, WSAVERNOTSUPPORTED_);
        return;
    }
    if (!p || !gm_valid(p, 400)) {
        set_eax(c, WSAEFAULT_);
        return;
    }
    uint16_t ver = (maj > 2 || (maj == 2 && min >= 2)) ? 0x0202 : (uint16_t)req;
    memset(g_mem + p, 0, 400);
    wr16(p, ver);
    wr16(p + 2, 0x0202);
    gm_put_str(p + 4, "WinSock 2.0", 257);
    gm_put_str(p + 4 + 257, "Running", 129);
    bool first;
    {
        NetLock l;
        mailbox_once();
        if (!g_change_ev)
            g_change_ev = win32_create_event(true, false);
        if (g_mb && !g_thread_started) {
            g_thread_started = true;
            std::thread(net_thread_main).detach();
        }
        first = g_startups++ == 0;
        if (first)
            set_want(1);
    }
    if (first && g_mb) {
        // A network session starts: wait (descheduled) for the page to be on the
        // switch, so the game's first look at its own address finds it. At most
        // 5 s; a page that never attached is waited for once.
        double t0 = now_ms();
        for (;;) {
            uint32_t ps = ld(&g_mb->page_state);
            if (ps == lan::PAGE_ONLINE || ps == lan::PAGE_OFFLINE)
                break;
            if (ps == lan::PAGE_NONE && g_waited_unattached)
                break;
            if (now_ms() - t0 >= 5000.0) {
                if (ps == lan::PAGE_NONE)
                    g_waited_unattached = true;
                break;
            }
            guest_sleep_ms(10);
        }
        NetLock l;
        pump();
        uint32_t ps = ld(&g_mb->page_state);
        if (ps == lan::PAGE_ONLINE)
            LOGW("lan: WSAStartup(%u.%u): online as %s (%.0f ms)", maj, min, ipstr(g_vip).c_str(), now_ms() - t0);
        else
            LOGW("lan: WSAStartup(%u.%u): the page is %s: sockets work, the switch is out of reach for now", maj,
                 min, ps == lan::PAGE_NONE ? "not attached (lan.js missing?)" : "not on the switch");
    }
    set_eax(c, 0);
}

void w_WSACleanup(X86 *c) {
    LAN_OR_LEGACY("WSACleanup");
    NetLock l;
    if (!g_startups)
        FAIL(WSANOTINITIALISED_);
    if (--g_startups == 0) {
        int n = 0;
        for (int i = 1; i < NSOCK; ++i)
            if (g_s[i].used) {
                sclose(g_s[i], true);
                ++n;
            }
        if (n)
            LOGW("lan: WSACleanup closed %d socket(s)", n);
        set_want(0);
        stats_line("session end");
    }
    set_eax(c, 0);
}

void w_WSAGetLastError(X86 *c) {
    LAN_OR_LEGACY("WSAGetLastError");
    set_eax(c, t_wsa_error);
}
void w_WSASetLastError(X86 *c) {
    set_err(arg(c, 0));
    set_eax(c, 0);
}

void w_socket(X86 *c) {
    LAN_OR_LEGACY("socket");
    uint32_t af = arg(c, 0), type = arg(c, 1), proto = arg(c, 2);
    NetLock l;
    NEED_STARTUP();
    if (af != 2)
        FAIL(WSAEAFNOSUPPORT_);
    if (type == 1) {
        if (proto && proto != 6)
            FAIL(WSAEPROTONOSUPPORT_);
    } else if (type == 2) {
        if (proto && proto != 17)
            FAIL(WSAEPROTONOSUPPORT_);
    } else if (type == 3) {
        if (proto != 1)
            FAIL(WSAEPROTONOSUPPORT_); // raw: ICMP only
    } else {
        FAIL(WSAESOCKTNOSUPPORT_);
    }
    Sock *s = salloc();
    if (!s)
        FAIL(WSAEMFILE_);
    s->type = (int)type;
    set_eax(c, shandle(s));
}

void w_closesocket(X86 *c) {
    LAN_OR_LEGACY("closesocket");
    NetLock l;
    NEED_STARTUP();
    Sock *s = sget(arg(c, 0));
    if (!s)
        FAIL(WSAENOTSOCK_);
    sclose(*s, s->linger_on && !s->linger_s);
    set_eax(c, 0);
}

void w_bind(X86 *c) {
    LAN_OR_LEGACY("bind");
    NetLock l;
    NEED_STARTUP();
    Sock *s = sget(arg(c, 0));
    if (!s)
        FAIL(WSAENOTSOCK_);
    uint32_t ip;
    uint16_t port;
    if (uint32_t e = sa_in(arg(c, 1), arg(c, 2), &ip, &port))
        FAIL(e);
    if (s->bound)
        FAIL(WSAEINVAL_);
    if (ip && !own_ip(ip) && !is_loop(ip))
        FAIL(WSAEADDRNOTAVAIL_);
    bool tcp = s->type == 1, reuse_all;
    if (s->type == 3) { // a raw socket has an address but no port
        port = 0;
    } else if (!port) {
        port = ephemeral(tcp);
        if (!port)
            FAIL(WSAENOBUFS_);
    } else if (port_used(tcp, port, ip, s, &reuse_all) && !(s->so_reuse && reuse_all)) {
        LOGW("lan: bind %s %s:%u -> WSAEADDRINUSE", tcp ? "tcp" : "udp", ipstr(ip).c_str(), port);
        FAIL(WSAEADDRINUSE_);
    }
    s->bound = true;
    s->laddr = ip;
    s->lport = port;
    if (s->state == ST_NEW)
        s->state = ST_BOUND;
    set_eax(c, 0);
}

void w_listen(X86 *c) {
    LAN_OR_LEGACY("listen");
    NetLock l;
    NEED_STARTUP();
    Sock *s = sget(arg(c, 0));
    if (!s)
        FAIL(WSAENOTSOCK_);
    int bl = (int)arg(c, 1);
    if (s->type != 1)
        FAIL(WSAEOPNOTSUPP_);
    if (!s->bound)
        FAIL(WSAEINVAL_);
    if (s->state == ST_CONNECTED || s->state == ST_CONNECTING)
        FAIL(WSAEISCONN_);
    if (s->state != ST_LISTEN) {
        s->backlog = std::max(1, std::min(bl, 128));
        s->state = ST_LISTEN;
        LOGW("lan: listening on tcp %s:%u (backlog %d)", ipstr(s->laddr).c_str(), s->lport, s->backlog);
    }
    set_eax(c, 0);
}

void w_accept(X86 *c) {
    LAN_OR_LEGACY("accept");
    uint32_t h = arg(c, 0), pa = arg(c, 1), pl = arg(c, 2);
    for (bool first = true;; first = false) {
        wait_prepare();
        uint32_t wait_ms;
        {
            NetLock l;
            NEED_STARTUP();
            pump();
            Sock *ls = sget(h);
            if (!ls)
                FAIL(first ? WSAENOTSOCK_ : WSAEINTR_); // later: closed while we waited
            if (ls->state != ST_LISTEN)
                FAIL(WSAEINVAL_);
            if (pa && (!pl || !gm_valid(pl, 4) || (int32_t)rd32(pl) < 16))
                FAIL(WSAEFAULT_);
            if (!ls->accq.empty()) {
                Sock *ns = salloc();
                if (!ns)
                    FAIL(WSAEMFILE_);
                ls = sget(h);
                Conn *k = ls->accq.front();
                ls->accq.pop_front();
                k->listener = -1;
                k->sock = (int)(ns - g_s);
                ns->type = 1;
                ns->state = ST_CONNECTED;
                ns->bound = true;
                ns->laddr = k->lip;
                ns->lport = k->lport;
                ns->has_peer = true;
                ns->raddr = k->rip;
                ns->rport = k->rport;
                ns->conn = k;
                ns->nb = ls->nb; // Windows: properties of the listener
                ns->so_keepalive = ls->so_keepalive;
                ns->so_oobinline = ls->so_oobinline;
                sa_out(pa, pl, k->rip, k->rport);
                set_eax(c, shandle(ns));
                return;
            }
            if (ls->nb)
                FAIL(WSAEWOULDBLOCK_);
            wait_ms = WAIT_SLICE_MS;
        }
        wait_change(wait_ms);
    }
}

// Remote: the SYN goes out and the answer comes later. This browser's own
// address and 127/8 are answered here; an address off the switch fails after
// the connect timeout, as a real stack's would.
uint32_t start_connect(Sock &s, uint32_t ip, uint16_t port) {
    if (s.conn) { // an earlier attempt
        conn_free(s.conn);
        s.conn = nullptr;
    }
    if (!s.bound) {
        uint16_t p = ephemeral(true);
        if (!p)
            return WSAENOBUFS_;
        s.bound = true;
        s.laddr = 0;
        s.lport = p;
    }
    bool loop = is_loop(ip), self = own_ip(ip);
    uint32_t lip = loop ? 0x7f000001u : g_vip;
    uint32_t cid;
    do
        cid = rand32();
    while (!cid);
    s.laddr = lip;
    s.has_peer = true;
    s.raddr = ip;
    s.rport = port;
    s.state = ST_CONNECTING;
    if (loop || self) {
        int li = find_listener(ip, port);
        Conn *a = conn_new(lip, s.lport, ip, port, cid, C_EST);
        a->local = true;
        a->sock = (int)(&s - g_s);
        s.conn = a;
        if (li < 0 || (int)g_s[li].accq.size() >= g_s[li].backlog + 4) {
            ++S.tcp_refused;
            a->state = C_REFUSED;
            a->fail_err = WSAECONNREFUSED_;
            return 0;
        }
        Conn *z = conn_new(ip, port, lip, s.lport, cid, C_EST);
        z->local = true;
        a->peer = z;
        z->peer = a;
        acc_push(li, z);
        return 0;
    }
    Conn *a = conn_new(lip, s.lport, ip, port, cid, C_SYN_SENT);
    a->sock = (int)(&s - g_s);
    a->deadline = now_ms() + CONNECT_TMO_MS;
    s.conn = a;
    if (!online_locked() || !in_subnet(ip)) {
        ++S.tcp_unreach;
        a->fail_err = WSAEHOSTUNREACH_;
        LOGW("lan: tcp connect %s:%u -> %s (WSAEHOSTUNREACH in %.0f s)", ipstr(ip).c_str(), port,
             online_locked() ? "outside the virtual LAN" : "the page is not on the switch", CONNECT_TMO_MS / 1000.0);
        return 0;
    }
    a->fail_err = WSAETIMEDOUT_;
    if (!tx(lan::KIND_TCP, ip, s.lport, port, lan::TCP_SYN, 64, cid, nullptr, 0))
        return 0; // the TX ring is full: the attempt times out, as a lost SYN would
    ++S.tcp_out;
    LOGW("lan: tcp connect %s:%u -> %s:%u", ipstr(lip).c_str(), s.lport, ipstr(ip).c_str(), port);
    return 0;
}

void w_connect(X86 *c) {
    LAN_OR_LEGACY("connect");
    uint32_t h = arg(c, 0);
    {
        NetLock l;
        NEED_STARTUP();
        Sock *s = sget(h);
        if (!s)
            FAIL(WSAENOTSOCK_);
        uint32_t ip;
        uint16_t port;
        if (uint32_t e = sa_in(arg(c, 1), arg(c, 2), &ip, &port))
            FAIL(e);
        if (s->type != 1) {
            // UDP (and raw): a default destination, no packet. A local-address
            // probe ("connect toward an address, then getsockname") comes here.
            if (!ip && !port) {
                s->has_peer = false;
                set_eax(c, 0);
                return;
            }
            if (uint32_t e = implicit_bind(*s))
                FAIL(e);
            s->has_peer = true;
            s->raddr = ip;
            s->rport = port;
            set_eax(c, 0);
            return;
        }
        if (s->state == ST_CONNECTING)
            FAIL(WSAEALREADY_);
        if (s->state == ST_CONNECTED)
            FAIL(WSAEISCONN_);
        if (s->state == ST_LISTEN)
            FAIL(WSAEINVAL_);
        if (!ip || ip == lan::BROADCAST || !port)
            FAIL(WSAEADDRNOTAVAIL_);
        if (uint32_t e = start_connect(*s, ip, port))
            FAIL(e);
        // Windows: a non-blocking connect never completes inside the call; the
        // outcome comes through select (write or except set) and SO_ERROR.
        if (s->nb)
            FAIL(WSAEWOULDBLOCK_);
    }
    // A blocking connect waits for the outcome, descheduled.
    for (;;) {
        wait_prepare();
        {
            NetLock l;
            pump();
            Sock *s = sget(h);
            if (!s)
                FAIL(WSAEINTR_);
            conn_update(*s);
            if (s->state == ST_CONNECTED) {
                set_eax(c, 0);
                return;
            }
            if (s->state == ST_FAILED) {
                uint32_t e = s->pending_err;
                s->pending_err = 0;
                s->state = ST_BOUND;
                s->has_peer = false;
                if (s->conn) {
                    conn_free(s->conn);
                    s->conn = nullptr;
                }
                FAIL(e);
            }
        }
        wait_change(WAIT_SLICE_MS);
    }
}

// One send of up to len bytes; *sent says how many went.
uint32_t send_one(Sock &s, const uint8_t *buf, uint32_t len, uint32_t ip, uint16_t port, bool has_addr,
                  uint32_t *sent) {
    *sent = 0;
    if (s.type == 1) {
        conn_update(s);
        if (s.state != ST_CONNECTED || !s.conn)
            return WSAENOTCONN_;
        if (s.shut_wr)
            return WSAESHUTDOWN_;
        Conn *k = s.conn;
        if (k->rx_rst || k->rst_sent || k->tx_fin)
            return WSAECONNRESET_;
        if (k->local) {
            Conn *pk = k->peer;
            if (!pk || pk->rx_rst)
                return WSAECONNRESET_;
            size_t room = pk->rlen() < LOCAL_CAP ? LOCAL_CAP - pk->rlen() : 0;
            uint32_t m = (uint32_t)std::min<size_t>(len, room);
            if (!m && len)
                return WSAEWOULDBLOCK_;
            rb_put(pk, buf, m, LOCAL_CAP);
            kick_conn(pk);
            *sent = m;
            return 0;
        }
        uint32_t fr = online_locked() ? tx_free() : 0, slots = fr > TX_RESERVE ? fr - TX_RESERVE : 0;
        uint32_t off = 0;
        while (off < len && slots) {
            uint32_t m = std::min(len - off, TCP_CHUNK);
            if (!tx(lan::KIND_TCP, k->rip, k->lport, k->rport, lan::TCP_DATA, 64, k->cid, buf + off, m))
                break;
            off += m;
            --slots;
        }
        if (!off && len) {
            if (!online_locked()) {
                // The link went: its connections are being reset.
                return WSAECONNRESET_;
            }
            return WSAEWOULDBLOCK_;
        }
        S.tcp_bytes_out += off;
        *sent = off;
        return 0;
    }
    // Datagram or raw.
    if (!has_addr) {
        if (!s.has_peer)
            return WSAENOTCONN_;
        ip = s.raddr;
        port = s.rport;
    }
    if (s.shut_wr)
        return WSAESHUTDOWN_;
    if (s.type == 2 && len > 65507)
        return WSAEMSGSIZE_;
    if (s.type == 2 && is_bcast(ip) && !s.so_broadcast)
        return WSAEACCES_;
    if (uint32_t e = implicit_bind(s))
        return e;
    *sent = len;
    bool loop = is_loop(ip), self = own_ip(ip), bc = is_bcast(ip);
    uint32_t src = loop ? 0x7f000001u : g_vip;
    if (s.type == 3) {
        // ICMP (a ping): an echo request is answered here after a fixed round
        // trip, wherever it went - a latency check only needs an answer.
        ++S.icmp_out;
        if (len >= 8 && buf[0] == 8 && !bc) {
            Echo e;
            e.due = now_ms() + ping_ms();
            e.from = ip;
            e.msg.assign(buf, buf + len);
            e.msg[0] = 0; // echo reply
            e.msg[1] = 0;
            icmp_cksum(e.msg.data(), e.msg.size());
            g_echoes.push_back(std::move(e));
        }
        return 0;
    }
    if (len > UDP_MAX) {
        log_once("lan udp oversize", "lan: a %u-byte datagram to %s:%u is larger than %u bytes: dropped", len,
                 ipstr(ip).c_str(), port, UDP_MAX);
        return 0;
    }
    ++S.udp_out;
    if (loop || self) {
        if (!udp_deliver(src, s.lport, ip, port, buf, len, false))
            dg_push((int)(&s - g_s), ip, port, true, nullptr, 0); // port unreachable
    } else if (bc) {
        udp_deliver(g_vip, s.lport, lan::BROADCAST, port, buf, len, true); // our own copy, as on Windows
        tx(lan::KIND_UDP, lan::BROADCAST, s.lport, port, lan::UDP_DATA, (uint8_t)s.ip_ttl, 0, buf, len);
    } else if (in_subnet(ip)) {
        if (tx(lan::KIND_UDP, ip, s.lport, port, lan::UDP_DATA, (uint8_t)s.ip_ttl, 0, buf, len))
            sent_note(s, ip, port);
    } else {
        ++S.udp_outside;
        char key[48];
        snprintf(key, sizeof key, "lan udp outside %08x", ip);
        log_once(key, "lan: datagram to %s:%u dropped: outside the virtual LAN", ipstr(ip).c_str(), port);
    }
    return 0;
}

void do_send(X86 *c, bool has_addr) {
    uint32_t h = arg(c, 0), buf = arg(c, 1), len = arg(c, 2), fl = arg(c, 3);
    uint32_t total = 0;
    double dl = 0;
    bool first = true;
    for (;;) {
        if (!first)
            wait_prepare();
        {
            NetLock l;
            NEED_STARTUP();
            pump();
            Sock *s = sget(h);
            if (!s)
                FAIL(first ? WSAENOTSOCK_ : WSAEINTR_);
            if ((int32_t)len < 0 || (len && !gm_valid(buf, len)))
                FAIL(WSAEFAULT_);
            uint32_t ip = 0;
            uint16_t port = 0;
            if (has_addr && s->type != 1)
                if (uint32_t e = sa_in(arg(c, 4), arg(c, 5), &ip, &port))
                    FAIL(e);
            if (fl & ~(MSG_OOB_ | 4u))
                log_once("lan send flags", "lan: send flags 0x%x ignored", fl);
            if (first)
                dl = deadline_of(s->sndtimeo);
            first = false;
            uint32_t sent = 0;
            uint32_t e = send_one(*s, g_mem + buf + total, len - total, ip, port, has_addr && s->type != 1, &sent);
            if (!e) {
                total += sent;
                if (s->type != 1 || total >= len || s->nb) {
                    set_eax(c, total);
                    return;
                }
                continue; // a blocking stream send sends everything
            }
            if (e != WSAEWOULDBLOCK_ || s->nb) {
                if (total) {
                    set_eax(c, total);
                    return;
                }
                FAIL(e);
            }
            if (dl && now_ms() >= dl) {
                if (total) {
                    set_eax(c, total);
                    return;
                }
                FAIL(WSAETIMEDOUT_);
            }
        }
        wait_change(remaining(dl));
    }
}
void w_send(X86 *c) {
    LAN_OR_LEGACY("send");
    do_send(c, false);
}
void w_sendto(X86 *c) {
    LAN_OR_LEGACY("sendto");
    do_send(c, true);
}

// Whether a receive on s can only fail at once (and with what), before any data.
uint32_t recv_precheck(Sock &s) {
    if (s.type == 1) {
        conn_update(s);
        if (s.state != ST_CONNECTED)
            return WSAENOTCONN_;
        if (s.shut_rd)
            return WSAESHUTDOWN_;
        return 0;
    }
    if (!s.bound)
        return WSAEINVAL_;
    if (s.shut_rd)
        return WSAESHUTDOWN_;
    return 0;
}

void do_recv(X86 *c, bool has_from) {
    uint32_t h = arg(c, 0), buf = arg(c, 1), len = arg(c, 2), fl = arg(c, 3);
    uint32_t pf = has_from ? arg(c, 4) : 0, pfl = has_from ? arg(c, 5) : 0;
    double dl = 0;
    bool first = true;
    for (;;) {
        if (!first)
            wait_prepare();
        {
            NetLock l;
            NEED_STARTUP();
            pump();
            Sock *s = sget(h);
            if (!s)
                FAIL(first ? WSAENOTSOCK_ : WSAEINTR_);
            if ((int32_t)len < 0 || (len && !gm_valid(buf, len)))
                FAIL(WSAEFAULT_);
            if (pf && (!pfl || !gm_valid(pfl, 4) || (int32_t)rd32(pfl) < 16 || !gm_valid(pf, 16)))
                FAIL(WSAEFAULT_);
            if ((fl & MSG_OOB_) && s->type != 1)
                FAIL(WSAEOPNOTSUPP_);
            if (uint32_t e = recv_precheck(*s))
                FAIL(e);
            if (first)
                dl = deadline_of(s->rcvtimeo);
            first = false;
            // Receives already posted overlapped come first, as in a kernel's queue.
            RecvResult r;
            if (s->pend.empty() && try_recv(*s, Bufs{{buf, len}}, (fl & MSG_PEEK_) != 0, &r)) {
                if (pf && s->type != 1 && (!r.err || r.err == WSAEMSGSIZE_))
                    sa_out(pf, pfl, r.ip, r.port);
                if (r.err)
                    FAIL(r.err);
                set_eax(c, r.bytes);
                return;
            }
            if (s->nb)
                FAIL(WSAEWOULDBLOCK_);
            if (dl && now_ms() >= dl)
                FAIL(WSAETIMEDOUT_);
        }
        wait_change(remaining(dl));
    }
}
void w_recv(X86 *c) {
    LAN_OR_LEGACY("recv");
    do_recv(c, false);
}
void w_recvfrom(X86 *c) {
    LAN_OR_LEGACY("recvfrom");
    do_recv(c, true);
}

void w_shutdown(X86 *c) {
    LAN_OR_LEGACY("shutdown");
    NetLock l;
    NEED_STARTUP();
    Sock *s = sget(arg(c, 0));
    if (!s)
        FAIL(WSAENOTSOCK_);
    uint32_t how = arg(c, 1);
    if (how > 2)
        FAIL(WSAEINVAL_);
    if (s->type == 1) {
        conn_update(*s);
        if (s->state != ST_CONNECTED)
            FAIL(WSAENOTCONN_);
        if (how != 0 && s->conn)
            send_fin(s->conn);
    }
    // Datagram sockets only stop sending or receiving, no error.
    if (how != 1)
        s->shut_rd = true;
    if (how != 0)
        s->shut_wr = true;
    changed();
    set_eax(c, 0);
}

void getname(X86 *c, bool peer) {
    NetLock l;
    NEED_STARTUP();
    pump();
    Sock *s = sget(arg(c, 0));
    if (!s)
        FAIL(WSAENOTSOCK_);
    uint32_t p = arg(c, 1), pl = arg(c, 2);
    if (!p || !pl || !gm_valid(pl, 4) || (int32_t)rd32(pl) < 16 || !gm_valid(p, 16))
        FAIL(WSAEFAULT_);
    uint32_t ip = 0;
    uint16_t port = 0;
    if (peer) {
        if (s->type == 1) {
            conn_update(*s);
            // A reset connection is no longer connected: how DirtySock notices a drop.
            if (s->state != ST_CONNECTED || !s->conn || s->conn->rx_rst)
                FAIL(WSAENOTCONN_);
        } else if (!s->has_peer) {
            FAIL(WSAENOTCONN_);
        }
        ip = s->raddr;
        port = s->rport;
    } else {
        if (!s->bound)
            FAIL(WSAEINVAL_);
        ip = s->laddr;
        port = s->lport;
        // A wildcard bind names its interface once the socket has a peer.
        if (!ip && s->has_peer)
            ip = route_from(s->raddr);
    }
    write_sockaddr(p, ip, port);
    wr32(pl, 16);
    set_eax(c, 0);
}
void w_getsockname(X86 *c) {
    LAN_OR_LEGACY("getsockname");
    getname(c, false);
}
void w_getpeername(X86 *c) {
    LAN_OR_LEGACY("getpeername");
    getname(c, true);
}

uint32_t optint(uint32_t p, uint32_t l, int *v) {
    if (!p || (int32_t)l < 1 || !gm_valid(p, l < 4 ? l : 4))
        return WSAEFAULT_;
    *v = l >= 4 ? (int)rd32(p) : (int)rd8(p);
    return 0;
}
void w_setsockopt(X86 *c) {
    LAN_OR_LEGACY("setsockopt");
    NetLock l;
    NEED_STARTUP();
    Sock *s = sget(arg(c, 0));
    if (!s)
        FAIL(WSAENOTSOCK_);
    uint32_t lvl = arg(c, 1), opt = arg(c, 2), p = arg(c, 3), len = arg(c, 4);
    int v = 0;
    if (lvl == 0xffff) {
        switch (opt) {
        case 0x0004: // SO_REUSEADDR
            if (uint32_t e = optint(p, len, &v))
                FAIL(e);
            s->so_reuse = v != 0;
            set_eax(c, 0);
            return;
        case 0xfffffffbu: // SO_EXCLUSIVEADDRUSE
            if (uint32_t e = optint(p, len, &v))
                FAIL(e);
            set_eax(c, 0);
            return;
        case 0x0020: // SO_BROADCAST
            if (uint32_t e = optint(p, len, &v))
                FAIL(e);
            if (s->type == 1)
                FAIL(WSAENOPROTOOPT_);
            s->so_broadcast = v != 0;
            set_eax(c, 0);
            return;
        case 0x0008: // SO_KEEPALIVE
            if (uint32_t e = optint(p, len, &v))
                FAIL(e);
            s->so_keepalive = v != 0;
            set_eax(c, 0);
            return;
        case 0x0010: // SO_DONTROUTE
            if (uint32_t e = optint(p, len, &v))
                FAIL(e);
            s->so_dontroute = v != 0;
            set_eax(c, 0);
            return;
        case 0x0100: // SO_OOBINLINE
            if (uint32_t e = optint(p, len, &v))
                FAIL(e);
            s->so_oobinline = v != 0;
            set_eax(c, 0);
            return;
        case 0x0080: // SO_LINGER: struct linger { u_short l_onoff, l_linger }
            if (!p || (int32_t)len < 4 || !gm_valid(p, 4))
                FAIL(WSAEFAULT_);
            if (s->type != 1)
                FAIL(WSAENOPROTOOPT_);
            s->linger_on = rd16(p) != 0;
            s->linger_s = rd16(p + 2);
            set_eax(c, 0);
            return;
        case 0xffffff7fu: // SO_DONTLINGER
            if (uint32_t e = optint(p, len, &v))
                FAIL(e);
            if (s->type != 1)
                FAIL(WSAENOPROTOOPT_);
            if (v)
                s->linger_on = false;
            set_eax(c, 0);
            return;
        case 0x1001: // SO_SNDBUF
        case 0x1002: // SO_RCVBUF
            if (uint32_t e = optint(p, len, &v))
                FAIL(e);
            (opt == 0x1001 ? s->sndbuf : s->rcvbuf) = v;
            set_eax(c, 0);
            return;
        case 0x1005: // SO_SNDTIMEO: DWORD ms
        case 0x1006: // SO_RCVTIMEO
            if (uint32_t e = optint(p, len, &v))
                FAIL(e);
            (opt == 0x1005 ? s->sndtimeo : s->rcvtimeo) = (uint32_t)v;
            set_eax(c, 0);
            return;
        default:
            break;
        }
    } else if (lvl == 0) { // IPPROTO_IP
        if (opt == 4) {    // IP_TTL (ProtoPing sets it per send)
            if (uint32_t e = optint(p, len, &v))
                FAIL(e);
            if (v < 0 || v > 255)
                FAIL(WSAEINVAL_);
            s->ip_ttl = v;
            set_eax(c, 0);
            return;
        }
        if (opt == 3) { // IP_TOS
            if (uint32_t e = optint(p, len, &v))
                FAIL(e);
            set_eax(c, 0);
            return;
        }
    } else if (lvl == 6 && opt == 1) { // IPPROTO_TCP, TCP_NODELAY
        if (uint32_t e = optint(p, len, &v))
            FAIL(e);
        if (s->type != 1)
            FAIL(WSAENOPROTOOPT_);
        s->nodelay = v != 0;
        set_eax(c, 0);
        return;
    }
    char key[64];
    snprintf(key, sizeof key, "lan setsockopt %x %x", lvl, opt);
    log_once(key, "lan: setsockopt(level 0x%x, option 0x%x) is not supported: WSAENOPROTOOPT", lvl, opt);
    FAIL(lvl == 0xffff || lvl == 0 || lvl == 6 ? WSAENOPROTOOPT_ : WSAEINVAL_);
}

void w_getsockopt(X86 *c) {
    LAN_OR_LEGACY("getsockopt");
    NetLock l;
    NEED_STARTUP();
    pump();
    Sock *s = sget(arg(c, 0));
    if (!s)
        FAIL(WSAENOTSOCK_);
    uint32_t lvl = arg(c, 1), opt = arg(c, 2), p = arg(c, 3), pl = arg(c, 4);
    if (!p || !pl || !gm_valid(pl, 4) || (int32_t)rd32(pl) < 1)
        FAIL(WSAEFAULT_);
    int32_t cap = (int32_t)rd32(pl);
    uint32_t v = 0;
    bool known = true;
    if (lvl == 0xffff) {
        switch (opt) {
        case 0x0002: v = s->state == ST_LISTEN; break; // SO_ACCEPTCONN
        case 0x0004: v = s->so_reuse; break;
        case 0x0008: v = s->so_keepalive; break;
        case 0x0010: v = s->so_dontroute; break;
        case 0x0020: v = s->so_broadcast; break;
        case 0x0100: v = s->so_oobinline; break;
        case 0x0080: // SO_LINGER, 4 bytes
            if (cap < 4 || !gm_valid(p, 4))
                FAIL(WSAEFAULT_);
            wr16(p, s->linger_on);
            wr16(p + 2, s->linger_s);
            wr32(pl, 4);
            set_eax(c, 0);
            return;
        case 0xffffff7fu: v = !s->linger_on; break;
        case 0x1001: v = (uint32_t)s->sndbuf; break;
        case 0x1002: v = (uint32_t)s->rcvbuf; break;
        case 0x1005: v = s->sndtimeo; break;
        case 0x1006: v = s->rcvtimeo; break;
        case 0x1007: // SO_ERROR, then cleared
            conn_update(*s);
            v = s->pending_err;
            s->pending_err = 0;
            break;
        case 0x1008: v = (uint32_t)s->type; break; // SO_TYPE
        default: known = false; break;
        }
    } else if (lvl == 0 && opt == 4) {
        v = (uint32_t)s->ip_ttl;
    } else if (lvl == 6 && opt == 1) {
        v = s->nodelay;
    } else {
        known = false;
    }
    if (!known) {
        char key[64];
        snprintf(key, sizeof key, "lan getsockopt %x %x", lvl, opt);
        log_once(key, "lan: getsockopt(level 0x%x, option 0x%x) is not supported: WSAENOPROTOOPT", lvl, opt);
        FAIL(WSAENOPROTOOPT_);
    }
    // BOOL and int options: Windows takes a 1-byte buffer for a BOOL.
    uint32_t bytes = cap >= 4 ? 4 : 1;
    if (!gm_valid(p, bytes))
        FAIL(WSAEFAULT_);
    if (bytes == 4)
        wr32(p, v);
    else
        wr8(p, (uint8_t)v);
    wr32(pl, bytes);
    set_eax(c, 0);
}

uint32_t fionread(Sock &s) {
    if (s.type == 1)
        return s.conn ? (uint32_t)s.conn->rlen() : 0;
    for (const Dgram &d : s.q) // the size of the next datagram
        if (!d.notice)
            return (uint32_t)d.data.size() + (s.type == 3 ? 20u : 0u);
    return 0;
}
void w_ioctlsocket(X86 *c) {
    LAN_OR_LEGACY("ioctlsocket");
    NetLock l;
    NEED_STARTUP();
    pump();
    Sock *s = sget(arg(c, 0));
    if (!s)
        FAIL(WSAENOTSOCK_);
    uint32_t cmd = arg(c, 1), p = arg(c, 2);
    if (!p || !gm_valid(p, 4))
        FAIL(WSAEFAULT_);
    if (cmd == FIONBIO_) {
        s->nb = rd32(p) != 0;
    } else if (cmd == FIONREAD_) {
        wr32(p, fionread(*s));
    } else if (cmd == SIOCATMARK_) {
        wr32(p, 1); // no out-of-band data
    } else {
        log_once("lan ioctlsocket", "lan: ioctlsocket(0x%08x) is not supported: WSAEINVAL", cmd);
        FAIL(WSAEINVAL_);
    }
    set_eax(c, 0);
}

void w_WSAIoctl(X86 *c) {
    LAN_OR_LEGACY("WSAIoctl");
    NetLock l;
    NEED_STARTUP();
    pump();
    Sock *s = sget(arg(c, 0));
    if (!s)
        FAIL(WSAENOTSOCK_);
    uint32_t code = arg(c, 1), in = arg(c, 2), inl = arg(c, 3), out = arg(c, 4), outl = arg(c, 5),
             pret = arg(c, 6), ov = arg(c, 7);
    if (ov)
        log_once("lan WSAIoctl ov", "lan: WSAIoctl with an OVERLAPPED completes at once");
    auto ret = [&](uint32_t n) {
        if (pret && gm_valid(pret, 4))
            wr32(pret, n);
    };
    if (code == SIO_ROUTING_INTERFACE_QUERY_) {
        uint32_t ip;
        uint16_t port;
        if (uint32_t e = sa_in(in, inl, &ip, &port))
            FAIL(e);
        if (!out || outl < 16 || !gm_valid(out, 16)) {
            ret(16);
            FAIL(WSAEFAULT_);
        }
        uint32_t from = route_from(ip);
        if (!from)
            FAIL(WSAENETUNREACH_);
        write_sockaddr(out, from, 0);
        ret(16);
        set_eax(c, 0);
        return;
    }
    if (code == FIONBIO_) {
        if (!in || inl < 4 || !gm_valid(in, 4))
            FAIL(WSAEFAULT_);
        s->nb = rd32(in) != 0;
        ret(0);
        set_eax(c, 0);
        return;
    }
    if (code == FIONREAD_) {
        if (!out || outl < 4 || !gm_valid(out, 4))
            FAIL(WSAEFAULT_);
        wr32(out, fionread(*s));
        ret(4);
        set_eax(c, 0);
        return;
    }
    if (code == SIO_UDP_CONNRESET_) {
        if (in && inl >= 4 && gm_valid(in, 4))
            s->connreset = rd32(in) != 0;
        ret(0);
        set_eax(c, 0);
        return;
    }
    char key[48];
    snprintf(key, sizeof key, "lan WSAIoctl %08x", code);
    log_once(key, "lan: WSAIoctl(0x%08x) is not supported: WSAEOPNOTSUPP", code);
    FAIL(WSAEOPNOTSUPP_);
}

// select: readiness by Windows' rules. A connecting socket turns writable when
// the connect completes and shows up only in the except set when it fails; a
// listener is readable with a connection to accept; a connected stream is
// readable with data, an end or a reset, and writable while it can send (a
// reset one too: the send then fails at once, as on Windows).
void w_select(X86 *c) {
    LAN_OR_LEGACY("select");
    uint32_t set[3] = {arg(c, 1), arg(c, 2), arg(c, 3)}, tv = arg(c, 4);
    std::vector<uint32_t> hs[3];
    {
        NetLock l;
        NEED_STARTUP();
        uint32_t total = 0;
        for (int k = 0; k < 3; ++k) {
            if (!set[k])
                continue;
            if (!gm_valid(set[k], 4))
                FAIL(WSAEFAULT_);
            uint32_t n = rd32(set[k]);
            if (n > 4096)
                FAIL(WSAEINVAL_);
            if (n && !gm_valid(set[k] + 4, 4 * n))
                FAIL(WSAEFAULT_);
            // Every set is read before any is written back: the same fd_set may
            // be passed twice.
            for (uint32_t j = 0; j < n; ++j)
                hs[k].push_back(rd32(set[k] + 4 + 4 * j));
            total += n;
        }
        if (!total)
            FAIL(WSAEINVAL_);
        for (int k = 0; k < 3; ++k)
            for (uint32_t h : hs[k])
                if (!sget(h))
                    FAIL(WSAENOTSOCK_);
    }
    double dl = 0;
    bool forever = true;
    if (tv) {
        if (!gm_valid(tv, 8))
            FAIL(WSAEFAULT_);
        int32_t sec = std::max(0, (int32_t)rd32(tv)), usec = std::max(0, (int32_t)rd32(tv + 4));
        dl = now_ms() + sec * 1000.0 + usec / 1000.0;
        forever = false;
    }
    bool first = true;
    for (;;) {
        if (!first)
            wait_prepare();
        first = false;
        {
            NetLock l;
            pump();
            std::vector<uint8_t> rdy[3];
            int ready = 0;
            for (int k = 0; k < 3; ++k) {
                for (uint32_t h : hs[k]) {
                    Sock *s = sget(h);
                    if (!s)
                        FAIL(WSAEINTR_); // closed while we waited
                    bool rd = false, wr = false, ex = false;
                    if (s->type == 1) {
                        conn_update(*s);
                        Conn *kk = s->conn;
                        if (s->state == ST_LISTEN) {
                            rd = !s->accq.empty();
                        } else if (s->state == ST_CONNECTED && kk) {
                            rd = kk->rlen() || kk->rx_fin || kk->rx_rst || (kk->local && !kk->peer);
                            if (kk->rx_rst || kk->rst_sent)
                                wr = true;
                            else if (kk->local)
                                wr = kk->peer && kk->peer->rlen() < LOCAL_CAP;
                            else
                                wr = !online_locked() || tx_free() > TX_RESERVE;
                        } else if (s->state == ST_FAILED) {
                            ex = true; // a failed connect: except set only
                        }
                    } else {
                        for (const Dgram &d : s->q)
                            if (!d.notice || s->connreset) {
                                rd = true;
                                break;
                            }
                        wr = true;
                    }
                    uint8_t v = k == 0 ? rd : k == 1 ? wr : ex;
                    rdy[k].push_back(v);
                    ready += v;
                }
            }
            if (ready || (!forever && now_ms() >= dl)) {
                for (int k = 0; k < 3; ++k) {
                    if (!set[k])
                        continue;
                    uint32_t n = 0;
                    for (size_t j = 0; j < hs[k].size(); ++j)
                        if (rdy[k][j])
                            wr32(set[k] + 4 + 4 * n++, hs[k][j]);
                    wr32(set[k], n);
                }
                set_eax(c, (uint32_t)ready);
                return;
            }
        }
        wait_change(forever ? WAIT_SLICE_MS : remaining(dl));
    }
}

// ---------------------------------------------------------------------------
// Overlapped receives and WSA events
// ---------------------------------------------------------------------------
uint32_t read_bufs(uint32_t p, uint32_t n, Bufs *out) {
    if (!n || n > 64 || !p || !gm_valid(p, 8 * n))
        return WSAEFAULT_;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t len = rd32(p + 8 * i), buf = rd32(p + 8 * i + 4); // WSABUF { ULONG len; CHAR *buf; }
        if (len && !gm_valid(buf, len))
            return WSAEFAULT_;
        out->push_back({buf, len});
    }
    return 0;
}

// WSARecv and WSARecvFrom. Without an OVERLAPPED they are recv/recvfrom over
// several buffers. With one, data already here completes the call at once
// (the event is signalled too, as Windows does); otherwise the receive is
// posted and WSA_IO_PENDING returned, and whoever brings data completes it:
// the network thread for frames from the switch, or a guest thread for
// datagrams that stay in this browser.
void do_wsarecv(X86 *c, bool has_from) {
    uint32_t h = arg(c, 0), pbufs = arg(c, 1), nbufs = arg(c, 2), pbytes = arg(c, 3), pflags = arg(c, 4);
    uint32_t from = has_from ? arg(c, 5) : 0, fromlen = has_from ? arg(c, 6) : 0;
    uint32_t ov = arg(c, has_from ? 7 : 5), cr = arg(c, has_from ? 8 : 6);
    if (cr) {
        log_once("lan wsarecv cr", "lan: WSARecv with a completion routine is not supported");
        set_err(WSAEOPNOTSUPP_);
        set_eax(c, SOCKET_ERROR_);
        return;
    }
    double dl = 0;
    bool first = true;
    for (;;) {
        if (!first)
            wait_prepare();
        {
            NetLock l;
            NEED_STARTUP();
            pump();
            Sock *s = sget(h);
            if (!s)
                FAIL(first ? WSAENOTSOCK_ : WSAEINTR_);
            Bufs bufs;
            if (uint32_t e = read_bufs(pbufs, nbufs, &bufs))
                FAIL(e);
            if (!pflags || !gm_valid(pflags, 4))
                FAIL(WSAEFAULT_);
            uint32_t fl = rd32(pflags);
            if (from && (!fromlen || !gm_valid(fromlen, 4) || (int32_t)rd32(fromlen) < 16 || !gm_valid(from, 16)))
                FAIL(WSAEFAULT_);
            if ((fl & MSG_OOB_) && s->type != 1)
                FAIL(WSAEOPNOTSUPP_);
            if (uint32_t e = recv_precheck(*s))
                FAIL(e);
            bool peek = (fl & MSG_PEEK_) != 0;
            if (ov) {
                if (!gm_valid(ov, 20))
                    FAIL(WSAEFAULT_);
                uint32_t ev = rd32(ov + 16);
                // Starting an operation resets its event (the I/O manager does).
                if (ev)
                    win32_reset_event(ev);
                g_done.erase(ov);
                ++S.ov_posted;
                Pending p;
                p.ov = ov;
                p.ev = ev;
                p.bufs = bufs;
                p.from = from;
                p.fromlen = fromlen;
                p.peek = peek;
                RecvResult r;
                if (s->pend.empty() && try_recv(*s, bufs, peek, &r)) {
                    finish_pending((int)(s - g_s), p, r); // also signals the event
                    if (pbytes && gm_valid(pbytes, 4))
                        wr32(pbytes, r.bytes);
                    wr32(pflags, r.flags);
                    if (r.err)
                        FAIL(r.err);
                    set_eax(c, 0);
                    return;
                }
                wr32(ov + 0, STATUS_PENDING_);
                wr32(ov + 4, 0);
                s->pend.push_back(p);
                FAIL(WSA_IO_PENDING_);
            }
            if (first)
                dl = deadline_of(s->rcvtimeo);
            first = false;
            RecvResult r;
            if (s->pend.empty() && try_recv(*s, bufs, peek, &r)) {
                if (from && s->type != 1 && (!r.err || r.err == WSAEMSGSIZE_))
                    sa_out(from, fromlen, r.ip, r.port);
                if (pbytes && gm_valid(pbytes, 4))
                    wr32(pbytes, r.bytes);
                wr32(pflags, r.flags);
                if (r.err)
                    FAIL(r.err);
                set_eax(c, 0);
                return;
            }
            if (s->nb)
                FAIL(WSAEWOULDBLOCK_);
            if (dl && now_ms() >= dl)
                FAIL(WSAETIMEDOUT_);
        }
        wait_change(remaining(dl));
    }
}
void w_WSARecv(X86 *c) {
    LAN_OR_LEGACY("WSARecv");
    do_wsarecv(c, false);
}
void w_WSARecvFrom(X86 *c) {
    LAN_OR_LEGACY("WSARecvFrom");
    do_wsarecv(c, true);
}

// WSASend and WSASendTo (not imported by Carbon; registered in LAN mode for a
// game that asks GetProcAddress): sends here never wait, so an overlapped one
// completes at once.
void do_wsasend(X86 *c, bool has_to) {
    uint32_t h = arg(c, 0), pbufs = arg(c, 1), nbufs = arg(c, 2), psent = arg(c, 3);
    uint32_t to = has_to ? arg(c, 5) : 0, tolen = has_to ? arg(c, 6) : 0;
    uint32_t ov = arg(c, has_to ? 7 : 5), cr = arg(c, has_to ? 8 : 6);
    if (cr) {
        log_once("lan wsasend cr", "lan: WSASend with a completion routine is not supported");
        FAIL(WSAEOPNOTSUPP_);
    }
    NetLock l;
    NEED_STARTUP();
    pump();
    Sock *s = sget(h);
    if (!s)
        FAIL(WSAENOTSOCK_);
    Bufs bufs;
    if (uint32_t e = read_bufs(pbufs, nbufs, &bufs))
        FAIL(e);
    std::vector<uint8_t> data;
    for (auto &b : bufs)
        data.insert(data.end(), g_mem + b.first, g_mem + b.first + b.second);
    uint32_t ip = 0;
    uint16_t port = 0;
    bool has_addr = to && s->type != 1;
    if (has_addr)
        if (uint32_t e = sa_in(to, tolen, &ip, &port))
            FAIL(e);
    uint32_t sent = 0;
    uint32_t e = send_one(*s, data.data(), (uint32_t)data.size(), ip, port, has_addr, &sent);
    if (e)
        FAIL(e);
    if (psent && gm_valid(psent, 4))
        wr32(psent, sent);
    if (ov && gm_valid(ov, 20)) {
        wr32(ov, 0);
        wr32(ov + 4, sent);
        g_done[ov] = {0, sent, 0, (int)(s - g_s)};
        if (uint32_t ev = rd32(ov + 16))
            g_sig.push_back(ev);
    }
    set_eax(c, 0);
}
void w_WSASend(X86 *c) {
    do_wsasend(c, false);
}
void w_WSASendTo(X86 *c) {
    do_wsasend(c, true);
}

bool ov_pending(uint32_t ov, uint32_t *ev) {
    for (int i = 1; i < NSOCK; ++i)
        for (const Pending &p : g_s[i].pend)
            if (p.ov == ov) {
                *ev = p.ev;
                return true;
            }
    return false;
}
void w_WSAGetOverlappedResult(X86 *c) {
    LAN_OR_LEGACY("WSAGetOverlappedResult");
    uint32_t h = arg(c, 0), ov = arg(c, 1), pcb = arg(c, 2), wait = arg(c, 3), pflags = arg(c, 4);
    for (;;) {
        uint32_t ev = 0;
        {
            NetLock l;
            NEED_STARTUP();
            pump();
            if (!sget(h) && !g_done.count(ov)) {
                set_err(WSAENOTSOCK_);
                set_eax(c, 0);
                return;
            }
            if (!ov || !gm_valid(ov, 20) || !pcb || !gm_valid(pcb, 4) || !pflags || !gm_valid(pflags, 4)) {
                set_err(WSAEFAULT_);
                set_eax(c, 0);
                return;
            }
            if (!ov_pending(ov, &ev)) {
                auto it = g_done.find(ov);
                uint32_t err, bytes, flags = 0;
                if (it != g_done.end()) {
                    err = it->second.err;
                    bytes = it->second.bytes;
                    flags = it->second.flags;
                } else if (rd32(ov) == STATUS_PENDING_) {
                    // Not ours and still pending: nothing will complete it.
                    set_err(WSA_IO_INCOMPLETE_);
                    set_eax(c, 0);
                    return;
                } else {
                    bytes = rd32(ov + 4);
                    err = rd32(ov) ? WSAECONNRESET_ : 0;
                }
                wr32(pcb, bytes);
                wr32(pflags, flags);
                if (err) {
                    set_err(err);
                    set_eax(c, 0);
                } else {
                    set_eax(c, 1);
                }
                return;
            }
            if (!wait) {
                set_err(WSA_IO_INCOMPLETE_);
                set_eax(c, 0);
                return;
            }
        }
        // fWait: until the operation's event says it is done.
        if (ev)
            guest_wait_objects(&ev, 1, false, INFINITE_);
        else
            wait_change(WAIT_SLICE_MS);
    }
}

// WSA events are kernel events, manual-reset and unsignalled, so the game may
// wait on them with WaitForMultipleObjects too and mix them with CreateEvent's
// (DirtySock's receive thread waits on one of each).
void w_WSACreateEvent(X86 *c) {
    LAN_OR_LEGACY("WSACreateEvent");
    set_eax(c, win32_create_event(true, false));
}
void w_WSACloseEvent(X86 *c) {
    LAN_OR_LEGACY("WSACloseEvent");
    if (!win32_close_event(arg(c, 0))) {
        set_err(WSA_INVALID_HANDLE_);
        set_eax(c, 0);
        return;
    }
    set_eax(c, 1);
}
void w_WSASetEvent(X86 *c) {
    LAN_OR_LEGACY("WSASetEvent");
    if (!win32_signal_event(arg(c, 0), false)) {
        set_err(WSA_INVALID_HANDLE_);
        set_eax(c, 0);
        return;
    }
    set_eax(c, 1);
}
void w_WSAResetEvent(X86 *c) {
    LAN_OR_LEGACY("WSAResetEvent");
    if (!win32_reset_event(arg(c, 0))) {
        set_err(WSA_INVALID_HANDLE_);
        set_eax(c, 0);
        return;
    }
    set_eax(c, 1);
}
// The guest thread is descheduled until an event is set (by the game, or by
// the network thread completing a receive) or the time is up. Same return
// values as WaitForMultipleObjects; alertable waits have no APCs to run here.
void w_WSAWaitForMultipleEvents(X86 *c) {
    LAN_OR_LEGACY("WSAWaitForMultipleEvents");
    uint32_t n = arg(c, 0), pev = arg(c, 1), all = arg(c, 2), tmo = arg(c, 3);
    if (!n || n > 64 || !pev || !gm_valid(pev, 4 * n)) {
        set_err(WSA_INVALID_PARAMETER_);
        set_eax(c, WAIT_FAILED_);
        return;
    }
    uint32_t hs[64];
    for (uint32_t i = 0; i < n; ++i)
        hs[i] = rd32(pev + 4 * i);
    {
        NetLock l;
        pump();
    }
    uint32_t r = guest_wait_objects(hs, n, all != 0, tmo);
    if (r == WAIT_FAILED_)
        set_err(WSA_INVALID_HANDLE_);
    set_eax(c, r);
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
// Per-thread guest buffers for hostent, servent and inet_ntoa, as Windows
// keeps one per thread.
uint32_t tbuf(uint32_t id) {
    static std::map<uint64_t, uint32_t> bufs;
    uint64_t key = (uint64_t)guest_current_thread_id() << 8 | id;
    auto it = bufs.find(key);
    if (it != bufs.end() && heap_owns(it->second))
        return it->second;
    uint32_t b = heap_alloc(512, true);
    bufs[key] = b;
    return b;
}
std::string host_name() {
    if (!g_vip)
        return "VLAN";
    char b[32];
    snprintf(b, sizeof b, "VLAN-%u", g_vip & ~g_mask); // the host number: unique on the switch
    return b;
}
// hostent { char *h_name; char **h_aliases; short h_addrtype; short h_length; char **h_addr_list; }
uint32_t make_hostent(const std::string &name, const std::vector<uint32_t> &ips) {
    uint32_t b = tbuf(1);
    if (!b)
        return 0;
    memset(g_mem + b, 0, 512);
    uint32_t aliases = b + 16, list = b + 20, addrs = b + 60, nm = b + 96;
    size_t n = std::min<size_t>(ips.size(), 8);
    gm_put_str(nm, name.c_str(), 256);
    wr32(b, nm);
    wr32(b + 4, aliases);
    wr16(b + 8, 2);
    wr16(b + 10, 4);
    wr32(b + 12, list);
    wr32(aliases, 0);
    for (size_t i = 0; i < n; ++i) {
        wr32(addrs + 4 * (uint32_t)i, __builtin_bswap32(ips[i]));
        wr32(list + 4 * (uint32_t)i, addrs + 4 * (uint32_t)i);
    }
    wr32(list + 4 * (uint32_t)n, 0);
    return b;
}
bool own_name(const std::string &n) {
    if (n.empty())
        return true;
    std::string me = host_name();
    size_t l = me.size();
    return !strncasecmp(n.c_str(), me.c_str(), l) &&
           (!n[l] || !strcasecmp(n.c_str() + l, ".local") || !strcasecmp(n.c_str() + l, ".lan"));
}
// The table of spec section 1, plus the names every host knows.
bool resolve(const std::string &name, uint32_t *ip, std::string *canon) {
    *canon = name;
    if (own_name(name)) {
        *ip = g_vip ? g_vip : 0x7f000001u;
        *canon = host_name();
        return true;
    }
    if (strcasecmp(name.c_str(), "localhost") == 0) {
        *ip = 0x7f000001u;
        return true;
    }
    if (parse_ip(name.c_str(), ip))
        return true;
    if (name_table(name, ip))
        return true;
    return false; // not in the table: no DNS exists on the virtual LAN
}
void w_gethostbyname(X86 *c) {
    LAN_OR_LEGACY("gethostbyname");
    if (!g_startups) {
        set_err(WSANOTINITIALISED_);
        set_eax(c, 0);
        return;
    }
    std::string name = gm_str(arg(c, 0), 255);
    uint32_t ip;
    std::string canon;
    {
        NetLock l;
        pump();
        if (!resolve(name, &ip, &canon)) {
            LOGW("lan: gethostbyname(\"%s\") on thread %u: not found", name.c_str(), guest_current_thread_id());
            set_err(WSAHOST_NOT_FOUND_);
            set_eax(c, 0);
            return;
        }
    }
    // Lookups are rare (DirtySock makes one per server), so each is told.
    LOGW("lan: gethostbyname(\"%s\") on thread %u = %s", name.c_str(), guest_current_thread_id(),
         ipstr(ip).c_str());
    set_eax(c, make_hostent(canon, {ip}));
}
void w_gethostbyaddr(X86 *c) {
    LAN_OR_LEGACY("gethostbyaddr");
    uint32_t p = arg(c, 0), len = arg(c, 1), type = arg(c, 2);
    if (!g_startups) {
        set_err(WSANOTINITIALISED_);
        set_eax(c, 0);
        return;
    }
    if (type != 2 || len < 4 || !p || !gm_valid(p, 4)) {
        set_err(type != 2 ? WSAEAFNOSUPPORT_ : WSAEFAULT_);
        set_eax(c, 0);
        return;
    }
    uint32_t ip = __builtin_bswap32(rd32(p));
    std::string name;
    {
        NetLock l;
        if (own_ip(ip))
            name = host_name();
        else if (is_loop(ip))
            name = "localhost";
    }
    if (name.empty()) {
        set_err(WSAHOST_NOT_FOUND_);
        set_eax(c, 0);
        return;
    }
    set_eax(c, make_hostent(name, {ip}));
}
void w_gethostname(X86 *c) {
    LAN_OR_LEGACY("gethostname");
    if (!g_startups)
        FAIL(WSANOTINITIALISED_);
    uint32_t p = arg(c, 0);
    int32_t len = (int32_t)arg(c, 1);
    std::string n;
    {
        NetLock l;
        pump();
        n = host_name();
    }
    if (!p || len <= (int32_t)n.size() || !gm_valid(p, (uint32_t)n.size() + 1))
        FAIL(WSAEFAULT_);
    gm_put_str(p, n.c_str(), (uint32_t)n.size() + 1);
    set_eax(c, 0);
}

// A few well-known services; a game names its ports by number.
struct Service {
    const char *name;
    uint16_t port;
    const char *proto;
};
const Service k_services[] = {{"http", 80, "tcp"}, {"https", 443, "tcp"}, {"ftp", 21, "tcp"},
                              {"domain", 53, "udp"}, {"ntp", 123, "udp"}};
// servent { char *s_name; char **s_aliases; short s_port; char *s_proto; } (32-bit layout)
uint32_t make_servent(const Service &sv) {
    uint32_t b = tbuf(2);
    if (!b)
        return 0;
    memset(g_mem + b, 0, 512);
    gm_put_str(b + 32, sv.name, 32);
    gm_put_str(b + 64, sv.proto, 8);
    wr32(b, b + 32);
    wr32(b + 4, b + 16);
    wr16(b + 8, (uint16_t)(sv.port >> 8 | sv.port << 8));
    wr32(b + 12, b + 64);
    return b;
}
void w_getservbyname(X86 *c) {
    LAN_OR_LEGACY("getservbyname");
    std::string name = gm_str(arg(c, 0), 64), proto = arg(c, 1) ? gm_str(arg(c, 1), 16) : "";
    for (const Service &sv : k_services)
        if (!strcasecmp(name.c_str(), sv.name) && (proto.empty() || !strcasecmp(proto.c_str(), sv.proto))) {
            set_eax(c, make_servent(sv));
            return;
        }
    set_err(WSANO_DATA_);
    set_eax(c, 0);
}
void w_getservbyport(X86 *c) {
    LAN_OR_LEGACY("getservbyport");
    uint16_t port = (uint16_t)(arg(c, 0) & 0xffff);
    port = (uint16_t)(port >> 8 | port << 8);
    std::string proto = arg(c, 1) ? gm_str(arg(c, 1), 16) : "";
    for (const Service &sv : k_services)
        if (sv.port == port && (proto.empty() || !strcasecmp(proto.c_str(), sv.proto))) {
            set_eax(c, make_servent(sv));
            return;
        }
    set_err(WSANO_DATA_);
    set_eax(c, 0);
}

// getaddrinfo / freeaddrinfo / getnameinfo (LAN mode only: Carbon does not
// import them, but its ad client asks GetProcAddress for them and otherwise
// falls back to gethostbyname, as on Windows 2000).
// addrinfo { int flags, family, socktype, protocol; size_t addrlen; char *canonname;
//            sockaddr *addr; addrinfo *next; } = 32 bytes on 32-bit Windows.
void w_getaddrinfo(X86 *c) {
    uint32_t pnode = arg(c, 0), pserv = arg(c, 1), phints = arg(c, 2), pres = arg(c, 3);
    if (!pres || !gm_valid(pres, 4)) {
        set_eax(c, WSAEINVAL_);
        return;
    }
    wr32(pres, 0);
    if (!pnode && !pserv) {
        set_err(WSAHOST_NOT_FOUND_);
        set_eax(c, WSAHOST_NOT_FOUND_);
        return;
    }
    uint32_t flags = 0, family = 0, socktype = 0, protocol = 0;
    if (phints && gm_valid(phints, 32)) {
        flags = rd32(phints);
        family = rd32(phints + 4);
        socktype = rd32(phints + 8);
        protocol = rd32(phints + 12);
    }
    if (family != 0 && family != 2) {
        set_err(WSAEAFNOSUPPORT_);
        set_eax(c, WSAEAFNOSUPPORT_);
        return;
    }
    uint16_t port = 0;
    if (pserv) {
        std::string sv = gm_str(pserv, 64);
        char *end = nullptr;
        unsigned long v = strtoul(sv.c_str(), &end, 10);
        if (!sv.empty() && end && !*end && v < 65536) {
            port = (uint16_t)v;
        } else {
            bool found = false;
            for (const Service &s : k_services)
                if (!strcasecmp(sv.c_str(), s.name)) {
                    port = s.port;
                    found = true;
                }
            if (!found) {
                set_err(WSATYPE_NOT_FOUND_);
                set_eax(c, WSATYPE_NOT_FOUND_);
                return;
            }
        }
    }
    uint32_t ip = 0;
    std::string canon;
    if (pnode) {
        std::string node = gm_str(pnode, 255);
        NetLock l;
        bool found = resolve(node, &ip, &canon);
        LOGW("lan: getaddrinfo(\"%s\") on thread %u = %s", node.c_str(), guest_current_thread_id(),
             found ? ipstr(ip).c_str() : "not found");
        if (!found) {
            set_err(WSAHOST_NOT_FOUND_);
            set_eax(c, WSAHOST_NOT_FOUND_);
            return;
        }
    } else {
        ip = (flags & 1) ? 0 : 0x7f000001u; // AI_PASSIVE: the wildcard address
    }
    uint32_t ai = heap_alloc(32 + 16 + 256, true);
    if (!ai) {
        set_eax(c, WSA_NOT_ENOUGH_MEMORY_);
        return;
    }
    uint32_t sa = ai + 32, cn = ai + 48;
    write_sockaddr(sa, ip, port);
    wr32(ai + 0, flags & 2 ? 2 : 0); // AI_CANONNAME kept
    wr32(ai + 4, 2);
    wr32(ai + 8, socktype);
    wr32(ai + 12, protocol ? protocol : socktype == 1 ? 6 : socktype == 2 ? 17 : 0);
    wr32(ai + 16, 16);
    if ((flags & 2) && !canon.empty()) {
        gm_put_str(cn, canon.c_str(), 256);
        wr32(ai + 20, cn);
    }
    wr32(ai + 24, sa);
    wr32(ai + 28, 0);
    wr32(pres, ai);
    set_eax(c, 0);
}
void w_freeaddrinfo(X86 *c) {
    uint32_t ai = arg(c, 0);
    while (ai && heap_owns(ai)) {
        uint32_t next = rd32(ai + 28);
        heap_free(ai);
        ai = next;
    }
    set_eax(c, 0);
}
void w_getnameinfo(X86 *c) {
    uint32_t sa = arg(c, 0), salen = arg(c, 1), host = arg(c, 2), hostlen = arg(c, 3), serv = arg(c, 4),
             servlen = arg(c, 5), flags = arg(c, 6);
    uint32_t ip;
    uint16_t port;
    if (uint32_t e = sa_in(sa, salen, &ip, &port)) {
        set_eax(c, e);
        return;
    }
    if (host && hostlen) {
        std::string name;
        {
            NetLock l;
            if (!(flags & 2) && own_ip(ip)) // NI_NUMERICHOST off: our own name
                name = host_name();
        }
        if (name.empty())
            name = ipstr(ip);
        if (name.size() + 1 > hostlen || !gm_valid(host, (uint32_t)name.size() + 1)) {
            set_eax(c, WSAEFAULT_);
            return;
        }
        gm_put_str(host, name.c_str(), hostlen);
    }
    if (serv && servlen) {
        char b[16];
        snprintf(b, sizeof b, "%u", port);
        if (strlen(b) + 1 > servlen || !gm_valid(serv, (uint32_t)strlen(b) + 1)) {
            set_eax(c, WSAEFAULT_);
            return;
        }
        gm_put_str(serv, b, servlen);
    }
    set_eax(c, 0);
}
void w_WSAFDIsSet(X86 *c) {
    uint32_t h = arg(c, 0), set = arg(c, 1), r = 0;
    if (set && gm_valid(set, 4)) {
        uint32_t k = std::min<uint32_t>(rd32(set), 4096);
        for (uint32_t i = 0; i < k && gm_valid(set + 4 + 4 * i, 4); ++i)
            if (rd32(set + 4 + 4 * i) == h) {
                r = 1;
                break;
            }
    }
    set_eax(c, r);
}

// ---------------------------------------------------------------------------
// netapi32!Netbios: NCBRESET, NCBASTAT and NCBENUM on one adapter, whose MAC
// is made from this browser's address on the switch (02:4e:56:4c:hh:ll, a
// locally administered one, as nfsu2-web makes it). Out of LAN mode it stays
// what it was: no NetBIOS (NRC_ENVNOTDEF), which EA's library takes calmly.
// ---------------------------------------------------------------------------
void w_Netbios(X86 *c) {
    uint32_t ncb = arg(c, 0);
    if (!lan_on()) {
        if (ncb && gm_valid(ncb, 0x40)) {
            g_mem[ncb + 1] = 0x34;    // ncb_retcode
            g_mem[ncb + 0x31] = 0x34; // ncb_cmd_cplt
        }
        set_eax(c, 0x34);
        return;
    }
    if (!ncb || !gm_valid(ncb, 64)) {
        set_eax(c, 0x01); // NRC_BUFLEN
        return;
    }
    uint8_t cmd = rd8(ncb) & 0x7f, lana = rd8(ncb + 0x30);
    uint32_t buf = rd32(ncb + 4);
    uint16_t blen = rd16(ncb + 8);
    uint8_t rc = 0;
    uint32_t vip;
    {
        NetLock l;
        if (g_mb)
            pump();
        vip = g_vip;
    }
    switch (cmd) {
    case 0x32: // NCBRESET
        rc = lana == 0 ? 0 : 0x23; // NRC_BRIDGE: no such adapter
        break;
    case 0x33: { // NCBASTAT
        if (lana != 0) {
            rc = 0x23;
            break;
        }
        if (rd8(ncb + 0xA) != '*') { // a remote adapter's status
            rc = 0x05;               // NRC_CMDTMO
            break;
        }
        uint8_t as[60] = {};
        as[0] = 0x02;
        as[1] = 0x4e;
        as[2] = 0x56;
        as[3] = 0x4c;
        as[4] = (uint8_t)(vip >> 8);
        as[5] = (uint8_t)vip;
        as[6] = 3;    // rev_major
        as[8] = 0xfe; // adapter_type: Ethernet
        const uint16_t w[] = {255, 255, 255, 0, 1464, 0, 64, 64, 1700, 0}; // free_ncbs .. name_count
        for (int i = 0; i < 10; ++i) {
            as[40 + 2 * i] = (uint8_t)w[i];
            as[41 + 2 * i] = (uint8_t)(w[i] >> 8);
        }
        uint32_t n = std::min<uint32_t>(blen, 60);
        if (n && !gm_valid(buf, n)) {
            rc = 0x01;
            break;
        }
        if (n)
            memcpy(g_mem + buf, as, n);
        wr16(ncb + 8, (uint16_t)n);
        rc = n < 60 ? 0x06 : 0; // NRC_INCOMP when the buffer was short
        break;
    }
    case 0x37: // NCBENUM: LANA_ENUM { length, lana[] }
        if (blen < 2 || !gm_valid(buf, 2)) {
            rc = 0x01;
            break;
        }
        wr8(buf, 1);
        wr8(buf + 1, 0);
        wr16(ncb + 8, 2);
        break;
    default:
        log_once("lan netbios", "lan: Netbios command 0x%02x is not supported: NRC_ILLCMD", cmd);
        rc = 0x03;
        break;
    }
    wr8(ncb + 1, rc);
    wr8(ncb + 0x31, rc);
    set_eax(c, rc);
}

// ---------------------------------------------------------------------------
// Registration. These replace misc.cpp's logging-only WS2_32 entries; each
// still answers as they did when LAN mode is off.
// ---------------------------------------------------------------------------
const ImportShim k_ws2_shims[] = {
    {"WS2_32.dll", "accept", 3, w_accept},
    {"WS2_32.dll", "bind", 3, w_bind},
    {"WS2_32.dll", "closesocket", 1, w_closesocket},
    {"WS2_32.dll", "connect", 3, w_connect},
    {"WS2_32.dll", "getpeername", 3, w_getpeername},
    {"WS2_32.dll", "getsockname", 3, w_getsockname},
    {"WS2_32.dll", "getsockopt", 5, w_getsockopt},
    {"WS2_32.dll", "ioctlsocket", 3, w_ioctlsocket},
    {"WS2_32.dll", "listen", 2, w_listen},
    {"WS2_32.dll", "recv", 4, w_recv},
    {"WS2_32.dll", "recvfrom", 6, w_recvfrom},
    {"WS2_32.dll", "select", 5, w_select},
    {"WS2_32.dll", "send", 4, w_send},
    {"WS2_32.dll", "sendto", 6, w_sendto},
    {"WS2_32.dll", "setsockopt", 5, w_setsockopt},
    {"WS2_32.dll", "shutdown", 2, w_shutdown},
    {"WS2_32.dll", "socket", 3, w_socket},
    {"WS2_32.dll", "gethostbyname", 1, w_gethostbyname},
    {"WS2_32.dll", "gethostbyaddr", 3, w_gethostbyaddr},
    {"WS2_32.dll", "gethostname", 2, w_gethostname},
    {"WS2_32.dll", "getservbyname", 2, w_getservbyname},
    {"WS2_32.dll", "getservbyport", 2, w_getservbyport},
    {"WS2_32.dll", "WSAGetLastError", 0, w_WSAGetLastError},
    {"WS2_32.dll", "WSAStartup", 2, w_WSAStartup},
    {"WS2_32.dll", "WSACleanup", 0, w_WSACleanup},
    {"WS2_32.dll", "WSAIoctl", 9, w_WSAIoctl},
    {"WS2_32.dll", "WSACreateEvent", 0, w_WSACreateEvent},
    {"WS2_32.dll", "WSACloseEvent", 1, w_WSACloseEvent},
    {"WS2_32.dll", "WSASetEvent", 1, w_WSASetEvent},
    {"WS2_32.dll", "WSAResetEvent", 1, w_WSAResetEvent},
    {"WS2_32.dll", "WSAWaitForMultipleEvents", 5, w_WSAWaitForMultipleEvents},
    {"WS2_32.dll", "WSARecv", 7, w_WSARecv},
    {"WS2_32.dll", "WSARecvFrom", 9, w_WSARecvFrom},
    {"WS2_32.dll", "WSAGetOverlappedResult", 5, w_WSAGetOverlappedResult},
    // WS2_32's own ordinals for the same calls.
    {"WS2_32.dll", "ord1", 3, w_accept},
    {"WS2_32.dll", "ord2", 3, w_bind},
    {"WS2_32.dll", "ord3", 1, w_closesocket},
    {"WS2_32.dll", "ord4", 3, w_connect},
    {"WS2_32.dll", "ord5", 3, w_getpeername},
    {"WS2_32.dll", "ord6", 3, w_getsockname},
    {"WS2_32.dll", "ord7", 5, w_getsockopt},
    {"WS2_32.dll", "ord10", 3, w_ioctlsocket},
    {"WS2_32.dll", "ord13", 2, w_listen},
    {"WS2_32.dll", "ord16", 4, w_recv},
    {"WS2_32.dll", "ord17", 6, w_recvfrom},
    {"WS2_32.dll", "ord18", 5, w_select},
    {"WS2_32.dll", "ord19", 4, w_send},
    {"WS2_32.dll", "ord20", 6, w_sendto},
    {"WS2_32.dll", "ord21", 5, w_setsockopt},
    {"WS2_32.dll", "ord22", 2, w_shutdown},
    {"WS2_32.dll", "ord23", 3, w_socket},
    {"WS2_32.dll", "ord52", 1, w_gethostbyname},
    {"WS2_32.dll", "ord57", 2, w_gethostname},
    {"WS2_32.dll", "ord111", 0, w_WSAGetLastError},
    {"WS2_32.dll", "ord115", 2, w_WSAStartup},
    {"WS2_32.dll", "ord116", 0, w_WSACleanup},
    {"NETAPI32.dll", "Netbios", 1, w_Netbios},
};
// Names the kit never answered before: registered only in LAN mode, so that
// GetProcAddress still fails for them in normal play.
const ImportShim k_ws2_lan_only[] = {
    {"WS2_32.dll", "WSASetLastError", 1, w_WSASetLastError},
    {"WS2_32.dll", "ord112", 1, w_WSASetLastError},
    {"WS2_32.dll", "WSASend", 7, w_WSASend},
    {"WS2_32.dll", "WSASendTo", 9, w_WSASendTo},
    {"WS2_32.dll", "getaddrinfo", 4, w_getaddrinfo},
    {"WS2_32.dll", "freeaddrinfo", 1, w_freeaddrinfo},
    {"WS2_32.dll", "getnameinfo", 7, w_getnameinfo},
    {"WS2_32.dll", "__WSAFDIsSet", 2, w_WSAFDIsSet},
    {"WS2_32.dll", "ord151", 2, w_WSAFDIsSet},
};

} // namespace

void winsock_register() {
    imports_register(k_ws2_shims, sizeof k_ws2_shims / sizeof k_ws2_shims[0]);
    if (lan_on()) {
        imports_register(k_ws2_lan_only, sizeof k_ws2_lan_only / sizeof k_ws2_lan_only[0]);
        // DirtySock closes the handles of its receive, idle and lookup threads
        // at once; they must still run.
        sched_set_thread_start_from_create(true);
        LOGW("lan: virtual LAN on (RECOMP_LAN): Winsock goes through the page's link to the server's switch");
    }
}

lan::Mailbox *winsock_lan_mailbox() {
    NetLock l;
    return g_mb;
}
void winsock_test_set_lan(int on) {
    g_lan_test = on;
}
void winsock_test_pump() {
    NetLock l;
    pump();
}
