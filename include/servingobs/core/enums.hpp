#pragma once
// Serving Observatory — enumerations.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/core/types.hpp"

#include <string>
#include <string_view>

namespace servingobs {

// ------------------------------------------------------------ observation types
// The observation kind carried on the wire and in the canonical trace.
enum class ObsType : u16 {
    UNKNOWN = 0,
    REQUEST = 1,
    ADMISSION = 2,
    QUEUE_ENTER = 3,
    QUEUE_LEAVE = 4,
    BATCH_FORM = 5,
    BATCH_SEAL = 6,
    BATCH_SPLIT = 7,
    BATCH_MERGE = 8,
    DISPATCH = 9,
    PREFILL_START = 10,
    PREFILL_END = 11,
    DECODE_STEP_START = 12,
    DECODE_STEP_END = 13,
    SPECULATION_START = 14,
    SPECULATION_VERIFY = 15,
    KV_LOOKUP = 16,
    KV_HIT = 17,
    KV_MISS = 18,
    KV_GROWTH = 19,
    MODEL_RESIDENCY = 20,
    ADAPTER_RESIDENCY = 21,
    KERNEL_LOOKUP = 22,
    KERNEL_HIT = 23,
    KERNEL_MISS = 24,
    GRAPH_LOOKUP = 25,
    GRAPH_HIT = 26,
    GRAPH_MISS = 27,
    ALLOCATION = 28,
    FREE = 29,
    TRANSFER_START = 30,
    TRANSFER_END = 31,
    RETRY = 32,
    CANCEL = 33,
    FAILURE = 34,
    WORKER_DOWN = 35,
    WORKER_RESTART = 36,
    RECOVERY = 37,
    COMPLETION = 38,
    SLO_STATE = 39,
    QUOTA_STATE = 40,
    CAPACITY_STATE = 41,
};

string_view obs_type_name(ObsType t);
string_view obs_type_code(ObsType t);
ObsType obs_type_from_code(string_view code);

// Number of distinct declared observation types (including UNKNOWN).
constexpr int kObsTypeCount = 42;

// ------------------------------------------------------------ provenance
// How a value came to exist. Never collapsed into one confidence-free value.
enum class Provenance : u8 {
    UNKNOWN = 0,
    MEASURED = 1,     // directly measured by an instrument.
    REPORTED = 2,     // reported by a source that did not measure directly.
    DERIVED = 3,      // computed from other values.
    ESTIMATED = 4,    // estimated with a stated model.
    RECONSTRUCTED = 5,// rebuilt from evidence (may be lossy).
    HEURISTIC = 6,    // best-effort rule.
};

string_view provenance_name(Provenance p);
Provenance provenance_from_code(string_view code);

// ------------------------------------------------------------ explanation class
// Causal discipline: never claim causation from correlation alone.
enum class ExplanationClass : u8 {
    UNKNOWN = 0,
    DIRECTLY_EVIDENCED = 1,
    STRUCTURALLY_DERIVED = 2,
    TEMPORALLY_CORRELATED = 3,
    PLAUSIBLE_BUT_UNPROVEN = 4,
    CONTRADICTED = 5,
};

string_view explanation_class_name(ExplanationClass c);
ExplanationClass explanation_class_from_code(string_view code);

// ------------------------------------------------------------ source health
enum class SourceHealth : u8 {
    UNKNOWN = 0,
    HEALTHY = 1,
    DEGRADED = 2,
    STALE = 3,
    DOWN = 4,
    RESTARTING = 5,
    REJECTED = 6,
};
string_view source_health_name(SourceHealth h);
SourceHealth source_health_from_code(string_view code);

// ------------------------------------------------------------ terminal outcome
enum class Outcome : u8 {
    UNKNOWN = 0,
    COMPLETED = 1,
    FAILED = 2,
    CANCELLED = 3,
    TIMED_OUT = 4,
    PREEMPTED = 5,
    ABANDONED = 6,
    RETRIED = 7,
    PARTIAL = 8,
};
string_view outcome_name(Outcome o);
Outcome outcome_from_code(string_view code);

// ------------------------------------------------------------ queue kind
enum class QueueKind : u8 {
    UNKNOWN = 0,
    ADMISSION = 1,
    SCHEDULER = 2,
    BATCH = 3,
    PREEMPTION = 4,
};
string_view queue_kind_name(QueueKind k);
QueueKind queue_kind_from_code(string_view code);

} // namespace servingobs
