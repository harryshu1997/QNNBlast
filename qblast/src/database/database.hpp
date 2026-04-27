// qblast tuner database lookup.
//
// Per plan §117-119: runtime queries `(soc_id, kernel_name, shape_key)`
// against the offline-tuned database; miss → fall back to the kernel's
// `default` bucket.
//
// Phase-1 scope: single SoC ("sd8e_gen5") and single kernel
// ("gemv_w4a16"). Adding more is a matter of including more generated
// headers below and routing in lookup_variant().

#ifndef QBLAST_DATABASE_HPP
#define QBLAST_DATABASE_HPP

#include "database_structure.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "kernels/gemv_w4a16/sd8e_gen5.hpp"

namespace qblast {

// shape_key format must match what variant_builder.py / leaderboard.json use:
// "<M>_<K>_<q_block>" decimal with single-underscore separators, e.g.
// "4096_4096_64". format_shape_key() builds it from numeric inputs to keep
// callers from re-implementing it inconsistently.
inline std::string format_shape_key(unsigned int M,
                                    unsigned int K,
                                    unsigned int q_block) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u_%u_%u", M, K, q_block);
    return std::string(buf);
}

// Returns a pointer to the matching DatabaseEntry, or to the kernel's
// `default` entry if the (kernel, soc, shape) triple isn't in the database.
// Never returns null.
inline const DatabaseEntry* lookup_variant(const char* kernel,
                                            const char* soc_id,
                                            const char* shape_key) {
    if (kernel == nullptr || soc_id == nullptr || shape_key == nullptr) {
        return &gemv_w4a16::sd8e_gen5::kDefault;
    }

    if (std::strcmp(kernel, "gemv_w4a16") == 0
        && std::strcmp(soc_id, "sd8e_gen5") == 0) {
        for (std::size_t i = 0; i < gemv_w4a16::sd8e_gen5::kNumEntries; ++i) {
            const DatabaseEntry& e = gemv_w4a16::sd8e_gen5::kEntries[i];
            if (std::strcmp(e.shape_key, shape_key) == 0) {
                return &e;
            }
        }
        return &gemv_w4a16::sd8e_gen5::kDefault;
    }

    // Unknown (kernel, soc) — eventually we'd add more #include + branches.
    return &gemv_w4a16::sd8e_gen5::kDefault;
}

}  // namespace qblast

#endif  // QBLAST_DATABASE_HPP
