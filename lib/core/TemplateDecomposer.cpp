#include "cobra/core/TemplateDecomposer.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        constexpr uint32_t kNProbes = 16;
        constexpr uint32_t kMaxVars = 6;

        using ProbeVals = std::array< uint64_t, kNProbes >;

        enum class Gate { kAnd, kOr, kXor, kAdd, kMul };
        constexpr std::array kAllGates = { Gate::kAnd, Gate::kOr, Gate::kXor, Gate::kAdd,
                                           Gate::kMul };

        bool GateInvertible(Gate g) { return g == Gate::kXor || g == Gate::kAdd; }

        struct ValHash
        {
            size_t operator()(const ProbeVals &v) const {
                size_t h = kNProbes;
                for (auto x : v) {
                    h ^= std::hash< uint64_t >{}(x) + 0x9e3779b97f4a7c15ULL + (h << 6)
                        + (h >> 2);
                }
                return h;
            }
        };

        using ValMap = std::unordered_map< ProbeVals, size_t, ValHash >;

        struct Atom
        {
            std::unique_ptr< Expr > expr;
            ProbeVals vals;
        };

        // Evaluate expr at all probe points.
        ProbeVals Probe(
            const Expr &e, const std::vector< std::vector< uint64_t > > &pts, uint32_t bw
        ) { // NOLINT(hicpp-named-parameter,readability-named-parameter)
            ProbeVals v;
            for (size_t i = 0; i < kNProbes; ++i) { v[i] = EvalExpr(e, pts[i], bw); }
            return v;
        }

        // Add atom with deduplication; keep the cheapest expression.
        // NOLINTNEXTLINE(readability-identifier-naming)
        void Push(
            std::vector< Atom > &pool, ValMap &idx, std::unique_ptr< Expr > e, ProbeVals vals
        ) {
            auto it = idx.find(vals);
            if (it != idx.end()) {
                if (IsBetter(ComputeCost(*e).cost, ComputeCost(*pool[it->second].expr).cost)) {
                    pool[it->second].expr = std::move(e);
                }
                return;
            }
            idx[vals] = pool.size();
            pool.push_back({ .expr = std::move(e), .vals = vals });
        }

        // Build the bounded atom pool.
        // NOLINTNEXTLINE(readability-identifier-naming)
        void Populate(
            std::vector< Atom > &pool, ValMap &idx, uint32_t nv,
            const std::vector< std::vector< uint64_t > > &pts, uint32_t bw
        ) {
            const uint64_t mask = (bw >= 64) ? UINT64_MAX : ((1ULL << bw) - 1);
            auto add            = [&](std::unique_ptr< Expr > e) {
                auto v = Probe(*e, pts, bw);
                Push(pool, idx, std::move(e), v);
            };

            // Constants
            add(Expr::Constant(0));
            add(Expr::Constant(1));
            add(Expr::Constant(2));
            add(Expr::Constant(mask));

            // Variables, negations, NOT
            for (uint32_t i = 0; i < nv; ++i) {
                add(Expr::Variable(i));
                add(Expr::Negate(Expr::Variable(i)));
                add(Expr::BitwiseNot(Expr::Variable(i)));
            }

            // Pairwise commutative: &, |, ^, +, *
            for (uint32_t i = 0; i < nv; ++i) {
                for (uint32_t j = i; j < nv; ++j) {
                    auto vi = [i] { return Expr::Variable(i); };
                    auto vj = [j] { return Expr::Variable(j); };
                    add(Expr::BitwiseAnd(vi(), vj()));
                    add(Expr::BitwiseOr(vi(), vj()));
                    add(Expr::BitwiseXor(vi(), vj()));
                    add(Expr::Add(vi(), vj()));
                    add(Expr::Mul(vi(), vj()));
                }
            }

            // Subtraction (non-commutative)
            for (uint32_t i = 0; i < nv; ++i) {
                for (uint32_t j = 0; j < nv; ++j) {
                    if (i == j) { continue; }
                    add(Expr::Add(Expr::Variable(i), Expr::Negate(Expr::Variable(j))));
                }
            }

            // Neg and NOT of every atom so far
            const size_t base = pool.size();
            for (size_t k = 0; k < base; ++k) {
                add(Expr::Negate(CloneExpr(*pool[k].expr)));
                add(Expr::BitwiseNot(CloneExpr(*pool[k].expr)));
            }
        }

        // Apply gate element-wise at probe points.
        ProbeVals GateApply(const ProbeVals &a, const ProbeVals &b, Gate g, uint64_t mask) {
            ProbeVals r;
            for (size_t i = 0; i < kNProbes; ++i) {
                switch (g) {
                    case Gate::kAnd:
                        r[i] = a[i] & b[i];
                        break;
                    case Gate::kOr:
                        r[i] = a[i] | b[i];
                        break;
                    case Gate::kXor:
                        r[i] = a[i] ^ b[i];
                        break;
                    case Gate::kAdd:
                        r[i] = (a[i] + b[i]) & mask;
                        break;
                    case Gate::kMul:
                        r[i] = (a[i] * b[i]) & mask;
                        break;
                }
            }
            return r;
        }

        // Check if G(a, b) == target, short-circuiting on first mismatch.
        bool GateMatches(
            const ProbeVals &a, const ProbeVals &b, const ProbeVals &target, Gate g,
            uint64_t mask
        ) {
            for (size_t i = 0; i < kNProbes; ++i) {
                uint64_t r = 0;
                switch (g) {
                    case Gate::kAnd:
                        r = a[i] & b[i];
                        break;
                    case Gate::kOr:
                        r = a[i] | b[i];
                        break;
                    case Gate::kXor:
                        r = a[i] ^ b[i];
                        break;
                    case Gate::kAdd:
                        r = (a[i] + b[i]) & mask;
                        break;
                    case Gate::kMul:
                        r = (a[i] * b[i]) & mask;
                        break;
                    default:
                        r = 0;
                        break;
                }
                if (r != target[i]) { return false; }
            }
            return true;
        }

        // Compute residual for invertible gate: B = target G^{-1} A.
        ProbeVals
        GateResidual(const ProbeVals &target, const ProbeVals &a, Gate g, uint64_t mask) {
            ProbeVals r;
            for (size_t i = 0; i < kNProbes; ++i) {
                switch (g) {
                    case Gate::kXor:
                        r[i] = target[i] ^ a[i];
                        break;
                    case Gate::kAdd:
                        r[i] = (target[i] - a[i]) & mask;
                        break;
                    default:
                        r[i] = 0;
                        break;
                }
            }
            return r;
        }

        // Build an expression tree from a gate and two operands.
        std::unique_ptr< Expr >
        GateExpr(Gate g, std::unique_ptr< Expr > a, std::unique_ptr< Expr > b) {
            switch (g) {
                case Gate::kAnd:
                    return Expr::BitwiseAnd(std::move(a), std::move(b));
                case Gate::kOr:
                    return Expr::BitwiseOr(std::move(a), std::move(b));
                case Gate::kXor:
                    return Expr::BitwiseXor(std::move(a), std::move(b));
                case Gate::kAdd:
                    return Expr::Add(std::move(a), std::move(b));
                case Gate::kMul:
                    return Expr::Mul(std::move(a), std::move(b));
            }
            return Expr::Constant(0);
        }

        // Precomputed inner composition: G_in(atom_B, atom_C).
        struct InnerComp
        {
            Gate gate;
            size_t bi;
            size_t ci;
            ProbeVals vals;
        };

        // Try to verify and update the best result.
        // NOLINTNEXTLINE(readability-identifier-naming)
        bool TryUpdate(
            std::optional< SubResult > &best, std::unique_ptr< Expr > candidate,
            const Evaluator &eval, uint32_t num_vars, uint32_t bw, const ExprCost *baseline
        ) {
            auto chk = FullWidthCheckEval(eval, num_vars, *candidate, bw);
            if (!chk.passed) { return false; }

            auto info = ComputeCost(*candidate);
            if ((baseline != nullptr) && !IsBetter(info.cost, *baseline)) { return false; }
            if (best.has_value() && !IsBetter(info.cost, best->cost)) { return false; }

            SubResult s;
            s.expr     = std::move(candidate);
            s.cost     = info.cost;
            s.verified = true;
            best       = std::move(s);
            return true;
        }

        // Layer 1: target = G(A, B) for atoms A, B.
        std::optional< SubResult > Layer1(
            const ProbeVals &target, const std::vector< Atom > &pool, const ValMap &vmap,
            uint64_t mask, const Evaluator &eval, uint32_t nv, uint32_t bw,
            const ExprCost *baseline
        ) {
            std::optional< SubResult > best;

            for (auto g : kAllGates) {
                if (GateInvertible(g)) {
                    for (size_t ai = 0; ai < pool.size(); ++ai) {
                        auto res = GateResidual(target, pool[ai].vals, g, mask);
                        auto it  = vmap.find(res);
                        if (it == vmap.end()) { continue; }
                        auto e = GateExpr(
                            g, CloneExpr(*pool[ai].expr), CloneExpr(*pool[it->second].expr)
                        );
                        TryUpdate(best, std::move(e), eval, nv, bw, baseline);
                    }
                } else {
                    for (size_t ai = 0; ai < pool.size(); ++ai) {
                        for (size_t bi = 0; bi < pool.size(); ++bi) {
                            if (!GateMatches(pool[ai].vals, pool[bi].vals, target, g, mask)) {
                                continue;
                            }
                            auto e = GateExpr(
                                g, CloneExpr(*pool[ai].expr), CloneExpr(*pool[bi].expr)
                            );
                            TryUpdate(best, std::move(e), eval, nv, bw, baseline);
                        }
                    }
                }
                if (best.has_value()) { return best; }
            }
            return best;
        }

        // Layer 1, invertible gates only (XOR, ADD).
        // Uses hash lookup — O(pool) per gate.
        std::optional< SubResult > Layer1Fast(
            const ProbeVals &target, const std::vector< Atom > &pool, const ValMap &vmap,
            uint64_t mask, const Evaluator &eval, uint32_t nv, uint32_t bw,
            const ExprCost *baseline
        ) {
            std::optional< SubResult > best;
            for (auto g : kAllGates) {
                if (!GateInvertible(g)) { continue; }
                for (size_t ai = 0; ai < pool.size(); ++ai) {
                    auto res = GateResidual(target, pool[ai].vals, g, mask);
                    auto it  = vmap.find(res);
                    if (it == vmap.end()) { continue; }
                    auto e = GateExpr(
                        g, CloneExpr(*pool[ai].expr), CloneExpr(*pool[it->second].expr)
                    );
                    TryUpdate(best, std::move(e), eval, nv, bw, baseline);
                }
                if (best.has_value()) { return best; }
            }
            return best;
        }

        // Check if atom A is compatible as a left operand of a
        // non-invertible gate that produces the target.
        bool Compatible(const ProbeVals &a, const ProbeVals &target, Gate g) {
            for (size_t i = 0; i < kNProbes; ++i) {
                switch (g) {
                    case Gate::kOr:
                        if ((a[i] & ~target[i]) != 0) { return false; }
                        break;
                    case Gate::kAnd:
                        if ((target[i] & ~a[i]) != 0) { return false; }
                        break;
                    default:
                        break;
                }
            }
            return true;
        }

        // Layer 2: target = G_out(A, G_in(B, C)).
        std::optional< SubResult > Layer2(
            const ProbeVals &target, const std::vector< Atom > &pool, const ValMap &vmap,
            uint64_t mask, const Evaluator &eval, uint32_t nv, uint32_t bw,
            const ExprCost *baseline
        ) {
            // Precompute inner compositions not already in base pool.
            std::vector< InnerComp > inner;
            ValMap inner_idx;

            for (auto g_in : kAllGates) {
                for (size_t bi = 0; bi < pool.size(); ++bi) {
                    for (size_t ci = bi; ci < pool.size(); ++ci) {
                        auto v = GateApply(pool[bi].vals, pool[ci].vals, g_in, mask);
                        if (vmap.contains(v) != 0u) { continue; }
                        if (inner_idx.contains(v) != 0u) { continue; }
                        inner_idx[v] = inner.size();
                        inner.push_back({ .gate = g_in, .bi = bi, .ci = ci, .vals = v });
                    }
                }
            }

            std::optional< SubResult > best;

            auto make_inner = [&](const InnerComp &ic) {
                return GateExpr(
                    ic.gate, CloneExpr(*pool[ic.bi].expr), CloneExpr(*pool[ic.ci].expr)
                );
            };

            // Case A: target = G_out(base_atom, inner_comp)
            for (auto g_out : kAllGates) {
                if (GateInvertible(g_out)) {
                    for (const auto &ai : pool) {
                        auto res = GateResidual(target, ai.vals, g_out, mask);
                        auto it  = inner_idx.find(res);
                        if (it == inner_idx.end()) { continue; }
                        auto e =
                            GateExpr(g_out, CloneExpr(*ai.expr), make_inner(inner[it->second]));
                        TryUpdate(best, std::move(e), eval, nv, bw, baseline);
                    }
                } else {
                    for (const auto &ai : pool) {
                        if (!Compatible(ai.vals, target, g_out)) { continue; }
                        for (auto &ii : inner) {
                            if (!GateMatches(ai.vals, ii.vals, target, g_out, mask)) {
                                continue;
                            }
                            auto e = GateExpr(g_out, CloneExpr(*ai.expr), make_inner(ii));
                            TryUpdate(best, std::move(e), eval, nv, bw, baseline);
                            if (best.has_value()) { return best; }
                        }
                    }
                }
                if (best.has_value()) { return best; }
            }

            // Case B: target = G_out(inner_comp, inner_comp)
            //         (invertible outer only — too expensive otherwise)
            for (auto g_out : kAllGates) {
                if (!GateInvertible(g_out)) { continue; }
                for (size_t ii = 0; ii < inner.size(); ++ii) {
                    auto res = GateResidual(target, inner[ii].vals, g_out, mask);
                    auto it  = inner_idx.find(res);
                    if (it == inner_idx.end()) { continue; }
                    auto e =
                        GateExpr(g_out, make_inner(inner[ii]), make_inner(inner[it->second]));
                    TryUpdate(best, std::move(e), eval, nv, bw, baseline);
                }
                if (best.has_value()) { return best; }
            }

            return best;
        }

    } // namespace

    std::optional< SubResult > TryTemplateDecomposition(
        const SignatureContext &ctx, const Options &opts, uint32_t num_vars,
        const ExprCost *baseline_cost
    ) {
        if (!ctx.eval || num_vars > kMaxVars) { return std::nullopt; }

        // Zero real variables: the function is a constant.
        if (num_vars == 0) {
            const uint64_t kVal = (*ctx.eval)({});
            auto e              = Expr::Constant(kVal);
            auto info           = ComputeCost(*e);
            if ((baseline_cost != nullptr) && !IsBetter(info.cost, *baseline_cost)) {
                return std::nullopt;
            }
            SubResult s;
            s.expr     = std::move(e);
            s.cost     = info.cost;
            s.verified = true;
            return s;
        }

        const uint64_t kMask =
            (opts.bitwidth >= 64) ? UINT64_MAX : ((1ULL << opts.bitwidth) - 1);

        // Generate reproducible probe points.
        std::mt19937_64 rng(0xC0B4A);
        std::vector< std::vector< uint64_t > > pts(kNProbes);
        for (auto &p : pts) {
            p.resize(num_vars);
            for (auto &v : p) { v = rng() & kMask; }
        }

        // Evaluate target at probe points.
        ProbeVals target;
        for (size_t i = 0; i < kNProbes; ++i) { target[i] = (*ctx.eval)(pts[i]); }

        // Build atom pool.
        std::vector< Atom > pool;
        ValMap vmap;
        Populate(pool, vmap, num_vars, pts, opts.bitwidth);

        // Direct atom match.
        {
            auto it = vmap.find(target);
            if (it != vmap.end()) {
                auto e   = CloneExpr(*pool[it->second].expr);
                auto chk = FullWidthCheckEval(*ctx.eval, num_vars, *e, opts.bitwidth);
                if (chk.passed) {
                    auto info = ComputeCost(*e);
                    if ((baseline_cost == nullptr) || IsBetter(info.cost, *baseline_cost)) {
                        SubResult s;
                        s.expr     = std::move(e);
                        s.cost     = info.cost;
                        s.verified = true;
                        return s;
                    }
                }
            }
        }

        // Layer 1.
        auto r1 = Layer1(
            target, pool, vmap, kMask, *ctx.eval, num_vars, opts.bitwidth, baseline_cost
        );
        if (r1.has_value()) { return r1; }

        // Layer 2.
        auto r2 = Layer2(
            target, pool, vmap, kMask, *ctx.eval, num_vars, opts.bitwidth, baseline_cost
        );
        if (r2.has_value()) { return r2; }

        // Unary wrapping: check Neg(target) and Not(target)
        // against Layers 1 and 2.
        for (int wrap = 0; wrap < 2; ++wrap) {
            ProbeVals lifted;
            for (size_t i = 0; i < kNProbes; ++i) {
                if (wrap == 0) {
                    lifted[i] = (-target[i]) & kMask; // Neg
                } else {
                    lifted[i] = (~target[i]) & kMask; // Not
                }
            }

            auto check_wrap = [&](std::unique_ptr< Expr > inner) -> std::optional< SubResult > {
                auto wrapped = (wrap == 0) ? Expr::Negate(std::move(inner))
                                           : Expr::BitwiseNot(std::move(inner));
                auto chk     = FullWidthCheckEval(*ctx.eval, num_vars, *wrapped, opts.bitwidth);
                if (!chk.passed) { return std::nullopt; }
                auto info = ComputeCost(*wrapped);
                if (baseline_cost && !IsBetter(info.cost, *baseline_cost)) {
                    return std::nullopt;
                }
                SubResult s;
                s.expr     = std::move(wrapped);
                s.cost     = info.cost;
                s.verified = true;
                return s;
            };

            // Atom match on lifted
            {
                auto it = vmap.find(lifted);
                if (it != vmap.end()) {
                    auto r = check_wrap(CloneExpr(*pool[it->second].expr));
                    if (r.has_value()) { return r; }
                }
            }

            // Layer 1 on lifted
            auto w1 =
                Layer1(lifted, pool, vmap, kMask, *ctx.eval, num_vars, opts.bitwidth, nullptr);
            if (w1.has_value()) {
                auto r = check_wrap(std::move(w1->expr));
                if (r.has_value()) { return r; }
            }
        }

        // Layer 3: target = G1(A, G2(B, R)) where R is a base
        // atom or inner composition G_in(C, D).
        // G1, G2 must be invertible (XOR, ADD) for hash-lookup.
        // Cost: O(pool^2 * 4) hash lookups — very fast.
        {
            // Precompute inner compositions once.
            std::vector< InnerComp > inner;
            ValMap inner_idx;
            for (auto g_in : kAllGates) {
                for (size_t bi = 0; bi < pool.size(); ++bi) {
                    for (size_t ci = bi; ci < pool.size(); ++ci) {
                        auto v = GateApply(pool[bi].vals, pool[ci].vals, g_in, kMask);
                        if (vmap.contains(v) != 0u) { continue; }
                        if (inner_idx.contains(v) != 0u) { continue; }
                        inner_idx[v] = inner.size();
                        inner.push_back({ .gate = g_in, .bi = bi, .ci = ci, .vals = v });
                    }
                }
            }

            auto make_inner = [&](const InnerComp &ic) {
                return GateExpr(
                    ic.gate, CloneExpr(*pool[ic.bi].expr), CloneExpr(*pool[ic.ci].expr)
                );
            };

            for (auto g1 : kAllGates) {
                if (!GateInvertible(g1)) { continue; }
                for (size_t ai = 0; ai < pool.size(); ++ai) {
                    auto r1 = GateResidual(target, pool[ai].vals, g1, kMask);
                    if (vmap.contains(r1) != 0u) { continue; }

                    for (auto g2 : kAllGates) {
                        if (!GateInvertible(g2)) { continue; }
                        for (size_t bi = 0; bi < pool.size(); ++bi) {
                            auto r2 = GateResidual(r1, pool[bi].vals, g2, kMask);
                            // Look up r2 in base or inner pool
                            std::unique_ptr< Expr > r2_expr;
                            {
                                auto it = vmap.find(r2);
                                if (it != vmap.end()) {
                                    r2_expr = CloneExpr(*pool[it->second].expr);
                                }
                            }
                            if (!r2_expr) {
                                auto it = inner_idx.find(r2);
                                if (it != inner_idx.end()) {
                                    r2_expr = make_inner(inner[it->second]);
                                }
                            }
                            if (!r2_expr) { continue; }
                            auto mid =
                                GateExpr(g2, CloneExpr(*pool[bi].expr), std::move(r2_expr));
                            auto e = GateExpr(g1, CloneExpr(*pool[ai].expr), std::move(mid));
                            auto chk =
                                FullWidthCheckEval(*ctx.eval, num_vars, *e, opts.bitwidth);
                            if (!chk.passed) { continue; }
                            auto info = ComputeCost(*e);
                            if ((baseline_cost != nullptr)
                                && !IsBetter(info.cost, *baseline_cost))
                            {
                                continue;
                            }
                            SubResult s;
                            s.expr     = std::move(e);
                            s.cost     = info.cost;
                            s.verified = true;
                            return s;
                        }
                    }
                }
            }
        }

        return std::nullopt;
    }

} // namespace cobra
