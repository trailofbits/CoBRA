#pragma once

#include <cstdint>
#include <functional>
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

    // Enhanced overload: after boolean spurious detection, probes the
    // evaluator at random full-width inputs to catch variables that are
    // spurious on {0,1} but live at full width (e.g., x*y vs x&y).
    // The evaluator must operate in the original variable space.
    EliminationResult EliminateAuxVars(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        const std::function< uint64_t(const std::vector< uint64_t > &) > &eval,
        uint32_t bitwidth
    );

} // namespace cobra
