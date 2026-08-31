// Serving Observatory — per-request resource attribution.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/trace/attribution.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace servingobs {

namespace {
void mark(ResourceAttribution& a, string metric, Provenance p, bool available) {
    a.metric_provenance[metric] = p;
    a.metric_available[metric] = available;
}
// Phase interval derived from request trace.
TimestampNs dur(const RequestTrace& t, Phase p) {
    const PhaseInterval& pi = t.get_phase(p);
    return pi.exists && pi.end >= pi.start ? pi.end - pi.start : 0;
}
u64 sum_u64(const RequestTrace& t, const char* key) {
    u64 s = 0;
    for (const auto& o : t.evidence) s += o.u64_field(key);
    return s;
}
}

ResourceAttribution attribute_resources(const RequestTrace& t) {
    ResourceAttribution a;

    // Times from phases.
    a.queue_time = dur(t, Phase::QUEUE);
    a.batch_wait = dur(t, Phase::BATCH_WAIT);
    a.dispatch_delay = dur(t, Phase::DISPATCH);
    a.transfer_time = dur(t, Phase::TRANSFER);
    a.prefill_time = dur(t, Phase::PREFILL);
    a.first_token_latency = dur(t, Phase::FIRST_TOKEN);
    a.decode_time = dur(t, Phase::DECODE);
    a.retry_time = dur(t, Phase::RETRY);
    a.recovery_time = dur(t, Phase::RECOVERY);
    a.total_latency = dur(t, Phase::TOTAL);

    const PhaseInterval& q = t.get_phase(Phase::QUEUE);
    const PhaseInterval& bw = t.get_phase(Phase::BATCH_WAIT);
    const PhaseInterval& dp = t.get_phase(Phase::DISPATCH);
    const PhaseInterval& tr = t.get_phase(Phase::TRANSFER);
    const PhaseInterval& pf = t.get_phase(Phase::PREFILL);
    const PhaseInterval& ft = t.get_phase(Phase::FIRST_TOKEN);
    const PhaseInterval& dc = t.get_phase(Phase::DECODE);
    const PhaseInterval& rt = t.get_phase(Phase::RETRY);
    const PhaseInterval& rv = t.get_phase(Phase::RECOVERY);
    const PhaseInterval& tot = t.get_phase(Phase::TOTAL);

    auto rec = [&](const char* k, TimestampNs v, bool has, Provenance p) {
        (void)v;
        mark(a, k, p, has);
    };
    rec("queue", q.exists ? a.queue_time : 0, q.exists, q.provenance);
    rec("batch_wait", bw.exists ? a.batch_wait : 0, bw.exists, bw.provenance);
    rec("dispatch", dp.exists ? a.dispatch_delay : 0, dp.exists, dp.provenance);
    rec("transfer", tr.exists ? a.transfer_time : 0, tr.exists, tr.provenance);
    rec("prefill", pf.exists ? a.prefill_time : 0, pf.exists, pf.provenance);
    rec("first_token", ft.exists ? a.first_token_latency : 0, ft.exists, ft.provenance);
    rec("decode", dc.exists ? a.decode_time : 0, dc.exists, dc.provenance);
    rec("retry", rt.exists ? a.retry_time : 0, rt.exists, rt.provenance);
    rec("recovery", rv.exists ? a.recovery_time : 0, rv.exists, rv.provenance);
    rec("total", tot.exists ? a.total_latency : 0, tot.exists, tot.provenance);

    // Execute time from GpuMs / CpuMs fields.
    f64 gpu_ms = 0.0; bool has_gpu = false;
    for (const auto& o : t.evidence) {
        if (o.fields.count(FieldKeys::GpuMs)) { gpu_ms += o.f64_field(FieldKeys::GpuMs); has_gpu = true; }
        if (o.fields.count(FieldKeys::CpuMs)) { a.cpu_exec_time += static_cast<TimestampNs>(o.f64_field(FieldKeys::CpuMs) * 1e6); }
    }
    if (has_gpu) { a.gpu_exec_time = static_cast<TimestampNs>(gpu_ms * 1e6); mark(a, "gpu_exec", Provenance::MEASURED, true); }
    else mark(a, "gpu_exec", Provenance::UNKNOWN, false);
    mark(a, "cpu_exec", Provenance::REPORTED, a.cpu_exec_time > 0);

    // Byte counts.
    a.bytes_h2d = sum_u64(t, FieldKeys::BytesH2D);
    a.bytes_d2h = sum_u64(t, FieldKeys::BytesD2H);
    a.bytes_inter_node = sum_u64(t, FieldKeys::BytesInterNode);
    a.bytes_kv = sum_u64(t, FieldKeys::BytesKV);
    a.bytes_tensor = sum_u64(t, FieldKeys::BytesTensor);
    a.bytes_workspace = sum_u64(t, FieldKeys::BytesWorkspace);
    mark(a, "bytes_h2d", Provenance::MEASURED, a.bytes_h2d > 0 || t.evidence.size() > 0);
    mark(a, "bytes_d2h", Provenance::MEASURED, a.bytes_d2h > 0);
    mark(a, "bytes_inter_node", Provenance::MEASURED, a.bytes_inter_node > 0);
    mark(a, "bytes_kv", Provenance::MEASURED, a.bytes_kv > 0);
    mark(a, "bytes_tensor", Provenance::MEASURED, a.bytes_tensor > 0);
    mark(a, "bytes_workspace", Provenance::MEASURED, a.bytes_workspace > 0);

    // Cache / reuse.
    for (const auto& o : t.evidence) {
        if (o.type == ObsType::KERNEL_HIT) a.kernel_hits += o.u64_field(FieldKeys::Count, 1);
        else if (o.type == ObsType::KERNEL_MISS) a.kernel_misses += o.u64_field(FieldKeys::Count, 1);
        else if (o.type == ObsType::GRAPH_HIT) a.graph_hits += o.u64_field(FieldKeys::Count, 1);
        else if (o.type == ObsType::GRAPH_MISS) a.graph_misses += o.u64_field(FieldKeys::Count, 1);
        else if (o.type == ObsType::KV_LOOKUP) a.kv_lookups += o.u64_field(FieldKeys::Count, 1);
        else if (o.type == ObsType::KV_HIT) a.kv_hits += o.u64_field(FieldKeys::Count, 1);
        else if (o.type == ObsType::KV_MISS) a.kv_misses += o.u64_field(FieldKeys::Count, 1);
        else if (o.type == ObsType::MODEL_RESIDENCY) a.model_residency_refs += o.u64_field(FieldKeys::Count, 1);
        else if (o.type == ObsType::ADAPTER_RESIDENCY) a.adapter_residency_refs += o.u64_field(FieldKeys::Count, 1);
    }
    mark(a, "kernel_hits", Provenance::MEASURED, a.kernel_hits > 0);
    mark(a, "kernel_misses", Provenance::MEASURED, a.kernel_misses > 0);
    mark(a, "graph_hits", Provenance::MEASURED, a.graph_hits > 0);
    mark(a, "graph_misses", Provenance::MEASURED, a.graph_misses > 0);
    mark(a, "kv_lookups", Provenance::MEASURED, a.kv_lookups > 0);
    mark(a, "kv_hits", Provenance::MEASURED, a.kv_hits > 0);
    mark(a, "kv_misses", Provenance::MEASURED, a.kv_misses > 0);

    // Speculation.
    for (const auto& o : t.evidence) {
        if (o.type == ObsType::SPECULATION_START) a.spec_proposed += o.u64_field(FieldKeys::SpecTokens, 1);
        else if (o.type == ObsType::SPECULATION_VERIFY) {
            a.spec_accepted += o.u64_field(FieldKeys::AcceptedTokens, 0);
            a.spec_rejected += o.u64_field(FieldKeys::RejectedTokens, 0);
        }
    }
    mark(a, "spec_proposed", Provenance::MEASURED, a.spec_proposed > 0);
    mark(a, "spec_accepted", Provenance::MEASURED, a.spec_accepted > 0);
    mark(a, "spec_rejected", Provenance::MEASURED, a.spec_rejected > 0);

    // Tokens / steps.
    a.decode_steps = 0;
    a.input_tokens = 0; a.output_tokens = 0;
    for (const auto& o : t.evidence) {
        if (o.type == ObsType::DECODE_STEP_START) { ++a.decode_steps; a.output_tokens = std::max(a.output_tokens, o.u64_field(FieldKeys::OutputTokens)); }
        if (o.type == ObsType::REQUEST || o.type == ObsType::PREFILL_START) a.input_tokens = std::max(a.input_tokens, o.u64_field(FieldKeys::InputTokens));
    }
    if (a.decode_steps > 0 && a.decode_time > 0) {
        // inter-token interval in ms
        a.inter_token_latency_avg_ms = (static_cast<double>(a.decode_time) / static_cast<double>(a.decode_steps)) / 1e6;
        mark(a, "inter_token_ms", Provenance::DERIVED, true);
    } else {
        mark(a, "inter_token_ms", Provenance::UNKNOWN, false);
    }
    mark(a, "decode_steps", Provenance::MEASURED, a.decode_steps > 0);
    mark(a, "input_tokens", Provenance::MEASURED, a.input_tokens > 0);
    mark(a, "output_tokens", Provenance::REPORTED, a.output_tokens > 0);

    return a;
}

Json ResourceAttribution::to_json() const {
    Json j = Json::object();
    auto pub = [&](const char* k, u64 v) { j.set(k, v); };
    pub("queue_ns", queue_time); pub("batch_wait_ns", batch_wait);
    pub("dispatch_ns", dispatch_delay); pub("transfer_ns", transfer_time);
    pub("prefill_ns", prefill_time); pub("first_token_ns", first_token_latency);
    pub("decode_ns", decode_time);
    j.set("inter_token_avg_ms", inter_token_latency_avg_ms);
    pub("retry_ns", retry_time); pub("recovery_ns", recovery_time);
    pub("total_ns", total_latency); pub("gpu_exec_ns", gpu_exec_time);
    pub("cpu_exec_ns", cpu_exec_time);
    pub("bytes_h2d", bytes_h2d); pub("bytes_d2h", bytes_d2h); pub("bytes_inter_node", bytes_inter_node);
    pub("bytes_kv", bytes_kv); pub("bytes_tensor", bytes_tensor); pub("bytes_workspace", bytes_workspace);
    pub("kernel_hits", kernel_hits); pub("kernel_misses", kernel_misses);
    pub("graph_hits", graph_hits); pub("graph_misses", graph_misses);
    pub("kv_hits", kv_hits); pub("kv_misses", kv_misses);
    pub("spec_proposed", spec_proposed); pub("spec_accepted", spec_accepted); pub("spec_rejected", spec_rejected);
    pub("decode_steps", decode_steps); pub("input_tokens", input_tokens); pub("output_tokens", output_tokens);

    // provenance map
    Json prov = Json::object();
    for (const auto& [k, p] : metric_provenance) prov.set(k, string(provenance_name(p)));
    j.set("provenance", std::move(prov));
    Json avail = Json::object();
    for (const auto& [k, v] : metric_available) avail.set(k, v);
    j.set("available", std::move(avail));
    return j;
}

string attribution_text(const ResourceAttribution& a) {
    char buf[256];
    string out;
    out += "  queue_ns=" + std::to_string(a.queue_time) +
           " batch_ns=" + std::to_string(a.batch_wait) +
           " dispatch_ns=" + std::to_string(a.dispatch_delay) + "\n";
    out += "  transfer_ns=" + std::to_string(a.transfer_time) +
           " prefill_ns=" + std::to_string(a.prefill_time) +
           " first_tok_ns=" + std::to_string(a.first_token_latency) + "\n";
    out += "  decode_ns=" + std::to_string(a.decode_time) +
           " itl_ms=";
    std::snprintf(buf, sizeof buf, "%.3f", a.inter_token_latency_avg_ms);
    out += buf;
    out += " total_ns=" + std::to_string(a.total_latency) + "\n";
    out += "  gpu_ns=" + std::to_string(a.gpu_exec_time) +
           " cpu_ns=" + std::to_string(a.cpu_exec_time) + "\n";
    out += "  bytes h2d=" + std::to_string(a.bytes_h2d) + " d2h=" + std::to_string(a.bytes_d2h) +
           " kv=" + std::to_string(a.bytes_kv) + " tensor=" + std::to_string(a.bytes_tensor) + "\n";
    out += "  kv hit=" + std::to_string(a.kv_hits) + " miss=" + std::to_string(a.kv_misses) +
           " kernel h=" + std::to_string(a.kernel_hits) + " m=" + std::to_string(a.kernel_misses) +
           " spec p=" + std::to_string(a.spec_proposed) + " a=" + std::to_string(a.spec_accepted) +
           " r=" + std::to_string(a.spec_rejected) + "\n";
    return out;
}

} // namespace servingobs
