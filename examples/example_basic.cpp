
// Serving Observatory — basic request-trace example.
// Copyright 2026 Summon Software Labs. Apache-2.0.
// Builds a serving-like observation stream and reconstructs a request trace,
// latency decomposition, and provenance-aware explanation from pure evidence.
#include "servingobs/core/identity.hpp"
#include "servingobs/model/observation.hpp"
#include "servingobs/trace/trace.hpp"
#include "servingobs/trace/latency.hpp"
#include "servingobs/trace/attribution.hpp"
#include "servingobs/trace/analysis.hpp"
#include <cstdio>
#include <vector>
using namespace servingobs;

int main() {
    RequestId req = RequestId::from_u64(1);
    SourceId src = SourceId::from_u64(1);
    WorkerId wrk = WorkerId::from_u64(10);
    WorkerBootId boot = WorkerBootId::from_raw(0x11, 0x22);
    u64 seq = 0;
    TimestampNs t = 1000;
    auto mk = [&](ObsType ty) -> Observation {
        Observation o;
        o.type = ty; o.source = src; o.worker = wrk; o.boot = boot;
        o.src_gen = 1; o.obs_gen = 1; o.epoch = 1; o.seq = ++seq;
        o.clock_domain = "mono_ns"; o.provenance = Provenance::MEASURED;
        o.has_obs_ts = true; o.obs_ts = t; o.has_recv_ts = true; o.recv_ts = t;
        o.set_id(FieldKeys::RequestId, req.raw());
        o.id = derive_observation_id(src.raw(), wrk.raw(), boot.raw(), seq, ty);
        return o;
    };
    std::vector<Observation> obs;
    obs.push_back(mk(ObsType::REQUEST));
    obs.push_back(mk(ObsType::ADMISSION)); t += 200;
    obs.push_back(mk(ObsType::QUEUE_ENTER)); obs.push_back(mk(ObsType::QUEUE_LEAVE)); t += 600;
    obs.push_back(mk(ObsType::BATCH_FORM)); obs.push_back(mk(ObsType::BATCH_SEAL)); t += 200;
    obs.push_back(mk(ObsType::DISPATCH)); t += 100;
    obs.push_back(mk(ObsType::PREFILL_START)); obs.back().set_u64(FieldKeys::InputTokens, 128); t += 600;
    obs.push_back(mk(ObsType::PREFILL_END)); t += 100;
    for (int i = 0; i < 3; ++i) {
        obs.push_back(mk(ObsType::DECODE_STEP_START)); obs.back().set_u64(FieldKeys::OutputTokens, i + 1);
        t += 60;
        obs.push_back(mk(ObsType::DECODE_STEP_END));
    }
    obs.push_back(mk(ObsType::KV_HIT)); obs.back().set_u64(FieldKeys::Count, 9);
    obs.push_back(mk(ObsType::COMPLETION)); obs.back().set_str(FieldKeys::Outcome, "completed");

    RequestTrace tr = reconstruct_trace(obs, req);
    LatencyBreakdown bd = decompose_latency(tr);
    ResourceAttribution at = attribute_resources(tr);
    Explanation ex = explain_request(tr);

    std::printf("basic request trace example\n");
    std::printf("request %s outcome=%s total_ns=%llu\n", tr.request_id.to_hex().c_str(),
                std::string(outcome_name(tr.outcome)).c_str(), (unsigned long long)bd.total_ns);
    std::printf("%s", latency_text(bd).c_str());
    std::printf("%s", attribution_text(at).c_str());
    std::printf("%s", explanation_text(ex).c_str());
    return 0;
}
