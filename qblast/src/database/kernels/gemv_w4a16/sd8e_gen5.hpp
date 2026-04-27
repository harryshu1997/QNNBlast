// AUTO-GENERATED from leaderboard.json — do not edit.
// Regenerate with: scripts/database/database.py

#ifndef QBLAST_DB_GEMV_W4A16_SD8E_GEN5_HPP
#define QBLAST_DB_GEMV_W4A16_SD8E_GEN5_HPP

#include "../../database_structure.hpp"

namespace qblast { namespace gemv_w4a16 { namespace sd8e_gen5 {

constexpr DatabaseEntry kEntries[] = {
    { "11008_4096_128", 1, { 128, 1, 1 }, 161596634ULL },
    { "11008_4096_64", 0, { 64, 1, 1 }, 171747142ULL },
    { "14336_4096_128", 1, { 128, 1, 1 }, 209212089ULL },
    { "14336_4096_64", 0, { 64, 1, 1 }, 227028858ULL },
    { "32000_4096_128", 1, { 128, 1, 1 }, 543423642ULL },
    { "32000_4096_64", 0, { 64, 1, 1 }, 401994737ULL },
    { "4096_11008_128", 1, { 128, 1, 1 }, 116200028ULL },
    { "4096_11008_64", 0, { 64, 1, 1 }, 172453975ULL },
    { "4096_4096_128", 1, { 128, 1, 1 }, 59007146ULL },
    { "4096_4096_64", 0, { 64, 1, 1 }, 67167606ULL },
};

constexpr std::size_t kNumEntries = sizeof(kEntries) / sizeof(kEntries[0]);

// Fallback cfg used when a (M, K, q_block) shape isn't in kEntries.
// Plan §289 says default = baseline Q=64 / TILE_M=1 / single-thread.
constexpr DatabaseEntry kDefault = { "default", 0, { 64, 1, 1 }, 0ULL };

} } }  // namespace qblast::gemv_w4a16::sd8e_gen5

#endif  // QBLAST_DB_GEMV_W4A16_SD8E_GEN5_HPP
