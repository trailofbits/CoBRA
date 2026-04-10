#pragma once

// Opt-in counters for signature evaluation.
//
// When COBRA_SIG_STATS is defined at compile time, every call to
// EvaluateBooleanSignature increments thread-local counters.  The
// stats can be queried/reset via the free functions below.
//
// When COBRA_SIG_STATS is NOT defined, all functions are no-ops and
// the struct is empty — zero overhead.

#include <cstdint>
#include <ostream>

namespace cobra {

    struct SigEvalStats
    {
        uint64_t calls        = 0; // total calls
        uint64_t expr_calls   = 0; // calls via Expr overload (tree-walk)
        uint64_t eval_calls   = 0; // calls via Evaluator overload
        uint64_t total_points = 0; // sum of 2^n across all calls
        uint64_t total_nodes  = 0; // sum of AST node count (Expr only)
        double total_us       = 0; // wall-clock microseconds
    };

#ifdef COBRA_SIG_STATS

    // Increment counters (called internally by SignatureEval.cpp).
    void SigStatsRecordExpr(uint32_t num_vars, uint32_t node_count, double elapsed_us);
    void SigStatsRecordEval(uint32_t num_vars, double elapsed_us);

    // Read and reset.
    SigEvalStats SigStatsSnapshot();
    void SigStatsReset();

#else

    inline void SigStatsRecordExpr(uint32_t, uint32_t, double) {}

    inline void SigStatsRecordEval(uint32_t, double) {}

    inline SigEvalStats SigStatsSnapshot() { return {}; }

    inline void SigStatsReset() {}

#endif

    // Pretty-print a snapshot.
    inline std::ostream &operator<<(std::ostream &os, const SigEvalStats &s) {
        os << "sig_eval: calls=" << s.calls << " (expr=" << s.expr_calls
           << " eval=" << s.eval_calls << ")"
           << " points=" << s.total_points << " nodes=" << s.total_nodes
           << " time=" << s.total_us << "us";
        return os;
    }

} // namespace cobra
