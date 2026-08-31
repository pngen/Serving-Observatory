# Benchmarks

All numbers below are **measured** with the Release build (MSVC 2022 19.44,
`/O2`, `/W4`, C++20) on an NVIDIA GeForce RTX 5090 workstation (sm_120), and are
reported as observed; they are not synthetic estimates. Run them with
`obscli benchmark --mode <mode> --count <n>`.

## Observation ingestion

    obscli benchmark --mode ingest --count 50000
    ingest count=50000 time_ms=57.48 ops_per_sec=869847

## Per-request trace reconstruction (replay)

    obscli benchmark --mode replay --count 20000
    replay count=20000 traces=19999 time_ms=876.25

(One trace per request id; request id 0 is a null id and is not grouped.)

## Deterministic JSON serialization

    obscli benchmark --mode json --count 100000
    json count=100000 time_ms=326.63 ops_per_sec=306155

## Persistence save + recovery (verify)

    obscli benchmark --mode persist --count 5000
    persist count=5000 save=1 load=1 time_ms=471.91

## Concurrent ingestion (8 threads)

    obscli benchmark --mode concurrent --count 20000 --threads 8
    concurrent count=20000 threads=8 time_ms=8.49 ops_per_sec=2356268

## CUDA serving-evidence (RTX 5090 / sm_120)

    sobs_cuda_proof
    cold_prefill_ms=0.1646 warm_prefill_ms=0.0161 decode_ms=0.1097
    cold-vs-warm delta=0.1486 ms  (real device execution, measured via events)

These numbers vary run to run; they are real measured device/CPU durations.

## Multiprocess source-authority proof

    obscli multiprocess --dir proof_run

Real coordinator + 2 source OS processes over framed TCP: 31 observations
accepted, 4 stale authority mutations (epoch/boot/source-generation/
observation-generation) rejected over TCP, deterministic replay after process
restart reproduced identical digests (`replay_match=1`).

## Measurement notes

- Ingestion, replay, JSON, persist, and concurrent modes use the in-process
  library paths with the coordinator/reader; only the multiprocess and CUDA
  proofs use real TCP and real CUDA execution.
- Wall-clock `std::chrono::steady_clock` timing; single sample per mode here.
- The `replay` mode reconstructs every request trace and its explanation, so it
  is the heaviest single-threaded path measured.