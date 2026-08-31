// Serving Observatory — strict versioned persistence.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/store/persistence.hpp"
#include "servingobs/core/binary_codec.hpp"

#include <cstdio>
#include <fstream>
#include <set>

namespace servingobs {

namespace {
// Serialize one observation as length-prefixed bytes in the body.
void write_obs_entry(BinaryWriter& w, const Observation& o) {
    w.write_bytes(encode_observation_payload(o));
}
Result<Observation> read_obs_entry(BinaryReader& r) {
    bytes b = r.read_bytes();
    if (!r.ok()) return Result<Observation>::Err("truncated observation entry: " + r.error());
    return read_observation_payload(b);
}
inline Digest digest_from_bytes(const bytes& b) {
    Digest d{};
    for (std::size_t i = 0; i < 32 && i < b.size(); ++i) d[i] = static_cast<u8>(b[i]);
    return d;
}
}

Result<bytes> file_read_all(const string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Result<bytes>::Err("cannot open " + path);
    f.seekg(0, std::ios::end);
    std::streamsize n = f.tellg();
    f.seekg(0, std::ios::beg);
    if (n < 0) return Result<bytes>::Err("cannot determine file size");
    bytes out(static_cast<std::size_t>(n));
    if (n > 0) f.read(reinterpret_cast<char*>(out.data()), n);
    if (!f) return Result<bytes>::Err("read failed: " + path);
    return Result<bytes>::Ok(std::move(out));
}

Result<ArchiveContents> build_archive(const Coordinator& c) {
    ArchiveContents a;
    a.epoch = c.epoch();
    a.created_ns = 0;
    // snapshot
    std::vector<SourceAuthority> reg = c.registry_snapshot();
    a.registry = reg;
    a.stale = c.stale_history();
    std::vector<Observation> accepted = c.all_accepted();
    a.accepted = accepted;

    // run deterministic replay to produce digests
    ReplayResult rp = replay(accepted);
    a.stored_evidence_digest = rp.evidence_digest;
    a.stored_trace_digest = rp.trace_digest;
    a.stored_explanation_digest = rp.explanation_digest;
    a.stored_aggregate_digest = rp.aggregate_digest;
    a.verified_replay = rp;
    a.replay_match = true;
    return Result<ArchiveContents>::Ok(std::move(a));
}

Result<ArchiveContents> save_archive(const Coordinator& c, const string& path) {
    auto arch = build_archive(c);
    if (!arch.ok()) return arch;
    const ArchiveContents& a = arch.value();

    BinaryWriter body;
    body.write_u32(kArchiveMagic);
    body.write_u16(kArchiveVersion);
    body.write_u8(0);
    body.write_u64(a.epoch);
    body.write_u64(a.created_ns);
    body.write_count(a.registry.size());
    for (const auto& auth : a.registry) write_authority(body, auth);
    body.write_count(a.accepted.size());
    for (const auto& o : a.accepted) write_obs_entry(body, o);
    body.write_count(a.stale.size());
    for (const auto& [o, reason] : a.stale) {
        write_obs_entry(body, o);
        body.write_str(reason);
    }
    BinaryWriter digestw;
    auto put_digest = [&](const Digest& d) { for (auto c : d) digestw.write_u8(c); };
    put_digest(a.stored_evidence_digest);
    put_digest(a.stored_trace_digest);
    put_digest(a.stored_explanation_digest);
    put_digest(a.stored_aggregate_digest);
    body.write_bytes(digestw.data());

    bytes bodyb = body.data();
    Digest content_digest = Sha256::hash(bodyb);

    BinaryWriter w;
    w.write_u32(static_cast<u32>(bodyb.size()));
    for (auto b : bodyb) w.write_u8(static_cast<u8>(b));
    for (auto b : content_digest) w.write_u8(static_cast<u8>(b));

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return Result<ArchiveContents>::Err("cannot open for write: " + path);
    f.write(reinterpret_cast<const char*>(w.data().data()), static_cast<std::streamsize>(w.data().size()));
    if (!f) return Result<ArchiveContents>::Err("write failed: " + path);
    return arch;
}

Result<ArchiveContents> load_archive(const string& path, bool verify_replay) {
    auto fbr = file_read_all(path);
    if (!fbr.ok()) return Result<ArchiveContents>::Err(fbr.error());
    const bytes& all = fbr.value();
    BinaryReader r(all);
    u32 body_len = r.read_u32();
    if (!r.ok()) return Result<ArchiveContents>::Err("truncated archive header");
    // Strict size: 4 (len) + body_len + 32 (digest) must equal file size.
    if (all.size() != 4 + static_cast<std::size_t>(body_len) + 32) {
        return Result<ArchiveContents>::Err("archive size mismatch: " + std::to_string(all.size()) +
                                            " != expected " + std::to_string(4 + body_len + 32));
    }
    bytes bodyb = r.read_raw(body_len);
    if (bodyb.size() != body_len) return Result<ArchiveContents>::Err("truncated archive body");
    bytes stored_hash = r.read_raw(32);
    if (stored_hash.size() != 32) return Result<ArchiveContents>::Err("truncated archive digest");
    if (!r.require_end()) return Result<ArchiveContents>::Err("trailing garbage after archive");
    Digest computed = Sha256::hash(bodyb);
    if (!digest_equal(computed, digest_from_bytes(stored_hash))) {
        return Result<ArchiveContents>::Err("archive checksum mismatch");
    }

    BinaryReader b(bodyb);
    u32 magic = b.read_u32();
    if (magic != kArchiveMagic) return Result<ArchiveContents>::Err("bad archive magic");
    u16 ver = b.read_u16();
    if (ver != kArchiveVersion) return Result<ArchiveContents>::Err("unsupported archive version " + std::to_string(ver));
    b.read_u8(); // reserved

    ArchiveContents a;
    a.epoch = b.read_u64();
    a.created_ns = b.read_u64();
    u64 nreg = b.read_count();
    if (nreg > 1000000) return Result<ArchiveContents>::Err("registry count overflow");
    for (u64 i = 0; i < nreg; ++i) {
        auto auth = read_authority(b);
        if (!auth.ok()) return Result<ArchiveContents>::Err("bad authority in archive: " + auth.error());
        a.registry.push_back(auth.value());
    }
    u64 nobs = b.read_count();
    if (nobs > 1000000000ull) return Result<ArchiveContents>::Err("observation count overflow");
    std::set<ObservationId, std::less<>> seen;
    for (u64 i = 0; i < nobs; ++i) {
        auto o = read_obs_entry(b);
        if (!o.ok()) return Result<ArchiveContents>::Err(o.error());
        if (seen.count(o.value().id)) return Result<ArchiveContents>::Err("duplicate observation id in archive");
        seen.insert(o.value().id);
        a.accepted.push_back(std::move(o.value()));
    }
    u64 nstale = b.read_count();
    if (nstale > 100000000ull) return Result<ArchiveContents>::Err("stale count overflow");
    for (u64 i = 0; i < nstale; ++i) {
        auto o = read_obs_entry(b);
        if (!o.ok()) return Result<ArchiveContents>::Err(o.error());
        string reason = b.read_str();
        if (!b.ok()) return Result<ArchiveContents>::Err("truncated stale reason");
        a.stale.push_back({ std::move(o.value()), std::move(reason) });
    }
    bytes dbytes = b.read_bytes();
    if (!b.require_end()) return Result<ArchiveContents>::Err("trailing garbage in archive body");
    if (dbytes.size() != 128) return Result<ArchiveContents>::Err("bad stored digest block");
    auto copy32 = [](const bytes& src, size_t off) -> Digest {
        Digest d{}; for (int i = 0; i < 32; ++i) d[i] = static_cast<u8>(src[off + i]); return d;
    };
    a.stored_evidence_digest = copy32(dbytes, 0);
    a.stored_trace_digest = copy32(dbytes, 32);
    a.stored_explanation_digest = copy32(dbytes, 64);
    a.stored_aggregate_digest = copy32(dbytes, 96);

    if (verify_replay) {
        ReplayResult rp = replay(a.accepted);
        a.verified_replay = rp;
        a.replay_match = (rp.evidence_digest == a.stored_evidence_digest) &&
                         (rp.trace_digest == a.stored_trace_digest) &&
                         (rp.explanation_digest == a.stored_explanation_digest) &&
                         (rp.aggregate_digest == a.stored_aggregate_digest);
        if (!a.replay_match) {
            return Result<ArchiveContents>::Err("replay digests do not match stored digests");
        }
    }
    return Result<ArchiveContents>::Ok(std::move(a));
}

Json ArchiveContents::to_json() const {
    Json j = Json::object();
    j.set("magic", static_cast<u64>(kArchiveMagic));
    j.set("version", static_cast<u64>(kArchiveVersion));
    j.set("epoch", epoch);
    j.set("created_ns", created_ns);
    j.set("registry_count", registry.size());
    j.set("accepted_count", accepted.size());
    j.set("stale_count", stale.size());
    j.set("stored_evidence_digest", digest_hex(stored_evidence_digest));
    j.set("stored_trace_digest", digest_hex(stored_trace_digest));
    j.set("stored_explanation_digest", digest_hex(stored_explanation_digest));
    j.set("stored_aggregate_digest", digest_hex(stored_aggregate_digest));
    j.set("replay_match", replay_match);
    if (replay_match) j.set("replay", verified_replay.to_json());
    return j;
}

} // namespace servingobs
