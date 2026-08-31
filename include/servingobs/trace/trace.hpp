#pragma once
// Serving Observatory — canonical per-request trace reconstruction.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// A RequestTrace is derived purely from observations. It preserves the source
// authority of each observation (boot, generations, epoch, seq) and the
// provenance of every derived value. Phases are extracted as intervals in the
// coordinator receive-clock domain; where only source-clock timestamps exist
// the derived interval is labelled reconstructed/estimated, never measured.

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"
#include "servingobs/core/enums.hpp"
#include "servingobs/core/json.hpp"
#include "servingobs/model/observation.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace servingobs {

// ------------------------------------------------------------ phases
enum class Phase : u8 {
    ARRIVAL = 0,
    ADMISSION = 1,
    QUEUE = 2,
    BATCH_WAIT = 3,
    DISPATCH = 4,
    TRANSFER = 5,
    PREFILL = 6,
    FIRST_TOKEN = 7,
    DECODE = 8,
    SPECULATION = 9,
    RETRY = 10,
    RECOVERY = 11,
    COMPLETION = 12,
    TOTAL = 13,
};
string_view phase_name(Phase p);

// An interval with provenance. start/end are in a common (coordinator receive)
// clock domain. A point interval has start==end.
struct PhaseInterval {
    Phase phase = Phase::TOTAL;
    bool exists = false;
    TimestampNs start = 0;
    TimestampNs end = 0;
    Provenance provenance = Provenance::UNKNOWN;
    string source;   // contributing source id (hex) when single-source
    string detail;
};

// ------------------------------------------------------------ trace event
struct TraceEvent {
    u32 ordinal = 0;               // ordering index within the trace
    ObsType type = ObsType::UNKNOWN;
    TimestampNs ts = 0;            // coordinator receive clock domain
    bool has_ts = false;
    Provenance provenance = Provenance::UNKNOWN;
    SourceId source;
    WorkerId worker;
    WorkerBootId boot;
    SourceGeneration src_gen = 0;
    ObservationGeneration obs_gen = 0;
    CoordinatorEpoch epoch = 0;
    SeqNum seq = 0;
    string label;                  // human-readable
    std::map<string, FieldValue> fields;  // relevant payload snapshot
};

// ------------------------------------------------------------ request trace
struct RequestTrace {
    TraceId trace_id;
    RequestId request_id;
    TenantId tenant;
    WorkloadId workload;
    ModelId model;
    ModelRevisionId model_rev;
    AdapterId adapter;

    // Raw evidence in timeline order (coordinator receive domain).
    std::vector<Observation> evidence;
    // Derived, ordered event timeline.
    std::vector<TraceEvent> timeline;
    // Extracted phase intervals.
    std::array<PhaseInterval, 14> phases;

    Outcome outcome = Outcome::UNKNOWN;
    string terminal_reason;

    // Coordination metadata (from the evidence).
    CoordinatorEpoch min_epoch = 0;
    CoordinatorEpoch max_epoch = 0;
    bool spans_multiple_epochs = false;
    // distinct boots/generations feeding this request
    std::vector<WorkerBootId> boots;
    std::vector<SourceGeneration> src_gens;
    std::vector<ObservationGeneration> obs_gens;

    void add_phase(Phase p, TimestampNs start, TimestampNs end, Provenance prov,
                   string source, string detail);
    PhaseInterval get_phase(Phase p) const { return phases[static_cast<size_t>(p)]; }
    bool has_phase(Phase p) const { return phases[static_cast<size_t>(p)].exists; }
};

// Choose the common-domain timestamp for an observation (recv domain preferred,
// else source domain with a reconstructed label).
struct TimedObs {
    TimestampNs ts;
    bool in_recv_domain; // true if based on recv_ts
};
TimedObs obs_time(const Observation& o);

// Reconstruct a request trace from all observations bearing the same request id.
// Observations must already be filtered to that request. Returns a trace that
// preserves every generation/authority transition.
RequestTrace reconstruct_trace(const std::vector<Observation>& obs, const RequestId& request_id);

// JSON view.
Json trace_to_json(const RequestTrace& t);

} // namespace servingobs
