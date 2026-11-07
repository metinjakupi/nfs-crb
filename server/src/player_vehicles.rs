//! Shared player-only exclusions. AI cops/traffic messages do not use this admission check.
use std::{collections::HashSet, sync::OnceLock};

pub fn blocked(key: u32) -> bool {
    static BLOCKED: OnceLock<HashSet<u32>> = OnceLock::new();
    BLOCKED.get_or_init(|| {
        let policy: serde_json::Value = serde_json::from_str(include_str!(
            "../../tools/carbon-player-vehicles.json"
        )).expect("embedded player vehicle policy");
        policy["blocked"].as_array().expect("blocked vehicles").iter()
            .map(|v| v["key"].as_u64().expect("vehicle key") as u32).collect()
    }).contains(&key)
}
