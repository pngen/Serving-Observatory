#pragma once
// Serving Observatory — tail analysis, correlation, causality, explanation.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// Causality discipline is explicit: every explanatory statement carries an
// ExplanationClass (directly evidenced / structurally derived / temporally
// correlated / plausible but unproven / contradicted). Correlation is never
// conflated with causation. Every metric carries a provenance.

#include "servingobs/core/types.hpp"
#include "servingobs/core/enums.hpp"
#include "servingobs/core/identity.hpp"
#include "servingobs/core/json.hpp"
#include "servingobs/trace/trace.hpp"
#include "servingobs/trace/latency.hpp"
#include "servingobs/trace/attribution.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace servingobs {

// ------------------------------------------------------------ percentiles
// Returns the p-th percentile (p in 0..100) of a vector of latencies.
u64 percentile_of(const std::vector<u64>& values, double p);

struct TailMetrics {
    u64 count = 0;
    u64 min_ns = 0, max_ns = 0;
    u64 p50_ns = 0, p90_ns = 0, p95_ns = 0, p99_ns = 0, p999_ns = 0;
    double mean_ns = 0.0, stddev_ns = 0.0;
    Json to_json() const;
};
TailMetrics tail_metrics(const std::vector<u64>& latencies);

// ------------------------------------------------------------ explanation
struct ExplanationStatement {
    string claim;
    ExplanationClass cls = ExplanationClass::UNKNOWN;
    Provenance provenance = Provenance::UNKNOWN;
    string evidence;    // which observations / numbers support it
    string phase;       // related phase name if any
    double severity = 0.0; // 0..1 relative contribution
};

struct Explanation {
    RequestId request_id;
    LatencyBreakdown breakdown;
    ResourceAttribution attribution;
    std::vector<ExplanationStatement> statements;
    Json to_json() const;
};
string explanation_text(const Explanation& e);

// Build a structured, provenance-aware explanation of why a request behaved as
// it did. Uses causal discipline: statements are classified, never just
// correlated-and-asserted.
Explanation explain_request(const RequestTrace& t);

// ------------------------------------------------------------ tail explanation
struct TailFactor {
    string name;
    double severity = 0.0;     // fraction of total latency attributed
    ExplanationClass cls = ExplanationClass::UNKNOWN;
    string note;
};
struct TailExplanation {
    RequestId request_id;
    u64 latency_ns = 0;
    TailMetrics metrics;
    std::vector<TailFactor> factors;
    Json to_json() const;
};
string tail_text(const TailExplanation& e);

// Explain the tail of a set of traces: those at/above the given percentile.
std::vector<TailExplanation> explain_tail(const std::vector<RequestTrace>& traces,
                                          double percentile = 90.0);

// ------------------------------------------------------------ correlation
struct GroupSummary {
    string key;
    u64 count = 0;
    u64 total_latency_ns = 0;
    u64 p50_ns = 0, p90_ns = 0, p99_ns = 0;
    u64 total_retries = 0;
    u64 worker_restart_exposures = 0;
    u64 kv_hits = 0, kv_misses = 0;
    u64 kernel_hits = 0, kernel_misses = 0;
    double mean_latency_ns = 0.0;
    Json to_json() const;
};

// Correlate traces grouped by the provided key extractor.
std::vector<GroupSummary> correlate_by(const std::vector<RequestTrace>& traces,
                                       const std::function<string(const RequestTrace&)>& key);

// Which requests share a given trait (predicate). Returns their ids and latencies.
struct SharedFactorResult {
    string factor;
    std::vector<std::pair<RequestId, u64>> requests; // id, latency ns
    Json to_json() const;
};
SharedFactorResult which_share(const std::vector<RequestTrace>& traces,
                               const std::function<bool(const RequestTrace&)>& pred,
                               string factor_name);

} // namespace servingobs
