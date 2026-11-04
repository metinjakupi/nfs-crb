//! Persistent, invitation-only crews. The relay serializes all calls. Public account IDs
//! are random and are unrelated to name-claim proofs. No member-count limit is imposed.
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::PathBuf;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use serde_json::{json, Map, Value};

#[derive(Clone)]
struct Account {
    id: String,
    name: String,
    crew: Option<String>,
}
#[derive(Clone)]
struct Crew {
    name: String,
    leader: String,
    members: BTreeSet<String>,
    revision: u64,
}
#[derive(Clone)]
struct Invite {
    crew: String,
    target: String,
    expires: u64,
}
#[derive(Clone, Default)]
struct Data {
    accounts: BTreeMap<String, Account>, // private owner digest -> public account
    crews: BTreeMap<String, Crew>,
    invites: BTreeMap<String, Invite>,
}
#[derive(Default)]
pub struct Crews {
    admins: BTreeSet<String>,
    data: Data,
    file: Option<PathBuf>,
    buckets: HashMap<String, (f64, Instant)>,
}

pub fn now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs())
}
fn id() -> Result<String, &'static str> {
    let mut bytes = [0u8; 16];
    File::open("/dev/urandom").and_then(|mut f| f.read_exact(&mut bytes)).map_err(|_| "storage")?;
    Ok(bytes.iter().map(|b| format!("{b:02x}")).collect())
}
fn key(name: &str) -> String {
    name.to_ascii_lowercase().chars().filter(|c| !c.is_ascii_whitespace()).collect()
}
fn name(value: &str) -> Result<String, &'static str> {
    let n = value.trim();
    if n.is_empty()
        || n.len() > 16
        || !n.bytes().all(|c| c.is_ascii_alphanumeric() || b" _.-".contains(&c))
        || !n.bytes().any(|c| c.is_ascii_alphanumeric())
    {
        return Err("name");
    }
    Ok(n.into())
}
fn string(v: &Value, field: &str) -> Result<String, String> {
    v.get(field).and_then(Value::as_str).map(str::to_string).ok_or_else(|| format!("missing {field}"))
}

impl Crews {
    pub fn set_admins(&mut self, admins:BTreeSet<String>) { self.admins=admins; }
    pub fn open(dir: Option<PathBuf>) -> Result<Self, String> {
        let Some(dir) = dir else { return Ok(Self::default()) };
        fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        let file = dir.join("crews.json");
        let mut out = Self { file: Some(file.clone()), ..Self::default() };
        match fs::read(&file) {
            Ok(bytes) => {
                let v: Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
                if v["version"] != 1 {
                    return Err("unsupported crew schema".into());
                }
                for (owner, a) in v["accounts"].as_object().ok_or("invalid crew accounts")? {
                    out.data.accounts.insert(
                        owner.clone(),
                        Account { id: string(a, "id")?, name: string(a, "name")?, crew: a["crew"].as_str().map(str::to_string) },
                    );
                }
                for (id, c) in v["crews"].as_object().ok_or("invalid crews")? {
                    let members = c["members"]
                        .as_array()
                        .ok_or("invalid members")?
                        .iter()
                        .map(|v| v.as_str().map(str::to_string).ok_or("invalid member"))
                        .collect::<Result<BTreeSet<_>, _>>()?;
                    let n = string(c, "name")?;
                    name(&n).map_err(str::to_string)?;
                    out.data.crews.insert(
                        id.clone(),
                        Crew {
                            name: n,
                            leader: string(c, "leader")?,
                            members,
                            revision: c["revision"].as_u64().ok_or("invalid crew revision")?,
                        },
                    );
                }
                for (id, i) in v["invites"].as_object().ok_or("invalid invitations")? {
                    out.data.invites.insert(
                        id.clone(),
                        Invite {
                            crew: string(i, "crew")?,
                            target: string(i, "target")?,
                            expires: i["expires"].as_u64().ok_or("invalid invitation expiry")?,
                        },
                    );
                }
                out.validate()?;
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => return Err(e.to_string()),
        }
        Ok(out)
    }
    pub fn enabled(&self) -> bool {
        self.file.is_some()
    }
    fn validate(&self) -> Result<(), String> {
        let mut ids = BTreeSet::new();
        let mut names = BTreeSet::new();
        let accounts: HashMap<&str, &Account> = self.data.accounts.values().map(|a| (a.id.as_str(), a)).collect();
        for a in self.data.accounts.values() {
            if !ids.insert(a.id.clone()) {
                return Err("duplicate crew account".into());
            }
            if let Some(c) = &a.crew {
                if !self.data.crews.get(c).is_some_and(|c| c.members.contains(&a.id)) {
                    return Err("broken membership".into());
                }
            }
        }
        for (id, c) in &self.data.crews {
            if !names.insert(key(&c.name)) || !c.members.contains(&c.leader) {
                return Err("invalid crew ownership".into());
            }
            for member in &c.members {
                if !accounts.get(member.as_str()).is_some_and(|a| a.crew.as_ref() == Some(id)) {
                    return Err("broken roster".into());
                }
            }
        }
        for i in self.data.invites.values() {
            if !self.data.crews.contains_key(&i.crew) || !ids.contains(&i.target) {
                return Err("broken invitation".into());
            }
        }
        Ok(())
    }
    fn value(&self) -> Value {
        json!({"version":1,
            "accounts":self.data.accounts.iter().map(|(o,a)| (o.clone(),json!({"id":a.id,"name":a.name,"crew":a.crew}))).collect::<Map<_,_>>(),
            "crews":self.data.crews.iter().map(|(id,c)| (id.clone(),json!({"name":c.name,"leader":c.leader,"members":c.members,"revision":c.revision}))).collect::<Map<_,_>>(),
            "invites":self.data.invites.iter().map(|(id,i)| (id.clone(),json!({"crew":i.crew,"target":i.target,"expires":i.expires}))).collect::<Map<_,_>>()})
    }
    fn save(&self) -> Result<(), &'static str> {
        let file = self.file.as_ref().ok_or("unavailable")?;
        let tmp = file.with_extension("json.tmp");
        let bytes = serde_json::to_vec(&self.value()).map_err(|_| "storage")?;
        let result = (|| -> std::io::Result<()> {
            let mut f = File::create(&tmp)?;
            f.write_all(&bytes)?;
            f.sync_all()?;
            // A last committed copy is available for manual recovery. Never auto-reset bad data.
            if file.exists() {
                fs::copy(file, file.with_extension("json.bak"))?;
            }
            fs::rename(&tmp, file)?;
            Ok(())
        })();
        result.map_err(|_| "storage")
    }
    fn transaction<T>(&mut self, f: impl FnOnce(&mut Data) -> Result<T, &'static str>) -> Result<T, &'static str> {
        if !self.enabled() {
            return Err("unavailable");
        }
        let before = self.data.clone();
        let result = f(&mut self.data).and_then(|v| self.save().map(|_| v));
        if result.is_err() {
            self.data = before;
        }
        result
    }
    pub fn account(&self, owner: &str) -> Option<&str> {
        self.data.accounts.get(owner).map(|a| a.id.as_str())
    }
    pub fn register(&mut self, owner: &str, shown: &str) -> Result<String, &'static str> {
        if let Some(a) = self.data.accounts.get(owner) {
            if a.name == shown {
                return Ok(a.id.clone());
            }
        }
        let owner = owner.to_string();
        let shown = shown.to_string();
        self.transaction(|d| {
            if let Some(a) = d.accounts.get_mut(&owner) {
                a.name = shown;
                return Ok(a.id.clone());
            }
            let id = id()?;
            d.accounts.insert(owner, Account { id: id.clone(), name: shown, crew: None });
            Ok(id)
        })
    }
    pub fn membership(&self, owner: &str) -> Option<(String, String)> {
        let id = self.data.accounts.get(owner)?.crew.as_ref()?;
        Some((id.clone(), self.data.crews.get(id)?.name.clone()))
    }
    pub fn summary(&self, owner: &str) -> Value {
        let account = self.data.accounts.get(owner);
        let crew = account.and_then(|a| a.crew.as_ref()).and_then(|id| {
            self.data.crews.get(id).map(|c| json!({"id":id,"name":c.name,"leader":c.leader,"revision":c.revision,"count":c.members.len()}))
        });
        let pending = self.data.invites.values().filter(|i| i.expires > now() && account.is_some_and(|a| a.id == i.target)).count();
        json!({"t":"crewstate","account":account.map(|a| &a.id),"crew":crew,"pending":pending})
    }
    // Account-wide bucket survives reconnects and opening additional tabs.
    pub fn allow(&mut self, account: &str) -> bool {
        let at = Instant::now();
        let b = self.buckets.entry(account.into()).or_insert((12.0, at));
        b.0 = (b.0 + at.duration_since(b.1).as_secs_f64() * 0.5).min(12.0);
        b.1 = at;
        if b.0 < 1.0 {
            false
        } else {
            b.0 -= 1.0;
            true
        }
    }
    pub fn action(&mut self, owner: &str, m: &Map<String, Value>, target: Option<String>) -> Result<(), &'static str> {
        let actor = self.account(owner).ok_or("identity")?.to_string();
        if !self.allow(&actor) {
            return Err("busy");
        }
        let op = m.get("op").and_then(Value::as_str).unwrap_or("");
        let field = |k| m.get(k).and_then(Value::as_str).unwrap_or("");
        let is_admin = self.admins.contains(owner);
        if matches!(op, "create" | "rename") && crate::moderation::reserved(field("name")) && !(is_admin && field("name").eq_ignore_ascii_case("admin")) { return Err("reserved"); }
        let admin_accounts: BTreeSet<String> = self.admins.iter().filter_map(|o|self.account(o).map(str::to_string)).collect();
        self.transaction(|d| {
            d.invites.retain(|_, i| i.expires > now());
            let current = d.accounts.get(owner).and_then(|a| a.crew.clone());
            match op {
                "create" => {
                    if current.is_some() {
                        return Err("one-crew");
                    }
                    let n = name(field("name"))?;
                    if d.crews.values().any(|c| key(&c.name) == key(&n)) {
                        return Err("taken");
                    }
                    let id = id()?;
                    d.crews
                        .insert(id.clone(), Crew { name: n, leader: actor.clone(), members: BTreeSet::from([actor.clone()]), revision: 1 });
                    d.accounts.get_mut(owner).unwrap().crew = Some(id);
                    d.invites.retain(|_, i| i.target != actor);
                }
                "accept" | "decline" => {
                    let inv = d.invites.get(field("invite")).ok_or("invitation")?.clone();
                    if inv.target != actor {
                        return Err("forbidden");
                    }
                    if op == "accept" {
                        if current.is_some() { return Err("one-crew"); }
                        if d.crews.get(&inv.crew).is_some_and(|c|crate::moderation::reserved(&c.name)) && !is_admin {return Err("reserved");}
                        let c = d.crews.get_mut(&inv.crew).ok_or("invitation")?;
                        c.members.insert(actor.clone());
                        c.revision += 1;
                        d.accounts.get_mut(owner).unwrap().crew = Some(inv.crew);
                        d.invites.retain(|_, i| i.target != actor);
                    } else {
                        d.invites.remove(field("invite"));
                    }
                }
                "leave" => {
                    let cid = current.ok_or("no-crew")?;
                    let c = d.crews.get_mut(&cid).ok_or("no-crew")?;
                    if field("crew") != cid || m.get("revision").and_then(Value::as_u64) != Some(c.revision) {
                        return Err("stale");
                    }
                    if c.leader == actor {
                        return Err("leader-leave");
                    }
                    c.members.remove(&actor);
                    c.revision += 1;
                    d.accounts.get_mut(owner).unwrap().crew = None;
                }
                "invite" | "revoke" | "kick" | "transfer" | "disband" | "rename" => {
                    let cid = current.ok_or("no-crew")?;
                    let c = d.crews.get_mut(&cid).ok_or("no-crew")?;
                    if field("crew") != cid || m.get("revision").and_then(Value::as_u64) != Some(c.revision) {
                        return Err("stale");
                    }
                    if c.leader != actor {
                        return Err("forbidden");
                    }
                    if crate::moderation::reserved(&c.name) && !is_admin {return Err("reserved");}
                    if crate::moderation::reserved(&c.name) && matches!(op,"invite"|"transfer") {
                        let target_id=if op=="invite" {target.as_deref().unwrap_or("")} else {field("member")};
                        if !admin_accounts.contains(target_id) {return Err("reserved");}
                    }
                    match op {
                        "invite" => {
                            let target = target.ok_or("target")?;
                            let a = d.accounts.values().find(|a| a.id == target).ok_or("target")?;
                            if a.crew.is_some() {
                                return Err("target-crew");
                            }
                            if !d.invites.values().any(|i| i.crew == cid && i.target == target) {
                                d.invites.insert(id()?, Invite { crew: cid, target, expires: now() + 7 * 86400 });
                            }
                        }
                        "revoke" => {
                            if !d.invites.get(field("invite")).is_some_and(|i| i.crew == cid) {
                                return Err("invitation");
                            }
                            d.invites.remove(field("invite"));
                        }
                        "kick" | "transfer" => {
                            let target = field("member");
                            if target == actor || !c.members.contains(target) {
                                return Err("member");
                            }
                            if op == "transfer" {
                                c.leader = target.into();
                            } else {
                                c.members.remove(target);
                                d.accounts.values_mut().find(|a| a.id == target).ok_or("member")?.crew = None;
                            }
                            c.revision += 1;
                        }
                        "disband" => {
                            for a in d.accounts.values_mut().filter(|a| a.crew.as_ref() == Some(&cid)) {
                                a.crew = None;
                            }
                            d.crews.remove(&cid);
                            d.invites.retain(|_, i| i.crew != cid);
                        }
                        "rename" => {
                            let n = name(field("name"))?;
                            if d.crews.iter().any(|(id, c)| id != &cid && key(&c.name) == key(&n)) {
                                return Err("taken");
                            }
                            let c = d.crews.get_mut(&cid).unwrap();
                            c.name = n;
                            c.revision += 1;
                        }
                        _ => unreachable!(),
                    }
                }
                _ => return Err("operation"),
            }
            Ok(())
        })
    }
    /// Pages bound each response, not the number of members. Only the owner sees a roster.
    pub fn page(&self, owner: &str, kind: &str, offset: usize) -> Result<Value, &'static str> {
        let a = self.data.accounts.get(owner).ok_or("identity")?;
        let accounts: HashMap<&str, &Account> = self.data.accounts.values().map(|a| (a.id.as_str(), a)).collect();
        let (total, rows): (usize, Vec<Value>) = match kind {
            "roster" => {
                let c = a.crew.as_ref().and_then(|id| self.data.crews.get(id)).ok_or("no-crew")?;
                (
                    c.members.len(),
                    c.members
                        .iter()
                        .skip(offset)
                        .take(50)
                        .filter_map(|id| accounts.get(id.as_str()))
                        .map(|a| json!({"id":a.id,"name":a.name,"leader":a.id==c.leader}))
                        .collect(),
                )
            }
            "inbox" | "outbox" => {
                let own = a.crew.as_ref().and_then(|id| self.data.crews.get(id).filter(|c| c.leader == a.id).map(|_| id));
                let matching: Vec<_> = self
                    .data
                    .invites
                    .iter()
                    .filter(|(_, i)| i.expires > now() && if kind == "inbox" { i.target == a.id } else { own == Some(&i.crew) })
                    .collect();
                (
                    matching.len(),
                    matching
                        .into_iter()
                        .skip(offset)
                        .take(50)
                        .filter_map(|(id, i)| {
                            Some(json!({"id":id,"crew":self.data.crews.get(&i.crew)?.name,
                        "name":accounts.get(i.target.as_str())?.name,"expires":i.expires}))
                        })
                        .collect(),
                )
            }
            _ => return Err("operation"),
        };
        Ok(json!({"kind":kind,"offset":offset,"total":total,"rows":rows}))
    }
}
