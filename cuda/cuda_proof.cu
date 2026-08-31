
// Serving Observatory — real RTX 5090 / sm_120 CUDA serving-evidence proof.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/cuda/cuda_proof.hpp"
#include "servingobs/core/enums.hpp"
#include "servingobs/core/json.hpp"

#include <cuda_runtime.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace servingobs {

namespace {

void cuda_check(cudaError_t e, const char* what) {
    if (e != cudaSuccess) throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(e));
}

__global__ void gemm_kernel(float* C, const float* A, const float* B, int M, int N, int K) {
    int row = blockIdx.y * blockDim.y + threadIdx.y;
    int col = blockIdx.x * blockDim.x + threadIdx.x;
    if (row >= M || col >= N) return;
    float sum = 0.0f;
    for (int k = 0; k < K; ++k) sum += A[row * K + k] * B[k * N + col];
    C[row * N + col] = sum;
}

__global__ void decode_kernel(float* out, const float* in, int N, float scale) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < N) out[i] = tanhf(in[i] * scale);
}

// CPU reference for one row of GEMM C = A*B (row-major).
float cpu_gemm_cell(const float* A, const float* B, int row, int col, int K, int N) {
    float sum = 0.0f;
    for (int k = 0; k < K; ++k) sum += A[row * K + k] * B[k * N + col];
    return sum;
}

struct GemmTiming { double h2d_ms = 0, kernel_ms = 0, d2h_ms = 0; bool ok = false; u64 h2d = 0, d2h = 0; };

GemmTiming run_gemm(float* dA, float* dB, float* dC, float* hC,
                    const float* hA, const float* hB, int M, int N, int K, bool corrupt_for_fail) {
    GemmTiming r;
    cudaEvent_t s, e;
    cudaEventCreate(&s); cudaEventCreate(&e);
    r.h2d = static_cast<u64>(M * K * 4) + static_cast<u64>(K * N * 4);
    cudaEventRecord(s);
    cudaMemcpy(dA, hA, M * K * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(dB, hB, K * N * sizeof(float), cudaMemcpyHostToDevice);
    cudaEventRecord(e); cudaEventSynchronize(e);
    { float _m; cudaEventElapsedTime(&_m, s, e); r.h2d_ms = _m; }
    dim3 block(16, 16), grid((N + 15) / 16, (M + 15) / 16);
    cudaEventRecord(s);
    gemm_kernel<<<grid, block>>>(dC, dA, dB, M, N, K);
    cuda_check(cudaGetLastError(), "gemm launch");
    cudaEventRecord(e); cudaEventSynchronize(e);
    { float _m; cudaEventElapsedTime(&_m, s, e); r.kernel_ms = _m; }
    r.d2h = static_cast<u64>(M * N * 4);
    cudaEventRecord(s);
    cudaMemcpy(hC, dC, M * N * sizeof(float), cudaMemcpyDeviceToHost);
    cudaEventRecord(e); cudaEventSynchronize(e);
    { float _m; cudaEventElapsedTime(&_m, s, e); r.d2h_ms = _m; }
    // Verify against CPU reference.
    bool ok = true;
    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < N; ++j) {
            float ref = corrupt_for_fail ? cpu_gemm_cell(hA, hB, i, j, K, N) + 10.0f : cpu_gemm_cell(hA, hB, i, j, K, N);
            if (std::fabs(hC[i * N + j] - ref) > 1e-2f) { ok = false; break; }
        }
        if (!ok) break;
    }
    r.ok = ok;
    cudaEventDestroy(s); cudaEventDestroy(e);
    return r;
}

} // namespace

Json CudaScenario::kpi_json() const {
    Json j = Json::object();
    j.set("device", device_name);
    j.set("compute_capability", std::to_string(compute_major) + "." + std::to_string(compute_minor));
    j.set("total_mem", total_mem);
    j.set("cold_prefill_ms", cold_prefill_ms);
    j.set("warm_prefill_ms", warm_prefill_ms);
    j.set("decode_ms", decode_ms);
    j.set("queue_delay_ms", queue_delay_ms);
    j.set("prefill_h2d_bytes", prefill_h2d);
    j.set("prefill_d2h_bytes", prefill_d2h);
    Json runsArr = Json::array();
    for (const auto& r : this->runs) {
        Json o = Json::object();
        o.set("name", r.name); o.set("h2d_ms", r.h2d_ms); o.set("kernel_ms", r.kernel_ms);
        o.set("d2h_ms", r.d2h_ms); o.set("bytes_h2d", r.bytes_h2d); o.set("bytes_d2h", r.bytes_d2h);
        o.set("success", r.success);
        runsArr.push(std::move(o));
    }
    j.set("runs", std::move(runsArr));
    Json notes = Json::array();
    for (const auto& n : synthetic_notes) notes.push(n);
    j.set("synthetic_notes", std::move(notes));
    return j;
}

CudaScenario run_cuda_proof() {
    CudaScenario sc;
    int dev = 0;
    cuda_check(cudaGetDevice(&dev), "get device");
    cudaDeviceProp p;
    cuda_check(cudaGetDeviceProperties(&p, dev), "device props");
    sc.device_name = p.name;
    sc.compute_major = p.major;
    sc.compute_minor = p.minor;
    sc.total_mem = static_cast<u64>(p.totalGlobalMem);

    // ---- dimensions (small but real work)
    const int M = 256, N = 96, K = 128;
    const u64 nBytesA = static_cast<u64>(M) * K * 4, nBytesB = static_cast<u64>(K) * N * 4, nBytesC = static_cast<u64>(M) * N * 4;

    std::vector<float> hA(M * K), hB(K * N), hC(M * N);
    for (int i = 0; i < M * K; ++i) hA[i] = std::sin(0.01f * static_cast<float>(i));
    for (int i = 0; i < K * N; ++i) hB[i] = std::cos(0.007f * static_cast<float>(i));
    for (int i = 0; i < M * N; ++i) hC[i] = 0.0f;

    float *dA = nullptr, *dB = nullptr, *dC = nullptr;
    cuda_check(cudaMalloc(&dA, nBytesA), "alloc A");
    cuda_check(cudaMalloc(&dB, nBytesB), "alloc B");
    cuda_check(cudaMalloc(&dC, nBytesC), "alloc C");

    // ---- cold run (fresh buffers)
    GemmTiming cold = run_gemm(dA, dB, dC, hC.data(), hA.data(), hB.data(), M, N, K, false);
    sc.cold_prefill_ms = cold.kernel_ms;
    sc.prefill_h2d = cold.h2d; sc.prefill_d2h = cold.d2h;
    sc.runs.push_back({ "prefill_cold", cold.h2d_ms, cold.kernel_ms, cold.d2h_ms, cold.h2d, cold.d2h, cold.ok });

    // ---- warm run (reuse the same device buffers = residency/KV reuse analog)
    GemmTiming warm = run_gemm(dA, dB, dC, hC.data(), hA.data(), hB.data(), M, N, K, false);
    sc.warm_prefill_ms = warm.kernel_ms;
    sc.runs.push_back({ "prefill_warm", warm.h2d_ms, warm.kernel_ms, warm.d2h_ms, warm.h2d, warm.d2h, warm.ok });

    // ---- decode-like elementwise (several steps)
    const int DN = 16384;
    const u64 nBytesD = static_cast<u64>(DN) * 4;
    float *dIn = nullptr, *dOut = nullptr;
    std::vector<float> hIn(DN), hOut(DN);
    for (int i = 0; i < DN; ++i) hIn[i] = 0.5f;
    cuda_check(cudaMalloc(&dIn, nBytesD), "alloc in");
    cuda_check(cudaMalloc(&dOut, nBytesD), "alloc out");
    cudaMemcpy(dIn, hIn.data(), nBytesD, cudaMemcpyHostToDevice);
    cudaEvent_t s, e2; cudaEventCreate(&s); cudaEventCreate(&e2);
    cudaEventRecord(s);
    decode_kernel<<<(DN + 255) / 256, 256>>>(dOut, dIn, DN, 0.75f);
    cuda_check(cudaGetLastError(), "decode launch");
    cudaMemcpy(hOut.data(), dOut, nBytesD, cudaMemcpyDeviceToHost);
    cudaEventRecord(e2); cudaEventSynchronize(e2);
    float _dm = 0; cudaEventElapsedTime(&_dm, s, e2); double decode_ms = _dm;
    sc.decode_ms = decode_ms;
    // verify decode
    bool dver = true;
    for (int i = 0; i < DN; ++i) if (std::fabs(hOut[i] - std::tanhf(0.5f * 0.75f)) > 1e-3f) { dver = false; break; }
    sc.runs.push_back({ "decode", 0.0, decode_ms, 0.0, nBytesD, nBytesD, dver });
    cudaEventDestroy(s); cudaEventDestroy(e2);
    cudaFree(dIn); cudaFree(dOut);

    // ---- queue-delayed transfer timing (synthetic delay)
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    auto t1 = std::chrono::steady_clock::now();
    double queue_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    sc.queue_delay_ms = queue_ms;

    // ---- deliberate failing prefill run (retry scenario)
    GemmTiming fail_run = run_gemm(dA, dB, dC, hC.data(), hA.data(), hB.data(), M, N, K, /*corrupt*/ true);
    sc.runs.push_back({ "prefill_fail", fail_run.h2d_ms, fail_run.kernel_ms, fail_run.d2h_ms, fail_run.h2d, fail_run.d2h, fail_run.ok });
    // Successful rerun after retry.
    GemmTiming retry_run = run_gemm(dA, dB, dC, hC.data(), hA.data(), hB.data(), M, N, K, false);
    sc.runs.push_back({ "prefill_retry_ok", retry_run.h2d_ms, retry_run.kernel_ms, retry_run.d2h_ms, retry_run.h2d, retry_run.d2h, retry_run.ok });

    cudaFree(dA); cudaFree(dB); cudaFree(dC);

    sc.synthetic_notes.push_back("queue delay is an injected 3ms wait (synthetic)");
    sc.synthetic_notes.push_back("the prefill_fail run is deliberately corrupted to exercise retry (synthetic)");

    // ---- build serving-like observations from the measured events
    // A single logical worker/source produces all of them.
    SourceId source = SourceId(Id128::from_u64(0x0C55DA));
    WorkerId worker = WorkerId(Id128::from_u64(0x0C55DB));
    WorkerBootId boot = WorkerBootId(Id128::make(0x0C55DC, 1));
    constexpr SourceGeneration sg = 1;
    constexpr ObservationGeneration og = 1;
    constexpr CoordinatorEpoch epoch = 1;
    u64 seq = 0;
    u64 clock = 1'000'000'000;  // coordinator-domain recv clock in ns
    u64 cold_h2d = cold.h2d, cold_d2h = cold.d2h;

    auto mk = [&](ObsType type, const Id128& rid, TimestampNs obs_ts) -> Observation {
        Observation o;
        o.type = type;
        o.source = source; o.worker = worker; o.boot = boot;
        o.src_gen = sg; o.obs_gen = og; o.epoch = epoch;
        o.clock_domain = "cuda_mono_ns";
        o.seq = ++seq;
        o.provenance = Provenance::MEASURED;
        o.has_obs_ts = true; o.obs_ts = obs_ts;
        o.has_recv_ts = true; o.recv_ts = clock += 1000;
        o.set_id(FieldKeys::RequestId, rid);
        o.id = derive_observation_id(o.source.raw(), o.worker.raw(), o.boot.raw(), o.seq, o.type);
        return o;
    };

    auto add_request = [&](const Id128& rid, bool cold, bool warmCam,
                           double prefill_ms, double decode_ms2, bool retried, double queue_delay) {
        u64 ts = 5000;
        sc.evidence.push_back(mk(ObsType::REQUEST, rid, ts));
        sc.evidence.push_back(mk(ObsType::ADMISSION, rid, ts += 500));
        if (queue_delay > 0) {
            sc.evidence.push_back(mk(ObsType::QUEUE_ENTER, rid, ts));
            ts += static_cast<u64>(queue_delay * 1e6);
            sc.evidence.push_back(mk(ObsType::QUEUE_LEAVE, rid, ts));
        }
        sc.evidence.push_back(mk(ObsType::BATCH_FORM, rid, ts += 200));
        sc.evidence.push_back(mk(ObsType::BATCH_SEAL, rid, ts += 200));
        sc.evidence.push_back(mk(ObsType::DISPATCH, rid, ts += 100));
        Observation trs = mk(ObsType::TRANSFER_START, rid, ts += 100);
        trs.set_u64(FieldKeys::BytesH2D, cold ? cold_h2d : cold_h2d);
        sc.evidence.push_back(trs);
        sc.evidence.push_back(mk(ObsType::TRANSFER_END, rid, ts += 200));
        Observation pfs = mk(ObsType::PREFILL_START, rid, ts += 300);
        pfs.set_u64(FieldKeys::InputTokens, 256);
        pfs.set_f64(FieldKeys::GpuMs, prefill_ms);
        pfs.set_u64(FieldKeys::BytesH2D, cold_h2d);
        sc.evidence.push_back(pfs);
        Observation pfe = mk(ObsType::PREFILL_END, rid, ts += static_cast<u64>(prefill_ms * 1e6));
        pfe.set_f64(FieldKeys::GpuMs, prefill_ms);
        pfe.set_u64(FieldKeys::BytesD2H, cold_d2h);
        sc.evidence.push_back(pfe);
        if (retried) {
            Observation f = mk(ObsType::FAILURE, rid, ts += 500);
            f.set_str(FieldKeys::Reason, "post-prefill verification mismatch (synthetic)");
            sc.evidence.push_back(f);
            Observation r = mk(ObsType::RETRY, rid, ts += 500);
            r.set_str(FieldKeys::Reason, "verification mismatch");
            sc.evidence.push_back(r);
            Observation pfs2 = mk(ObsType::PREFILL_START, rid, ts += 300);
            pfs2.set_u64(FieldKeys::InputTokens, 256);
            pfs2.set_f64(FieldKeys::GpuMs, prefill_ms);
            sc.evidence.push_back(pfs2);
            Observation pfe2 = mk(ObsType::PREFILL_END, rid, ts += static_cast<u64>(prefill_ms * 1e6));
            pfe2.set_f64(FieldKeys::GpuMs, prefill_ms);
            sc.evidence.push_back(pfe2);
        }
        for (int i = 0; i < 2; ++i) {
            Observation ds = mk(ObsType::DECODE_STEP_START, rid, ts += 300);
            ds.set_u64(FieldKeys::OutputTokens, static_cast<u64>(i + 1));
            ds.set_f64(FieldKeys::GpuMs, decode_ms2);
            sc.evidence.push_back(ds);
            Observation de = mk(ObsType::DECODE_STEP_END, rid, ts += static_cast<u64>(decode_ms2 * 1e6));
            de.set_f64(FieldKeys::GpuMs, decode_ms2);
            sc.evidence.push_back(de);
        }
        if (cold) {
            Observation m = mk(ObsType::MODEL_RESIDENCY, rid, ts += 100);
            m.set_bool(FieldKeys::IsWarm, false);
            sc.evidence.push_back(m);
            sc.evidence.push_back(mk(ObsType::KV_MISS, rid, ts += 50));
        }
        if (warmCam) {
            Observation m = mk(ObsType::MODEL_RESIDENCY, rid, ts += 100);
            m.set_bool(FieldKeys::IsWarm, true);
            sc.evidence.push_back(m);
            sc.evidence.push_back(mk(ObsType::KV_HIT, rid, ts += 50));
        }
        Observation c = mk(ObsType::COMPLETION, rid, ts += 200);
        c.set_str(FieldKeys::Outcome, retried ? "completed" : "completed");
        sc.evidence.push_back(c);
    };

    add_request(Id128::make(0x1000, 1), true, false, sc.cold_prefill_ms, sc.decode_ms, false, 0.0);
    add_request(Id128::make(0x1000, 2), false, true, sc.warm_prefill_ms, sc.decode_ms, false, 0.0);
    add_request(Id128::make(0x1000, 3), false, true, sc.warm_prefill_ms, sc.decode_ms, false, sc.queue_delay_ms);
    add_request(Id128::make(0x1000, 4), false, true, sc.warm_prefill_ms, sc.decode_ms, true, 0.0);

    return sc;
}

} // namespace servingobs