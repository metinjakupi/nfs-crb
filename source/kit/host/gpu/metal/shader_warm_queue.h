// Scheduling half of the Metal shader warm-up (d3d9_metal.mm, class ShaderWarm), kept free of
// Metal so its ordering rules can be tested without a device (tests/shader_warm_queue_tests.cpp).
//
// One background worker walks the recorded keys in first-use order; the draw path asks for a key
// when it first needs that library. The rules:
//  - a key the worker already finished is handed over once (take returns it and forgets it);
//  - a key the worker is compiling right now is waited for (bounded), never compiled twice;
//  - a queued key the draw path needs before the worker reaches it is claimed: the caller compiles
//    it and the worker later skips it;
//  - nothing is held locked while a library compiles (the worker compiles between begin and finish).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

template <class Lib> class ShaderWarmQueue {
  public:
    // Registers a key the worker will visit (seed or recorded list). Returns false if already known.
    bool add_known(const std::string &k) {
        std::lock_guard<std::mutex> lock(mutex_);
        return known_.insert(k).second;
    }
    bool known(const std::string &k) {
        std::lock_guard<std::mutex> lock(mutex_);
        return known_.count(k) != 0;
    }
    size_t known_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return known_.size();
    }
    // Worker: about to compile k. False when the draw path already claimed it (skip it).
    bool begin(const std::string &k) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (claimed_.count(k))
            return false;
        current_ = k;
        return true;
    }
    // Worker: compile of k finished (lib may be empty/null on failure).
    void finish(const std::string &k, Lib lib, bool ok) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ok)
                ready_[k] = lib;
            if (current_ == k)
                current_.clear();
        }
        done_.notify_all();
    }
    // Draw path. Returns true and sets *out when a finished library is available; otherwise false,
    // and the caller compiles the source itself.
    bool take(const std::string &k, Lib *out,
              std::chrono::milliseconds bound = std::chrono::milliseconds(2000)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (current_ == k) {
            ++coalesced_;
            done_.wait_for(lock, bound, [&] { return current_ != k; });
        }
        auto it = ready_.find(k);
        if (it == ready_.end()) {
            if (known_.count(k) && claimed_.insert(k).second)
                ++claimed_count_;
            return false;
        }
        *out = it->second;
        ready_.erase(it);
        ++hits_;
        return true;
    }
    uint32_t hits() {
        std::lock_guard<std::mutex> lock(mutex_);
        return hits_;
    }
    uint32_t coalesced() {
        std::lock_guard<std::mutex> lock(mutex_);
        return coalesced_;
    }
    uint32_t claimed() {
        std::lock_guard<std::mutex> lock(mutex_);
        return claimed_count_;
    }

  private:
    std::set<std::string> known_, claimed_;
    std::unordered_map<std::string, Lib> ready_;
    std::string current_;   // the key the worker is compiling now (empty: none)
    std::mutex mutex_;
    std::condition_variable done_;
    uint32_t hits_ = 0, coalesced_ = 0, claimed_count_ = 0;
};
