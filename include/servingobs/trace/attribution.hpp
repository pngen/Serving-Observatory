#pragma once
// Serving Observatory — per-request resource attribution.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// Attributes observable resource consumption to a request only where evidence
// permits. Every metric carries a per-metric provenance; a metric that cannot
// be evidenced is marked unavailable (not fabricated to zero). Nothing here
// claims an unobserved resource.

#include "servingobs/core/types.hpp"
#include "servingobs/core/enums.hpp"
#include "servingobs/core/json.hpp"
#include "servingobs/trace/trace.hpp"

#include <map>
#include <string>

namespace servingobs {

struct ResourceAttribution {
    // times (ns)
    TimestampNs queue_time = 0;       // measured queue residence
    TimestampNs batch_wait = 0;
    TimestampNs dispatch_delay = 0;
    TimestampNs transfer_time = 0;
    TimestampNs prefill_time = 0;
    TimestampNs first_token_latency = 0;
    TimestampNs decode_time = 0;
    double    inter_token_latency_avg_ms = 0.0;
    TimestampNs retry_time = 0;
    TimestampNs recovery_time = 0;
    TimestampNs total_latency = 0;
    TimestampNs gpu_exec_time = 0;
    TimestampNs cpu_exec_time = 0;

    // bytes
    u64 bytes_h2d = 0, bytes_d2h = 0, bytes_inter_node = 0,
       bytes_kv = 0, bytes_tensor = 0, bytes_workspace = 0;

    // cache / reuse
    u64 kernel_hits = 0, kernel_misses = 0, graph_hits = 0, graph_misses = 0,
       kv_lookups = 0, kv_hits = 0, kv_misses = 0;
    u64 model_residency_refs = 0, adapter_residency_refs = 0;

    // speculation
    u64 spec_proposed = 0, spec_accepted = 0, spec_rejected = 0;

    // decode steps
    u64 decode_steps = 0;
    u64 output_tokens = 0, input_tokens = 0;

    // provenance per metric name; unknown metrics are absent (unavailable).
    std::map<string, Provenance> metric_provenance;

    // availability
    std::map<string, bool> metric_available;

    bool has(string_view metric) const { auto it = metric_available.find(string(metric)); return it != metric_available.end() && it->second; }
    Provenance prov(string_view metric) const {
        auto it = metric_provenance.find(string(metric));
        return it == metric_provenance.end() ? Provenance::UNKNOWN : it->second;
    }

    Json to_json() const;
};

// Compute per-request resource attribution from a trace (pure, deterministic).
ResourceAttribution attribute_resources(const RequestTrace& t);

string attribution_text(const ResourceAttribution& a);

} // namespace servingobs
