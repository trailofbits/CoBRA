#include "cobra/core/TemplateDecomposer.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/Profile.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/Trace.h"
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

    namespace template_decomposer {

        enum Subcode : uint16_t {
            kNoEvaluator  = 1,
            kTooManyVars  = 2,
            kCostRejected = 3,
            kNoMatch      = 10,
        };

    } // namespace template_decomposer

    namespace {

        constexpr uint32_t kNProbes = 16;
        constexpr uint32_t kMaxVars = 6;

        using ProbeVals = std::array< uint64_t, kNProbes >;

        enum class Gate { kAnd, kOr, kXor, kAdd, kMul };
        constexpr std::array kAllGates = { Gate::kAnd, Gate::kOr, Gate::kXor, Gate::kAdd,
                                           Gate::kMul };

        bool GateInvertible(Gate g) { return g == Gate::kXor || g == Gate::kAdd; }

        // Fingerprint-indexed map: maps ProbeVals → size_t using a cheap
        // 64-bit fingerprint as primary key, with full equality check on
        // lookup to handle the (rare) collisions.  Avoids hashing 128 bytes
        // per lookup that unordered_map<ProbeVals,...> would require.
        uint64_t Fingerprint(const ProbeVals &v) {
            // XOR-fold with position-dependent rotation to avoid
            // commutative cancellation (a^b == b^a).
            uint64_t h = 0;
            for (size_t i = 0; i < kNProbes; ++i) { h ^= v[i] * (0x9E3779B97F4A7C15ULL + i); }
            return h;
        }

        class ValMap
        {
          public:
            struct Entry
            {
                size_t value;
            };

            void insert(const ProbeVals &key, size_t value) {
                auto fp = Fingerprint(key);
                buckets_[fp].push_back({ .key = key, .value = value });
            }

            // Returns pointer to value if found, nullptr otherwise.
            const size_t *find(const ProbeVals &key) const {
                auto fp = Fingerprint(key);
                auto it = buckets_.find(fp);
                if (it == buckets_.end()) { return nullptr; }
                for (const auto &entry : it->second) {
                    if (entry.key == key) { return &entry.value; }
                }
                return nullptr;
            }

            bool contains(const ProbeVals &key) const { return find(key) != nullptr; }

          private:
            struct Slot
            {
                ProbeVals key;
                size_t value;
            };

            std::unordered_map< uint64_t, std::vector< Slot > > buckets_;
        };

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
            const auto *slot = idx.find(vals);
            if (slot != nullptr) {
                if (IsBetter(ComputeCost(*e).cost, ComputeCost(*pool[*slot].expr).cost)) {
                    pool[*slot].expr = std::move(e);
                }
                return;
            }
            idx.insert(vals, pool.size());
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

        struct InnerCompositions
        {
            std::vector< InnerComp > comps;
            ValMap index;
        };

        InnerCompositions BuildInnerCompositions(
            const std::vector< Atom > &pool, const ValMap &vmap, uint64_t mask
        ) {
            InnerCompositions inner;
            for (auto g_in : kAllGates) {
                for (size_t bi = 0; bi < pool.size(); ++bi) {
                    for (size_t ci = bi; ci < pool.size(); ++ci) {
                        auto v = GateApply(pool[bi].vals, pool[ci].vals, g_in, mask);
                        if (vmap.contains(v)) { continue; }
                        if (inner.index.contains(v)) { continue; }
                        inner.index.insert(v, inner.comps.size());
                        inner.comps.push_back({ .gate = g_in, .bi = bi, .ci = ci, .vals = v });
                    }
                }
            }
            return inner;
        }

        // Try to verify and update the best result.
        // NOLINTNEXTLINE(readability-identifier-naming)
        bool TryUpdate(
            std::optional< SignaturePayload > &best, std::unique_ptr< Expr > candidate,
            const Evaluator &eval, uint32_t num_vars, uint32_t bw, const ExprCost *baseline
        ) {
            COBRA_ZONE_N("TemplateFWCheck");
            auto chk = FullWidthCheckEval(eval, num_vars, *candidate, bw);
            if (!chk.passed) { return false; }

            auto info = ComputeCost(*candidate);
            if ((baseline != nullptr) && !IsBetter(info.cost, *baseline)) { return false; }
            if (best.has_value() && !IsBetter(info.cost, best->cost)) { return false; }

            best = SignaturePayload{
                .expr         = std::move(candidate),
                .cost         = info.cost,
                .verification = VerificationState::kVerified,
            };
            return true;
        }

        // Layer 1: target = G(A, B) for atoms A, B.
        std::optional< SignaturePayload > Layer1(
            const ProbeVals &target, const std::vector< Atom > &pool, const ValMap &vmap,
            uint64_t mask, const Evaluator &eval, uint32_t nv, uint32_t bw,
            const ExprCost *baseline
        ) {
            std::optional< SignaturePayload > best;

            for (auto g : kAllGates) {
                if (GateInvertible(g)) {
                    for (size_t ai = 0; ai < pool.size(); ++ai) {
                        auto res         = GateResidual(target, pool[ai].vals, g, mask);
                        const auto *slot = vmap.find(res);
                        if (slot == nullptr) { continue; }
                        auto e = GateExpr(
                            g, CloneExpr(*pool[ai].expr), CloneExpr(*pool[*slot].expr)
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
        std::optional< SignaturePayload > Layer1Fast(
            const ProbeVals &target, const std::vector< Atom > &pool, const ValMap &vmap,
            uint64_t mask, const Evaluator &eval, uint32_t nv, uint32_t bw,
            const ExprCost *baseline
        ) {
            std::optional< SignaturePayload > best;
            for (auto g : kAllGates) {
                if (!GateInvertible(g)) { continue; }
                for (size_t ai = 0; ai < pool.size(); ++ai) {
                    auto res         = GateResidual(target, pool[ai].vals, g, mask);
                    const auto *slot = vmap.find(res);
                    if (slot == nullptr) { continue; }
                    auto e =
                        GateExpr(g, CloneExpr(*pool[ai].expr), CloneExpr(*pool[*slot].expr));
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

        template< typename CandidateVec >
        std::vector< size_t > CollectCompatibleIndices(
            const CandidateVec &candidates, const ProbeVals &target, Gate g
        ) {
            std::vector< size_t > indices;
            indices.reserve(candidates.size());
            for (size_t i = 0; i < candidates.size(); ++i) {
                if (Compatible(candidates[i].vals, target, g)) { indices.push_back(i); }
            }
            return indices;
        }

        bool AndMatches(const ProbeVals &a, const ProbeVals &b, const ProbeVals &target) {
            for (size_t i = 0; i < kNProbes; ++i) {
                if ((a[i] & b[i]) != target[i]) { return false; }
            }
            return true;
        }

        bool OrMatches(const ProbeVals &a, const ProbeVals &b, const ProbeVals &target) {
            for (size_t i = 0; i < kNProbes; ++i) {
                if ((a[i] | b[i]) != target[i]) { return false; }
            }
            return true;
        }

        // Layer 2: target = G_out(A, G_in(B, C)).
        std::optional< SignaturePayload > Layer2(
            const ProbeVals &target, const std::vector< Atom > &pool, const ValMap &vmap,
            const InnerCompositions &inner_cache, uint64_t mask, const Evaluator &eval,
            uint32_t nv, uint32_t bw, const ExprCost *baseline
        ) {
            const auto &inner     = inner_cache.comps;
            const auto &inner_idx = inner_cache.index;
            std::optional< SignaturePayload > best;

            COBRA_PLOT("L2InnerSize", static_cast< int64_t >(inner.size()));

            auto make_inner = [&](const InnerComp &ic) {
                return GateExpr(
                    ic.gate, CloneExpr(*pool[ic.bi].expr), CloneExpr(*pool[ic.ci].expr)
                );
            };

            // --- Invertible gates (XOR, ADD): hash lookup O(pool + inner) ---
            {
                COBRA_ZONE_N("L2Invertible");
                // Case A: G_out(base_atom, inner_comp)
                for (auto g_out : kAllGates) {
                    if (!GateInvertible(g_out)) { continue; }
                    for (const auto &ai : pool) {
                        auto res         = GateResidual(target, ai.vals, g_out, mask);
                        const auto *slot = inner_idx.find(res);
                        if (slot == nullptr) { continue; }
                        auto e = GateExpr(g_out, CloneExpr(*ai.expr), make_inner(inner[*slot]));
                        TryUpdate(best, std::move(e), eval, nv, bw, baseline);
                    }
                    if (best.has_value()) { return best; }
                }
                // Case B: G_out(inner_comp, inner_comp)
                for (auto g_out : kAllGates) {
                    if (!GateInvertible(g_out)) { continue; }
                    for (size_t ii = 0; ii < inner.size(); ++ii) {
                        auto res         = GateResidual(target, inner[ii].vals, g_out, mask);
                        const auto *slot = inner_idx.find(res);
                        if (slot == nullptr) { continue; }
                        auto e =
                            GateExpr(g_out, make_inner(inner[ii]), make_inner(inner[*slot]));
                        TryUpdate(best, std::move(e), eval, nv, bw, baseline);
                    }
                    if (best.has_value()) { return best; }
                }
            }

            // --- AND/OR gates: pre-filtered compatible scan ---
            {
                COBRA_ZONE_N("L2AndOr");
                const auto compat_pool_and = CollectCompatibleIndices(pool, target, Gate::kAnd);
                const auto compat_pool_or  = CollectCompatibleIndices(pool, target, Gate::kOr);
                const auto compat_inner_and =
                    CollectCompatibleIndices(inner, target, Gate::kAnd);
                const auto compat_inner_or = CollectCompatibleIndices(inner, target, Gate::kOr);
                COBRA_PLOT("L2CompatAndPool", static_cast< int64_t >(compat_pool_and.size()));
                COBRA_PLOT("L2CompatOrPool", static_cast< int64_t >(compat_pool_or.size()));
                COBRA_PLOT("L2CompatAndInner", static_cast< int64_t >(compat_inner_and.size()));
                COBRA_PLOT("L2CompatOrInner", static_cast< int64_t >(compat_inner_or.size()));

                for (auto g_out : { Gate::kAnd, Gate::kOr }) {
                    const auto &pool_compat =
                        (g_out == Gate::kAnd) ? compat_pool_and : compat_pool_or;
                    const auto &inner_compat =
                        (g_out == Gate::kAnd) ? compat_inner_and : compat_inner_or;
                    for (size_t ai_idx : pool_compat) {
                        const auto &ai = pool[ai_idx];
                        for (size_t ii_idx : inner_compat) {
                            const auto &ii     = inner[ii_idx];
                            const bool matches = (g_out == Gate::kAnd)
                                ? AndMatches(ai.vals, ii.vals, target)
                                : OrMatches(ai.vals, ii.vals, target);
                            if (!matches) { continue; }
                            auto e = GateExpr(g_out, CloneExpr(*ai.expr), make_inner(ii));
                            TryUpdate(best, std::move(e), eval, nv, bw, baseline);
                            if (best.has_value()) { return best; }
                        }
                    }
                    if (best.has_value()) { return best; }
                }
            }

            // --- MUL gate: full scan (no algebraic shortcut) ---
            {
                COBRA_ZONE_N("L2Mul");
                for (const auto &ai : pool) {
                    for (const auto &ii : inner) {
                        if (!GateMatches(ai.vals, ii.vals, target, Gate::kMul, mask)) {
                            continue;
                        }
                        auto e = GateExpr(Gate::kMul, CloneExpr(*ai.expr), make_inner(ii));
                        TryUpdate(best, std::move(e), eval, nv, bw, baseline);
                        if (best.has_value()) { return best; }
                    }
                }
            }

            return best;
        }

        std::optional< SignaturePayload > Layer3(
            const ProbeVals &target, const std::vector< Atom > &pool, const ValMap &vmap,
            const InnerCompositions &inner_cache, uint64_t mask, const Evaluator &eval,
            uint32_t nv, uint32_t bw, const ExprCost *baseline
        ) {
            const auto &inner     = inner_cache.comps;
            const auto &inner_idx = inner_cache.index;

            auto make_inner = [&](const InnerComp &ic) {
                return GateExpr(
                    ic.gate, CloneExpr(*pool[ic.bi].expr), CloneExpr(*pool[ic.ci].expr)
                );
            };

            for (auto g1 : kAllGates) {
                if (!GateInvertible(g1)) { continue; }
                for (size_t ai = 0; ai < pool.size(); ++ai) {
                    auto r1 = GateResidual(target, pool[ai].vals, g1, mask);
                    if (vmap.contains(r1)) { continue; }

                    for (auto g2 : kAllGates) {
                        if (!GateInvertible(g2)) { continue; }
                        for (size_t bi = 0; bi < pool.size(); ++bi) {
                            auto r2 = GateResidual(r1, pool[bi].vals, g2, mask);
                            std::unique_ptr< Expr > r2_expr;
                            {
                                const auto *p = vmap.find(r2);
                                if (p != nullptr) { r2_expr = CloneExpr(*pool[*p].expr); }
                            }
                            if (!r2_expr) {
                                const auto *p = inner_idx.find(r2);
                                if (p != nullptr) { r2_expr = make_inner(inner[*p]); }
                            }
                            if (!r2_expr) { continue; }
                            auto mid =
                                GateExpr(g2, CloneExpr(*pool[bi].expr), std::move(r2_expr));
                            auto e   = GateExpr(g1, CloneExpr(*pool[ai].expr), std::move(mid));
                            auto chk = FullWidthCheckEval(eval, nv, *e, bw);
                            if (!chk.passed) { continue; }
                            auto info = ComputeCost(*e);
                            if ((baseline != nullptr) && !IsBetter(info.cost, *baseline)) {
                                continue;
                            }
                            SignaturePayload s{
                                .expr         = std::move(e),
                                .cost         = info.cost,
                                .verification = VerificationState::kVerified,
                            };
                            return s;
                        }
                    }
                }
            }

            return std::nullopt;
        }

    } // namespace

    SolverResult< SignaturePayload > TryTemplateDecomposition(
        const SignatureContext &ctx, const Options &opts, uint32_t num_vars,
        const ExprCost *baseline_cost
    ) {
        COBRA_TRACE("TemplateDecomp", "TryTemplateDecomposition: vars={}", num_vars);
        if (!ctx.eval) {
            ReasonDetail reason{
                .top = { .code    = { ReasonCategory::kGuardFailed,
                                      ReasonDomain::kTemplateDecomposer,
                                      template_decomposer::kNoEvaluator },
                        .message = "no evaluator available" }
            };
            return SolverResult< SignaturePayload >::Inapplicable(std::move(reason));
        }
        if (num_vars > kMaxVars) {
            ReasonDetail reason{
                .top = { .code    = { ReasonCategory::kGuardFailed,
                                      ReasonDomain::kTemplateDecomposer,
                                      template_decomposer::kTooManyVars },
                        .message = "too many variables" }
            };
            return SolverResult< SignaturePayload >::Inapplicable(std::move(reason));
        }

        // Zero real variables: the function is a constant.
        if (num_vars == 0) {
            const uint64_t kVal = (*ctx.eval)({});
            auto e              = Expr::Constant(kVal);
            auto info           = ComputeCost(*e);
            if ((baseline_cost != nullptr) && !IsBetter(info.cost, *baseline_cost)) {
                ReasonDetail reason{
                    .top = { .code    = { ReasonCategory::kCostRejected,
                                          ReasonDomain::kTemplateDecomposer,
                                          template_decomposer::kCostRejected },
                            .message = "constant result not cheaper than baseline" }
                };
                return SolverResult< SignaturePayload >::Blocked(std::move(reason));
            }
            SignaturePayload s{
                .expr         = std::move(e),
                .cost         = info.cost,
                .verification = VerificationState::kVerified,
            };
            return SolverResult< SignaturePayload >::Success(std::move(s));
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
        {
            COBRA_ZONE_N("TemplatePopulate");
            Populate(pool, vmap, num_vars, pts, opts.bitwidth);
        }
        COBRA_PLOT("TemplatePoolSize", static_cast< int64_t >(pool.size()));

        // Direct atom match.
        {
            const auto *slot = vmap.find(target);
            if (slot != nullptr) {
                auto e   = CloneExpr(*pool[*slot].expr);
                auto chk = FullWidthCheckEval(*ctx.eval, num_vars, *e, opts.bitwidth);
                if (chk.passed) {
                    auto info = ComputeCost(*e);
                    if ((baseline_cost == nullptr) || IsBetter(info.cost, *baseline_cost)) {
                        SignaturePayload s{
                            .expr         = std::move(e),
                            .cost         = info.cost,
                            .verification = VerificationState::kVerified,
                        };
                        return SolverResult< SignaturePayload >::Success(std::move(s));
                    }
                }
            }
        }

        // Layer 1.
        {
            COBRA_ZONE_N("TemplateLayer1");
            auto r1 = Layer1(
                target, pool, vmap, kMask, *ctx.eval, num_vars, opts.bitwidth, baseline_cost
            );
            if (r1.has_value()) {
                return SolverResult< SignaturePayload >::Success(std::move(*r1));
            }
        }

        std::optional< InnerCompositions > inner_cache;

        // Layer 2.
        {
            COBRA_ZONE_N("TemplateLayer2");
            auto inner = BuildInnerCompositions(pool, vmap, kMask);
            auto r2    = Layer2(
                target, pool, vmap, inner, kMask, *ctx.eval, num_vars, opts.bitwidth,
                baseline_cost
            );
            if (r2.has_value()) {
                return SolverResult< SignaturePayload >::Success(std::move(*r2));
            }
            inner_cache = std::move(inner);
        }

        // Unary wrapping: check Neg(target) and Not(target)
        // against Layers 1 and 2.
        COBRA_ZONE_N("TemplateUnaryWrap");
        for (int wrap = 0; wrap < 2; ++wrap) {
            ProbeVals lifted;
            for (size_t i = 0; i < kNProbes; ++i) {
                if (wrap == 0) {
                    lifted[i] = (-target[i]) & kMask; // Neg
                } else {
                    lifted[i] = (~target[i]) & kMask; // Not
                }
            }

            auto check_wrap =
                [&](std::unique_ptr< Expr > inner) -> std::optional< SignaturePayload > {
                auto wrapped = (wrap == 0) ? Expr::Negate(std::move(inner))
                                           : Expr::BitwiseNot(std::move(inner));
                auto chk     = FullWidthCheckEval(*ctx.eval, num_vars, *wrapped, opts.bitwidth);
                if (!chk.passed) { return std::nullopt; }
                auto info = ComputeCost(*wrapped);
                if (baseline_cost && !IsBetter(info.cost, *baseline_cost)) {
                    return std::nullopt;
                }
                SignaturePayload s{
                    .expr         = std::move(wrapped),
                    .cost         = info.cost,
                    .verification = VerificationState::kVerified,
                };
                return s;
            };

            // Atom match on lifted
            {
                const auto *slot = vmap.find(lifted);
                if (slot != nullptr) {
                    auto r = check_wrap(CloneExpr(*pool[*slot].expr));
                    if (r.has_value()) {
                        return SolverResult< SignaturePayload >::Success(std::move(*r));
                    }
                }
            }

            // Layer 1 on lifted
            auto w1 =
                Layer1(lifted, pool, vmap, kMask, *ctx.eval, num_vars, opts.bitwidth, nullptr);
            if (w1.has_value()) {
                auto r = check_wrap(std::move(w1->expr));
                if (r.has_value()) {
                    return SolverResult< SignaturePayload >::Success(std::move(*r));
                }
            }
        }

        // Layer 3: target = G1(A, G2(B, R)) where R is a base
        // atom or inner composition G_in(C, D).
        // G1, G2 must be invertible (XOR, ADD) for hash-lookup.
        // Cost: O(pool^2 * 4) hash lookups — very fast.
        {
            COBRA_ZONE_N("TemplateLayer3");
            auto r3 = Layer3(
                target, pool, vmap, *inner_cache, kMask, *ctx.eval, num_vars, opts.bitwidth,
                baseline_cost
            );
            if (r3.has_value()) {
                return SolverResult< SignaturePayload >::Success(std::move(*r3));
            }
        }

        COBRA_TRACE("TemplateDecomp", "TryTemplateDecomposition: found={}", false);
        ReasonDetail reason{
            .top = { .code    = { ReasonCategory::kSearchExhausted,
                                  ReasonDomain::kTemplateDecomposer,
                                  template_decomposer::kNoMatch },
                    .message = "no template match found" }
        };
        return SolverResult< SignaturePayload >::Blocked(std::move(reason));
    }

} // namespace cobra
