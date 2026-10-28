// settings.cpp - one JSON file per profile, holding every mod's settings.
//
// Values are 64-bit integers; a toggle is 0 or 1. That is the whole type
// system the settings page needs and a type a mod cannot misread.
//
// A transaction covers one mod's init: begin before it runs, roll back if it
// fails. Rollback restores BOTH the declarations and the persisted values,
// because a mod that changed another mod's setting and then failed must leave
// no trace of either.
#include "mods_internal.h"
#include "../runtime/layout.h"
#include "display_settings.h"

#include <algorithm>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <filesystem>
#include "../platform/os.h"

namespace {

struct Entry {
    uint32_t owner;
    std::string mod_id, key, label;
    int32_t kind;
    int64_t value, min, max;
    int64_t step = 1;
    std::string unit;
    std::string group, help, apply, choices; // settings menu metadata (manifest)
    int64_t def = 0;                         // the declared default (Test85: Restore defaults)
};

std::vector<Entry> &entries() {
    static std::vector<Entry> v;
    return v;
}
// "mod_id/key" -> value, the persisted form.
std::map<std::string, int64_t> &stored() {
    static std::map<std::string, int64_t> m;
    return m;
}
std::string g_path;

// Transaction snapshot.
bool g_in_txn = false;
std::vector<Entry> g_txn_entries;
std::map<std::string, int64_t> g_txn_stored;

Entry *find(uint32_t owner, const char *key) {
    for (Entry &e : entries())
        if (e.owner == owner && e.key == key)
            return &e;
    return nullptr;
}

void sort_entries() {
    std::stable_sort(entries().begin(), entries().end(),
                     [](const Entry &a, const Entry &b) { return a.owner < b.owner; });
}

} // namespace

const char *mods_settings_path() {
    if (g_path.empty())
        g_path = std::string(mods_overlay_profile_dir()) + "/mod-settings.json";
    return g_path.c_str();
}

void mods_settings_reset() {
    mods_display_reset();
    entries().clear();
    stored().clear();
    g_path.clear();
    g_in_txn = false;
}

bool mods_settings_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return true; // no file yet is not an error
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        text.append(buf, n);
    fclose(f);

    // {"mod.id/key": 7, ...} - the same shape the registry store uses.
    size_t i = 0;
    while ((i = text.find('"', i)) != std::string::npos) {
        size_t end = text.find('"', i + 1);
        if (end == std::string::npos)
            break;
        std::string key = text.substr(i + 1, end - i - 1);
        size_t colon = text.find(':', end);
        if (colon == std::string::npos)
            break;
        stored()[key] = strtoll(text.c_str() + colon + 1, nullptr, 10);
        i = text.find(',', colon);
        if (i == std::string::npos)
            break;
    }
    for (Entry &e : entries()) {
        auto it = stored().find(e.mod_id + "/" + e.key);
        if (it != stored().end())
            e.value = it->second;
    }
    return true;
}

bool mods_settings_save() {
    if (g_in_txn)
        return false; // an init rollback must never reach disk
    for (const Entry &e : entries())
        stored()[e.mod_id + "/" + e.key] = e.value;
    const std::string path = mods_settings_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec)
        return false;
    std::string temporary = path + ".tmp.XXXXXX";
    int fd = os_mkstemp(temporary.data());
    if (fd < 0)
        return false;
    FILE *f = (FILE *)os_fdopen(fd, "wb");
    if (!f) {
        os_fd_close(fd);
        os_unlink(temporary.c_str());
        return false;
    }
    fprintf(f, "{\n");
    bool first = true;
    for (const auto &kv : stored()) {
        fprintf(f, "%s \"%s\": %lld", first ? "" : ",\n", kv.first.c_str(), (long long)kv.second);
        first = false;
    }
    fprintf(f, "\n}\n");
    bool ok = !ferror(f) && fflush(f) == 0 && os_fd_fsync(fd) == 0;
    if (fclose(f) != 0)
        ok = false;
    if (ok)
        ok = os_rename(temporary.c_str(), path.c_str()) == 0;
    if (!ok)
        os_unlink(temporary.c_str());
    return ok;
}

void mods_settings_declare(uint32_t owner, const char *mod_id, const char *key, const char *label,
                           int32_t kind, int64_t def, int64_t min, int64_t max) {
    if (!key || find(owner, key))
        return;
    Entry e{owner, mod_id ? mod_id : "", key, label && *label ? label : key, kind, def, min, max};
    e.def = def;
    auto it = stored().find(e.mod_id + "/" + e.key);
    if (it != stored().end() && it->second >= min && it->second <= max)
        e.value = it->second;
    entries().push_back(e);
    sort_entries();
}

void mods_settings_set_range(uint32_t owner, const char *key, int64_t min, int64_t max) {
    Entry *e = key ? find(owner, key) : nullptr;
    if (!e || min > max)
        return;
    e->min = min;
    e->max = max;
    if (e->value < min)
        e->value = min;
    if (e->value > max)
        e->value = max;
}

// Test85: last-known-good graphics. Apply in the settings menu records the values it replaces in
// <settings>.graphics-pending; the file goes once the game has run 20 s after that Apply, or at a clean
// shutdown. If it is still there at the next launch, that Apply never proved itself (crash, hang, killed), so its
// previous values are put back before anything reads them.
namespace {
std::string pending_path() {
    return std::string(mods_settings_path()) + ".graphics-pending";
}
std::map<std::string, int64_t> parse_values(const std::string &text) {
    std::map<std::string, int64_t> out;
    size_t i = 0;
    while ((i = text.find('"', i)) != std::string::npos) {
        const size_t end = text.find('"', i + 1);
        if (end == std::string::npos)
            break;
        const size_t colon = text.find(':', end);
        if (colon == std::string::npos)
            break;
        out[text.substr(i + 1, end - i - 1)] = strtoll(text.c_str() + colon + 1, nullptr, 10);
        i = text.find(',', colon);
        if (i == std::string::npos)
            break;
    }
    return out;
}
} // namespace

void mods_cpp_settings_mark_pending(const std::vector<std::pair<std::string, int64_t>> &previous) {
    if (previous.empty())
        return;
    FILE *f = fopen(pending_path().c_str(), "wb");
    if (!f)
        return;
    fprintf(f, "{\n");
    for (size_t i = 0; i < previous.size(); ++i)
        fprintf(f, "%s \"%s\": %lld", i ? ",\n" : "", previous[i].first.c_str(), (long long)previous[i].second);
    fprintf(f, "\n}\n");
    fclose(f);
}

void mods_settings_clear_pending() {
    std::error_code ec;
    if (std::filesystem::remove(pending_path(), ec))
        printf("mods: graphics change confirmed (the game kept running); last-known-good updated\n");
}

int mods_settings_recover_pending() {
    FILE *f = fopen(pending_path().c_str(), "rb");
    if (!f)
        return 0;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        text.append(buf, n);
    fclose(f);
    const auto previous = parse_values(text);
    std::string list;
    for (const auto &[key, value] : previous) {
        stored()[key] = value;
        for (Entry &e : entries())
            if (e.mod_id + "/" + e.key == key && value >= e.min && value <= e.max)
                e.value = value;
        list += " " + key + "=" + std::to_string(value);
    }
    std::error_code ec;
    std::filesystem::remove(pending_path(), ec);
    if (!previous.empty()) {
        // Written straight into the stored form: nothing is declared yet at this point.
        const std::string path = mods_settings_path();
        if (FILE *w = fopen((path + ".tmp-recover").c_str(), "wb")) {
            fprintf(w, "{\n");
            bool first = true;
            for (const auto &kv : stored()) {
                fprintf(w, "%s \"%s\": %lld", first ? "" : ",\n", kv.first.c_str(), (long long)kv.second);
                first = false;
            }
            fprintf(w, "\n}\n");
            fclose(w);
            std::filesystem::rename(path + ".tmp-recover", path, ec);
        }
    }
    printf("mods: the last graphics Apply did not see the game run 20 s (crash, hang or forced quit); its "
           "previous values are back:%s\n", list.c_str());
    fflush(stdout);
    return int(previous.size());
}

int64_t mods_setting_value(const char *mod_id_slash_key, int64_t fallback) {
    if (!mod_id_slash_key)
        return fallback;
    for (const Entry &e : entries())
        if (e.mod_id.size() + 1 + e.key.size() == strlen(mod_id_slash_key) &&
            !strncmp(mod_id_slash_key, e.mod_id.c_str(), e.mod_id.size()) &&
            mod_id_slash_key[e.mod_id.size()] == '/' && e.key == mod_id_slash_key + e.mod_id.size() + 1)
            return e.value;
    return fallback;
}

bool mods_settings_default(uint32_t i, int64_t *out) {
    if (i >= entries().size() || !out)
        return false;
    *out = entries()[i].def;
    return true;
}

bool mods_settings_stored_value(const char *mod_id_slash_key, int64_t *out) {
    if (!mod_id_slash_key || !out)
        return false;
    auto it = stored().find(mod_id_slash_key);
    if (it == stored().end())
        return false;
    *out = it->second;
    return true;
}

PopModStatus mods_settings_get(uint32_t owner, const char *key, int64_t *out) {
    if (!key || !out)
        return POP_E_INVAL;
    Entry *e = find(owner, key);
    if (!e)
        return POP_E_NOTFOUND;
    *out = e->value;
    return POP_OK;
}

PopModStatus mods_settings_set(uint32_t owner, const char *key, int64_t v) {
    if (!key)
        return POP_E_INVAL;
    Entry *e = find(owner, key);
    if (!e)
        return POP_E_NOTFOUND;
    if (v < e->min || v > e->max)
        return POP_E_RANGE;
    if (v == e->value)
        return POP_OK;
    const int64_t previous = e->value;
    e->value = v;
    stored()[e->mod_id + "/" + e->key] = v;
    if (!g_in_txn && !mods_settings_save()) {
        e->value = previous;
        stored()[e->mod_id + "/" + e->key] = previous;
        LOGW("settings: could not save %s", mods_settings_path());
        return POP_E_STATE;
    }
    return POP_OK;
}

void mods_settings_remove_all(uint32_t owner) {
    for (auto it = entries().begin(); it != entries().end();)
        it = (it->owner == owner) ? entries().erase(it) : it + 1;
}

void mods_settings_txn_begin() {
    g_txn_entries = entries();
    g_txn_stored = stored();
    g_in_txn = true;
}

void mods_settings_txn_commit() {
    g_in_txn = false;
}

void mods_settings_txn_rollback() {
    if (!g_in_txn)
        return;
    entries() = g_txn_entries;
    stored() = g_txn_stored;
    g_in_txn = false;
}

void mods_settings_set_presentation(uint32_t owner, const char *key, int64_t step,
                                    const char *unit) {
    Entry *e = key ? find(owner, key) : nullptr;
    if (!e)
        return;
    e->step = step >= 1 ? step : 1;
    e->unit = unit && !strcmp(unit, "$") ? "$" : "";
}

void mods_settings_set_meta(uint32_t owner, const char *key, const char *group, const char *help,
                            const char *apply, const char *choices) {
    Entry *e = key ? find(owner, key) : nullptr;
    if (!e)
        return;
    e->group = group ? group : "";
    e->help = help ? help : "";
    e->apply = apply ? apply : "";
    e->choices = choices ? choices : "";
}

bool mods_settings_meta(uint32_t i, const char **group, const char **help, const char **apply,
                        const char **choices) {
    if (i >= entries().size())
        return false;
    const Entry &e = entries()[i];
    if (group)
        *group = e.group.c_str();
    if (help)
        *help = e.help.c_str();
    if (apply)
        *apply = e.apply.c_str();
    if (choices)
        *choices = e.choices.c_str();
    return true;
}

bool mods_settings_presentation(uint32_t i, int64_t *step, const char **unit) {
    if (i >= entries().size())
        return false;
    if (step)
        *step = entries()[i].step;
    if (unit)
        *unit = entries()[i].unit.c_str();
    return true;
}

uint32_t mods_settings_entry_count() {
    return (uint32_t)entries().size();
}

bool mods_settings_entry(uint32_t i, uint32_t *owner, const char **mod_id, const char **key,
                         const char **label, int32_t *kind, int64_t *value, int64_t *min,
                         int64_t *max) {
    if (i >= entries().size())
        return false;
    const Entry &e = entries()[i];
    if (owner)
        *owner = e.owner;
    if (mod_id)
        *mod_id = e.mod_id.c_str();
    if (key)
        *key = e.key.c_str();
    if (label)
        *label = e.label.c_str();
    if (kind)
        *kind = e.kind;
    if (value)
        *value = e.value;
    if (min)
        *min = e.min;
    if (max)
        *max = e.max;
    return true;
}

// The profile directory is the overlay's to own (Task 7). Weak here so this
// task's tests link in wave 1; the overlay's definition wins in every build
// that has one.
extern "C" __attribute__((weak)) const char *mods_overlay_profile_dir(void) {
    return host_layout().profile_dir.c_str();
}
extern "C" __attribute__((weak)) void mods_overlay_set_profile_dir(const char *) {}
