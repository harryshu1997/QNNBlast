// Sanity tests for the qblast database layer.
//
// Builds + runs on the host (x86_64 Linux), no Hexagon toolchain needed.
// Verifies:
//   * exact-match lookup for every (M, K, q_block) shape we measured
//   * default-bucket fallback for shapes not in the database
//   * unknown kernel / soc routes to default

#include "database.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void expect(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        g_failures += 1;
    } else {
        std::fprintf(stdout, "  ok: %s\n", msg);
    }
}

void check_shape(unsigned int M, unsigned int K, unsigned int q,
                 int expected_cfg_id, uint16_t expected_q_block) {
    auto key = qblast::format_shape_key(M, K, q);
    const auto* entry = qblast::lookup_variant("gemv_w4a16", "sd8e_gen5", key.c_str());
    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "%s -> cfg_id=%d Q_BLOCK=%u  (expected cfg=%d Q=%u)",
                  key.c_str(), entry->cfg_id, entry->params.q_block,
                  expected_cfg_id, expected_q_block);
    expect(entry->cfg_id == expected_cfg_id
           && entry->params.q_block == expected_q_block, msg);
}

}  // namespace

int main() {
    // Each (shape, expected winner) per the Day-5 LLaMA leaderboard.
    // cfg 0 = Q=64 variant, cfg 1 = Q=128 variant.
    std::printf("=== exact-match lookups (10 measured shapes) ===\n");

    // Q/K/V/O projection
    check_shape(4096, 4096, 64,  0, 64);
    check_shape(4096, 4096, 128, 1, 128);

    // FFN up 7B
    check_shape(11008, 4096, 64,  0, 64);
    check_shape(11008, 4096, 128, 1, 128);

    // FFN down 7B
    check_shape(4096, 11008, 64,  0, 64);
    check_shape(4096, 11008, 128, 1, 128);

    // FFN up 13B
    check_shape(14336, 4096, 64,  0, 64);
    check_shape(14336, 4096, 128, 1, 128);

    // LM head — note the LM-head q=64 shape stays on Q=64 (Day-5 surprise:
    // Q=64 beats Q=128 here despite Q=128 winning everywhere else).
    check_shape(32000, 4096, 64,  0, 64);
    check_shape(32000, 4096, 128, 1, 128);

    std::printf("\n=== fallback / default-bucket cases ===\n");

    // Shape not in database — should fall back to default (cfg=0, Q=64).
    auto* unknown = qblast::lookup_variant("gemv_w4a16", "sd8e_gen5", "999_999_64");
    expect(std::strcmp(unknown->shape_key, "default") == 0,
           "unknown shape -> default entry");
    expect(unknown->cfg_id == 0 && unknown->params.q_block == 64,
           "default has cfg=0, Q=64");

    // Unknown kernel -> default.
    auto* bad_kernel = qblast::lookup_variant("nonexistent", "sd8e_gen5", "4096_4096_64");
    expect(std::strcmp(bad_kernel->shape_key, "default") == 0,
           "unknown kernel -> default entry");

    // Unknown SoC -> default.
    auto* bad_soc = qblast::lookup_variant("gemv_w4a16", "alien_chip", "4096_4096_64");
    expect(std::strcmp(bad_soc->shape_key, "default") == 0,
           "unknown soc -> default entry");

    // null inputs -> default (no crash).
    auto* nulls = qblast::lookup_variant(nullptr, nullptr, nullptr);
    expect(nulls != nullptr && std::strcmp(nulls->shape_key, "default") == 0,
           "all-null inputs -> default entry, no crash");

    std::printf("\n=== database contents (for reference) ===\n");
    for (std::size_t i = 0; i < qblast::gemv_w4a16::sd8e_gen5::kNumEntries; ++i) {
        const auto& e = qblast::gemv_w4a16::sd8e_gen5::kEntries[i];
        std::printf("  %-22s cfg=%d Q=%u T=%u N=%u  cycles_med=%llu\n",
                    e.shape_key, e.cfg_id, e.params.q_block,
                    e.params.tile_m, e.params.n_hw_threads,
                    (unsigned long long)e.dsp_cycles_med);
    }

    std::printf("\n=== summary ===\n");
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURES\n", g_failures);
    return 1;
}
