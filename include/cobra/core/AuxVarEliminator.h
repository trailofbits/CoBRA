#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cobra {

    struct EliminationResult
    {
        std::vector< uint64_t > reduced_sig;
        std::vector< std::string > real_vars;
        std::vector< std::string > spurious_vars;
    };

    // Detect and remove spurious variables from a signature vector.
    // A variable is spurious iff toggling it never changes the paired
    // signature entries across all assignments of remaining variables.
    EliminationResult EliminateAuxVars(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars
    );

} // namespace cobra
