
// Serving Observatory — regression tests: source registration authority, the
// coordinator epoch, and idle source connections.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "framework.hpp"

#include "servingobs/core/identity.hpp"
#include "servingobs/model/observation.hpp"
#include "servingobs/model/source_authority.hpp"
#include "servingobs/store/coordinator.hpp"
#include "servingobs/store/source.hpp"

#include <chrono>
#include <thread>

using namespace servingobs;

namespace {

Observation authority_obs(const SourceId& s, const WorkerId& w, const WorkerBootId& b, u64 gen,
                          CoordinatorEpoch epoch, SeqNum seq) {
    Observation o;
    o.type = ObsType::REQUEST;
    o.source = s; o.worker = w; o.boot = b;
    o.src_gen = gen; o.obs_gen = gen; o.epoch = epoch;
    o.clock_domain = "test_mono";
    o.seq = seq;
    o.provenance = Provenance::MEASURED;
    o.id = derive_observation_id(s.raw(), w.raw(), b.raw(), seq, o.type);
    return o;
}

HelloPayload hello(const SourceId& s, const WorkerId& w, const WorkerBootId& b, u64 gen, CoordinatorEpoch epoch) {
    HelloPayload h;
    h.source = s; h.worker = w; h.boot = b;
    h.src_gen = gen; h.obs_gen = gen; h.epoch = epoch;
    h.clock_domain = "test_mono";
    return h;
}

} // namespace

TEST(regression_stale_registration_does_not_replace_current_boot) {
    SourceRegistry reg;
    const SourceId s(Id128::from_u64(41));
    const WorkerId w(Id128::from_u64(42));
    const WorkerBootId b1(Id128::make(1, 1)), b2(Id128::make(2, 2)), b3(Id128::make(3, 3));
    CHECK(reg.register_source(authority_obs(s, w, b1, 1, 1, 0), 1).ok());
    // The restart: a fresh boot with strictly higher generations.
    CHECK(reg.register_source(authority_obs(s, w, b2, 2, 1, 0), 1).ok());
    // A replay of the old boot's registration does not take authority back.
    CHECK(!reg.register_source(authority_obs(s, w, b1, 1, 1, 0), 1).ok());
    // Nor does another boot without higher generations.
    CHECK(!reg.register_source(authority_obs(s, w, b3, 2, 1, 0), 1).ok());
    CHECK(reg.get(s, w).boot == b2);
    string reason;
    CHECK(reg.validate(authority_obs(s, w, b2, 2, 1, 1), reason) == AuthorityDecision::ACCEPTED);
    CHECK(reg.validate(authority_obs(s, w, b1, 1, 1, 2), reason) != AuthorityDecision::ACCEPTED);
}

TEST(regression_coordinator_epoch_is_enforced) {
    Coordinator coord;
    coord.set_epoch(5);
    const SourceId s(Id128::from_u64(51));
    const WorkerId w(Id128::from_u64(52));
    const WorkerBootId b(Id128::make(5, 5));
    // A registration carrying an older coordinator epoch is stale.
    CHECK(!coord.register_hello(hello(s, w, b, 1, 1)).ok());
    CHECK(coord.register_hello(hello(s, w, b, 1, 5)).ok());
    CHECK(coord.ingest(authority_obs(s, w, b, 1, 5, 1)).accepted);
    // Once the coordinator moves to a new epoch, an observation from the old one
    // no longer changes current state.
    coord.set_epoch(6);
    CHECK(!coord.ingest(authority_obs(s, w, b, 1, 5, 2)).accepted);
    CHECK_EQ(coord.accepted_count(), 1u);
}

TEST(regression_idle_source_keeps_its_connection) {
    Coordinator coord;
    coord.set_epoch(1);
    // Accepted connections inherit the listener's 100 ms receive timeout.
    CoordinatorServer server(coord, "127.0.0.1", 0, 100);
    CHECK(server.start());
    SourceConfig cfg;
    cfg.source = SourceId(Id128::from_u64(61));
    cfg.worker = WorkerId(Id128::from_u64(62));
    cfg.boot = WorkerBootId(Id128::make(6, 6));
    Source src(cfg);
    CHECK(src.connect("127.0.0.1", server.port()));
    CHECK(src.send_hello());
    // Stay idle across several of the server's receive timeouts.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto ack = src.send_observation(src.make_observation(ObsType::REQUEST));
    CHECK(ack.ok() && ack.value().payload.accepted);
    src.disconnect();
    server.stop();
    CHECK_EQ(coord.accepted_count(), 1u);
}
