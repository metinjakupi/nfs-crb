//! Small helpers. Most keep the Node relay's JavaScript string rules (slice, trim, the name
//! filter, decodeURIComponent, URLSearchParams), so both servers clean a name the same way.

use std::fmt::Write as _;
use std::sync::{Mutex, MutexGuard};
use std::time::{SystemTime, UNIX_EPOCH};

use serde_json::Value;

/// A lock that a panicking task cannot poison for everyone else: the state behind it is
/// plain data that stays consistent between statements.
pub fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// JavaScript's `s.slice(0, n)` on a string: the first n UTF-16 code units. A character
/// that would be cut in half is left out (JavaScript would keep half a surrogate pair).
pub fn js_slice(s: &str, n: usize) -> &str {
    let mut units = 0;
    for (i, c) in s.char_indices() {
        units += c.len_utf16();
        if units > n {
            return &s[..i];
        }
    }
    s
}

/// JavaScript's trim(): Unicode White_Space plus U+FEFF. (Rust's own also trims U+0085,
/// which JavaScript does not, but the callers have replaced or removed control characters.)
pub fn js_trim(s: &str) -> &str {
    s.trim_matches(|c: char| c.is_whitespace() || c == '\u{feff}')
}

/// A name or crew: letters, digits, space, _ . - only, trimmed, at most `max` long, or
/// `fallback` when nothing is left. Anything but a JSON string counts as "" (the Node relay
/// never coerces client fields). Letters are Rust's Alphabetic, which also keeps combining
/// vowel signs (Other_Alphabetic) that the Node relay's \p{L} drops.
pub fn label(value: Option<&Value>, fallback: &str, max: usize) -> String {
    let raw = value.and_then(Value::as_str).unwrap_or("");
    let kept: String = raw
        .chars()
        .filter(|&c| c.is_alphabetic() || c.is_numeric() || matches!(c, ' ' | '_' | '.' | '-'))
        .collect();
    let out = js_slice(js_trim(&kept), max);
    if out.is_empty() { fallback.to_string() } else { out.to_string() }
}

/// A JSON string literal, escaped as JSON.stringify does.
pub fn json_str(s: &str) -> String {
    serde_json::to_string(s).unwrap_or_else(|_| "\"\"".into())
}

/// Equal bytes, compared without stopping at the first difference (the key check).
pub fn same_bytes(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).fold(0u8, |d, (x, y)| d | (x ^ y)) == 0
}

fn hex_val(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(b - b'a' + 10),
        b'A'..=b'F' => Some(b - b'A' + 10),
        _ => None,
    }
}

/// JavaScript's decodeURIComponent: None where it throws (a % without two hex digits, or
/// escapes that are not UTF-8).
pub fn decode_component(s: &str) -> Option<String> {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' {
            let hi = hex_val(*b.get(i + 1)?)?;
            let lo = hex_val(*b.get(i + 2)?)?;
            out.push(hi << 4 | lo);
            i += 3;
        } else {
            out.push(b[i]);
            i += 1;
        }
    }
    String::from_utf8(out).ok()
}

/// One application/x-www-form-urlencoded name or value, as URLSearchParams reads it:
/// + is a space, a bad % escape stays as it is, bytes that are not UTF-8 become U+FFFD.
pub fn form_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'+' => out.push(b' '),
            b'%' if i + 2 < b.len() && hex_val(b[i + 1]).is_some() && hex_val(b[i + 2]).is_some() => {
                out.push(hex_val(b[i + 1]).unwrap() << 4 | hex_val(b[i + 2]).unwrap());
                i += 2;
            }
            c => out.push(c),
        }
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Milliseconds since 1970 (0 for a time before it).
pub fn unix_ms(t: SystemTime) -> i64 {
    t.duration_since(UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0)
}

/// (year, month, day, hour, minute, second, millisecond, weekday 0 = Sunday) in UTC.
fn civil(ms: i64) -> (i64, u32, u32, u32, u32, u32, u32, u32) {
    let days = ms.div_euclid(86_400_000);
    let rest = ms.rem_euclid(86_400_000);
    // Howard Hinnant's civil_from_days.
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let y = yoe + era * 400 + if m <= 2 { 1 } else { 0 };
    let wd = (days + 4).rem_euclid(7) as u32;
    let (h, mi, s, milli) = (rest / 3_600_000, rest / 60_000 % 60, rest / 1000 % 60, rest % 1000);
    (y, m, d, h as u32, mi as u32, s as u32, milli as u32, wd)
}

/// An HTTP date, as JavaScript's toUTCString: "Sat, 24 Oct 2026 17:05:09 GMT".
pub fn http_date(t: SystemTime) -> String {
    const DAYS: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
    const MONTHS: [&str; 12] = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    let (y, m, d, h, mi, s, _, wd) = civil(unix_ms(t));
    format!("{}, {d:02} {} {y} {h:02}:{mi:02}:{s:02} GMT", DAYS[wd as usize], MONTHS[m as usize - 1])
}

/// JavaScript's toISOString: "2026-10-24T17:05:09.123Z".
pub fn iso_date(t: SystemTime) -> String {
    let (y, m, d, h, mi, s, ms, _) = civil(unix_ms(t));
    format!("{y:04}-{m:02}-{d:02}T{h:02}:{mi:02}:{s:02}.{ms:03}Z")
}

/// A file-name time stamp: "20261024-170509".
pub fn file_stamp(t: SystemTime) -> String {
    let (y, m, d, h, mi, s, _, _) = civil(unix_ms(t));
    format!("{y:04}{m:02}{d:02}-{h:02}{mi:02}{s:02}")
}

/// Standard base64 with padding (the WebSocket handshake's accept key).
pub fn base64(bytes: &[u8]) -> String {
    const ABC: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for c in bytes.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            if i <= c.len() {
                out.push(ABC[(n >> (18 - 6 * i) & 63) as usize] as char);
            } else {
                out.push('=');
            }
        }
    }
    out
}

/// Standard base64 back to bytes (padding optional, no whitespace); None if it is not base64.
pub fn base64_decode(text: &str) -> Option<Vec<u8>> {
    let t = text.trim_end_matches('=');
    let mut out = Vec::with_capacity(t.len() * 3 / 4);
    let (mut acc, mut bits) = (0u32, 0u32);
    for c in t.bytes() {
        let v = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            _ => return None,
        } as u32;
        acc = acc << 6 | v;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
            acc &= (1 << bits) - 1;
        }
    }
    (bits < 6).then_some(out)
}

/// Lowercase hex.
pub fn hex(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        let _ = write!(s, "{b:02x}");
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn javascript_string_rules() {
        assert_eq!(js_slice("abc", 2), "ab");
        assert_eq!(js_slice("a😀b", 2), "a"); // the emoji is two UTF-16 units
        assert_eq!(js_slice("a😀b", 3), "a😀");
        assert_eq!(js_trim("\u{feff} x \u{3000}"), "x");
        let v = |s: &str| Value::String(s.into());
        assert_eq!(label(Some(&v("  Night<Owls>!  ")), "", 16), "NightOwls");
        assert_eq!(label(Some(&v("café_1.-")), "", 16), "café_1.-");
        assert_eq!(label(Some(&Value::from(42)), "Player", 20), "Player");
        assert_eq!(label(None, "Player", 20), "Player");
        assert_eq!(label(Some(&v("abcdefghij klmnopqrstuvwxyz")), "", 11), "abcdefghij ");
    }

    #[test]
    fn uri_decoding() {
        assert_eq!(decode_component("friends%20%2F%20caf%C3%A9").as_deref(), Some("friends / café"));
        assert_eq!(decode_component("%zz"), None);
        assert_eq!(decode_component("%E0%A4"), None);
        assert_eq!(decode_component("%4"), None);
        assert_eq!(form_decode("a+b%20c%zz%C3%A9"), "a b c%zzé");
        assert_eq!(form_decode("%"), "%");
        assert_eq!(form_decode("x%4"), "x%4");
    }

    #[test]
    fn dates() {
        let t = UNIX_EPOCH + Duration::from_millis(1_793_034_309_123); // values from Node's Date
        assert_eq!(iso_date(t), "2026-10-26T17:05:09.123Z");
        assert_eq!(http_date(t), "Mon, 26 Oct 2026 17:05:09 GMT");
        assert_eq!(file_stamp(t), "20261026-170509");
        assert_eq!(http_date(UNIX_EPOCH), "Thu, 01 Jan 1970 00:00:00 GMT");
        let leap = UNIX_EPOCH + Duration::from_secs(951_782_400); // 2000-02-29
        assert_eq!(iso_date(leap), "2000-02-29T00:00:00.000Z");
    }

    #[test]
    fn encodings() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
        for s in [&b""[..], b"f", b"fo", b"foo", b"TXN=ReportMetrics\n\0\xff"] {
            assert_eq!(base64_decode(&base64(s)).as_deref(), Some(s));
        }
        assert_eq!(base64_decode("Zm9v!"), None);
        assert_eq!(base64_decode("Z"), None);
        assert_eq!(hex(&[0, 0xab]), "00ab");
        assert!(same_bytes(b"key", b"key"));
        assert!(!same_bytes(b"key", b"kez"));
        assert!(!same_bytes(b"key", b"keys"));
    }
}
