#pragma once
// Serving Observatory — source authority and generation tracking.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// The coordinator maintains a registry of source authorities. An observer must
// carry a coherent authority envelope: epoch, source generation, observation
// generation, WorkerBootId, and a monotonic sequence number. Any observation
// whose authority is older than the current authoritative view is rejected for
// current state mutation and retained only as explicitly stale historical
// evidence. A worker restart rolls the authority with a fresh WorkerBootId and
// higher generations; the old boot never becomes current again.

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"
#include "servingobs/core/enums.hpp"
#include "servingobs/model/observation.hpp"

#include <map>
#include <string>

namespace servingobs {

// Why an observation was accepted or rejected w.r.t. source authority.
enum class AuthorityDecision : u8 {
    ACCEPTED = 0,
    STALE_EPOCH = 1,
    STALE_SOURCE_GENERATION = 2,
    STALE_OBSERVATION_GENERATION = 3,
    STALE_BOOT = 4,
    DUPLICATE_SEQUENCE = 5,
    OUT_OF_ORDER_SEQUENCE = 6,
    UNREGISTERED_SOURCE = 7,
    FRESH_BUT_DOWNHEALTH = 8,
};
string_view authority_decision_name(AuthorityDecision d);

// The authoritative view the coordinator currently holds for one source.
struct SourceAuthority {
    SourceId source;
    WorkerId worker;
    CoordinatorEpoch epoch = 0;
    SourceGeneration src_gen = 0;
    ObservationGeneration obs_gen = 0;
    WorkerBootId boot; // current (authoritative) boot
    SeqNum last_seq = 0;
    TimestampNs last_obs_ts = 0;
    bool has_last_ts = false;
    SourceHealth health = SourceHealth::UNKNOWN;
    bool registered = false;
    int restart_count = 0;
};

// Registry, keyed by (source, worker). Validates the authority envelope of each
// incoming observation against the current authoritative view.
class SourceRegistry {
public:
    // Register / re-register a source (boot). A fresh boot must carry a new
    // WorkerBootId and strictly higher source+observation generations and the
    // coordinator epoch. Returns the resulting authority or an error.
    Result<SourceAuthority> register_source(
        const Observation& o, CoordinatorEpoch epoch, bool force_reassign_boot = false);

    // Validate an observation against the registry. On ACCEPTED the registry is
    // advanced (last_seq, last_obs_ts, etc.). On a stale decision the registry
    // is left unmodified. The historic observation is NOT stored here.
    AuthorityDecision validate(const Observation& o, string& reason);

    // Roll authority after a worker restart: fresh boot id, higher generations.
    SourceAuthority mark_restart(const SourceId& source, const WorkerId& worker,
                                 const WorkerBootId& new_boot,
                                 CoordinatorEpoch epoch);

    bool has(const SourceId& source, const WorkerId& worker) const;
    SourceAuthority get(const SourceId& source, const WorkerId& worker) const;
    size_t count() const { return by_key_.size(); }

    const std::map<u64, SourceAuthority>& all() const { return by_key_; }

private:
    u64 key_of(const SourceId& s, const WorkerId& w) const {
        // Deterministic combining key: FNV-ish over both halves.
        u64 a = s.raw().hi() ^ (s.raw().lo() * 0x9e3779b97f4a7c15ULL);
        u64 b = w.raw().hi() ^ (w.raw().lo() * 0x9e3779b97f4a7c15ULL);
        return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
    }
    std::map<u64, SourceAuthority> by_key_;
};

// Compute the digest of a SourceAuthority (for replay / snapshot comparisons).
Digest source_authority_digest(const SourceAuthority& a);

// Canonical, validated binary codec for a SourceAuthority.
void write_authority(BinaryWriter& w, const SourceAuthority& a);
Result<SourceAuthority> read_authority(BinaryReader& r);

} // namespace servingobs
