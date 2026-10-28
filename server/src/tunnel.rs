//! DirtySock's ProtoTunnel, as NFS Carbon (v1.4) speaks it: the encrypted envelope around every
//! datagram between a GameManager player and its host on UDP port 1042 (the host's port is the
//! EGEG answer's P). Read from the game's own code (NFSC.exe; addresses are the exe's):
//!
//! - One tunnel per remote client (ProtoTunnelAlloc 00988080). Its key is a string: the game's
//!   EKEY from Theater, %-decoded and cut to 16 characters (GetString 008dde00 with a 17-byte
//!   buffer, 0093dba6; kept again in 17 bytes by the GameManager connection, 0095f35a).
//!   RC4 (CryptArc4Init 0098df10, one key-schedule pass; CryptArc4Apply 0098e060), one
//!   stream per direction, both from the same key.
//! - If a connection is not up after half its timeout (5 of 10 s), the game frees the tunnel,
//!   inverts every byte of the key (NOT, 0095fd25) and tries again, at the host's internal
//!   address (INT-IP/INT-PORT, the same on our network). So a host must accept both keys.
//! - A datagram (at most 1000 bytes, 00988bd7): a big-endian u16 H, the sender's position in
//!   its RC4 stream in 4-byte units, then n sub-packet headers of 2 bytes, then the n
//!   sub-packets' data in the same order. Everything after H is encrypted except the data of
//!   sub-packets whose port is not flagged "encrypted"; the encrypted data comes first.
//!   The stream then moves on by the encrypted length, rounded up to 4 bytes (009879c0
//!   send, 00987e00 receive). A receiver skips forward to H and drops a datagram older than
//!   its position (RC4 cannot go back).
//! - Sub-packet header: [len >> 4, (len & 15) << 4 | port index]. The port index is a
//!   position in the tunnel's port list (GameManager's: 6000 plain, 6001 and 6002 encrypted,
//!   0095e970); index 7 is the tunnel's own control record, always encrypted: 6 bytes
//!   [0, the sender's client id (big-endian u32), 1 once it has received from us].
//!   A sender adds it until the other end says it receives (00988b44..00988b89).
//! - A client id is a hash of the player's user id written in decimal ("%I64d", 0095e910):
//!   h = c ^ ((h << 5) ^ (h >> 27, arithmetic)) per character. A receiver whose tunnel has not
//!   heard from the other end yet only accepts a datagram whose control record carries the
//!   id it expects (00988810), so the host must send the id of the user named as HUID.

/// RC4, as CryptArc4 does it.
#[derive(Clone)]
pub struct Rc4 {
    s: [u8; 256],
    i: u8,
    j: u8,
}

impl Rc4 {
    pub fn new(key: &[u8]) -> Rc4 {
        let mut s = [0u8; 256];
        for (i, v) in s.iter_mut().enumerate() {
            *v = i as u8;
        }
        let mut j = 0u8;
        if !key.is_empty() {
            for i in 0..256 {
                j = j.wrapping_add(s[i]).wrapping_add(key[i % key.len()]);
                s.swap(i, j as usize);
            }
        }
        Rc4 { s, i: 0, j: 0 }
    }

    fn next(&mut self) -> u8 {
        self.i = self.i.wrapping_add(1);
        self.j = self.j.wrapping_add(self.s[self.i as usize]);
        self.s.swap(self.i as usize, self.j as usize);
        self.s[(self.s[self.i as usize].wrapping_add(self.s[self.j as usize])) as usize]
    }

    pub fn apply(&mut self, data: &mut [u8]) {
        for b in data {
            *b ^= self.next();
        }
    }

    pub fn skip(&mut self, n: usize) {
        for _ in 0..n {
            self.next();
        }
    }
}

/// The tunnel key the game derives from Theater's EKEY value as sent ("...%3d%3d").
pub fn key_from_ekey(ekey: &str) -> Vec<u8> {
    let decoded = crate::util::decode_component(ekey).unwrap_or_else(|| ekey.to_string());
    decoded.into_bytes().into_iter().take(16).collect()
}

/// The key of the game's second try: every byte inverted.
pub fn inverted(key: &[u8]) -> Vec<u8> {
    key.iter().map(|b| !b).collect()
}

/// GameManager's client id for a user id (0095e910).
pub fn client_id(user_id: u64) -> u32 {
    let mut h: u32 = 0;
    for c in user_id.to_string().bytes() {
        let sar = ((h as i32) >> 27) as u32;
        h = (c as u32) ^ ((h << 5) ^ sar);
    }
    h
}

pub const CONTROL: u8 = 7;
/// The largest datagram a game's tunnel reads.
pub const MAX_DATAGRAM: usize = 1000;
/// Whether a port index's data is encrypted in GameManager's tunnels (0095e970): index
/// 0 is port 6000 (plain), 1 and 2 are 6001 and 6002, 7 the control record.
pub fn encrypted(index: u8) -> bool {
    matches!(index, 1 | 2 | CONTROL)
}

/// One direction of a tunnel: an RC4 stream and the position in it, in 4-byte units.
#[derive(Clone)]
pub struct Stream {
    rc4: Rc4,
    pos: u16,
}

impl Stream {
    pub fn new(key: &[u8]) -> Stream {
        Stream { rc4: Rc4::new(key), pos: 0 }
    }

    /// Steps over what was used: `n` bytes, then to the next 4-byte boundary.
    fn advance(&mut self, n: usize) {
        self.pos = self.pos.wrapping_add((n / 4) as u16);
        if n % 4 != 0 {
            self.rc4.skip(4 - n % 4);
            self.pos = self.pos.wrapping_add(1);
        }
    }
}

/// A sub-packet: its port index and its data.
pub type Part = (u8, Vec<u8>);

/// One datagram from parts, encrypted with `send` (which moves on). The control record, if
/// any, should be one of the parts. Parts must fit (MAX_DATAGRAM, 8 parts, 4095 bytes each).
pub fn seal(send: &mut Stream, parts: &[Part]) -> Vec<u8> {
    let mut order: Vec<&Part> = parts.iter().filter(|p| encrypted(p.0)).collect();
    order.extend(parts.iter().filter(|p| !encrypted(p.0)));
    let mut out = Vec::with_capacity(2 + parts.iter().map(|p| 2 + p.1.len()).sum::<usize>());
    out.extend_from_slice(&send.pos.to_be_bytes());
    for (idx, data) in &order {
        let len = data.len() as u16;
        out.push((len >> 4) as u8);
        out.push(((len & 15) << 4) as u8 | idx);
    }
    let mut secret = order.len() * 2;
    for (idx, data) in &order {
        out.extend_from_slice(data);
        if encrypted(*idx) {
            secret += data.len();
        }
    }
    send.rc4.apply(&mut out[2..2 + secret]);
    send.advance(secret);
    out
}

/// Why a datagram was not taken.
#[derive(Debug, PartialEq)]
pub enum Refused {
    /// Older than the stream position (a repeat or a late one).
    Stale,
    /// Its headers do not add up to its length (the wrong key, or garbage).
    Malformed,
}

/// The parts of one datagram, decrypted with `recv` (which moves on only if it is good).
pub fn open(recv: &mut Stream, data: &[u8]) -> Result<Vec<Part>, Refused> {
    if data.len() < 4 {
        return Err(Refused::Malformed);
    }
    let at = u16::from_be_bytes([data[0], data[1]]);
    let mut diff = at.wrapping_sub(recv.pos) as i16 as i32;
    if diff < 0 {
        if diff >= -0x4000 {
            return Err(Refused::Stale);
        }
        diff += 0x10000;
    }
    let mut s = recv.clone();
    s.rc4.skip(diff as usize * 4);
    s.pos = s.pos.wrapping_add(diff as u16);
    let mut buf = data[2..].to_vec();
    let mut heads = Vec::new();
    let (mut covered, mut secret) = (2usize, 0usize);
    while covered < data.len() {
        let k = heads.len() * 2;
        if k + 2 > buf.len() || heads.len() >= 8 {
            return Err(Refused::Malformed);
        }
        s.rc4.apply(&mut buf[k..k + 2]);
        let len = ((buf[k] as usize) << 4) | (buf[k + 1] >> 4) as usize;
        let idx = buf[k + 1] & 15;
        covered += 2 + len;
        if encrypted(idx) {
            secret += len;
        }
        heads.push((idx, len));
    }
    if covered != data.len() {
        return Err(Refused::Malformed);
    }
    let start = heads.len() * 2;
    s.rc4.apply(&mut buf[start..start + secret]);
    s.advance(start + secret);
    *recv = s;
    let mut parts = Vec::with_capacity(heads.len());
    let mut at = start;
    for (idx, len) in heads {
        parts.push((idx, buf[at..at + len].to_vec()));
        at += len;
    }
    Ok(parts)
}

/// A control record: the sender's client id, and whether it has heard from us.
pub fn control(client: u32, heard: bool) -> Part {
    let id = client.to_be_bytes();
    (CONTROL, vec![0, id[0], id[1], id[2], id[3], u8::from(heard)])
}

/// The client id and "heard from you" flag in a control record.
pub fn read_control(data: &[u8]) -> Option<(u32, bool)> {
    (data.len() >= 6).then(|| (u32::from_be_bytes([data[1], data[2], data[3], data[4]]), data[5] == 1))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(s: &str) -> Vec<u8> {
        s.split_whitespace().map(|b| u8::from_str_radix(b, 16).unwrap()).collect()
    }

    /// A joining game's first tunnel datagrams to the trace host (run4, 2026-10-24), keyed by
    /// EKEY=AAAAAAAAAAAAAAAAAAAAAA%3d%3d; the second try (after 5 s) with the inverted key.
    #[test]
    fn the_games_own_datagrams_open() {
        let key = key_from_ekey("AAAAAAAAAAAAAAAAAAAAAA%3d%3d");
        assert_eq!(key, b"AAAAAAAAAAAAAAAA");
        let first = hex("00 00 c2 ac e6 bc c0 37 38 09 f1 ab 69 5c e4 f1 14 b5 da 1b");
        let second = hex("00 05 14 ce 05 f8 6a ff 5c 55 7b e0 ff c5 6d 61 a0 b5 50 21");
        let mut recv = Stream::new(&key);
        let parts = open(&mut recv, &first).unwrap();
        // The control record (the joiner's UID 424415906 hashed) and CommUDP's INIT to 6001.
        assert_eq!(parts, vec![(7, vec![0, 0x94, 0xf5, 0xa8, 0x4b, 0]), (1, hex("00 00 00 01 21 9e ad 5e"))]);
        assert_eq!(read_control(&parts[0].1), Some((client_id(424_415_906), false)));
        assert_eq!(recv.pos, 5);
        // The same again, one stream step later.
        assert_eq!(open(&mut recv, &second).unwrap(), parts);
        // A repeat of the first is older than where the stream is.
        assert_eq!(open(&mut recv, &first), Err(Refused::Stale));
        // The retry: a new tunnel, the inverted key, from the start of its stream.
        let retry = hex("00 00 83 ee 88 13 88 ea ed 14 7c 91 b9 c5 8f ef f8 7d 51 49");
        assert_eq!(open(&mut Stream::new(&key), &retry), Err(Refused::Malformed));
        assert_eq!(open(&mut Stream::new(&inverted(&key)), &retry).unwrap(), parts);
    }

    #[test]
    fn sealed_datagrams_open_and_skip_ahead() {
        let key = key_from_ekey("VheUv4/XQSxSumZDAJ4ueg%3d%3d");
        assert_eq!(key, b"VheUv4/XQSxSumZD");
        let (mut send, mut recv) = (Stream::new(&key), Stream::new(&key));
        let parts = vec![control(client_id(354_668_344), true), (1, b"hello there".to_vec()), (0, b"plain".to_vec())];
        let a = seal(&mut send, &parts);
        // The plain part's data is sent as it is.
        assert!(a.ends_with(b"plain"));
        let b = seal(&mut send, &[(2, vec![7; 100])]);
        let c = seal(&mut send, &[(1, vec![1, 2, 3])]);
        assert_eq!(open(&mut recv, &a).unwrap(), parts);
        // b is lost: c still opens (the receiver skips to c's position), b is then stale.
        assert_eq!(open(&mut recv, &c).unwrap(), vec![(1, vec![1, 2, 3])]);
        assert_eq!(open(&mut recv, &b), Err(Refused::Stale));
    }

    #[test]
    fn client_ids_hash_the_decimal_user_id() {
        assert_eq!(client_id(424_415_906), 0x94f5_a84b);
        assert_eq!(client_id(354_668_344), 0x6f7c_fbee);
    }
}
