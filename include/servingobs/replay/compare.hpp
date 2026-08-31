#pragma once
// Serving Observatory — trace-to-trace and window-to-window comparison.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"
#include "servingobs/core/json.hpp"
#include "servingobs/trace/trace.hpp"
#include "servingobs/trace/attribution.hpp"

#include <string>
#include <vector>

namespace servingobs {

struct TraceComparison {
    RequestId request_a, request_b;
    u64 total_a = 0, total_b = 0;
    i64 delta_total = 0;
    std::vector<std::pair<string, i64>> phase_deltas; // (phase name, b-a ns)
    u64 retries_a = 0, retries_b = 0;
    u64 kv_hits_a = 0, kv_hits_b = 0;
    u64 kernel_hits_a = 0, kernel_hits_b = 0;
    u64 spec_accepted_a = 0, spec_accepted_b = 0;
    u64 bytes_h2d_a = 0, bytes_h2d_b = 0;
    bool worker_restart_a = false, worker_restart_b = false;
    bool cold_model_a = false, cold_model_b = false;

    Json to_json() const;
};
string comparison_text(const TraceComparison& c);

TraceComparison compare_traces(const RequestTrace& a, const RequestTrace& b);

// Window-to-window comparison: aggregate metrics over two trace sets.
struct WindowComparison {
    string label_a, label_b;
    u64 count_a = 0, count_b = 0;
    u64 total_latency_a = 0, total_latency_b = 0;
    u64 p90_a = 0, p90_b = 0;
    u64 retries_a = 0, retries_b = 0;
    u64 restarts_a = 0, restarts_b = 0;
    u64 kv_hits_a = 0, kv_hits_b = 0;
    Json to_json() const;
};
WindowComparison compare_windows(const std::vector<RequestTrace>& a, const std::vector<RequestTrace>& b,
                                 const string& label_a, const string& label_b);

} // namespace servingobs
