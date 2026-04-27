// CLBlast-style tuner database row layout.
//
// Phase 1 keeps Params lean — only the params our actual cfgs vary
// (Q_BLOCK / TILE_M / N_HW_THREADS). Plan §96 has a richer Params with
// VTCM byte budgets, DMA double-buffering, prefetch depth, etc.; those
// land here as Phase-2 kernel features come online.

#ifndef QBLAST_DATABASE_STRUCTURE_HPP
#define QBLAST_DATABASE_STRUCTURE_HPP

#include <cstddef>
#include <cstdint>

namespace qblast {

struct VariantParams {
    uint16_t q_block;        // quantization block size along K
    uint16_t tile_m;         // rows produced per outer-loop step
    uint16_t n_hw_threads;   // HVX-capable QuRT threads
    // Phase-2: vtcm_a_bytes, vtcm_x_bytes, dma_double_buf, prefetch_depth, acc_precision
};

struct DatabaseEntry {
    const char*    shape_key;        // "M_K_q" string, e.g. "4096_4096_64"
    int            cfg_id;           // matches variant_builder.py cfg_id ⇒ libgemv_*_v{N}_skel.so
    VariantParams  params;
    uint64_t       dsp_cycles_med;   // measured benchmark, for telemetry / regression detection
};

}  // namespace qblast

#endif  // QBLAST_DATABASE_STRUCTURE_HPP
