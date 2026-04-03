#pragma once

// STL and absl must be included before hexrays.hpp: the IDA SDK poisons
// stdout, stderr, fwrite, fflush, snprintf etc. via fpro.h macros, which
// breaks any subsequent libc++/absl header that references those identifiers.
#include <absl/container/flat_hash_set.h>

#include <cstdint>
#include <string>
#include <vector>

#include <hexrays.hpp>

namespace ida_cobra {

    struct MBACandidate
    {
        minsn_t *root = nullptr;
        std::vector< mop_t * > leaves;
        std::vector< std::string > var_names;
        std::vector< uint64_t > sig;
        uint32_t bitwidth = 64;
    };

    // Returns true if the instruction tree rooted at `insn` is an MBA
    // expression (at least 1 boolean and 1 arithmetic opcode, destination
    // fits in 64 bits).
    bool IsMba(const minsn_t &insn);

    // Evaluate a minsn tree with the given variable assignments.
    // Variables are matched by value equality (mop_t::operator==),
    // not pointer identity.
    uint64_t EvalMinsn(
        const minsn_t &insn,
        const std::vector< mop_t * > &var_keys,
        const std::vector< uint64_t > &var_vals,
        uint64_t mask
    );

    // Walk all top-level instructions in `mba`, detect MBA trees, compute
    // boolean signatures, and return candidates ready for simplification.
    std::vector< MBACandidate > DetectMbaCandidates(mba_t &mba);

    // Enhanced detection that follows use-def chains across block boundaries.
    // Falls back to Tier 1 (intra-block) if cross-block tracing is not viable.
    std::vector< MBACandidate > DetectMbaCandidatesCrossBlock(mba_t &mba);

} // namespace ida_cobra
