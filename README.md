# Serving Observatory

Serving Observatory (Copyright 2026 Summon Software Labs, Apache-2.0) is a
**serving-observability runtime** that reconstructs, correlates, attributes,
replays, compares, and explains serving behavior from typed evidence. It is not
a dashboard and not a log parser: it owns the data model, the source authority,
the canonical request trace, deterministic latency decomposition, persistence,
replay digests, and the multi-process and GPU proofs below.

## Core question

> What happened to this request while it was served, where did its time and
> resources go, which runtime decisions shaped the outcome, and can that
> behavior be reconstructed later from evidence rather than guessed from
> aggregate metrics?

**System boundary.** Serving Observatory observes and reconstructs behavior
across admission, queueing, batching, dispatch, prefill, decode, speculation,
KV reuse/growth, model & adapter residency, kernel/graph reuse, memory
allocation, transfers, retries, cancellations, preemption (where represented),
worker restarts, failures, recovery, request completion, tail latency, and
per-request resource attribution. It **never** makes scheduling, admission,
quota, latency-governance, residency, or execution decisions. It may explain
those decisions when evidence exists, and it never silently infers authority
that was not observed.

## Evidence & provenance model

Every value carries a provenance: measured, reported, derived, estimated,
reconstructed, heuristic, or unknown. Provenance is never collapsed into one
confidence-free number. Missing data stays missing; a later outcome never
rewrites the original observation or decision record. Derived relationships
are explicitly labelled; candidate sets and inferred causal chains remain
distinguishable from directly reported facts. Doubles are rejected when
NaN/Inf, and deterministic JSON encodes each observation with its full
authority envelope and provenance.

The observation codec is canonical and deterministic: 128-bit identities,
big-endian integers, length-prefixed strings/bytes, and a validated reader that
rejects truncation, malformed lengths, checksum mismatch, invalid enums,
invalid generations, invalid timestamps, NaN/Inf, and trailing garbage.

## Source authority

Sources are tracked by SourceId, WorkerId, WorkerBootId, source generation,
observation generation, coordinator epoch, and a monotonic sequence number,
plus their clock domain, observation/receive timestamps, provenance, health,
confidence, staleness, last accepted observation, and restart state. A
restarted worker must mint a fresh WorkerBootId and higher generations; a stale
observation from an old boot / epoch / source generation / observation
generation is rejected for current-state mutation and retained only as
explicitly stale historical evidence. The coordinator validates every
observation against the current authority; only an ACCEPTED decision mutates
reconstructed state.

## Request trace model

A per-request trace is reconstructed from all observations bearing the same
RequestId. It records arrival, admission result, queue residence, queue
transitions, batch membership over time, dispatch attempts, worker/device
placement, prefill interval, decode steps, speculative work, KV reuse, transfer
activity, model/adapter readiness, kernel/graph reuse, retries, cancellations,
failures, recovery, completion, and the terminal outcome - while preserving
generations and authority transitions (a request may span multiple boots and
epochs).

## Latency decomposition

decompose_latency produces a deterministic breakdown: absolute duration,
percentage of total latency, an exclusive non-overlapping duration computed by
a sweep line (so covered time is never double-counted), and critical-path
contribution. Each component carries its own provenance.

## Resource attribution

Per-request attribution reports queue time, batch wait, dispatch delay,
transfer time, prefill time, first-token latency, decode time, inter-token
latency, retry/recovery time, total latency, GPU/CPU execution time, H2D/D2H/
inter-node/KV/tensor/workspace bytes, kernel/graph hits & misses, KV reuse,
model/adapter residency references, and speculation proposed/accepted/rejected
work. Nothing is fabricated when evidence is missing - unavailable metrics are
reported as unavailable.

## Correlation discipline

Correlation is separate from causation. Explanations use explicit classes:
directly evidenced, structurally derived, temporally correlated, plausible but
unproven, contradicted, unknown. If an event sequence permits only correlation,
that is stated.

## Tail analysis

explain_tail produces p50/p90/p95/p99/p999 over the observed trace set and, for
tail requests, explains which phases dominated, which queues contributed,
whether batching increased waiting, whether placement/transfer contributed,
whether models/adapters were cold, whether KV reuse existed, whether retries or
worker restarts/failures occurred, whether capacity/quota/SLO state changed, and
which values are directly measured vs derived.

## Replay

Replay reconstructs request timelines, batch membership, worker/device
assignment history, attempts/retries, generation changes, source restarts,
latency decompositions, terminal outcomes, derived aggregates, and explanation
records, and recomputes stable digests for source evidence, the canonical trace,
request explanation, and the aggregate snapshot. Replaying the same valid
evidence reproduces identical digests. The evidence/trace/explanation/aggregate
digests are recomputed on load and verified.

## Comparison & counterfactual

compare does trace-to-trace and window-to-window comparison; counterfactual
performs bounded what-ifs (remove queue wait, remove retry, warm residency,
halve transfer). Counterfactual results are labelled derived or estimated and
never pretend the counterfactual happened.

## Persistence

Archives are strict, versioned, checksummed binary files (with deterministic
JSON views). Loading rejects malformed lengths, truncation, checksum mismatch,
trailing garbage, unsupported versions, duplicate observation ids, invalid
enums/generations/timestamps, NaN/Inf, and impossible orderings. Recovery
preserves evidence provenance and never converts stale historical evidence
into current authority.

## Distributed proof

obscli multiprocess runs a real coordinator plus two source/worker OS processes
over framed TCP. It kills one source as an OS process, restarts it with a fresh
WorkerBootId, replays stale epoch / boot / source-generation / observation-
generation mutations over TCP, and proves every stale current-state mutation is
rejected while historical evidence stays queryable, that fresh post-restart
observations are accepted, that a trace spanning two source boots reconstructs
correctly, and that save-reload-replay reproduces identical digests. See
EXAMPLES.md.

## CUDA proof

obscli cuda (and sobs_cuda_proof) exercise real RTX 5090 / sm_120 CUDA work:
device allocation, H2D, a prefill-like GEMM, a decode-like elementwise kernel,
synchronization, D2H, and CPU reference verification. It emits typed
observations carrying MEASURED durations and byte counts, then reconstructs
cold vs warm prefill, decode, and a fail-then-retry request from evidence. The
queue delay and the deliberately-corrupted failing run are labelled synthetic.
See EXAMPLES.md.

## Benchmarks

Real measured numbers for ingestion, replay, JSON serialization, persistence
save/recover, and concurrent ingestion are in BENCHMARKS.md.

## Build & install

Requires CMake >= 3.20, a C++20 compiler (MSVC 2022 /W4 /WX on Windows), and
optionally CUDA with sm_120 for the proof.

    cmake -S . -B build -G Ninja
    cmake --build build --target ServingObservatory obscli sobs_tests
    ctest --test-dir build
    cmake --install build --prefix <prefix>

A downstream consumer uses find_package(ServingObservatory CONFIG REQUIRED)
and links ServingObservatory::ServingObservatory (see cmake/consumer).
Versions/identities are strongly typed 128-bit; observation ids are
collision-free across sources, boots, sequences, and types.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
