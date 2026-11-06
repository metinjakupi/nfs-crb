//! Server-owned moderation. Public labels and crew membership never grant authority.
use serde_json::{json, Value};
use std::collections::BTreeSet;
use std::fs::{self, File};
use std::io::Write;
use std::path::PathBuf;

/// The Latin letter a Cyrillic or Greek letter cannot be told from, case by case: a capital В
/// looks like B, a small в does not.
fn latin_twin(c: char) -> Option<char> {
    Some(match c {
        'А' | 'Α' => 'A', 'В' | 'Β' => 'B', 'С' | 'Ϲ' => 'C', 'Е' | 'Ε' => 'E', 'Н' | 'Η' => 'H', 'І' | 'Ι' | 'Ӏ' => 'I',
        'Ј' => 'J', 'К' | 'Κ' => 'K', 'М' | 'Μ' => 'M', 'Ν' => 'N', 'О' | 'Ο' => 'O', 'Р' | 'Ρ' => 'P', 'Ѕ' => 'S',
        'Т' | 'Τ' => 'T', 'Υ' | 'Ү' => 'Y', 'Х' | 'Χ' => 'X', 'Ζ' => 'Z',
        'а' | 'α' => 'a', 'с' | 'ϲ' => 'c', 'ԁ' => 'd', 'е' => 'e', 'һ' => 'h', 'і' | 'ι' | 'ӏ' => 'i', 'ј' => 'j',
        'ո' | 'η' => 'n', 'о' | 'ο' => 'o', 'р' | 'ρ' => 'p', 'ѕ' => 's', 'ν' => 'v', 'х' | 'χ' => 'x', 'у' | 'γ' => 'y',
        _ => return None,
    })
}

/// A name with its lookalike letters made Latin, for comparing, or the name as it is if any
/// other non-Latin letter stays: Меtin compares as Metin, while Иван stays a name of its own.
pub fn without_lookalikes(name: &str) -> String {
    let folded: String = name.chars().map(|c| latin_twin(c).unwrap_or(c)).collect();
    if folded.is_ascii() { folded } else { name.to_string() }
}

pub fn reserved(name: &str) -> bool {
    let low = name.to_lowercase();
    if low.contains("админ") || low.contains("адмін") || low.contains("αδμιν") {
        return true;
    }
    name.chars()
        .map(|c| {
            let c = if ('\u{ff01}'..='\u{ff5e}').contains(&c) {
                char::from_u32(c as u32 - 0xfee0).unwrap()
            } else {
                latin_twin(c).unwrap_or(c)
            };
            match c.to_ascii_lowercase() {
                '4' | '@' | 'à' | 'á' | 'â' | 'ã' | 'ä' | 'å' | 'ā' | 'ă' | 'ą' => 'a',
                'ď' | 'đ' => 'd',
                '1' | '!' | '|' | 'ì' | 'í' | 'î' | 'ï' | 'ī' | 'į' | 'ı' => 'i',
                'ñ' | 'ń' | 'ņ' | 'ň' => 'n',
                'м' => 'm',
                'п' => 'n',
                c => c,
            }
        })
        .filter(|c| c.is_ascii_alphanumeric())
        .collect::<String>()
        .contains("admin")
}
const BAD: &[&str] = &[
    "fuck",
    "fucks",
    "fucked",
    "fucker",
    "fuckers",
    "fucking",
    "motherfucker",
    "motherfuckers",
    "motherfucking",
    "shit",
    "shits",
    "shitty",
    "bullshit",
    "bitch",
    "bitches",
    "asshole",
    "assholes",
    "bastard",
    "bastards",
    "cunt",
    "cunts",
    "dick",
    "dicks",
    "cock",
    "cocks",
    "pussy",
    "pussies",
    "nigger",
    "niggers",
    "nigga",
    "niggas",
    "fag",
    "fags",
    "faggot",
    "faggots",
];
/// Whole words, including letters separated by punctuation/spaces. No substring matching.
pub fn profane(text: &str) -> bool {
    let raw: Vec<char> = text
        .chars()
        .filter(|c| !matches!(c, '\u{200b}' | '\u{200c}' | '\u{200d}' | '\u{feff}'))
        .map(|c| {
            let c = if ('\u{ff01}'..='\u{ff5e}').contains(&c) {
                char::from_u32(c as u32 - 0xfee0).unwrap()
            } else {
                c
            };
            c.to_ascii_lowercase()
        })
        .collect();
    let chars: Vec<char> = raw
        .iter()
        .enumerate()
        .map(|(i, &c)| match c {
            '0' => 'o',
            '1' => 'i',
            '3' => 'e',
            '4' => 'a',
            '5' => 's',
            '7' => 't',
            '!' | '|'
                if i > 0
                    && raw[i - 1].is_ascii_alphanumeric()
                    && raw.get(i + 1).is_some_and(|c| c.is_ascii_alphanumeric()) =>
            {
                'i'
            }
            '@' if raw.get(i + 1).is_some_and(|c| c.is_ascii_alphabetic()) => 'a',
            '$' if raw.get(i + 1).is_some_and(|c| c.is_ascii_alphabetic()) => 's',
            _ => c,
        })
        .collect();
    for start in 0..chars.len() {
        if start > 0 && chars[start - 1].is_ascii_alphanumeric() {
            continue;
        }
        for word in BAD {
            let mut at = start;
            let mut matched = true;
            for (i, expected) in word.chars().enumerate() {
                if i > 0 {
                    while at < chars.len() && !chars[at].is_ascii_alphanumeric() {
                        at += 1;
                    }
                }
                if chars.get(at) != Some(&expected) {
                    matched = false;
                    break;
                }
                at += 1;
            }
            if matched && (at == chars.len() || !chars[at].is_ascii_alphanumeric()) {
                return true;
            }
        }
    }
    false
}

#[derive(Default)]
pub struct Moderation {
    pub admins: BTreeSet<String>,
    file: Option<PathBuf>,
    data: Value,
}
impl Moderation {
    pub fn open(dir: Option<PathBuf>, owners: Option<String>) -> Result<Self, String> {
        let admins: BTreeSet<String> = owners
            .unwrap_or_default()
            .split(',')
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .map(str::to_string)
            .collect();
        if admins.iter().any(|s| {
            s.len() != 64
                || !s
                    .bytes()
                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        }) {
            return Err("invalid CARBON_ADMIN_OWNERS".into());
        }
        let mut out = Self {
            admins,
            data: json!({"version":1,"mutes":{},"reports":[],"audit":[],"next":1}),
            ..Self::default()
        };
        if let Some(dir) = dir {
            fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
            let file = dir.join("moderation.json");
            match fs::read(&file) {
                Ok(bytes) => {
                    let data: Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
                    if data["version"] != 1
                        || !data["mutes"].is_object()
                        || !data["reports"].is_array()
                        || !data["audit"].is_array()
                        || data["next"].as_u64().is_none()
                    {
                        return Err("invalid moderation data".into());
                    }
                    for (key, mute) in data["mutes"].as_object().unwrap() {
                        if !(key.starts_with("owner:") || key.starts_with("name:"))
                            || mute["name"]
                                .as_str()
                                .is_none_or(|n| n.is_empty() || n.len() > 20)
                            || mute["until"].as_u64().is_none()
                        {
                            return Err("invalid saved mute".into());
                        }
                    }
                    for field in ["reports", "audit"] {
                        for record in data[field].as_array().unwrap() {
                            if record["id"].as_u64().is_none()
                                || record["at"].as_u64().is_none()
                                || record["op"].as_str().is_none()
                                || record["actor"].as_str().is_none()
                                || record["name"].as_str().is_none()
                                || record["text"].as_str().is_none()
                            {
                                return Err("invalid saved moderation record".into());
                            }
                        }
                    }
                    out.data = data;
                }
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => return Err(e.to_string()),
            }
            out.file = Some(file);
        }
        Ok(out)
    }
    pub fn enabled(&self) -> bool {
        self.file.is_some()
    }
    pub fn is_admin(&self, owner: Option<&str>, name: &str) -> bool {
        name.eq_ignore_ascii_case("admin") && owner.is_some_and(|o| self.admins.contains(o))
    }
    pub fn muted(&self, owner: Option<&str>, name: &str, now: u64) -> Option<u64> {
        let by_owner =
            owner.and_then(|o| self.data["mutes"][format!("owner:{o}")]["until"].as_u64());
        let by_name =
            self.data["mutes"][format!("name:{}", name.to_ascii_lowercase())]["until"].as_u64();
        by_owner
            .into_iter()
            .chain(by_name)
            .filter(|until| *until > now)
            .max()
    }
    fn commit(&mut self, next: Value) -> Result<(), &'static str> {
        let file = self.file.as_ref().ok_or("unavailable")?;
        let tmp = file.with_extension("json.tmp");
        let bytes = serde_json::to_vec(&next).map_err(|_| "storage")?;
        let result = (|| -> std::io::Result<()> {
            let mut f = File::create(&tmp)?;
            f.write_all(&bytes)?;
            f.sync_all()?;
            fs::rename(&tmp, file)?;
            Ok(())
        })();
        result.map_err(|_| "storage")?;
        self.data = next;
        Ok(())
    }
    /// Authority is checked by the relay before calling this serialized writer.
    pub fn action(
        &mut self,
        op: &str,
        actor: &str,
        target: &str,
        owner: Option<&str>,
        text: &str,
        minutes: u64,
        now: u64,
        offset: usize,
    ) -> Result<Value, &'static str> {
        if op == "report" {
            if text.is_empty() {
                return Err("text");
            }
            let recent = self.data["reports"]
                .as_array()
                .unwrap()
                .iter()
                .filter(|r| {
                    r["actor"].as_str() == Some(actor)
                        && r["at"]
                            .as_u64()
                            .is_some_and(|at| at.saturating_add(600) > now)
                })
                .count();
            if recent >= 3 {
                return Err("busy");
            }
        }
        let mut next = self.data.clone();
        next["mutes"]
            .as_object_mut()
            .unwrap()
            .retain(|_, v| v["until"].as_u64().is_some_and(|u| u > now));
        let key = owner
            .map(|o| format!("owner:{o}"))
            .unwrap_or_else(|| format!("name:{}", target.to_ascii_lowercase()));
        match op {
            "muted" => {
                let rows: Vec<_> = next["mutes"]
                    .as_object()
                    .unwrap()
                    .values()
                    .map(|v| json!({"name":v["name"],"until":v["until"]}))
                    .collect();
                return Ok(
                    json!({"total":rows.len(),"rows":rows.into_iter().skip(offset).take(50).collect::<Vec<_>>()}),
                );
            }
            "reports" | "history" => {
                let field = if op == "reports" { "reports" } else { "audit" };
                let rows = next[field].as_array().unwrap();
                // Private owner identifiers never leave the server.
                return Ok(
                    json!({"total":rows.len(),"rows":rows.iter().rev().skip(offset).take(25).map(|v|json!({"id":v["id"],"op":v["op"],"name":v["name"],"text":v["text"],"at":v["at"],"until":v["until"]})).collect::<Vec<_>>()}),
                );
            }
            "mute" => {
                if !(1..=10080).contains(&minutes) {
                    return Err("duration");
                }
                next["mutes"][&key] = json!({"name":target,"until":now+minutes*60});
            }
            "unmute" => {
                next["mutes"].as_object_mut().unwrap().remove(&key);
                next["mutes"]
                    .as_object_mut()
                    .unwrap()
                    .remove(&format!("name:{}", target.to_ascii_lowercase()));
            }
            "report" | "kick" | "announce" => {}
            _ => return Err("operation"),
        }
        let id = next["next"].as_u64().unwrap();
        next["next"] = json!(id.checked_add(1).ok_or("storage")?);
        let record = json!({"id":id,"op":op,"actor":actor,"name":target,"text":text,"at":now,"until":if op=="mute" {Some(now+minutes*60)}else{None}});
        let field = if op == "report" { "reports" } else { "audit" };
        let rows = next[field].as_array_mut().unwrap();
        rows.push(record);
        if rows.len() > 10000 {
            rows.remove(0);
        }
        self.commit(next)?;
        Ok(json!({"id":id,"name":target,"until":if op=="mute" {Some(now+minutes*60)}else{None}}))
    }
}

#[cfg(test)]
mod lookalike_tests {
    use super::*;

    #[test]
    fn lookalikes_of_admin_are_reserved() {
        for name in ["Admin", "\u{410}dmin", "\u{430}\u{501}\u{43c}\u{456}\u{578}", "\u{e1}dm\u{ed}n", "\u{410}\u{414}\u{41c}\u{418}\u{41d}", "\u{430}\u{434}\u{43c}\u{456}\u{43d}", "Super\u{391}dmin"] {
            assert!(reserved(name), "{name}");
        }
        for name in ["Adam", "Metin", "\u{418}\u{432}\u{430}\u{43d}", "Badminton2"] {
            assert_eq!(reserved(name), name == "Badminton2", "{name}");
        }
    }

    #[test]
    fn real_names_in_other_alphabets_stay_themselves() {
        assert_eq!(without_lookalikes("\u{41c}\u{435}tin"), "Metin");
        assert_eq!(without_lookalikes("\u{418}\u{432}\u{430}\u{43d}"), "\u{418}\u{432}\u{430}\u{43d}"); // Ivan
        assert_eq!(without_lookalikes("\u{412}\u{43e}\u{432}\u{430}"), "\u{412}\u{43e}\u{432}\u{430}"); // Vova: small v has no twin
        assert_eq!(without_lookalikes("Arb\u{eb}r"), "Arb\u{eb}r");
    }
}
