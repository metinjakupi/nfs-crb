//! The race messages of NFS Carbon's online mode ("OLMSG"), as the client's code reads and
//! writes them (NFSC.exe v1.4, 0081c000..00837fff). They travel as GameManager game data (not
//! system messages), one message per packet, each a bit stream written most significant bit
//! first (BitWriter 007ab050, BitReader 007ab0e0):
//!
//! - every message starts with its type, 32 bits (the table at 00a7b388: id, name, size);
//! - numbers are 32 bits (or as many as the message says), floats their 32 IEEE bits;
//! - byte strings are a 16-bit length and the bytes (007bd240), text the same without its NUL.
//!
//! B-version's tail, "+22012b18", is a hash of that table (the sum of its sizes | 0x22000000,
//! 0081f8d0), and GameInitInfo carries it again: a client whose table differs stops at once
//! (00825279 writes to address 0). Messages longer than 0x1e4 bytes go as BigMessage pieces.

/// OLMSG type ids (the client's own table, 00a7b388).
pub const GAME_INIT_INFO: u32 = 0x00;
pub const CLOCK_SYNC_REQUEST: u32 = 0x01;
pub const CLOCK_SYNC_START: u32 = 0x02;
pub const CLOCK_SYNC_END: u32 = 0x03;
pub const PLAYER_CAR_DATA: u32 = 0x04;
pub const CAR_STATE: u32 = 0x06;
pub const START_LOADING: u32 = 0x08;
pub const START_RACE_SYNC_BEGIN: u32 = 0x0a;
pub const LEADER_FINISHED: u32 = 0x0d;
pub const RACER_FINISHED: u32 = 0x0e;
pub const READY: u32 = 0x0f;
pub const GAME_RESULTS: u32 = 0x10;
pub const FINAL_GAME_RESULTS: u32 = 0x11;
pub const START_TIMER: u32 = 0x1b;
pub const ACTIVE_GAME_MESSAGE: u32 = 0x1c;
pub const MATCHMAKING_ON: u32 = 0x13;
pub const MATCHMAKING_OFF: u32 = 0x14;
pub const ENABLE_JOINS: u32 = 0x17;
pub const DISABLE_JOINS: u32 = 0x18;
pub const GAME_ATTRIBUTES: u32 = 0x1d;
pub const BIG_MESSAGE: u32 = 0x1e;
pub const PURSUIT_TAG_SYNC: u32 = 0x1f;

/// The table's hash, as B-version and GameInitInfo carry it.
pub const VERSION_HASH: u32 = 0x2201_2b18;

pub fn name(t: u32) -> &'static str {
    const NAMES: [&str; 34] = [
        "GameInitInfo",
        "ClockSyncRequest",
        "ClockSyncStart",
        "ClockSyncEnd",
        "PlayerCarData",
        "PlayerControlledAICar",
        "CarState",
        "CarStateBlock",
        "StartLoading",
        "ReadyToStart",
        "StartRaceSyncBegin",
        "StartRaceSyncMessage",
        "StartRace",
        "LeaderFinished",
        "RacerFinished",
        "Ready",
        "GameResults",
        "FinalGameResults",
        "LatencyInfo",
        "Matchmaking_On_Request",
        "Matchmaking_Off_Request",
        "Invites_On_Request",
        "Invites_Off_Request",
        "EnableJoins_Request",
        "DisableJoins_Request",
        "ActiveGame_CollectStats",
        "ActiveGame_UpdateStats",
        "Start_Timer",
        "ActiveGame_Message",
        "GameAttributes",
        "BigMessage",
        "PursuitTagSync",
        "KillRebroadcaster",
        "PostRaceSync",
    ];
    NAMES.get(t as usize).copied().unwrap_or("unknown")
}

/// The client's game attribute names, by the index GameAttributes sends (00a7b300).
pub const ATTRIBUTES: [&str; 21] = [
    "version",
    "matchmaking_state",
    "game_type",
    "help_type",
    "game_mode",
    "skill",
    "team_play",
    "car_tier",
    "max_online_player",
    "length",
    "track",
    "n2o",
    "collision_detection",
    "player_dnf",
    "location",
    "race_type_circuit",
    "race_type_sprint",
    "race_type_canyon_due",
    "race_type_speedtrap",
    "race_type_knockout",
    "race_type_pursuit_tag",
];

/// A bit stream being written, most significant bit first.
#[derive(Default)]
pub struct Bits {
    pub buf: Vec<u8>,
    bits: usize,
}

impl Bits {
    pub fn put(&mut self, value: u32, n: usize) -> &mut Bits {
        for k in (0..n).rev() {
            if self.bits % 8 == 0 {
                self.buf.push(0);
            }
            if (value >> k) & 1 == 1 {
                let last = self.buf.len() - 1;
                self.buf[last] |= 0x80 >> (self.bits % 8);
            }
            self.bits += 1;
        }
        self
    }
    pub fn u32(&mut self, v: u32) -> &mut Bits {
        self.put(v, 32)
    }
    pub fn f32(&mut self, v: f32) -> &mut Bits {
        self.put(v.to_bits(), 32)
    }
    pub fn bytes(&mut self, b: &[u8]) -> &mut Bits {
        self.put(b.len() as u32, 16);
        for &x in b {
            self.put(x as u32, 8);
        }
        self
    }
    pub fn text(&mut self, s: &str) -> &mut Bits {
        self.bytes(s.as_bytes())
    }
}

/// A bit stream being read; None when it runs out.
pub struct Read<'a> {
    b: &'a [u8],
    at: usize,
}

impl<'a> Read<'a> {
    pub fn new(b: &'a [u8]) -> Read<'a> {
        Read { b, at: 0 }
    }
    pub fn get(&mut self, n: usize) -> Option<u32> {
        if self.at + n > self.b.len() * 8 {
            return None;
        }
        let mut v = 0u32;
        for _ in 0..n {
            let bit = (self.b[self.at / 8] >> (7 - self.at % 8)) & 1;
            v = (v << 1) | bit as u32;
            self.at += 1;
        }
        Some(v)
    }
    pub fn u32(&mut self) -> Option<u32> {
        self.get(32)
    }
    pub fn f32(&mut self) -> Option<f32> {
        self.get(32).map(f32::from_bits)
    }
    pub fn bytes(&mut self) -> Option<Vec<u8>> {
        let n = self.get(16)? as usize;
        (0..n).map(|_| self.get(8).map(|b| b as u8)).collect()
    }
    pub fn text(&mut self) -> Option<String> {
        self.bytes().map(|b| String::from_utf8_lossy(&b).into_owned())
    }
}

/// The type of a message (its first 32 bits).
pub fn kind(msg: &[u8]) -> Option<u32> {
    Read::new(msg).u32()
}

/// GameInitInfo (pack 008252f0, client handler 00831710): sent to a player once it is in
/// the game. `racer_base` is the first of the ten racer ids the client may use for its cars
/// (it takes base..base+9, 0081df40); `refused` non-zero turns the player away (state 3).
pub fn game_init_info(racer_base: u32, clock: f32, refused: u32, extra: u32) -> Vec<u8> {
    let mut b = Bits::default();
    b.u32(GAME_INIT_INFO).u32(VERSION_HASH).u32(racer_base).f32(clock).u32(refused).u32(extra);
    b.buf
}

/// GameAttributes (pack 00826c10): a count (8 bits), then per attribute its index in
/// ATTRIBUTES (8 bits) and its value as text. The client keeps only names it already has
/// (00827e30).
pub fn read_attributes(msg: &[u8]) -> Option<Vec<(u8, String)>> {
    let mut r = Read::new(msg);
    if r.u32()? != GAME_ATTRIBUTES {
        return None;
    }
    let n = r.get(8)?;
    (0..n).map(|_| Some((r.get(8)? as u8, r.text()?))).collect()
}

pub fn game_attributes(list: &[(u8, String)]) -> Vec<u8> {
    let mut b = Bits::default();
    b.u32(GAME_ATTRIBUTES).put(list.len() as u32, 8);
    for (i, v) in list {
        b.put(*i as u32, 8).text(v);
    }
    b.buf
}

/// The attribute that names the race for a game_mode value (the client's table, 009f3ab0).
pub fn race_attribute(game_mode: u32) -> Option<&'static str> {
    Some(match game_mode {
        1 => "race_type_circuit",
        0 => "race_type_sprint",
        13 => "race_type_canyon_due",
        5 => "race_type_speedtrap",
        15 => "race_type_knockout",
        14 => "race_type_pursuit_tag",
        _ => return None,
    })
}

/// A new room's settings as the game's own first GameAttributes have them (Sprint, any track,
/// 2 players, any car tier, laps 1, NOS and collision on, every race "Any"), unranked.
pub fn new_room_attributes() -> Vec<(u8, String)> {
    let named = [("game_type", "1"), ("game_mode", "0"), ("team_play", "1"), ("car_tier", "0"), ("max_online_player", "2"), ("length", "1"), ("n2o", "1"), ("collision_detection", "1")];
    (0..ATTRIBUTES.len())
        .map(|i| {
            let name = ATTRIBUTES[i];
            let v = named.iter().find(|(n, _)| *n == name).map(|(_, v)| *v).unwrap_or(if name.starts_with("race_type_") { "ABSTAIN" } else { "" });
            (i as u8, v.to_string())
        })
        .collect()
}

/// The modes a room set to "Any" can become (game_mode values, GameAttributes index 4).
pub const ANY_MODES: [u32; 6] = [0, 1, 5, 13, 14, 15];

/// The race the games will load, settled before StartLoading: a game loads the race its
/// settings name (008296f0), and with game_mode or the race left "Any" (ABSTAIN, -999 to
/// 00829450) it has none to load. So "Any" becomes one of the modes the room's PlayNow offered
/// races for (Canyon, a duel, only for two players), and an "Any" race one of that mode's
/// races, as EA's host chose them. Returns the settings and what was picked ("" if nothing).
pub fn settle_race(mut list: Vec<(u8, String)>, races: &std::collections::BTreeMap<String, Vec<String>>, players: usize, seed: usize) -> (Vec<(u8, String)>, String) {
    fn set(list: &mut Vec<(u8, String)>, idx: u8, v: &str) {
        match list.iter_mut().find(|(i, _)| *i == idx) {
            Some(e) => e.1 = v.to_string(),
            None => list.push((idx, v.to_string())),
        }
    }
    let offered = |m: u32| race_attribute(m).is_some_and(|a| races.get(a).is_some_and(|r| !r.is_empty()));
    let mut picked = Vec::new();
    let mode = match list.iter().find(|(i, _)| *i == 4).and_then(|(_, v)| v.parse::<u32>().ok()) {
        Some(m) => m,
        None => {
            let modes: Vec<u32> = ANY_MODES.iter().copied().filter(|&m| offered(m) && (m != 13 || players == 2)).collect();
            if modes.is_empty() {
                return (list, String::new());
            }
            let m = modes[seed % modes.len()];
            set(&mut list, 4, &m.to_string());
            picked.push(format!("game_mode = {m}"));
            m
        }
    };
    if let Some(attr) = race_attribute(mode) {
        let idx = ATTRIBUTES.iter().position(|a| *a == attr).unwrap_or(0) as u8;
        let current = list.iter().find(|(i, _)| *i == idx).map(|(_, v)| v.clone()).unwrap_or_default();
        let choices = races.get(attr).cloned().unwrap_or_default();
        if (current.is_empty() || current == "ABSTAIN") && !choices.is_empty() {
            let pick = choices[seed % choices.len()].clone();
            set(&mut list, idx, &pick);
            picked.push(format!("{attr} = {pick}"));
        }
    }
    (list, picked.join(", "))
}

/// game_mode of a Pursuit Tag room (GameAttributes index 4).
pub const MODE_PURSUIT_TAG: u32 = 14;

/// PursuitTagSync's codes in place of a racer id (sent by 00830a30, read by 00834f00 when the
/// race is a Pursuit Tag, race kind 0xe):
/// - every 10 frames the racer's own game sends [racer][TAG_TIMER][its bust meter] (0042d480);
/// - [racer][buster]: the racer was busted (0044a596, by the racer's own game);
/// - [TAG_EVADED][new racer]: the racer evaded for a minute and hands over to the nearest cop
///   (00629650); [TAG_LEFT][new racer]: the racer's player left (006254a4).
/// A role change swaps both cars (0065dd60 -> 0065d620 per car, a toggle that also adds the
/// time as the racer), and the sending game does not swap its own until the message comes back.
pub const TAG_TIMER: i16 = -8;
pub const TAG_EVADED: i16 = -9;
pub const TAG_LEFT: i16 = -7;

/// game_mode of a Canyon duel room.
pub const MODE_CANYON: u32 = 13;
/// In a Canyon duel (race kind 0xd), [racer][-4] says the sending game's heat is over
/// (00616dc7, once per heat; whoever receives it marks that racer done, 00616e10).
pub const HEAT_OVER: i16 = -4;

/// PursuitTagSync: the type, i16 a, i16 b, f32, u32 (00825cf0).
pub fn tag_sync(msg: &[u8]) -> Option<(i16, i16)> {
    if msg.len() < 16 || kind(msg) != Some(PURSUIT_TAG_SYNC) {
        return None;
    }
    Some((i16::from_be_bytes([msg[4], msg[5]]), i16::from_be_bytes([msg[6], msg[7]])))
}

/// The longest message a client sends whole (0082a835); longer ones go as BigMessage pieces
/// of this many bytes.
pub const BIG_PIECE: usize = 0x1e4;

/// One BigMessage piece (pack 00824a20): type, the sender's number for the whole message,
/// the whole length, this piece's offset and length (each 32 bits), then the bytes.
pub struct BigPiece {
    pub seq: u32,
    pub total: usize,
    pub offset: usize,
    pub data: Vec<u8>,
}

impl BigPiece {
    pub fn read(msg: &[u8]) -> Option<BigPiece> {
        let mut r = Read::new(msg);
        let (_, seq, total, offset, _len) = (r.u32()?, r.u32()?, r.u32()?, r.u32()?, r.u32()?);
        let data = r.bytes()?;
        Some(BigPiece { seq: seq & 0xffff, total: total as usize, offset: offset as usize, data })
    }
}

/// A long message as BigMessage pieces.
pub fn big_pieces(seq: u32, msg: &[u8]) -> Vec<Vec<u8>> {
    msg.chunks(BIG_PIECE)
        .enumerate()
        .map(|(i, part)| {
            let mut b = Bits::default();
            b.u32(BIG_MESSAGE).u32(seq).u32(msg.len() as u32).u32((i * BIG_PIECE) as u32).u32(part.len() as u32).bytes(part);
            b.buf
        })
        .collect()
}

/// GameResults as games send them (007d4320, read by 007d3eb0): the type, a 14-byte header
/// minus the type (u8 racers, u8 race kind, u16 race number, u8, u8, u32 the sender's
/// player id), then 8 racer rows of 3 bytes and 11 u32s (player id, times as raw f32s, ...).
pub const GAME_RESULTS_LEN: usize = 14 + 8 * (3 + 11 * 4);

/// FinalGameResults, the host's word on the race (read by 00826110, used by 00830540): the
/// same header and rows as GameResults, then u16 and 8 u8s the host adds (written as 0:
/// this host keeps no rankings). None if `results` is not a GameResults.
pub fn final_game_results(results: &[u8]) -> Option<Vec<u8>> {
    if results.len() != GAME_RESULTS_LEN || kind(results) != Some(GAME_RESULTS) {
        return None;
    }
    let mut m = FINAL_GAME_RESULTS.to_be_bytes().to_vec();
    m.extend_from_slice(&results[4..]);
    m.extend_from_slice(&[0; 10]);
    Some(m)
}

/// ClockSyncRequest: the client answers ClockSyncStart with its clock (00830850).
pub fn clock_sync_request() -> Vec<u8> {
    let mut b = Bits::default();
    b.u32(CLOCK_SYNC_REQUEST);
    b.buf
}

/// ClockSyncEnd (pack 00825610): the client's clock as it sent it, and the host's, both in
/// seconds; the client takes the host's clock as ours plus half the round trip (00827d90) and
/// keeps it in 1/4000 s.
pub fn clock_sync_end(theirs: f32, ours: f32) -> Vec<u8> {
    let mut b = Bits::default();
    b.u32(CLOCK_SYNC_END).f32(theirs).f32(ours);
    b.buf
}

/// StartRaceSyncBegin (pack 00825c40): the moment the race clock started, in the client's own
/// milliseconds, and two numbers the countdown keeps (0081d9b0 -> 007aa530).
pub fn start_race_sync_begin(at_ms: u32, a: f32, b2: f32) -> Vec<u8> {
    let mut b = Bits::default();
    b.u32(START_RACE_SYNC_BEGIN).u32(at_ms).f32(a).f32(b2);
    b.buf
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bits_go_most_significant_first() {
        let mut b = Bits::default();
        b.put(1, 1).put(0, 2).put(0x1f, 5).u32(0xdead_beef).text("ab");
        assert_eq!(b.buf, vec![0x9f, 0xde, 0xad, 0xbe, 0xef, 0x00, 0x02, b'a', b'b']);
        let mut r = Read::new(&b.buf);
        assert_eq!((r.get(1), r.get(2), r.get(5), r.u32(), r.text()), (Some(1), Some(0), Some(0x1f), Some(0xdead_beef), Some("ab".into())));
        assert_eq!(r.get(1), None);
    }

    #[test]
    fn a_new_room_has_the_games_own_defaults_but_unranked() {
        // the leader's first GameAttributes in a new room (run m4, 2026-10-25)
        let hex = "0000001d150000000100000200013003000004000130050000060001310700013008000132090001310a00000b0001310c0001310d00000e00000f00074142535441494e1000074142535441494e1100074142535441494e1200074142535441494e1300074142535441494e1400074142535441494e";
        let bytes: Vec<u8> = (0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap()).collect();
        let mut game = read_attributes(&bytes).unwrap();
        assert_eq!(game[2], (2, "0".to_string()), "the game's default is ranked");
        game[2].1 = "1".into();
        assert_eq!(new_room_attributes(), game);
    }

    #[test]
    fn attributes_round_trip() {
        let list = vec![(4u8, "0".to_string()), (16, "ABSTAIN".to_string())];
        let m = game_attributes(&list);
        assert_eq!(m, vec![0, 0, 0, 0x1d, 2, 4, 0, 1, b'0', 16, 0, 7, b'A', b'B', b'S', b'T', b'A', b'I', b'N']);
        assert_eq!(read_attributes(&m), Some(list));
    }

    #[test]
    fn any_becomes_an_offered_mode_and_race() {
        let mut races = std::collections::BTreeMap::new();
        races.insert("race_type_canyon_due".to_string(), vec!["c.1".to_string()]);
        races.insert("race_type_circuit".to_string(), vec!["ci.1".to_string(), "ci.2".to_string()]);
        let any = vec![(4u8, "ABSTAIN".to_string()), (15u8, "ABSTAIN".to_string()), (17u8, "ABSTAIN".to_string())];
        // Three players: no Canyon (a duel), so a circuit.
        let (list, picked) = settle_race(any.clone(), &races, 3, 0);
        assert_eq!(picked, "game_mode = 1, race_type_circuit = ci.1");
        assert!(list.contains(&(4, "1".into())) && list.contains(&(15, "ci.1".into())));
        // Two players may get the duel.
        let (list, _) = settle_race(any, &races, 2, 1);
        assert!(list.contains(&(4, "13".into())) && list.contains(&(17, "c.1".into())), "{list:?}");
        // A chosen mode and race stay as they are.
        let set = vec![(4u8, "1".to_string()), (15u8, "ci.2".to_string())];
        assert_eq!(settle_race(set.clone(), &races, 2, 0), (set, String::new()));
    }

    #[test]
    fn final_results_are_a_players_results_with_the_hosts_part() {
        let mut r = GAME_RESULTS.to_be_bytes().to_vec();
        r.extend((4..GAME_RESULTS_LEN).map(|i| i as u8));
        let f = final_game_results(&r).expect("a GameResults");
        assert_eq!((f.len(), kind(&f)), (GAME_RESULTS_LEN + 10, Some(FINAL_GAME_RESULTS)));
        assert_eq!((&f[4..GAME_RESULTS_LEN], &f[GAME_RESULTS_LEN..]), (&r[4..], &[0u8; 10][..]));
        assert_eq!(final_game_results(&r[..100]), None);
    }

    #[test]
    fn big_messages_cut_and_join() {
        let msg: Vec<u8> = (0..1107u32).map(|i| i as u8).collect();
        let pieces = big_pieces(7, &msg);
        assert_eq!(pieces.len(), 3);
        // As the game's own first piece of a PlayerCarData began (run r2).
        assert_eq!(pieces[0][..22], [0, 0, 0, 0x1e, 0, 0, 0, 7, 0, 0, 4, 0x53, 0, 0, 0, 0, 0, 0, 1, 0xe4, 1, 0xe4]);
        let mut whole = vec![0u8; msg.len()];
        for p in &pieces {
            let b = BigPiece::read(p).unwrap();
            assert_eq!((b.seq, b.total), (7, msg.len()));
            whole[b.offset..b.offset + b.data.len()].copy_from_slice(&b.data);
        }
        assert_eq!(whole, msg);
    }
}
