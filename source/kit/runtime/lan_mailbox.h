// lan_mailbox.h - the LAN mailbox: frames between the kit's Winsock HLE
// (winsock.cpp) and the page's virtual-LAN link (web/player/lan.js), in the
// wasm memory both share. The numbers here and in lan.js must agree.
//
//   header (256 bytes)   fixed fields, then kit -> page words, then page -> kit words
//   TX ring              TX_SLOTS slots: the kit produces, the page sends them
//   RX ring              RX_SLOTS slots: the page produces what the switch sent
//
// Both rings are single-producer single-consumer with free-running u32
// counters (slot = counter % slots; empty when head == tail, full when
// head - tail == slots). The producer fills a slot, then stores head; the
// consumer reads the slot, then stores tail. Heads, tails and bells are only
// touched with atomics. The kit bumps tx_bell and notifies it after queuing
// (the page waits with Atomics.waitAsync); the page bumps rx_bell and notifies
// it (the kit's network thread waits on it with a timeout, so its timers run).
//
// Addresses in the mailbox are host-order numbers (10.77.0.1 = 0x0A4D0001),
// ports plain numbers; the page turns them into big-endian frames:
//   [0] kind  [1..3] 0  [4..7] source vip  [8..11] destination vip
//   [12..13] source port  [14..15] destination port  [16..] payload
// and a payload of kind UDP or TCP starts with the 8-byte LAN header:
//   [0] op  [1] ttl  [2..3] 0  [4..7] connection id (TCP; little-endian)
//
// plans/lan-protocol.md is the spec. The shape follows nfsu2-web's
// runtime/web/net_proto.h (Copyright (c) 2026 th0mk, MIT; see NOTICE).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace lan {

constexpr uint32_t MAGIC = 0x314E4C52u; // "RLN1" read as a little-endian u32
constexpr uint32_t VERSION = 1;
constexpr uint32_t HDR_BYTES = 256;
constexpr uint32_t SLOT_BYTES = 1536;
constexpr uint32_t SLOT_HDR = 32;
constexpr uint32_t TX_SLOTS = 128;
constexpr uint32_t RX_SLOTS = 256;
constexpr uint32_t MAILBOX_BYTES = HDR_BYTES + (TX_SLOTS + RX_SLOTS) * SLOT_BYTES;
// An Ethernet MTU's UDP payload: the most one frame carries, LAN header included.
constexpr uint32_t MAX_PAYLOAD = 1472;

// Frame kinds (byte 0 of a binary frame on the switch).
constexpr uint32_t KIND_UDP = 3;
constexpr uint32_t KIND_TCP = 4;

constexpr uint32_t LAN_HDR = 8;
// UDP ops.
constexpr uint8_t UDP_DATA = 0;
constexpr uint8_t UDP_UNREACH = 1; // nothing listens on that port: back to the sender
// TCP ops: a stream protocol of our own. The WebSocket and the switch are
// reliable and ordered, so there are no sequence numbers or retransmissions.
constexpr uint8_t TCP_SYN = 1;
constexpr uint8_t TCP_SYNACK = 2;
constexpr uint8_t TCP_RST = 3;
constexpr uint8_t TCP_DATA = 4;
constexpr uint8_t TCP_FIN = 5;
constexpr uint8_t TCP_GONE = 6; // from the server or the page: that address left the switch

// page_state.
constexpr uint32_t PAGE_NONE = 0;       // nothing attached yet
constexpr uint32_t PAGE_CONNECTING = 1; // attached; connecting (or will, once wanted)
constexpr uint32_t PAGE_ONLINE = 2;     // vip and netmask are valid
constexpr uint32_t PAGE_OFFLINE = 3;    // lost the switch or could not reach it; retrying

constexpr uint32_t SERVER_HOST = 0x0A4D0001u; // 10.77.0.1: the server's own host
constexpr uint32_t BROADCAST = 0xFFFFFFFFu;

struct Mailbox {
    // Fixed, written by the kit before it publishes the mailbox.
    uint32_t magic, version, hdr_bytes, slot_bytes; //   0
    uint32_t tx_slots, rx_slots, tx_off, rx_off;    //  16: offsets from the mailbox
    // Kit -> page.
    uint32_t want;      //  32: 1 from the game's WSAStartup to its last WSACleanup
    uint32_t tx_head;   //  36
    uint32_t tx_bell;   //  40
    uint32_t rx_tail;   //  44
    uint32_t kit_alive; //  48: +1 per network-thread loop
    uint32_t kit_pad[3];
    // Page -> kit.
    uint32_t page_state;      //  64
    uint32_t vip;             //  68
    uint32_t netmask;         //  72
    uint32_t generation;      //  76: +1 per new switch connection
    uint32_t tx_tail;         //  80
    uint32_t rx_head;         //  84
    uint32_t rx_bell;         //  88
    uint32_t rx_full_drop;    //  92
    uint32_t tx_offline_drop; //  96
    uint32_t pad[7];          // 100 .. 127
    // The player's name as the page uses it on the switch (UTF-8, NUL-terminated), written
    // before the page first changes page_state; the kit offers it to the game's mods as
    // RECOMP_LAN_NAME (the game's own Online mode signs in under it).
    char player_name[64];     // 128 .. 191
    uint32_t pad2[16];        // 192 .. 255
};

struct Slot {
    uint32_t len, kind, src_ip, dst_ip; //  0: len = payload bytes (LAN header included)
    uint16_t src_port, dst_port;        // 16
    uint32_t pad;                       // 20
    double t_ms;                        // 24: when it was queued (wall clock)
    uint8_t data[SLOT_BYTES - SLOT_HDR];
};

static_assert(sizeof(Mailbox) == HDR_BYTES, "the mailbox header is 256 bytes");
static_assert(offsetof(Mailbox, want) == 32 && offsetof(Mailbox, kit_alive) == 48, "kit words");
static_assert(offsetof(Mailbox, page_state) == 64 && offsetof(Mailbox, generation) == 76, "page words");
static_assert(offsetof(Mailbox, tx_tail) == 80 && offsetof(Mailbox, tx_offline_drop) == 96, "page words");
static_assert(offsetof(Mailbox, player_name) == 128, "the player's name");
static_assert(sizeof(Slot) == SLOT_BYTES && offsetof(Slot, t_ms) == 24 && offsetof(Slot, data) == SLOT_HDR,
              "slot layout");
static_assert(MAX_PAYLOAD <= SLOT_BYTES - SLOT_HDR, "a payload fits a slot");

} // namespace lan

// Test seams (kit/runtime/tests/winsock_tests.cpp plays the page and the switch).
// The mailbox once a WSAStartup in LAN mode made it, else null.
lan::Mailbox *winsock_lan_mailbox();
// -1 follows RECOMP_LAN; 0 or 1 overrides it.
void winsock_test_set_lan(int on);
// One pass of the network thread's work, on the calling thread.
void winsock_test_pump();
