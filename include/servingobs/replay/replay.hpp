#pragma once
// Serving Observatory — deterministic replay and stable digests.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// Replay reconstructs every request trace, latency decomposition, attribution,
// tail metrics and explanation from the retained evidence, and recomputes stable
// digests. Replaying the same valid evidence reproduces identical digests, so a
// trace saved in one process and replayed in a fresh process verifies bit-identical.

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"
#include "servingobs/core/digest.hpp"
#include "servingobs/core/json.hpp"
#include "servingobs/model/observation.hpp"
#include "servingobs/trace/trace.hpp"
#include "servingobs/trace/latency.hpp"
#include "servingobs/trace/attribution.hpp"
#include "servingobs/trace/analysis.hpp"

#include <string>
#include <vector>

namespace servingobs {

struct ReplayResult {
    u64 obs_count = 0;
    u64 request_trace_count = 0;
    Digest evidence_digest;
    Digest trace_digest;
    Digest explanation_digest;
    Digest aggregate_digest;
    std::vector<RequestTrace> traces;
    TailMetrics ungrouped_metrics;   // over all traces
    Json to_json() const;
};

// Replay evidence (observations) deterministically, grouping by request id.
ReplayResult replay(const std::vector<Observation>& observations);

// Helper: deterministic digest over a set of observations (canonical, sorted by id).
Digest evidence_digest_of(const std::vector<Observation>& observations);

// Canonical digest of a single request trace.
Digest trace_digest_of(const RequestTrace& t);

} // namespace servingobs
