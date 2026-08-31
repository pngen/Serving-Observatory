// Serving Observatory — request trace reconstruction.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/trace/trace.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace servingobs {

string_view phase_name(Phase p) {
    switch (p) {
        case Phase::ARRIVAL: return "arrival";
        case Phase::ADMISSION: return "admission";
        case Phase::QUEUE: return "queue";
        case Phase::BATCH_WAIT: return "batch_wait";
        case Phase::DISPATCH: return "dispatch";
        case Phase::TRANSFER: return "transfer";
        case Phase::PREFILL: return "prefill";
        case Phase::FIRST_TOKEN: return "first_token";
        case Phase::DECODE: return "decode";
        case Phase::SPECULATION: return "speculation";
        case Phase::RETRY: return "retry";
        case Phase::RECOVERY: return "recovery";
        case Phase::COMPLETION: return "completion";
        case Phase::TOTAL: return "total";
    }
    return "unknown";
}

TimedObs obs_time(const Observation& o) {
    if (o.has_recv_ts) return { o.recv_ts, true };
    if (o.has_obs_ts) return { o.obs_ts, false };
    return { 0, false };
}

void RequestTrace::add_phase(Phase p, TimestampNs start, TimestampNs end, Provenance prov,
                             string source, string detail) {
    size_t i = static_cast<size_t>(p);
    phases[i].phase = p;
    phases[i].exists = true;
    phases[i].start = start;
    phases[i].end = end;
    phases[i].provenance = prov;
    phases[i].source = std::move(source);
    phases[i].detail = std::move(detail);
}

namespace {
// First/last observation of a given type in a sorted vector.
const Observation* first_obs(const std::vector<Observation>& v, ObsType t) {
    for (const auto& o : v) if (o.type == t) return &o;
    return nullptr;
}
const Observation* last_obs(const std::vector<Observation>& v, ObsType t) {
    for (auto it = v.rbegin(); it != v.rend(); ++it) if (it->type == t) return &*it;
    return nullptr;
}
// Count of observations of a type.
u64 count_obs(const std::vector<Observation>& v, ObsType t) {
    u64 c = 0; for (const auto& o : v) if (o.type == t) ++c; return c;
}
}

RequestTrace reconstruct_trace(const std::vector<Observation>& obs_in, const RequestId& request_id) {
    RequestTrace tr;
    tr.request_id = request_id;
    tr.trace_id = TraceId(Id128::derive(0x5154, request_id.raw().to_bytes().data(), 16));

    auto obs = obs_in;
    std::sort(obs.begin(), obs.end(), [](const Observation& a, const Observation& b) {
        TimedObs ta = obs_time(a), tb = obs_time(b);
        if (ta.ts != tb.ts) return ta.ts < tb.ts;
        if (ta.in_recv_domain != tb.in_recv_domain) return ta.in_recv_domain > tb.in_recv_domain;
        if (a.seq != b.seq) return a.seq < b.seq;
        return a.id.raw() < b.id.raw();
    });
    tr.evidence = obs;

    // --- identity snapshot from first observation exposing each field
    for (const auto& o : obs) {
        if (tr.tenant.is_null()) tr.tenant = TenantId(o.id_field(FieldKeys::TenantId));
        if (tr.workload.is_null()) tr.workload = WorkloadId(o.id_field(FieldKeys::WorkloadId));
        if (tr.model.is_null()) tr.model = ModelId(o.id_field(FieldKeys::ModelId));
        if (tr.model_rev.is_null()) tr.model_rev = ModelRevisionId(o.id_field(FieldKeys::ModelRevId));
        if (tr.adapter.is_null()) tr.adapter = AdapterId(o.id_field(FieldKeys::AdapterId));
        // generation / boot / epoch metadata
        auto add_boot = [&](const WorkerBootId& b) {
            if (std::find(tr.boots.begin(), tr.boots.end(), b) == tr.boots.end()) tr.boots.push_back(b);
        };
        auto add_gen = [&](SourceGeneration g) {
            if (std::find(tr.src_gens.begin(), tr.src_gens.end(), g) == tr.src_gens.end()) tr.src_gens.push_back(g);
        };
        auto add_ogen = [&](ObservationGeneration g) {
            if (std::find(tr.obs_gens.begin(), tr.obs_gens.end(), g) == tr.obs_gens.end()) tr.obs_gens.push_back(g);
        };
        add_boot(o.boot); add_gen(o.src_gen); add_ogen(o.obs_gen);
        if (tr.min_epoch == 0 || o.epoch < tr.min_epoch) tr.min_epoch = o.epoch;
        if (o.epoch > tr.max_epoch) tr.max_epoch = o.epoch;
    }
    tr.spans_multiple_epochs = tr.min_epoch != tr.max_epoch;

    // --- timeline
    tr.timeline.reserve(obs.size());
    u32 ord = 0;
    for (const auto& o : obs) {
        TraceEvent ev;
        ev.ordinal = ord++;
        ev.type = o.type;
        TimedObs t = obs_time(o);
        ev.ts = t.ts; ev.has_ts = t.ts != 0;
        ev.provenance = o.provenance;
        ev.source = o.source; ev.worker = o.worker; ev.boot = o.boot;
        ev.src_gen = o.src_gen; ev.obs_gen = o.obs_gen; ev.epoch = o.epoch; ev.seq = o.seq;
        ev.label = string(obs_type_name(o.type));
        // Copy payload fields relevant to this observation.
        ev.fields = o.fields;
        tr.timeline.push_back(std::move(ev));
    }

    // --- phase extraction
    const Observation* req = first_obs(obs, ObsType::REQUEST);
    const Observation* adm = first_obs(obs, ObsType::ADMISSION);
    const Observation* qe = first_obs(obs, ObsType::QUEUE_ENTER);
    const Observation* ql = last_obs(obs, ObsType::QUEUE_LEAVE);
    const Observation* bf = first_obs(obs, ObsType::BATCH_FORM);
    const Observation* bs = first_obs(obs, ObsType::BATCH_SEAL);
    const Observation* disp = first_obs(obs, ObsType::DISPATCH);
    const Observation* tr_start = first_obs(obs, ObsType::TRANSFER_START);
    const Observation* tr_end = last_obs(obs, ObsType::TRANSFER_END);
    const Observation* pf_s = first_obs(obs, ObsType::PREFILL_START);
    const Observation* pf_e = last_obs(obs, ObsType::PREFILL_END);
    const Observation* ds_start = first_obs(obs, ObsType::DECODE_STEP_START);
    const Observation* ds_end = last_obs(obs, ObsType::DECODE_STEP_END);
    const Observation* spec_s = first_obs(obs, ObsType::SPECULATION_START);
    const Observation* spec_e = last_obs(obs, ObsType::SPECULATION_VERIFY);
    const Observation* retry = first_obs(obs, ObsType::RETRY);
    const Observation* fail = first_obs(obs, ObsType::FAILURE);
    const Observation* cancel = first_obs(obs, ObsType::CANCEL);
    const Observation* wdown = first_obs(obs, ObsType::WORKER_DOWN);
    const Observation* rec = first_obs(obs, ObsType::RECOVERY);
    (void)wdown;  // worker-down presence is preserved in the timeline events
    const Observation* comp = last_obs(obs, ObsType::COMPLETION);

    auto tdom = [&](const Observation* o) -> TimedObs { return o ? obs_time(*o) : TimedObs{0,false}; };

    // Arrival.
    TimedObs a = tdom(req);
    if (a.ts == 0) a = tdom(qe);
    if (a.ts == 0 && !obs.empty()) a = obs_time(obs.front());
    TimedObs c = tdom(comp);
    if (c.ts == 0 && !obs.empty()) c = obs_time(obs.back());

    if (a.ts != 0) tr.add_phase(Phase::ARRIVAL, a.ts, a.ts, req ? req->provenance : Provenance::RECONSTRUCTED,
                                req ? req->source.to_hex() : "", "arrival");
    if (adm && tdom(adm).ts != 0) {
        TimedObs t = tdom(adm);
        tr.add_phase(Phase::ADMISSION, t.ts, t.ts, adm->provenance, adm->source.to_hex(), "admission");
    }
    if (qe && ql && tdom(qe).ts != 0 && tdom(ql).ts != 0) {
        TimedObs s = tdom(qe), e = tdom(ql);
        tr.add_phase(Phase::QUEUE, s.ts, e.ts,
                     (s.in_recv_domain && e.in_recv_domain) ? Provenance::MEASURED : Provenance::RECONSTRUCTED,
                     qe->source.to_hex(), "queue residence (" + std::to_string(count_obs(obs, ObsType::QUEUE_ENTER)) + " enter)");
    }
    if ((bf || bs) && disp && tdom(disp).ts != 0) {
        const Observation* x = bf ? bf : bs;
        TimedObs s = tdom(x), e = tdom(disp);
        tr.add_phase(Phase::BATCH_WAIT, s.ts, e.ts,
                     (s.in_recv_domain && e.in_recv_domain) ? Provenance::MEASURED : Provenance::RECONSTRUCTED,
                     x->source.to_hex(), "batch wait");
    }
    if (disp && tdom(disp).ts != 0) {
        TimedObs s = tdom(disp);
        TimedObs e = tdom(pf_s);
        if (e.ts == 0) e = tdom(ds_start);
        if (e.ts < s.ts) e.ts = s.ts;
        tr.add_phase(Phase::DISPATCH, s.ts, e.ts,
                     (s.in_recv_domain && e.in_recv_domain) ? Provenance::MEASURED : Provenance::RECONSTRUCTED,
                     disp->source.to_hex(), "dispatch -> start of execution");
    }
    if (tr_start && tr_end && tdom(tr_start).ts != 0 && tdom(tr_end).ts != 0) {
        TimedObs s = tdom(tr_start), e = tdom(tr_end);
        tr.add_phase(Phase::TRANSFER, s.ts, e.ts,
                     (s.in_recv_domain && e.in_recv_domain) ? Provenance::MEASURED : Provenance::RECONSTRUCTED,
                     tr_start->source.to_hex(), "transfer");
    }
    TimedObs ps = tdom(pf_s), pe = tdom(pf_e);
    if (pf_s && pf_e && ps.ts != 0 && pe.ts != 0) {
        tr.add_phase(Phase::PREFILL, ps.ts, pe.ts,
                     (ps.in_recv_domain && pe.in_recv_domain) ? Provenance::MEASURED : Provenance::RECONSTRUCTED,
                     pf_s->source.to_hex(), "prefill");
    }
    TimedObs dss = tdom(ds_start), dse = tdom(ds_end);
    if (ds_start && ds_end && dss.ts != 0 && dse.ts != 0) {
        // first token: after prefill end to first decode step end (or start).
        TimedObs ft_end = tdom(ds_end);
        TimedObs ft_base = pe.ts != 0 ? pe : dss;
        if (ft_end.ts < ft_base.ts) ft_end = dss;
        tr.add_phase(Phase::FIRST_TOKEN, ft_base.ts, ft_end.ts,
                     Provenance::RECONSTRUCTED, ds_start->source.to_hex(), "first token");
        tr.add_phase(Phase::DECODE, dss.ts, dse.ts,
                     (dss.in_recv_domain && dse.in_recv_domain) ? Provenance::MEASURED : Provenance::RECONSTRUCTED,
                     ds_start->source.to_hex(), "decode (" + std::to_string(count_obs(obs, ObsType::DECODE_STEP_START)) + " steps)");
    }
    if (spec_s && spec_e && tdom(spec_s).ts != 0 && tdom(spec_e).ts != 0) {
        TimedObs s = tdom(spec_s), e = tdom(spec_e);
        tr.add_phase(Phase::SPECULATION, s.ts, e.ts,
                     (s.in_recv_domain && e.in_recv_domain) ? Provenance::MEASURED : Provenance::RECONSTRUCTED,
                     spec_s->source.to_hex(), "speculation");
    }
    if (retry && tdom(retry).ts != 0) {
        TimedObs t = tdom(retry);
        tr.add_phase(Phase::RETRY, t.ts, t.ts, retry->provenance, retry->source.to_hex(),
                     "retry (" + std::to_string(count_obs(obs, ObsType::RETRY)) + " events)");
    }
    if (rec && tdom(rec).ts != 0) {
        TimedObs t = tdom(rec);
        tr.add_phase(Phase::RECOVERY, t.ts, t.ts, rec->provenance, rec->source.to_hex(), "recovery");
    }
    if (comp && tdom(comp).ts != 0) {
        TimedObs t = tdom(comp);
        tr.add_phase(Phase::COMPLETION, t.ts, t.ts, comp->provenance, comp->source.to_hex(), "completion");
    }

    // Total: arrival -> completion (or last evidence).
    if (a.ts != 0) {
        TimedObs last = c.ts != 0 ? c : (obs.empty() ? a : obs_time(obs.back()));
        if (last.ts < a.ts) last.ts = a.ts;
        tr.add_phase(Phase::TOTAL, a.ts, last.ts, Provenance::RECONSTRUCTED, "", "total");
    }

    // Outcome.
    if (comp) {
        string oc = comp->str_field(FieldKeys::Outcome);
        tr.outcome = outcome_from_code(oc);
        tr.terminal_reason = comp->str_field(FieldKeys::Reason);
    } else if (cancel) {
        tr.outcome = Outcome::CANCELLED;
        tr.terminal_reason = cancel->str_field(FieldKeys::Reason);
    } else if (fail) {
        tr.outcome = Outcome::FAILED;
        tr.terminal_reason = fail->str_field(FieldKeys::Reason);
    } else if (!obs.empty()) {
        tr.outcome = Outcome::UNKNOWN;
    }
    if (tr.outcome == Outcome::UNKNOWN) {
        if (count_obs(obs, ObsType::FAILURE) > 0 && !comp) tr.outcome = Outcome::FAILED;
        else if (comp) tr.outcome = Outcome::COMPLETED;
    }

    return tr;
}

Json trace_to_json(const RequestTrace& t) {
    Json j = Json::object();
    j.set("trace_id", t.trace_id.to_hex());
    j.set("request_id", t.request_id.to_hex());
    auto setid = [&](const char* k, const auto& v) { j.set(k, v.to_hex()); };
    if (!t.tenant.is_null()) setid("tenant_id", t.tenant);
    if (!t.workload.is_null()) setid("workload_id", t.workload);
    if (!t.model.is_null()) setid("model_id", t.model);
    if (!t.model_rev.is_null()) setid("model_rev_id", t.model_rev);
    if (!t.adapter.is_null()) setid("adapter_id", t.adapter);
    j.set("outcome", string(outcome_name(t.outcome)));
    j.set("terminal_reason", t.terminal_reason);
    j.set("epoch_min", t.min_epoch);
    j.set("epoch_max", t.max_epoch);
    j.set("spans_multiple_epochs", t.spans_multiple_epochs);

    Json ev = Json::array();
    for (const auto& e : t.timeline) {
        Json o = Json::object();
        o.set("ordinal", static_cast<u64>(e.ordinal));
        o.set("kind", string(obs_type_name(e.type)));
        o.set("ts", e.ts);
        o.set("has_ts", e.has_ts);
        o.set("provenance", string(provenance_name(e.provenance)));
        o.set("source", e.source.to_hex());
        o.set("worker", e.worker.to_hex());
        o.set("boot", e.boot.to_hex());
        o.set("src_gen", e.src_gen);
        o.set("obs_gen", e.obs_gen);
        o.set("epoch", e.epoch);
        o.set("seq", e.seq);
        o.set("label", e.label);
        ev.push(std::move(o));
    }
    j.set("timeline", std::move(ev));

    Json ph = Json::object();
    for (size_t i = 0; i < t.phases.size(); ++i) {
        const PhaseInterval& p = t.phases[i];
        if (!p.exists) continue;
        Json o = Json::object();
        o.set("start", p.start);
        o.set("end", p.end);
        o.set("duration_ns", p.end >= p.start ? p.end - p.start : 0u);
        o.set("provenance", string(provenance_name(p.provenance)));
        if (!p.source.empty()) o.set("source", p.source);
        o.set("detail", p.detail);
        ph.set(string(phase_name(p.phase)), std::move(o));
    }
    j.set("phases", std::move(ph));

    Json boots = Json::array();
    for (const auto& b : t.boots) boots.push(b.to_hex());
    j.set("boots", std::move(boots));
    Json sgens = Json::array();
    for (auto g : t.src_gens) sgens.push(g);
    j.set("src_gens", std::move(sgens));
    Json ogens = Json::array();
    for (auto g : t.obs_gens) ogens.push(g);
    j.set("obs_gens", std::move(ogens));

    return j;
}

} // namespace servingobs
