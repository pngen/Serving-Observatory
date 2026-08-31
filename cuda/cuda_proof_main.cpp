
// Serving Observatory — RTX 5090 / sm_120 CUDA serving-evidence proof runner.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// Runs real CUDA kernels, builds serving-like observations from measured events,
// ingests them into a coordinator, reconstructs request traces, saves an archive,
// and prints a deterministic JSON report with the measured cold/warm/decode/retry
// differences and the stable replay digests.

#include "servingobs/cuda/cuda_proof.hpp"
#include "servingobs/core/json.hpp"
#include "servingobs/core/digest.hpp"
#include "servingobs/model/observation.hpp"
#include "servingobs/store/coordinator.hpp"
#include "servingobs/store/persistence.hpp"
#include "servingobs/trace/trace.hpp"
#include "servingobs/replay/replay.hpp"

#include <cstdio>

using namespace servingobs;

int main(int argc, char** argv) {
    std::string out = argc > 1 ? argv[1] : "cuda_proof.sobs";
    try {
        CudaScenario sc = run_cuda_proof();
        Coordinator coord;
        coord.set_epoch(1);
        // register the synthetic source
        HelloPayload h;
        if (!sc.evidence.empty()) {
            const Observation& o = sc.evidence.front();
            h.source = o.source; h.worker = o.worker; h.boot = o.boot;
            h.src_gen = o.src_gen; h.obs_gen = o.obs_gen; h.epoch = o.epoch; h.clock_domain = o.clock_domain;
            coord.register_hello(h);
        }
        u64 accepted = 0, stale = 0;
        for (const auto& o : sc.evidence) {
            IngestResult ir = coord.ingest(o);
            if (ir.accepted) ++accepted; else ++stale;
        }
        auto saved = save_archive(coord, out);
        auto rp = replay(coord.all_accepted());

        Json rep = Json::object();
        rep.set("kpi", sc.kpi_json());
        rep.set("evidence_count", sc.evidence.size());
        rep.set("accepted", accepted);
        rep.set("stale", stale);
        rep.set("trace_count", rp.request_trace_count);
        rep.set("archive_saved", saved.ok());
        if (saved.ok()) {
            rep.set("evidence_digest", digest_hex(rp.evidence_digest));
            rep.set("trace_digest", digest_hex(rp.trace_digest));
            rep.set("aggregate_digest", digest_hex(rp.aggregate_digest));
            rep.set("archive", out);
        }
        rep.set("cold_vs_warm_ms", sc.cold_prefill_ms - sc.warm_prefill_ms);
        // classify traces
        Json traces = Json::array();
        for (const auto& t : rp.traces) {
            Json o = Json::object();
            o.set("request_id", t.request_id.to_hex());
            o.set("outcome", string(outcome_name(t.outcome)));
            o.set("boots", t.boots.size());
            auto at = attribute_resources(t);
            o.set("prefill_ns", at.prefill_time);
            o.set("queue_ns", at.queue_time);
            u64 retries = 0;
            for (const auto& ob : t.evidence) if (ob.type == ObsType::RETRY) ++retries;
            o.set("retries", retries);
            traces.push(std::move(o));
        }
        rep.set("traces", std::move(traces));
        std::printf("%s\n", rep.to_pretty().c_str());
        return saved.ok() ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cuda proof failed: %s\n", e.what());
        return 1;
    }
}
