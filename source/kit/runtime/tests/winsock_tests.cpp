// winsock_tests.cpp - the Winsock HLE (winsock.cpp) against a page and a
// switch played by this test: a thread that takes frames from the mailbox's TX
// ring and answers as the server's host at 10.77.0.1 would - UDP and TCP echo
// on port 7, "port unreachable" and RST elsewhere - through the RX ring. No
// game image required.
#include "../imports.h"
#include "../lan_mailbox.h"
#include "../loader.h"
#include "../memory.h"
#include "../win32.h"
#include "../../platform/os.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

static int g_checks = 0, g_failures = 0;
static bool check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool check(bool ok, const char *fmt, ...) {
    ++g_checks;
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!ok)
        ++g_failures;
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", msg);
    return ok;
}

static uint32_t call_import(X86 *c, const char *dll, const char *name, const std::vector<uint32_t> &args) {
    uint32_t tramp = imports_resolve(dll, name);
    if (!tramp) {
        printf("  [FAIL] no trampoline for %s!%s\n", dll, name);
        ++g_failures;
        ++g_checks;
        return 0;
    }
    uint32_t esp = c->r[R_ESP], before = esp;
    for (size_t i = args.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, 0);
    c->r[R_ESP] = esp;
    imports_dispatch(c, tramp);
    if (c->r[R_ESP] != before) {
        printf("  [FAIL] %s!%s left ESP at %08x, expected %08x (bad argc?)\n", dll, name, c->r[R_ESP], before);
        ++g_failures;
        ++g_checks;
    }
    c->r[R_ESP] = before;
    return c->r[R_EAX];
}
static uint32_t ws(X86 *c, const char *name, const std::vector<uint32_t> &args) {
    return call_import(c, "WS2_32.dll", name, args);
}

// ---------------------------------------------------------------------------
// The page and the switch
// ---------------------------------------------------------------------------
constexpr uint32_t VIP = 0x0A4D0011u; // 10.77.0.17
struct Frame {
    uint32_t kind, src, dst;
    uint16_t sport, dport;
    std::vector<uint8_t> data; // LAN header included
};
static std::mutex g_page_m;
static std::vector<Frame> g_seen; // every frame the kit sent
static std::atomic<bool> g_page_stop{false};
static std::atomic<int> g_page_state_goal{lan::PAGE_ONLINE};

static void rx_put(lan::Mailbox *mb, const Frame &f) {
    for (;;) {
        uint32_t head = __atomic_load_n(&mb->rx_head, __ATOMIC_SEQ_CST),
                 tail = __atomic_load_n(&mb->rx_tail, __ATOMIC_SEQ_CST);
        if (head - tail < lan::RX_SLOTS) {
            lan::Slot *s = (lan::Slot *)((uint8_t *)mb + mb->rx_off + (head % lan::RX_SLOTS) * lan::SLOT_BYTES);
            s->len = (uint32_t)f.data.size();
            s->kind = f.kind;
            s->src_ip = f.src;
            s->dst_ip = f.dst;
            s->src_port = f.sport;
            s->dst_port = f.dport;
            memcpy(s->data, f.data.data(), f.data.size());
            __atomic_store_n(&mb->rx_head, head + 1, __ATOMIC_SEQ_CST);
            __atomic_add_fetch(&mb->rx_bell, 1, __ATOMIC_SEQ_CST);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static Frame reply(const Frame &f, uint8_t op, const std::vector<uint8_t> &payload) {
    Frame r;
    r.kind = f.kind;
    r.src = f.dst;
    r.dst = f.src;
    r.sport = f.dport;
    r.dport = f.sport;
    r.data.assign(f.data.begin(), f.data.begin() + lan::LAN_HDR);
    r.data[0] = op;
    r.data.insert(r.data.end(), payload.begin(), payload.end());
    return r;
}
// The server's host at 10.77.0.1: echo on port 7, nothing anywhere else.
static void host_answer(lan::Mailbox *mb, const Frame &f) {
    std::vector<uint8_t> payload(f.data.begin() + lan::LAN_HDR, f.data.end());
    uint8_t op = f.data[0];
    if (f.kind == lan::KIND_UDP) {
        if (f.dport == 7)
            rx_put(mb, reply(f, lan::UDP_DATA, payload));
        else
            rx_put(mb, reply(f, lan::UDP_UNREACH, {}));
    } else if (f.kind == lan::KIND_TCP) {
        if (op == lan::TCP_SYN)
            rx_put(mb, reply(f, f.dport == 7 ? lan::TCP_SYNACK : lan::TCP_RST, {}));
        else if (op == lan::TCP_DATA && f.dport == 7)
            rx_put(mb, reply(f, lan::TCP_DATA, payload));
        else if (op == lan::TCP_FIN && f.dport == 7)
            rx_put(mb, reply(f, lan::TCP_FIN, {}));
    }
}
static void page_main() {
    lan::Mailbox *mb = nullptr;
    while (!g_page_stop && !(mb = winsock_lan_mailbox()))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!mb)
        return;
    __atomic_store_n(&mb->page_state, lan::PAGE_CONNECTING, __ATOMIC_SEQ_CST);
    // Connecting takes a moment, as a WebSocket would.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    __atomic_store_n(&mb->vip, VIP, __ATOMIC_SEQ_CST);
    __atomic_store_n(&mb->netmask, 0xFFFFF800u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&mb->generation, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&mb->page_state, lan::PAGE_ONLINE, __ATOMIC_SEQ_CST);
    while (!g_page_stop) {
        uint32_t tail = __atomic_load_n(&mb->tx_tail, __ATOMIC_SEQ_CST);
        uint32_t head = __atomic_load_n(&mb->tx_head, __ATOMIC_SEQ_CST);
        if (tail == head) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const lan::Slot *s = (const lan::Slot *)((uint8_t *)mb + mb->tx_off + (tail % lan::TX_SLOTS) * lan::SLOT_BYTES);
        Frame f;
        f.kind = s->kind;
        f.src = s->src_ip;
        f.dst = s->dst_ip;
        f.sport = s->src_port;
        f.dport = s->dst_port;
        f.data.assign(s->data, s->data + s->len);
        __atomic_store_n(&mb->tx_tail, tail + 1, __ATOMIC_SEQ_CST);
        {
            std::lock_guard<std::mutex> l(g_page_m);
            g_seen.push_back(f);
        }
        if (f.dst == lan::SERVER_HOST)
            host_answer(mb, f);
    }
}
static int seen_count(uint32_t kind, uint8_t op, uint16_t dport) {
    std::lock_guard<std::mutex> l(g_page_m);
    int n = 0;
    for (const Frame &f : g_seen)
        if (f.kind == kind && f.data[0] == op && f.dport == dport)
            ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Guest-side helpers
// ---------------------------------------------------------------------------
static uint32_t g_scratch = 0x00300000;
static uint32_t alloc(uint32_t n) {
    uint32_t a = g_scratch;
    memset(g_mem + a, 0, n);
    g_scratch += (n + 15) & ~15u;
    return a;
}
static uint32_t put_str(const char *s) {
    uint32_t a = alloc((uint32_t)strlen(s) + 1);
    memcpy(g_mem + a, s, strlen(s) + 1);
    return a;
}
static uint32_t sockaddr(uint32_t ip, uint16_t port) {
    uint32_t a = alloc(16);
    wr16(a, 2);
    wr8(a + 2, (uint8_t)(port >> 8));
    wr8(a + 3, (uint8_t)port);
    wr32(a + 4, __builtin_bswap32(ip));
    return a;
}
static uint32_t sa_ip(uint32_t a) {
    return __builtin_bswap32(rd32(a + 4));
}
static uint16_t sa_port(uint32_t a) {
    return (uint16_t)(rd8(a + 2) << 8 | rd8(a + 3));
}
static uint32_t fdset(std::vector<uint32_t> hs) {
    uint32_t a = alloc(4 + 4 * 64);
    wr32(a, (uint32_t)hs.size());
    for (size_t i = 0; i < hs.size(); ++i)
        wr32(a + 4 + 4 * (uint32_t)i, hs[i]);
    return a;
}
static uint32_t timeval(uint32_t ms) {
    uint32_t a = alloc(8);
    wr32(a, ms / 1000);
    wr32(a + 4, (ms % 1000) * 1000);
    return a;
}
static uint32_t last_error(X86 *c) {
    return ws(c, "WSAGetLastError", {});
}

static void test_lan_off(X86 *c) {
    printf("\n== LAN mode off: the kit's old answers\n");
    winsock_test_set_lan(0);
    uint32_t data = alloc(400);
    check(ws(c, "WSAStartup", {0x0202, data}) == 0 && rd16(data) == 0, "WSAStartup returns 0 and fills nothing");
    check(ws(c, "socket", {2, 2, 17}) == 0, "socket returns 0 (logging-only stub)");
    check(ws(c, "WSAWaitForMultipleEvents", {1, alloc(4), 0, 0xffffffffu, 0}) == 0,
          "WSAWaitForMultipleEvents returns 0 at once");
    uint32_t ncb = alloc(64);
    check(call_import(c, "NETAPI32.dll", "Netbios", {ncb}) == 0x34 && rd8(ncb + 1) == 0x34,
          "Netbios answers NRC_ENVNOTDEF");
    check(imports_resolve("WS2_32.dll", "getaddrinfo") == 0, "getaddrinfo is not offered to GetProcAddress");
    check(winsock_lan_mailbox() == nullptr, "no mailbox is made");
    winsock_test_set_lan(-1);
}

static void test_startup(X86 *c) {
    printf("\n== WSAStartup waits for the page, names, local identity\n");
    uint32_t data = alloc(400);
    auto t0 = std::chrono::steady_clock::now();
    check(ws(c, "WSAStartup", {0x0202, data}) == 0, "WSAStartup(2.2) succeeds");
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    check(rd16(data) == 0x0202 && rd16(data + 2) == 0x0202, "WSADATA says 2.2");
    lan::Mailbox *mb = winsock_lan_mailbox();
    check(mb && mb->magic == lan::MAGIC && mb->want == 1, "the mailbox exists and wants the switch");
    check(mb && mb->page_state == lan::PAGE_ONLINE, "WSAStartup returned once the page was online (%.0f ms)", ms);

    uint32_t he = ws(c, "gethostbyname", {put_str("nfs-pc.fesl.ea.com")});
    check(he && rd16(he + 8) == 2 && rd16(he + 10) == 4 && __builtin_bswap32(rd32(rd32(rd32(he + 12)))) == 0x0A4D0001u,
          "gethostbyname(nfs-pc.fesl.ea.com) = 10.77.0.1");
    he = ws(c, "gethostbyname", {put_str("locate.madserver.net")});
    check(he && __builtin_bswap32(rd32(rd32(rd32(he + 12)))) == 0x0A4D0001u, "locate.madserver.net = 10.77.0.1");
    check(ws(c, "gethostbyname", {put_str("www.example.org")}) == 0 && last_error(c) == 11001,
          "other names: WSAHOST_NOT_FOUND");
    he = ws(c, "gethostbyname", {put_str("10.77.0.5")});
    check(he && __builtin_bswap32(rd32(rd32(rd32(he + 12)))) == 0x0A4D0005u, "a dotted address resolves to itself");
    uint32_t name = alloc(64);
    check(ws(c, "gethostname", {name, 64}) == 0 && gm_str(name) == "VLAN-17", "gethostname = VLAN-17 (%s)",
          gm_str(name).c_str());
    he = ws(c, "gethostbyname", {name});
    check(he && __builtin_bswap32(rd32(rd32(rd32(he + 12)))) == VIP, "our own name is our virtual address");
    uint32_t pai = alloc(4);
    check(ws(c, "getaddrinfo", {put_str("nfs-pc.fesl.ea.com"), put_str("18210"), 0, pai}) == 0 && rd32(pai) &&
              sa_ip(rd32(rd32(pai) + 24)) == 0x0A4D0001u && sa_port(rd32(rd32(pai) + 24)) == 18210,
          "getaddrinfo gives 10.77.0.1:18210");
    ws(c, "freeaddrinfo", {rd32(pai)});

    uint32_t ncb = alloc(64), buf = alloc(240);
    wr8(ncb, 0x32);
    check(call_import(c, "NETAPI32.dll", "Netbios", {ncb}) == 0, "Netbios NCBRESET on LANA 0");
    memset(g_mem + ncb, 0, 64);
    wr8(ncb, 0x33);
    wr32(ncb + 4, buf);
    wr16(ncb + 8, 240);
    memset(g_mem + ncb + 10, ' ', 16);
    wr8(ncb + 10, '*');
    check(call_import(c, "NETAPI32.dll", "Netbios", {ncb}) == 0 && rd8(buf) == 0x02 && rd8(buf + 4) == 0 &&
              rd8(buf + 5) == 0x11,
          "Netbios NCBASTAT: MAC 02:4e:56:4c:00:11 from the address");
}

static void test_udp(X86 *c) {
    printf("\n== UDP: echo, overlapped WSARecvFrom with a WSA event, port unreachable\n");
    uint32_t s = ws(c, "socket", {2, 2, 17});
    check(s != 0xffffffffu && s != 0, "socket(AF_INET, SOCK_DGRAM)");
    check(ws(c, "bind", {s, sockaddr(0, 0), 16}) == 0, "bind to any:0");
    uint32_t sa = alloc(16), sl = alloc(4);
    wr32(sl, 16);
    check(ws(c, "getsockname", {s, sa, sl}) == 0 && sa_port(sa) >= 49152 && sa_ip(sa) == 0,
          "getsockname: 0.0.0.0:%u", sa_port(sa));
    uint32_t one = alloc(4);
    wr32(one, 1);
    check(ws(c, "ioctlsocket", {s, 0x8004667Eu, one}) == 0, "FIONBIO on");
    uint32_t buf = alloc(1000);
    check(ws(c, "recvfrom", {s, buf, 1000, 0, 0, 0}) == 0xffffffffu && last_error(c) == 10035,
          "nothing yet: WSAEWOULDBLOCK");

    // A posted receive, completed later by the network thread.
    uint32_t ev = ws(c, "WSACreateEvent", {});
    check(ev != 0, "WSACreateEvent");
    uint32_t wsabuf = alloc(8), bytes = alloc(4), flags = alloc(4), from = alloc(16), fromlen = alloc(4),
             ov = alloc(20);
    wr32(wsabuf, 1000);
    wr32(wsabuf + 4, buf);
    wr32(fromlen, 16);
    wr32(ov + 16, ev);
    uint32_t r = ws(c, "WSARecvFrom", {s, wsabuf, 1, bytes, flags, from, fromlen, ov, 0});
    check(r == 0xffffffffu && last_error(c) == 997 && rd32(ov) == 0x103, "WSARecvFrom posts: WSA_IO_PENDING");
    uint32_t cb = alloc(4), fl2 = alloc(4);
    check(ws(c, "WSAGetOverlappedResult", {s, ov, cb, 0, fl2}) == 0 && last_error(c) == 996,
          "not done yet: WSA_IO_INCOMPLETE");
    check(ws(c, "sendto", {s, put_str("hello carbon"), 12, 0, sockaddr(0x0A4D0001u, 7), 16}) == 12,
          "sendto 10.77.0.1:7");
    uint32_t evs = alloc(4);
    wr32(evs, ev);
    auto t0 = std::chrono::steady_clock::now();
    uint32_t w = ws(c, "WSAWaitForMultipleEvents", {1, evs, 0, 0xffffffffu, 0});
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    check(w == 0, "WSAWaitForMultipleEvents(INFINITE) wakes when the echo completes the receive (%.1f ms)", ms);
    check(rd32(ov) == 0 && rd32(ov + 4) == 12, "the OVERLAPPED says done, 12 bytes");
    check(ws(c, "WSAGetOverlappedResult", {s, ov, cb, 0, fl2}) == 1 && rd32(cb) == 12 &&
              memcmp(g_mem + buf, "hello carbon", 12) == 0,
          "WSAGetOverlappedResult: TRUE, the echo in the buffer");
    check(sa_ip(from) == 0x0A4D0001u && sa_port(from) == 7 && rd32(fromlen) == 16, "from 10.77.0.1:7");
    check(ws(c, "WSAResetEvent", {ev}) == 1, "WSAResetEvent");

    // Data already here: the call completes at once and still signals the event.
    ws(c, "sendto", {s, put_str("again"), 5, 0, sockaddr(0x0A4D0001u, 7), 16});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    winsock_test_pump();
    r = ws(c, "WSARecvFrom", {s, wsabuf, 1, bytes, flags, from, fromlen, ov, 0});
    check(r == 0 && rd32(bytes) == 5 && memcmp(g_mem + buf, "again", 5) == 0, "immediate completion: 5 bytes");
    check(ws(c, "WSAWaitForMultipleEvents", {1, evs, 0, 0, 0}) == 0, "and its event is set");
    ws(c, "WSAResetEvent", {ev});
    check(ws(c, "WSAWaitForMultipleEvents", {1, evs, 0, 10, 0}) == 0x102, "a reset event times out");

    // Port unreachable from the host: the sender's next receive fails.
    check(ws(c, "sendto", {s, put_str("x"), 1, 0, sockaddr(0x0A4D0001u, 9), 16}) == 1, "sendto 10.77.0.1:9");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(ws(c, "recvfrom", {s, buf, 1000, 0, from, fromlen}) == 0xffffffffu && last_error(c) == 10054,
          "port unreachable: recvfrom fails with WSAECONNRESET");
    uint32_t fion = alloc(4);
    check(ws(c, "ioctlsocket", {s, 0x4004667Fu, fion}) == 0 && rd32(fion) == 0, "FIONREAD 0");

    // A receive still posted is cancelled by closesocket.
    r = ws(c, "WSARecvFrom", {s, wsabuf, 1, bytes, flags, from, fromlen, ov, 0});
    check(r == 0xffffffffu && last_error(c) == 997, "posted again");
    check(ws(c, "closesocket", {s}) == 0, "closesocket");
    check(ws(c, "WSAWaitForMultipleEvents", {1, evs, 0, 1000, 0}) == 0 &&
              ws(c, "WSAGetOverlappedResult", {s, ov, cb, 0, fl2}) == 0 && last_error(c) == 995,
          "the posted receive ends with WSA_OPERATION_ABORTED");
    check(ws(c, "WSACloseEvent", {ev}) == 1, "WSACloseEvent");
    check(ws(c, "closesocket", {s}) == 0xffffffffu && last_error(c) == 10038, "a closed socket: WSAENOTSOCK");

    // Two sockets in this browser talk without the switch, and a broadcast comes back to its sender.
    uint32_t a = ws(c, "socket", {2, 2, 0}), b = ws(c, "socket", {2, 2, 0});
    ws(c, "bind", {b, sockaddr(0, 5000), 16});
    ws(c, "bind", {a, sockaddr(0, 0), 16});
    check(ws(c, "sendto", {a, put_str("local"), 5, 0, sockaddr(0x7f000001u, 5000), 16}) == 5, "sendto 127.0.0.1:5000");
    wr32(fromlen, 16);
    check(ws(c, "recvfrom", {b, buf, 1000, 0, from, fromlen}) == 5 && sa_ip(from) == 0x7f000001u,
          "received on 127.0.0.1");
    check(ws(c, "sendto", {a, put_str("all"), 3, 0, sockaddr(0xffffffffu, 5000), 16}) == 0xffffffffu &&
              last_error(c) == 10013,
          "a broadcast without SO_BROADCAST: WSAEACCES");
    wr32(one, 1);
    check(ws(c, "setsockopt", {a, 0xffff, 0x20, one, 4}) == 0, "SO_BROADCAST");
    int before = seen_count(lan::KIND_UDP, lan::UDP_DATA, 5000);
    check(ws(c, "sendto", {a, put_str("all"), 3, 0, sockaddr(0xffffffffu, 5000), 16}) == 3, "broadcast sent");
    check(ws(c, "recvfrom", {b, buf, 1000, 0, from, fromlen}) == 3, "our own broadcast arrives here too");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(seen_count(lan::KIND_UDP, lan::UDP_DATA, 5000) == before + 1, "and goes to the switch");
    ws(c, "closesocket", {a});
    ws(c, "closesocket", {b});
}

static void test_tcp(X86 *c) {
    printf("\n== TCP over the switch: non-blocking connect, select, echo, refused, FIN\n");
    uint32_t s = ws(c, "socket", {2, 1, 6});
    uint32_t one = alloc(4);
    wr32(one, 1);
    ws(c, "ioctlsocket", {s, 0x8004667Eu, one});
    check(ws(c, "connect", {s, sockaddr(0x0A4D0001u, 7), 16}) == 0xffffffffu && last_error(c) == 10035,
          "non-blocking connect: WSAEWOULDBLOCK");
    uint32_t wset = fdset({s}), eset = fdset({s});
    uint32_t n = ws(c, "select", {0, 0, wset, eset, timeval(2000)});
    check(n == 1 && rd32(wset) == 1 && rd32(eset) == 0, "select: writable once the SYNACK came");
    uint32_t sa = alloc(16), sl = alloc(4);
    wr32(sl, 16);
    check(ws(c, "getpeername", {s, sa, sl}) == 0 && sa_ip(sa) == 0x0A4D0001u && sa_port(sa) == 7,
          "getpeername 10.77.0.1:7");
    check(ws(c, "getsockname", {s, sa, sl}) == 0 && sa_ip(sa) == VIP, "getsockname: our virtual address");
    check(ws(c, "send", {s, put_str("ping over tcp"), 13, 0}) == 13, "send 13 bytes");
    uint32_t rset = fdset({s});
    check(ws(c, "select", {0, rset, 0, 0, timeval(2000)}) == 1, "select: readable");
    uint32_t buf = alloc(64);
    check(ws(c, "recv", {s, buf, 64, 0}) == 13 && memcmp(g_mem + buf, "ping over tcp", 13) == 0,
          "recv: the echo");
    check(ws(c, "recv", {s, buf, 64, 0}) == 0xffffffffu && last_error(c) == 10035, "then WSAEWOULDBLOCK");
    check(ws(c, "shutdown", {s, 1}) == 0, "shutdown(SD_SEND) sends FIN");
    rset = fdset({s});
    ws(c, "select", {0, rset, 0, 0, timeval(2000)});
    check(ws(c, "recv", {s, buf, 64, 0}) == 0, "the host's FIN: recv returns 0");
    check(ws(c, "closesocket", {s}) == 0, "closesocket");
    check(seen_count(lan::KIND_TCP, lan::TCP_FIN, 7) == 1, "the switch saw one FIN");

    // Refused: the host answers RST.
    s = ws(c, "socket", {2, 1, 0});
    ws(c, "ioctlsocket", {s, 0x8004667Eu, one});
    ws(c, "connect", {s, sockaddr(0x0A4D0001u, 9), 16});
    wset = fdset({s});
    eset = fdset({s});
    n = ws(c, "select", {0, 0, wset, eset, timeval(2000)});
    check(n == 1 && rd32(wset) == 0 && rd32(eset) == 1, "select: a refused connect is in the except set only");
    uint32_t err = alloc(4), errl = alloc(4);
    wr32(errl, 4);
    check(ws(c, "getsockopt", {s, 0xffff, 0x1007, err, errl}) == 0 && rd32(err) == 10061,
          "SO_ERROR = WSAECONNREFUSED");
    ws(c, "closesocket", {s});

    // A blocking connect and recv wait descheduled.
    s = ws(c, "socket", {2, 1, 0});
    check(ws(c, "connect", {s, sockaddr(0x0A4D0001u, 7), 16}) == 0, "blocking connect returns 0");
    check(ws(c, "send", {s, put_str("blocking"), 8, 0}) == 8, "send");
    check(ws(c, "recv", {s, buf, 64, 0}) == 8, "blocking recv waits for the echo");
    ws(c, "closesocket", {s});

    // Listen and accept in this browser (127.0.0.1).
    uint32_t l = ws(c, "socket", {2, 1, 0});
    check(ws(c, "bind", {l, sockaddr(0x7f000001u, 6000), 16}) == 0 && ws(c, "listen", {l, 5}) == 0,
          "listen on 127.0.0.1:6000");
    uint32_t cl = ws(c, "socket", {2, 1, 0});
    ws(c, "ioctlsocket", {cl, 0x8004667Eu, one});
    ws(c, "connect", {cl, sockaddr(0x7f000001u, 6000), 16});
    rset = fdset({l});
    check(ws(c, "select", {0, rset, 0, 0, timeval(1000)}) == 1, "the listener is readable");
    uint32_t acc = ws(c, "accept", {l, sa, sl});
    check(acc != 0xffffffffu && sa_ip(sa) == 0x7f000001u, "accept");
    ws(c, "send", {cl, put_str("loop"), 4, 0});
    check(ws(c, "recv", {acc, buf, 64, 0}) == 4, "data across the local connection");
    ws(c, "closesocket", {cl});
    check(ws(c, "recv", {acc, buf, 64, 0}) == 0, "the peer's close: recv 0");
    ws(c, "closesocket", {acc});
    ws(c, "closesocket", {l});
}

static void test_icmp(X86 *c) {
    printf("\n== ICMP: a ping anywhere is answered after the fixed round trip\n");
    uint32_t s = ws(c, "socket", {2, 3, 1});
    check(s != 0xffffffffu, "socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)");
    uint32_t ev = ws(c, "WSACreateEvent", {}), evs = alloc(4);
    wr32(evs, ev);
    uint32_t buf = alloc(1000), wsabuf = alloc(8), bytes = alloc(4), flags = alloc(4), ov = alloc(20);
    wr32(wsabuf, 1000);
    wr32(wsabuf + 4, buf);
    wr32(ov + 16, ev);
    check(ws(c, "WSARecv", {s, wsabuf, 1, bytes, flags, ov, 0}) == 0xffffffffu && last_error(c) == 10022,
          "WSARecv before the socket is bound: WSAEINVAL, as on Windows");
    uint8_t echo[16] = {8, 0, 0, 0, 0x12, 0x34, 0, 1, 'p', 'i', 'n', 'g', 0, 0, 0, 0};
    uint32_t msg = alloc(16);
    memcpy(g_mem + msg, echo, 16);
    auto t0 = std::chrono::steady_clock::now();
    check(ws(c, "sendto", {s, msg, 16, 0, sockaddr(0x9F9946B5u, 0), 16}) == 16, "echo request to 159.153.70.181");
    check(ws(c, "WSARecv", {s, wsabuf, 1, bytes, flags, ov, 0}) == 0xffffffffu && last_error(c) == 997,
          "WSARecv posted");
    uint32_t w = ws(c, "WSAWaitForMultipleEvents", {1, evs, 0, 2000, 0});
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    uint32_t cb = alloc(4), fl = alloc(4);
    check(w == 0 && ws(c, "WSAGetOverlappedResult", {s, ov, cb, 0, fl}) == 1 && rd32(cb) == 36,
          "the echo reply: 20-byte IP header + 16 bytes after %.1f ms", ms);
    check(rd8(buf) == 0x45 && rd8(buf + 9) == 1 && __builtin_bswap32(rd32(buf + 12)) == 0x9F9946B5u &&
              rd8(buf + 20) == 0 && memcmp(g_mem + buf + 28, "ping", 4) == 0,
          "IPv4 header from the pinged address, then the echo reply");
    ws(c, "closesocket", {s});
    ws(c, "WSACloseEvent", {ev});

    uint32_t u = ws(c, "socket", {2, 2, 0}), out = alloc(16), ret = alloc(4);
    check(ws(c, "WSAIoctl", {u, 0xC8000014u, sockaddr(0x0A4D0001u, 0), 16, out, 16, ret, 0, 0}) == 0 &&
              sa_ip(out) == VIP,
          "SIO_ROUTING_INTERFACE_QUERY: our virtual address");
    ws(c, "closesocket", {u});
}

static void test_last_error(X86 *c) {
    printf("\n== WSAGetLastError is per thread\n");
    ws(c, "closesocket", {0x1234});
    check(last_error(c) == 10038, "this thread: WSAENOTSOCK");
    uint32_t other = 0;
    std::thread t([&] {
        X86 c2;
        loader_init_context(&c2);
        c2.r[R_ESP] = 0x00380000;
        other = ws(&c2, "WSAGetLastError", {});
    });
    t.join();
    check(other == 0, "another thread still has 0");
}

int main() {
    mem_init();
    imports_init();
    X86 c;
    loader_init_context(&c);
    test_lan_off(&c);
    winsock_test_set_lan(1);
    // The LAN-only names are registered at imports_init, in LAN mode only.
    winsock_register();
    std::thread page(page_main);
    test_startup(&c);
    test_udp(&c);
    test_tcp(&c);
    test_icmp(&c);
    test_last_error(&c);
    check(ws(&c, "WSACleanup", {}) == 0 && winsock_lan_mailbox()->want == 0, "WSACleanup: the switch is no longer wanted");
    g_page_stop = true;
    page.join();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    fflush(stdout);
    // The kit's network thread runs for the life of the process.
    _exit(g_failures ? 1 : 0);
}
