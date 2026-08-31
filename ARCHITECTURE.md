# Architecture

Serving Observatory is a C++20 runtime split into a header-installable static
library (`ServingObservatory`), a CLI (`obscli`), an optional CUDA module
(`ServingObservatoryCuda`), and the executable proofs. It ships a versioned
CMake package for downstream `find_package` consumption.

## Modules

**core/**  - `types` (u64/i64/byte/string/Result), `identity` (strongly typed
128-bit Id<Tag> + Id128 with deterministic hex/binary round-trip and a
collision-free observation-id derivation), `enums` (observation types, provenance,
explanation classes, source health, outcome, queue kind), `digest` (SHA-256),
`json` (sorted-key deterministic JSON tree), `binary_codec` (canonical
validated binary reader/writer).

**model/**  - `observation` (typed observations + field payloads; every
observation carries its full authority envelope and provenance),
`source_authority` (SourceRegistry: register boot, validate authority,
mark_restart; rejects stale epoch/boot/source-generation/observation-generation,
duplicate and out-of-order sequence).

**trace/**  - `trace` (reconstruct_trace: canonical per-request timeline + phase
intervals preserving generations/boots), `latency` (deterministic exclusive
sweep-line decomposition + critical path), `attribution` (per-request resource
accounting with per-metric provenance), `analysis` (percentiles, tail
explanation, correlation grouping, causal-discipline explanation).

**replay/**  - `replay` (deterministic replay + stable evidence/trace/explanation/
aggregate digests), `comparison` (trace-to-trace and window-to-window),
`counterfactual` (bounded what-if transforms labelled derived/estimated).

**store/**  - `protocol` (framed TCP: length+CRC-32 framed messages, strict
decoding), `network` (RAII Winsock sockets, per-connection write serialization,
exact-length reads), `coordinator` (thread-safe ingestion engine + concurrent
TCP server; no network I/O under the state lock), `source` (source/worker
client: HELLO registration, observation streaming, ack handling, restart boot
minting), `persistence` (strict versioned checksummed archive).

**cuda/**  - the RTX 5090 / sm_120 serving-evidence proof (real device work).

**apps/obscli**  - single-TU CLI dispatching serve/source/ingest/list/inspect/
trace/timeline/explain/latency/tail/compare/counterfactual/replay/recover/
batch/worker/device/snapshot/multiprocess/cuda/benchmark.

## Data flow

Sources register a boot authority (HELLO) and stream typed observations over
framed TCP. The coordinator validates each observation against the current
authority under a state lock, deduplicates by observation id, applies ACCEPTED
observations to reconstructed state, and retains rejected/stale evidence as
historical without mutation. On stop it saves a checksummed archive; loading
re-verifies the body checksum and re-runs the deterministic replay, confirming
the evidence/trace/explanation/aggregate digests match.

## Provenance & causality

Every value retains a provenance and every explanation a causal class. A later
outcome never rewrites an earlier observation or decision. Derived relationships
are labelled; counterfactual results are derived/estimated; correlation is never
asserted as causation.

## Key properties

- Deterministic identity serialization (hex + big-endian binary round-trips).
- Collision-free observation ids across source/boot/seq/type.
- Canonical validated binary + deterministic sorted-key JSON.
- Exclusive (non-overlapping) latency decomposition via sweep line.
- Stable SHA-256 digests for evidence, trace, explanation, aggregate.
- Strict persistence rejection of malformed/truncated/checksum-bad archives.
- Network I/O off the global state lock; per-connection write serialization.
- Real multiprocess source-authority proof and real CUDA serving-evidence proof.

## Build layout

    include/servingobs/   public headers (core, model, trace, replay, store, cuda)
    src/                  library implementation
    apps/obscli/          CLI
    cuda/                 CUDA proof module + runner
    tests/                deterministic test suite
    examples/             runnable examples
    benchmarks/           benchmark tooling/report
    cmake/                package config template + downstream consumer
    proof_run/            runtime production of multiprocess/CUDA proof artifacts
