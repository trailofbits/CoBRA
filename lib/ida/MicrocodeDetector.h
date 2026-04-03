#pragma once

// STL and absl must be included before hexrays.hpp: the IDA SDK poisons
// stdout, stderr, fwrite, fflush, snprintf etc. via fpro.h macros, which
// breaks any subsequent libc++/absl header that references those identifiers.
#include <absl/container/flat_hash_map.h>

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
    // expression (at least 1 boolean and 1 arithmetic opcode, no extensions
    // at root, destination fits in 64 bits).
    bool IsMba(const minsn_t &insn);

    // Evaluate a minsn tree with the given variable assignments.
    // Used for signature computation (DetectMbaCandidates) and
    // verification (ProbablyEquivalent).
    uint64_t EvalMinsn(
        const minsn_t &insn, const absl::flat_hash_map< const mop_t *, uint64_t > &var_values,
        uint64_t mask
    );

    // Walk all top-level instructions in `mba`, detect MBA trees, compute
    // boolean signatures, and return candidates ready for simplification.
    std::vector< MBACandidate > DetectMbaCandidates(mba_t &mba);

} // namespace ida_cobra
