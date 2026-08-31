#pragma once
// Serving Observatory — strict versioned persistence.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// Archive file layout: [u32 body_len][body bytes][u8[32] sha256(body)].
// body begins with a magic + version header, then the registry snapshot, all
// accepted observations, the rejected/stale historical evidence, the epoch, and
// the stored replay digests.
//
// Loading rejects: malformed lengths, truncation, checksum mismatch, trailing
// garbage, unsupported versions, duplicate observation ids, invalid enums,
// invalid generations, invalid timestamps, NaN/Inf, and impossible orderings.
// Recovery never converts stale historical evidence into current authority.

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"
#include "servingobs/core/digest.hpp"
#include "servingobs/core/json.hpp"
#include "servingobs/model/observation.hpp"
#include "servingobs/model/source_authority.hpp"
#include "servingobs/replay/replay.hpp"
#include "servingobs/store/coordinator.hpp"

#include <string>
#include <utility>
#include <vector>

namespace servingobs {

constexpr u32 kArchiveMagic = 0x534F4253u;   // "SOBS"
constexpr u16 kArchiveVersion = 1;

struct ArchiveContents {
    CoordinatorEpoch epoch = 1;
    std::vector<SourceAuthority> registry;
    std::vector<Observation> accepted;
    std::vector<std::pair<Observation, string>> stale;
    u64 created_ns = 0;

    // Stored digests (recomputed during save).
    Digest stored_evidence_digest;
    Digest stored_trace_digest;
    Digest stored_explanation_digest;
    Digest stored_aggregate_digest;

    // Recomputed on load and verified against the stored digests.
    ReplayResult verified_replay;
    bool replay_match = false;

    Json to_json() const;
};

// Build an in-memory archive from a coordinator (snapshot under lock).
Result<ArchiveContents> build_archive(const Coordinator& c);

// Save an archive (binary) to path.
Result<ArchiveContents> save_archive(const Coordinator& c, const string& path);

// Load + verify an archive. If verify_replay, re-run the replay and confirm the
// recomputed digests match the stored ones.
Result<ArchiveContents> load_archive(const string& path, bool verify_replay = true);

// Read all bytes from a file, or error.
Result<bytes> file_read_all(const string& path);

} // namespace servingobs
