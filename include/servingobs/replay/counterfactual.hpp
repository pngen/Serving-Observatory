#pragma once
// Serving Observatory — bounded counterfactual comparison.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// A counterfactual does NOT pretend the counterfactual happened. It replays the
// observed evidence through a stated "what-if" transform and reports the exact
// changed inputs and the resulting derived differences. Every result is labeled
// Derived (or Estimated where a stated model is used) — never Measured.

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"
#include "servingobs/core/enums.hpp"
#include "servingobs/core/json.hpp"
#include "servingobs/trace/trace.hpp"
#include "servingobs/trace/latency.hpp"

#include <string>
#include <vector>

namespace servingobs {

enum class CounterfactualRule : u8 {
    REMOVE_QUEUE_WAIT = 1,
    REMOVE_RETRY = 2,
    WARM_RESIDENCY = 3,
    REDUCE_TRANSFER_BY_HALF = 4,
};
string_view counterfactual_rule_name(CounterfactualRule r);

struct CounterfactualResult {
    RequestId request_id;
    CounterfactualRule rule;
    TimestampNs original_total = 0;
    TimestampNs counterfactual_total = 0;
    i64 delta_ns = 0;             // counterfactual - original (negative = faster)
    double delta_percent = 0.0;
    std::vector<string> changed_inputs;       // exact inputs that were changed
    std::vector<string> derived_differences;  // derived deltas produced
    Provenance provenance = Provenance::DERIVED;
    ExplanationClass cls = ExplanationClass::STRUCTURALLY_DERIVED;
    Json to_json() const;
};
string counterfactual_text(const CounterfactualResult& r);

// Compute a bounded counterfactual from observed evidence only.
CounterfactualResult counterfactual(const RequestTrace& t, CounterfactualRule rule);

} // namespace servingobs
