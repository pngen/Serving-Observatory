// Serving Observatory — source authority registry.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/model/source_authority.hpp"

#include <string>

namespace servingobs {

string_view authority_decision_name(AuthorityDecision d) {
    switch (d) {
        case AuthorityDecision::ACCEPTED: return "accepted";
        case AuthorityDecision::STALE_EPOCH: return "stale_epoch";
        case AuthorityDecision::STALE_SOURCE_GENERATION: return "stale_source_generation";
        case AuthorityDecision::STALE_OBSERVATION_GENERATION: return "stale_observation_generation";
        case AuthorityDecision::STALE_BOOT: return "stale_boot";
        case AuthorityDecision::DUPLICATE_SEQUENCE: return "duplicate_sequence";
        case AuthorityDecision::OUT_OF_ORDER_SEQUENCE: return "out_of_order_sequence";
        case AuthorityDecision::UNREGISTERED_SOURCE: return "unregistered_source";
        case AuthorityDecision::FRESH_BUT_DOWNHEALTH: return "fresh_but_downhealth";
    }
    return "unknown";
}

Result<SourceAuthority> SourceRegistry::register_source(
    const Observation& o, CoordinatorEpoch epoch, bool force_reassign_boot) {
    u64 k = key_of(o.source, o.worker);
    auto it = by_key_.find(k);
    if (it != by_key_.end()) {
        SourceAuthority& a = it->second;
        // Same boot and generations => keep existing (idempotent re-register).
        if (!force_reassign_boot && a.boot == o.boot && a.src_gen == o.src_gen &&
            a.obs_gen == o.obs_gen) {
            a.epoch = epoch;
            a.health = o.has_health ? o.health : a.health;
            return Result<SourceAuthority>::Ok(a);
        }
    }
    SourceAuthority a;
    a.source = o.source;
    a.worker = o.worker;
    a.epoch = epoch;
    a.src_gen = o.src_gen;
    a.obs_gen = o.obs_gen;
    a.boot = o.boot;
    a.last_seq = 0;
    a.has_last_ts = false;
    a.health = o.has_health ? o.health : SourceHealth::HEALTHY;
    a.registered = true;
    if (it != by_key_.end()) a.restart_count = it->second.restart_count + 1;
    by_key_[k] = a;
    return Result<SourceAuthority>::Ok(a);
}

AuthorityDecision SourceRegistry::validate(const Observation& o, string& reason) {
    u64 k = key_of(o.source, o.worker);
    auto it = by_key_.find(k);
    if (it == by_key_.end()) {
        reason = "source " + o.source.to_hex() + " worker " + o.worker.to_hex() + " not registered";
        return AuthorityDecision::UNREGISTERED_SOURCE;
    }
    const SourceAuthority& a = it->second;

    if (o.epoch < a.epoch) {
        reason = "stale epoch " + std::to_string(o.epoch) + " < " + std::to_string(a.epoch);
        return AuthorityDecision::STALE_EPOCH;
    }
    if (o.src_gen < a.src_gen) {
        reason = "stale source generation " + std::to_string(o.src_gen) + " < " + std::to_string(a.src_gen);
        return AuthorityDecision::STALE_SOURCE_GENERATION;
    }
    if (o.src_gen > a.src_gen) {
        reason = "source generation ahead of authority; must re-register";
        return AuthorityDecision::STALE_SOURCE_GENERATION;
    }
    if (o.obs_gen < a.obs_gen) {
        reason = "stale observation generation " + std::to_string(o.obs_gen) + " < " + std::to_string(a.obs_gen);
        return AuthorityDecision::STALE_OBSERVATION_GENERATION;
    }
    if (o.obs_gen > a.obs_gen) {
        reason = "observation generation ahead of authority; must re-register";
        return AuthorityDecision::STALE_OBSERVATION_GENERATION;
    }
    if (o.boot != a.boot) {
        reason = "stale boot " + o.boot.to_hex() + " != current " + a.boot.to_hex();
        return AuthorityDecision::STALE_BOOT;
    }
    if (o.seq < a.last_seq) {
        reason = "out-of-order sequence " + std::to_string(o.seq) + " < last " + std::to_string(a.last_seq);
        return AuthorityDecision::OUT_OF_ORDER_SEQUENCE;
    }
    if (o.seq == a.last_seq && a.last_seq != 0) {
        reason = "duplicate sequence " + std::to_string(o.seq);
        return AuthorityDecision::DUPLICATE_SEQUENCE;
    }

    // Accepted => advance authority.
    SourceAuthority& m = it->second;
    if (o.seq > m.last_seq) m.last_seq = o.seq;
    if (o.has_obs_ts) { m.last_obs_ts = o.obs_ts; m.has_last_ts = true; }
    if (o.has_health) m.health = o.health;
    reason.clear();
    return AuthorityDecision::ACCEPTED;
}

SourceAuthority SourceRegistry::mark_restart(const SourceId& source, const WorkerId& worker,
                                             const WorkerBootId& new_boot,
                                             CoordinatorEpoch epoch) {
    u64 k = key_of(source, worker);
    SourceAuthority a;
    a.source = source;
    a.worker = worker;
    a.epoch = epoch;
    auto it = by_key_.find(k);
    if (it != by_key_.end()) {
        a.src_gen = it->second.src_gen + 1;
        a.obs_gen = it->second.obs_gen + 1;
        a.restart_count = it->second.restart_count + 1;
    } else {
        a.src_gen = 1;
        a.obs_gen = 1;
        a.restart_count = 0;
    }
    a.boot = new_boot;
    a.last_seq = 0;
    a.has_last_ts = false;
    a.health = SourceHealth::RESTARTING;
    a.registered = true;
    by_key_[k] = a;
    return a;
}

bool SourceRegistry::has(const SourceId& source, const WorkerId& worker) const {
    return by_key_.find(key_of(source, worker)) != by_key_.end();
}
SourceAuthority SourceRegistry::get(const SourceId& source, const WorkerId& worker) const {
    auto it = by_key_.find(key_of(source, worker));
    if (it == by_key_.end()) return SourceAuthority{};
    return it->second;
}

Digest source_authority_digest(const SourceAuthority& a) {
    BinaryWriter w;
    write_authority(w, a);
    return Sha256::hash(w.data());
}

void write_authority(BinaryWriter& w, const SourceAuthority& a) {
    w.write_id(a.source.raw());
    w.write_id(a.worker.raw());
    w.write_u64(a.epoch);
    w.write_u64(a.src_gen);
    w.write_u64(a.obs_gen);
    w.write_id(a.boot.raw());
    w.write_u64(a.last_seq);
    w.write_u8(a.has_last_ts ? 1 : 0);
    if (a.has_last_ts) w.write_u64(a.last_obs_ts);
    w.write_u8(static_cast<u8>(a.health));
    w.write_u8(a.registered ? 1 : 0);
    w.write_i64(a.restart_count);
}

Result<SourceAuthority> read_authority(BinaryReader& r) {
    SourceAuthority a;
    a.source = SourceId(r.read_id());
    a.worker = WorkerId(r.read_id());
    a.epoch = r.read_u64();
    a.src_gen = r.read_u64();
    a.obs_gen = r.read_u64();
    a.boot = WorkerBootId(r.read_id());
    a.last_seq = r.read_u64();
    a.has_last_ts = r.read_u8() != 0;
    if (a.has_last_ts) a.last_obs_ts = r.read_u64();
    u8 h = r.read_u8();
    a.health = h <= static_cast<u8>(SourceHealth::REJECTED) ? static_cast<SourceHealth>(h) : SourceHealth::UNKNOWN;
    a.registered = r.read_u8() != 0;
    a.restart_count = static_cast<int>(r.read_i64());
    if (!r.ok()) return Result<SourceAuthority>::Err("bad authority: " + r.error());
    return Result<SourceAuthority>::Ok(std::move(a));
}

} // namespace servingobs
