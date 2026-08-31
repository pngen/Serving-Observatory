# Examples

Serving Observatory ships runnable examples. They use the `obscli` CLI and the
`examples/*.jsonl` scripts (line format: `OBS <type> <obs_ts_ns> key=value ...`).
Run them all with `examples\run_examples.cmd` (Windows) or the individual
commands below.

## 1. Basic request trace

Shows arrival -> admission -> queue -> batch -> dispatch -> transfer -> prefill
> decode -> KV hit -> completion, and the deterministic latency decomposition
with exclusive attribution and the per-request resource attribution.

    obscli ingest --script examples/basic.jsonl --out examples/basic.sobs --epoch 1
    obscli trace   --archive examples/basic.sobs --request aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1
    obscli latency --archive examples/basic.sobs --request aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1

## 2. Queue + batch timing

`basic.jsonl` includes a 600 ns queue residence and a 200 ns batch wait. The
latency breakdown reports queue=24% and batch_wait=8% of total, and the tail
explanation attributes the waiting to the queue.

## 3. Prefill / decode trace

The same script records prefill_start/end and several decode steps. The trace
timeline and the decomposition show prefill (24%), first-token, and decode
(6.4%) intervals.

## 4. KV hit vs miss

`basic.jsonl` emits `KV_HIT count=8` (attributed as kv_hits=8);
`cold_warm.jsonl` emits `KV_MISS`. `obscli latency` and `obscli trace` show the
reuse evidence. The reconstruction distinguishes a hit (reused) request from a
miss (cold) request.

## 5. Retry trace

`examples/retry.jsonl` records a `FAILURE` and a `RETRY` (reason=preemption)
then a successful `COMPLETION`. The explanation flags both as directly
evidenced and shows the retry/recovery in the causal statement set.

    obscli ingest --script examples/retry.jsonl --out examples/retry.sobs --epoch 1
    obscli explain --archive examples/retry.sobs --request bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1

## 6. Worker restart / source-authority transition

`obscli multiprocess --dir proof_run` runs a real coordinator plus two source
OS processes over framed TCP, kills one as an OS process, restarts it with a
fresh WorkerBootId, replays stale epoch/boot/source-generation/observation-
generation mutations over TCP, and proves every stale current-state mutation
is rejected while historical evidence stays queryable and a trace spanning two
source boots reconstructs correctly.

## 7. Cold vs warm serving

`cold_warm.jsonl` records a cold model residency (`is_warm=false`) and a KV
miss; the tail explanation reports a cold_model directly-evidenced factor and
a dominant prefill phase.

## 8. Tail-latency explanation

    obscli tail --archive examples/cold.sobs --pct 90

prints the percentile metrics and, for each tail request, the factors that
dominated (phase, queue, cold model, etc.) with their causal class and
provenance.

## 9. Trace comparison

    obscli compare --archive examples/basic.sobs --a aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1 --b aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1

(compare a request to itself to see the schema; use `--a`/`--b` with two
requests in one archive to see phase/retry/KV deltas.)

## 10. Counterfactual

    obscli counterfactual --archive examples/basic.sobs --request aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1 --rule remove_queue_wait

reports the changed inputs and the derived delta in total latency, labelled
derived (never claiming the counterfactual happened).

## 11. Persistence / replay / recovery

    obscli replay  --archive examples/basic.sobs
    obscli recover --archive examples/basic.sobs

`reload` in a fresh process re-reads the checksummed archive and re-runs the
deterministic replay, confirming the evidence/trace/explanation/aggregate
digests match the stored ones.

## 12. CUDA-backed serving trace (RTX 5090 / sm_120)

`sobs_cuda_proof` and `obscli cuda` run real device allocation, H2D, a
prefill-like GEMM, a decode-like elementwise kernel, D2H, and CPU reference
verification, then emit typed observations and reconstruct cold vs warm
prefill, decode, a queue-delayed request, and a fail-then-retry request from
evidence. The queue delay and the deliberately-corrupted failing run are
labelled synthetic.
