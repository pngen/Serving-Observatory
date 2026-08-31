#pragma once
// Serving Observatory — typed observations backed by the source authority model.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// Observations are the atomic unit of evidence. Every observation carries its
// full authority envelope: SourceId, WorkerId, WorkerBootId, source generation,
// observation generation, coordinator epoch, source-clock sequence number,
// observation/receive timestamps, provenance, health and confidence. Payload is
// a set of strongly-typed fields; sources provide partial evidence (missing
// fields simply do not exist). A restarted worker must use a fresh WorkerBootId
// and generation; stale observations from a prior boot / epoch / generation are
// rejected or retained only as historical evidence.

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"
#include "servingobs/core/enums.hpp"
#include "servingobs/core/binary_codec.hpp"
#include "servingobs/core/json.hpp"
#include "servingobs/core/digest.hpp"

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace servingobs {

// ------------------------------------------------------------ FieldValue
// Missings are represented by the ABSENCE of a field (partial evidence).
struct NullVal {};
inline bool operator==(NullVal, NullVal) { return true; }
inline bool operator!=(NullVal, NullVal) { return false; }

using FieldValue = std::variant<NullVal, bool, i64, u64, f64, string, Id128, bytes>;

inline FieldValue fv(bool v) { return v; }
inline FieldValue fv(i64 v) { return v; }
inline FieldValue fv(u64 v) { return v; }
inline FieldValue fv(f64 v) { return v; }
inline FieldValue fv(string v) { return v; }
inline FieldValue fv(const Id128& v) { return v; }
inline FieldValue fv(const bytes& v) { return v; }

// Well-known field keys.
namespace FieldKeys {
    // request identity
    inline constexpr const char* RequestId   = "request_id";
    inline constexpr const char* TenantId    = "tenant_id";
    inline constexpr const char* WorkloadId  = "workload_id";
    inline constexpr const char* ModelId     = "model_id";
    inline constexpr const char* ModelRevId  = "model_rev_id";
    inline constexpr const char* AdapterId   = "adapter_id";
    inline constexpr const char* SequenceId  = "sequence_id";
    inline constexpr const char* BatchId     = "batch_id";
    inline constexpr const char* PrefillId   = "prefill_id";
    inline constexpr const char* DecodeStepId= "decode_step_id";
    inline constexpr const char* AttemptId   = "attempt_id";
    inline constexpr const char* DispatchId  = "dispatch_id";
    inline constexpr const char* FlowId      = "flow_id";
    inline constexpr const char* TransferId  = "transfer_id";
    inline constexpr const char* ReservationId="reservation_id";
    inline constexpr const char* NodeId      = "node_id";
    inline constexpr const char* DeviceId    = "device_id";

    // queueing / batching
    inline constexpr const char* QueueKind   = "queue_kind";
    inline constexpr const char* QueueId     = "queue_id";
    inline constexpr const char* QueueLen    = "queue_len";
    inline constexpr const char* BatchSize   = "batch_size";
    inline constexpr const char* BatchLatencyMs = "batch_latency_ms";
    inline constexpr const char* WaitNs      = "wait_ns";

    // execution
    inline constexpr const char* Position    = "position";
    inline constexpr const char* InputTokens = "input_tokens";
    inline constexpr const char* OutputTokens= "output_tokens";
    inline constexpr const char* SpecTokens  = "spec_tokens";
    inline constexpr const char* AcceptedTokens = "accepted_tokens";
    inline constexpr const char* RejectedTokens = "rejected_tokens";
    inline constexpr const char* StepMs     = "step_ms";
    inline constexpr const char* GpuMs      = "gpu_ms";
    inline constexpr const char* CpuMs      = "cpu_ms";

    // memory / transfer
    inline constexpr const char* BytesH2D    = "bytes_h2d";
    inline constexpr const char* BytesD2H    = "bytes_d2h";
    inline constexpr const char* BytesInterNode = "bytes_inter_node";
    inline constexpr const char* BytesKV     = "bytes_kv";
    inline constexpr const char* BytesTensor = "bytes_tensor";
    inline constexpr const char* BytesWorkspace = "bytes_workspace";
    inline constexpr const char* Bytes       = "bytes";
    inline constexpr const char* Kb          = "kb";

    // residency / cache
    inline constexpr const char* ModelIdRef  = "model_id";
    inline constexpr const char* AdapterIdRef= "adapter_id";
    inline constexpr const char* IsWarm      = "is_warm";
    inline constexpr const char* IsHit       = "is_hit";
    inline constexpr const char* KvBytesAdded = "kv_bytes_added";
    inline constexpr const char* KernelId    = "kernel_id";
    inline constexpr const char* GraphId     = "graph_id";

    // outcome / control
    inline constexpr const char* Outcome     = "outcome";
    inline constexpr const char* Reason      = "reason";
    inline constexpr const char* Count       = "count";
    inline constexpr const char* RetryOf     = "retry_of";
    inline constexpr const char* RetryAttempt= "retry_attempt";
    inline constexpr const char* FailureCode = "failure_code";
    inline constexpr const char* Recovered   = "recovered";
    inline constexpr const char* SloClass    = "slo_class";
    inline constexpr const char* QuotaLimit  = "quota_limit";
    inline constexpr const char* Capacity    = "capacity";
    inline constexpr const char* Node        = "node";
    inline constexpr const char* RuleDesc    = "rule_desc";

    // provenance helpers carried in payload where useful
    inline constexpr const char* Confidence = "confidence";
    inline constexpr const char* StalenessNs = "staleness_ns";
}

// ------------------------------------------------------------ Observation
struct Observation {
    ObservationId id;
    ObsType type = ObsType::UNKNOWN;

    // ---- source authority envelope
    SourceId source;
    WorkerId worker;
    WorkerBootId boot;
    SourceGeneration src_gen = 0;
    ObservationGeneration obs_gen = 0;
    CoordinatorEpoch epoch = 0;
    SeqNum seq = 0;
    string clock_domain;

    // ---- time
    bool has_obs_ts = false;
    TimestampNs obs_ts = 0;
    bool has_recv_ts = false;
    TimestampNs recv_ts = 0;

    // ---- evidence semantics
    Provenance provenance = Provenance::UNKNOWN;
    bool has_health = false;
    SourceHealth health = SourceHealth::UNKNOWN;
    bool has_confidence = false;
    f32 confidence = 0.0f;

    // ---- payload (partial evidence: missing fields are simply absent)
    std::map<string, FieldValue> fields;

    // Convenience typed accessors (return provided default when absent).
    u64 u64_field(string_view k, u64 def = 0) const;
    i64 i64_field(string_view k, i64 def = 0) const;
    f64 f64_field(string_view k, f64 def = 0.0) const;
    bool bool_field(string_view k, bool def = false) const;
    string str_field(string_view k, string def = {}) const;
    Id128 id_field(string_view k) const;

    void set_u64(string_view k, u64 v) { fields[string(k)] = v; }
    void set_i64(string_view k, i64 v) { fields[string(k)] = v; }
    void set_f64(string_view k, f64 v) { fields[string(k)] = v; }
    void set_bool(string_view k, bool v) { fields[string(k)] = v; }
    void set_str(string_view k, string v) { fields[string(k)] = std::move(v); }
    void set_id(string_view k, Id128 v) { fields[string(k)] = v; }

    // Has any identity (request/sequence/batch) linking fields?
    RequestId request_id() const { return RequestId(id_field(FieldKeys::RequestId)); }
    SequenceId sequence_id() const { return SequenceId(id_field(FieldKeys::SequenceId)); }
    BatchId batch_id() const { return BatchId(id_field(FieldKeys::BatchId)); }

    bool links_request() const { return id_field(FieldKeys::RequestId) != Id128(); }
};

// ------------------------------------------------------------ serialization
// Canonical, deterministic, validated binary codec for an Observation.
void write_observation(BinaryWriter& w, const Observation& o);
Result<Observation> read_observation(BinaryReader& r);

// Standalone payload encode/decode (used as a TCP OBS frame payload).
bytes encode_observation_payload(const Observation& o);
Result<Observation> read_observation_payload(const bytes& b);

// Deterministic, collision-free observation id derived from the source authority
// and the sequence/type, so distinct boots / sources / seqs never collide.
ObservationId derive_observation_id(const Id128& source, const Id128& worker,
                                    const Id128& boot, SeqNum seq, ObsType type);

Digest observation_digest(const Observation& o);

// Decode an entry value (identifier of the payload record) into a human string.
string field_value_to_string(const FieldValue& v);

// JSON view (deterministic key order, hex doubles via bitstring in canonical
// writer; here doubles are decimal for readability).
Json observation_to_json(const Observation& o);

} // namespace servingobs
