#pragma once
// Serving Observatory — deterministic latency decomposition.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// Decomposes a request's lifetime into phase components. Every component reports
// its interval duration, a non-overlapping *exclusive* duration (computed via a
// deterministic sweep line that awards each nanosecond to exactly one phase so
// covered time is never double-counted), percentage of total latency, whether
// the component lies on the critical path, and the provenance of each number.

#include "servingobs/core/types.hpp"
#include "servingobs/core/enums.hpp"
#include "servingobs/core/json.hpp"
#include "servingobs/trace/trace.hpp"

#include <vector>

namespace servingobs {

struct LatencyComponent {
    Phase phase;
    string name;
    TimestampNs duration_ns = 0;   // interval span (may overlap others)
    TimestampNs exclusive_ns = 0;  // non-overlapping exclusive portion
    double percent_of_total = 0.0; // exclusive / total * 100
    bool on_critical_path = false;
    Provenance provenance = Provenance::UNKNOWN;
    bool point_event = false;      // zero-length marker (e.g. arrival, completion)
};

// Decomposes the trace's phase intervals. The optional priority table resolves
// overlaps deterministically.
class LatencyBreakdown {
public:
    TimestampNs total_ns = 0;
    TimestampNs covered_ns = 0;    // union of exclusive attributions
    TimestampNs gap_ns = 0;        // total - covered (unattributed / unknown)
    std::vector<LatencyComponent> components;

    Json to_json() const;

private:
    friend LatencyBreakdown decompose_latency(const RequestTrace& t);
};

// Compute the deterministic latency breakdown for a trace.
LatencyBreakdown decompose_latency(const RequestTrace& t);

// Human-readable multi-line description.
string latency_text(const LatencyBreakdown& b, size_t indent = 0);

} // namespace servingobs
