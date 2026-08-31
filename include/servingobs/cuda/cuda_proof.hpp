#pragma once
// Serving Observatory — real RTX 5090 / sm_120 CUDA serving-evidence proof.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// Executes real device memory allocation, H2D transfer, a prefill-like CUDA GEMM
// kernel, a decode-like elementwise kernel, a D2H copy, and CPU reference
// verification. It emits typed observations carrying MEASURED durations and byte
// counts so the Observatory can reconstruct a request trace and the cold vs warm
// / queue-delay / retry differences directly from evidence. Synthetic-only
// aspects (a deliberately injected queue wait and a deliberately failing run)
// are labelled synthetic.

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"
#include "servingobs/model/observation.hpp"

#include <string>
#include <vector>

namespace servingobs {

struct CudaKpiRun {
    string name;
    double h2d_ms = 0.0, kernel_ms = 0.0, d2h_ms = 0.0;
    u64 bytes_h2d = 0, bytes_d2h = 0;
    bool success = false;
};

struct CudaScenario {
    string device_name;
    int compute_major = 0, compute_minor = 0;
    u64 total_mem = 0;
    double cold_prefill_ms = 0.0;
    double warm_prefill_ms = 0.0;
    double decode_ms = 0.0;
    double queue_delay_ms = 0.0;
    u64 prefill_h2d = 0, prefill_d2h = 0;
    std::vector<CudaKpiRun> runs;
    // Serving-like observations produced from real measured events.
    std::vector<Observation> evidence;
    // Marker: which scenario elements are synthetic.
    std::vector<string> synthetic_notes;

    Json kpi_json() const;
};

// Run the CUDA proof. Throws std::runtime_error on a CUDA failure.
CudaScenario run_cuda_proof();

} // namespace servingobs
