#include "cobra/core/TemplateDecomposer.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/MathUtils.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/Profile.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/Trace.h"
#include <absl/container/flat_hash_map.h>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
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

        // 64-bit fingerprint of a ProbeVals array. Used as the hash key
        // in ValMap. Collisions are astronomically unlikely (~10^-12 for
        // typical pool sizes) and handled conservatively downstream.
        uint64_t Fingerprint(const ProbeVals &v) {
            // XOR-fold with position-dependent rotation to avoid
            // commutative cancellation (a^b == b^a).
            uint64_t h = 0;
            for (size_t i = 0; i < kNProbes; ++i) { h ^= v[i] * (0x9E3779B97F4A7C15ULL + i); }
            return h;
        }

        // Maps 64-bit probe-value fingerprints to pool/inner indices.
        // Uses absl::flat_hash_map for SIMD-accelerated lookup.
        class ValMap
        {
          public:
            void insert(const ProbeVals &key, size_t value) {
                map_.try_emplace(Fingerprint(key), static_cast< uint32_t >(value));
            }

            const size_t *find(const ProbeVals &key) const {
                auto it = map_.find(Fingerprint(key));
                if (it == map_.end()) { return nullptr; }
                idx_buf_ = it->second;
                return &idx_buf_;
            }

            bool contains(const ProbeVals &key) const { return find(key) != nullptr; }

          private:
            absl::flat_hash_map< uint64_t, uint32_t > map_;
            mutable size_t idx_buf_ = 0;
        };

        struct Atom
        {
            std::unique_ptr< Expr > expr;
            ProbeVals vals;
            ExprCost cost;
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
                auto new_cost = ComputeCost(*e).cost;
                if (IsBetter(new_cost, pool[*slot].cost)) {
                    pool[*slot].expr = std::move(e);
                    pool[*slot].cost = new_cost;
                }
                return;
            }
            auto c = ComputeCost(*e).cost;
            idx.insert(vals, pool.size());
            pool.push_back({ .expr = std::move(e), .vals = vals, .cost = c });
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

            // Neg and NOT of every atom so far.
            // Compute probe values arithmetically from existing vals
            // instead of re-evaluating through EvalExpr (16 calls each).
            const size_t base = pool.size();
            for (size_t k = 0; k < base; ++k) {
                ProbeVals neg_v;
                ProbeVals not_v;
                for (size_t i = 0; i < kNProbes; ++i) {
                    neg_v[i] = (-pool[k].vals[i]) & mask;
                    not_v[i] = (~pool[k].vals[i]) & mask;
                }
                Push(pool, idx, Expr::Negate(CloneExpr(*pool[k].expr)), neg_v);
                Push(pool, idx, Expr::BitwiseNot(CloneExpr(*pool[k].expr)), not_v);
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

        // Check G(a[0], b[0]) == target[0] at probe 0 only.
        // Cheap rejection filter before the full 16-probe GateMatches.
        bool Probe0Matches(uint64_t a0, uint64_t b0, uint64_t t0, Gate g, uint64_t mask) {
            switch (g) {
                case Gate::kAnd:
                    return (a0 & b0) == t0;
                case Gate::kOr:
                    return (a0 | b0) == t0;
                case Gate::kXor:
                    return (a0 ^ b0) == t0;
                case Gate::kAdd:
                    return ((a0 + b0) & mask) == t0;
                case Gate::kMul:
                    return ((a0 * b0) & mask) == t0;
            }
            return false;
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
            absl::flat_hash_map< uint64_t, std::vector< size_t > > mul_probe0_buckets;
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
                        const auto slot = inner.comps.size();
                        inner.index.insert(v, slot);
                        inner.mul_probe0_buckets[v[0]].push_back(slot);
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
                            if (!Probe0Matches(
                                    pool[ai].vals[0], pool[bi].vals[0], target[0], g, mask
                                ))
                            {
                                continue;
                            }
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

        // Check if atom A is compatible as an operand of a
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

        bool MulMatches(
            const ProbeVals &a, const ProbeVals &b, const ProbeVals &target, uint64_t mask,
            size_t start_probe = 0
        ) {
            for (size_t i = start_probe; i < kNProbes; ++i) {
                if (((a[i] * b[i]) & mask) != target[i]) { return false; }
            }
            return true;
        }

        // For target = A * inner (mod 2^bw), use probe 0 to derive the set of
        // admissible inner[0] values and restrict the scan to matching buckets.
        // Falls back to a full scan when probe 0 is too degenerate to be useful.
        bool CollectMulProbe0Candidates(
            const absl::flat_hash_map< uint64_t, std::vector< size_t > > &probe0_buckets,
            uint64_t lhs_probe0, uint64_t target_probe0, uint32_t bitwidth,
            std::vector< size_t > &out
        ) {
            constexpr uint32_t kMaxEnumeratedShift = 8;

            out.clear();
            lhs_probe0    &= Bitmask(bitwidth);
            target_probe0 &= Bitmask(bitwidth);

            if (lhs_probe0 == 0) { return target_probe0 != 0; }

            const auto twos =
                std::min(bitwidth, static_cast< uint32_t >(std::countr_zero(lhs_probe0)));
            if ((target_probe0 & Bitmask(twos)) != 0) { return true; }
            if (twos > kMaxEnumeratedShift) { return false; }

            const uint32_t reduced_bits = bitwidth - twos;
            uint64_t base_solution      = 0;
            if (reduced_bits > 0) {
                const auto odd_part       = lhs_probe0 >> twos;
                const auto reduced_target = target_probe0 >> twos;
                base_solution = (ModInverseOdd(odd_part, reduced_bits) * reduced_target)
                    & Bitmask(reduced_bits);
            }

            const auto append_bucket = [&](uint64_t probe0_value) {
                auto it = probe0_buckets.find(probe0_value);
                if (it == probe0_buckets.end()) { return; }
                out.insert(out.end(), it->second.begin(), it->second.end());
            };

            if (twos == 0) {
                append_bucket(base_solution & Bitmask(bitwidth));
                return true;
            }

            const uint64_t step           = 1ULL << reduced_bits;
            const uint64_t solution_count = 1ULL << twos;
            const uint64_t full_mask      = Bitmask(bitwidth);
            for (uint64_t k = 0; k < solution_count; ++k) {
                append_bucket((base_solution + (k * step)) & full_mask);
            }
            return true;
        }

        // Layer 2: target = G_out(A, G_in(B, C)).
        std::optional< SignaturePayload > Layer2(
            const ProbeVals &target, const std::vector< Atom > &pool,
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
                            const auto &ii = inner[ii_idx];
                            if (!Probe0Matches(ai.vals[0], ii.vals[0], target[0], g_out, mask))
                            {
                                continue;
                            }
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
                std::vector< size_t > mul_probe0_candidates;
                size_t mul_bucketed_outer = 0;
                size_t mul_fallback_outer = 0;

                for (const auto &ai : pool) {
                    const bool used_probe0_filter = CollectMulProbe0Candidates(
                        inner_cache.mul_probe0_buckets, ai.vals[0], target[0], bw,
                        mul_probe0_candidates
                    );
                    if (used_probe0_filter) {
                        ++mul_bucketed_outer;
                        for (size_t ii_idx : mul_probe0_candidates) {
                            const auto &ii = inner[ii_idx];
                            if (!MulMatches(ai.vals, ii.vals, target, mask, 1)) { continue; }
                            auto e = GateExpr(Gate::kMul, CloneExpr(*ai.expr), make_inner(ii));
                            TryUpdate(best, std::move(e), eval, nv, bw, baseline);
                            if (best.has_value()) { return best; }
                        }
                        continue;
                    }

                    ++mul_fallback_outer;
                    for (const auto &ii : inner) {
                        if (!MulMatches(ai.vals, ii.vals, target, mask)) { continue; }
                        auto e = GateExpr(Gate::kMul, CloneExpr(*ai.expr), make_inner(ii));
                        TryUpdate(best, std::move(e), eval, nv, bw, baseline);
                        if (best.has_value()) { return best; }
                    }
                }

                COBRA_PLOT("L2MulBucketedOuter", static_cast< int64_t >(mul_bucketed_outer));
                COBRA_PLOT("L2MulFallbackOuter", static_cast< int64_t >(mul_fallback_outer));
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
            std::optional< SignaturePayload > best;

            auto make_inner = [&](const InnerComp &ic) {
                return GateExpr(
                    ic.gate, CloneExpr(*pool[ic.bi].expr), CloneExpr(*pool[ic.ci].expr)
                );
            };

            // Invertible G1, invertible G2: hash-based lookup.
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

            return best;
        }

        // Layer 4: target = G1(A, Unary(G2(B, inner_C)))
        // where G1 is invertible, Unary is Neg or Not, and G2 is
        // any gate (And/Or/Mul scanned with compatibility filtering).
        //
        // This covers overlap-conditioned patterns like:
        //   Add(overlap, Neg(And(neg_var, Mul(overlap, var))))
        std::optional< SignaturePayload > Layer4(
            const ProbeVals &target, const std::vector< Atom > &pool, const ValMap &vmap,
            const InnerCompositions &inner_cache, uint64_t mask, const Evaluator &eval,
            uint32_t nv, uint32_t bw, const ExprCost *baseline
        ) {
            const auto &inner     = inner_cache.comps;
            const auto &inner_idx = inner_cache.index;
            std::optional< SignaturePayload > best;

            auto make_inner = [&](const InnerComp &ic) {
                return GateExpr(
                    ic.gate, CloneExpr(*pool[ic.bi].expr), CloneExpr(*pool[ic.ci].expr)
                );
            };

            std::vector< size_t > mul_candidates;
            for (auto g1 : kAllGates) {
                if (!GateInvertible(g1)) { continue; }
                for (size_t ai = 0; ai < pool.size(); ++ai) {
                    auto r1 = GateResidual(target, pool[ai].vals, g1, mask);
                    if (vmap.contains(r1)) { continue; }

                    // Try Neg and Not of the residual
                    for (int wrap = 0; wrap < 2; ++wrap) {
                        ProbeVals lifted;
                        for (size_t i = 0; i < kNProbes; ++i) {
                            lifted[i] = (wrap == 0) ? ((~r1[i] + 1) & mask) : (~r1[i] & mask);
                        }

                        // Direct inner match (lifted == inner)
                        {
                            const auto *p = inner_idx.find(lifted);
                            if (p != nullptr) {
                                auto inner_e = make_inner(inner[*p]);
                                auto wrapped = (wrap == 0)
                                    ? Expr::Negate(std::move(inner_e))
                                    : Expr::BitwiseNot(std::move(inner_e));
                                auto e =
                                    GateExpr(g1, CloneExpr(*pool[ai].expr), std::move(wrapped));
                                if (TryUpdate(best, std::move(e), eval, nv, bw, baseline)) {
                                    return best;
                                }
                            }
                        }

                        // And/Or scan: lifted = G2(atom_B, inner_C)
                        // Probe-0 check rejects most (pool, inner) pairs
                        // with a single integer comparison before touching
                        // the full 16-probe arrays.
                        for (auto g2 : { Gate::kAnd, Gate::kOr }) {
                            for (size_t bi = 0; bi < pool.size(); ++bi) {
                                if (!Compatible(pool[bi].vals, lifted, g2)) { continue; }
                                for (size_t ii = 0; ii < inner.size(); ++ii) {
                                    if (!Probe0Matches(
                                            pool[bi].vals[0], inner[ii].vals[0], lifted[0], g2,
                                            mask
                                        ))
                                    {
                                        continue;
                                    }
                                    const bool matches = (g2 == Gate::kAnd)
                                        ? AndMatches(pool[bi].vals, inner[ii].vals, lifted)
                                        : OrMatches(pool[bi].vals, inner[ii].vals, lifted);
                                    if (!matches) { continue; }
                                    auto g2_e = GateExpr(
                                        g2, CloneExpr(*pool[bi].expr), make_inner(inner[ii])
                                    );
                                    auto wrapped = (wrap == 0)
                                        ? Expr::Negate(std::move(g2_e))
                                        : Expr::BitwiseNot(std::move(g2_e));
                                    auto e       = GateExpr(
                                        g1, CloneExpr(*pool[ai].expr), std::move(wrapped)
                                    );
                                    if (TryUpdate(best, std::move(e), eval, nv, bw, baseline)) {
                                        return best;
                                    }
                                }
                            }
                        }

                        // Mul scan: lifted = Mul(atom_B, inner_C)
                        {
                            for (size_t bi = 0; bi < pool.size(); ++bi) {
                                const bool filtered = CollectMulProbe0Candidates(
                                    inner_cache.mul_probe0_buckets, pool[bi].vals[0], lifted[0],
                                    bw, mul_candidates
                                );
                                if (filtered) {
                                    for (size_t ii_idx : mul_candidates) {
                                        if (!MulMatches(
                                                pool[bi].vals, inner[ii_idx].vals, lifted, mask,
                                                1
                                            ))
                                        {
                                            continue;
                                        }
                                        auto g2_e = GateExpr(
                                            Gate::kMul, CloneExpr(*pool[bi].expr),
                                            make_inner(inner[ii_idx])
                                        );
                                        auto wrapped = (wrap == 0)
                                            ? Expr::Negate(std::move(g2_e))
                                            : Expr::BitwiseNot(std::move(g2_e));
                                        auto e       = GateExpr(
                                            g1, CloneExpr(*pool[ai].expr), std::move(wrapped)
                                        );
                                        if (TryUpdate(
                                                best, std::move(e), eval, nv, bw, baseline
                                            ))
                                        {
                                            return best;
                                        }
                                    }
                                    continue;
                                }
                                for (size_t ii = 0; ii < inner.size(); ++ii) {
                                    if (!MulMatches(
                                            pool[bi].vals, inner[ii].vals, lifted, mask
                                        ))
                                    {
                                        continue;
                                    }
                                    auto g2_e = GateExpr(
                                        Gate::kMul, CloneExpr(*pool[bi].expr),
                                        make_inner(inner[ii])
                                    );
                                    auto wrapped = (wrap == 0)
                                        ? Expr::Negate(std::move(g2_e))
                                        : Expr::BitwiseNot(std::move(g2_e));
                                    auto e       = GateExpr(
                                        g1, CloneExpr(*pool[ai].expr), std::move(wrapped)
                                    );
                                    if (TryUpdate(best, std::move(e), eval, nv, bw, baseline)) {
                                        return best;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            return best;
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
                target, pool, inner, kMask, *ctx.eval, num_vars, opts.bitwidth, baseline_cost
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

        // Layer 4: target = G1(A, Unary(G2(B, inner_C)))
        // Unary-wrapped non-invertible gate scan for overlap patterns.
        {
            COBRA_ZONE_N("TemplateLayer4");
            auto r4 = Layer4(
                target, pool, vmap, *inner_cache, kMask, *ctx.eval, num_vars, opts.bitwidth,
                baseline_cost
            );
            if (r4.has_value()) {
                return SolverResult< SignaturePayload >::Success(std::move(*r4));
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
