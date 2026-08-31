
// Downstream find_package consumer for ServingObservatory.
// Copyright 2026 Summon Software Labs. Apache-2.0.
#include "servingobs/core/identity.hpp"
#include "servingobs/model/observation.hpp"
#include "servingobs/trace/trace.hpp"
#include "servingobs/replay/replay.hpp"
#include <cstdio>
using namespace servingobs;
int main() {
    RequestId req = RequestId::from_u64(42);
    std::printf("consumer request=%s\n", req.to_hex().c_str());
    Observation o;
    o.type = ObsType::REQUEST;
    o.source = SourceId(Id128::from_u64(1));
    o.worker = WorkerId(Id128::from_u64(1));
    o.boot = WorkerBootId(Id128::make(1, 2));
    o.src_gen = 1; o.obs_gen = 1; o.epoch = 1; o.seq = 1;
    o.clock_domain = "mono_ns";
    o.provenance = Provenance::MEASURED;
    o.set_id(FieldKeys::RequestId, req.raw());
    o.has_obs_ts = true; o.obs_ts = 1000;
    o.id = derive_observation_id(o.source.raw(), o.worker.raw(), o.boot.raw(), o.seq, o.type);
    auto dec = read_observation_payload(encode_observation_payload(o));
    if (!dec.ok()) { std::printf("consumer FAILED codec\n"); return 1; }
    std::printf("consumer codec round-trip ok request=%s\n", dec.value().request_id().to_hex().c_str());
    auto rp = replay({ o });
    std::printf("consumer replay traces=%llu digest=%s\n", (unsigned long long)rp.request_trace_count,
                 digest_hex(rp.evidence_digest).c_str());
    return 0;
}
