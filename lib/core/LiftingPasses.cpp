#include "LiftingPasses.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/Profile.h"
#include "cobra/core/SignatureEval.h"

#include <algorithm>
#include <functional>
#include <unordered_map>

namespace cobra {

    uint64_t
    EvaluateExpr(const Expr &e, const std::vector< uint64_t > &vals, uint32_t bitwidth) {
        uint64_t mask = (bitwidth == 64) ? UINT64_MAX : ((uint64_t{ 1 } << bitwidth) - 1);
        switch (e.kind) {
            case Expr::Kind::kConstant:
                return e.constant_val & mask;
            case Expr::Kind::kVariable:
                return vals.at(e.var_index) & mask;
            case Expr::Kind::kAdd: {
                uint64_t r = 0;
                for (const auto &c : e.children) {
                    r = (r + EvaluateExpr(*c, vals, bitwidth)) & mask;
                }
                return r;
            }
            case Expr::Kind::kMul: {
                uint64_t r = 1;
                for (const auto &c : e.children) {
                    r = (r * EvaluateExpr(*c, vals, bitwidth)) & mask;
                }
                return r;
            }
            case Expr::Kind::kNeg:
                return (-EvaluateExpr(*e.children[0], vals, bitwidth)) & mask;
            case Expr::Kind::kAnd:
                return EvaluateExpr(*e.children[0], vals, bitwidth)
                    & EvaluateExpr(*e.children[1], vals, bitwidth);
            case Expr::Kind::kOr:
                return EvaluateExpr(*e.children[0], vals, bitwidth)
                    | EvaluateExpr(*e.children[1], vals, bitwidth);
            case Expr::Kind::kXor:
                return EvaluateExpr(*e.children[0], vals, bitwidth)
                    ^ EvaluateExpr(*e.children[1], vals, bitwidth);
            case Expr::Kind::kNot:
                return (~EvaluateExpr(*e.children[0], vals, bitwidth)) & mask;
            case Expr::Kind::kShr:
                return (EvaluateExpr(*e.children[0], vals, bitwidth)
                        >> e.children[1]->constant_val)
                    & mask;
        }
        return 0;
    }

    namespace {

        bool IsBitwiseKind(Expr::Kind k) {
            return k == Expr::Kind::kAnd || k == Expr::Kind::kOr || k == Expr::Kind::kXor
                || k == Expr::Kind::kNot;
        }

        bool IsPureArithmetic(const Expr &e) {
            switch (e.kind) {
                case Expr::Kind::kConstant:
                case Expr::Kind::kVariable:
                    return true;
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kNeg:
                    break;
                default:
                    return false;
            }
            for (const auto &c : e.children) {
                if (!IsPureArithmetic(*c)) { return false; }
            }
            return true;
        }

        struct LiftCandidate
        {
            const Expr *subtree;
            size_t hash;
            std::string rendered;
        };

        void CollectLiftableAtoms(
            const Expr &node, bool parent_is_bitwise, const std::vector< std::string > &vars,
            uint32_t bitwidth, std::vector< LiftCandidate > &out
        ) {
            if (parent_is_bitwise && IsPureArithmetic(node) && HasVarDep(node)
                && node.kind != Expr::Kind::kVariable)
            {
                size_t h      = std::hash< Expr >{}(node);
                auto rendered = Render(node, vars, bitwidth);
                out.push_back(
                    LiftCandidate{
                        .subtree  = &node,
                        .hash     = h,
                        .rendered = std::move(rendered),
                    }
                );
                return;
            }

            bool current_is_bitwise = IsBitwiseKind(node.kind);
            for (const auto &child : node.children) {
                CollectLiftableAtoms(*child, current_is_bitwise, vars, bitwidth, out);
            }
        }

        struct DeduplicatedAtom
        {
            const Expr *subtree;
            size_t hash;
            std::string rendered;
            uint32_t virtual_index;
        };

        std::vector< DeduplicatedAtom > DeduplicateAtoms(
            const std::vector< LiftCandidate > &candidates, uint32_t first_virtual_index
        ) {
            std::vector< DeduplicatedAtom > result;
            std::unordered_map< size_t, std::vector< size_t > > by_hash;

            for (const auto &cand : candidates) {
                bool found = false;
                auto it    = by_hash.find(cand.hash);
                if (it != by_hash.end()) {
                    for (size_t idx : it->second) {
                        if (result[idx].rendered == cand.rendered) {
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    auto vi = first_virtual_index + static_cast< uint32_t >(result.size());
                    by_hash[cand.hash].push_back(result.size());
                    result.push_back(
                        DeduplicatedAtom{
                            .subtree       = cand.subtree,
                            .hash          = cand.hash,
                            .rendered      = cand.rendered,
                            .virtual_index = vi,
                        }
                    );
                }
            }
            return result;
        }

        uint32_t FindVirtualIndex(
            const Expr &node, const std::vector< DeduplicatedAtom > &atoms,
            const std::vector< std::string > &vars, uint32_t bitwidth
        ) {
            size_t h      = std::hash< Expr >{}(node);
            auto rendered = Render(node, vars, bitwidth);
            for (const auto &atom : atoms) {
                if (atom.hash == h && atom.rendered == rendered) { return atom.virtual_index; }
            }
            return UINT32_MAX;
        }

        std::unique_ptr< Expr > ReplaceAtomsWithVirtual(
            const Expr &node, bool parent_is_bitwise,
            const std::vector< DeduplicatedAtom > &atoms,
            const std::vector< std::string > &vars, uint32_t bitwidth
        ) {
            if (parent_is_bitwise && IsPureArithmetic(node) && HasVarDep(node)
                && node.kind != Expr::Kind::kVariable)
            {
                uint32_t vi = FindVirtualIndex(node, atoms, vars, bitwidth);
                return Expr::Variable(vi);
            }

            auto result          = std::make_unique< Expr >();
            result->kind         = node.kind;
            result->constant_val = node.constant_val;
            result->var_index    = node.var_index;

            bool current_is_bitwise = IsBitwiseKind(node.kind);
            for (const auto &child : node.children) {
                result->children.push_back(
                    ReplaceAtomsWithVirtual(*child, current_is_bitwise, atoms, vars, bitwidth)
                );
            }
            return result;
        }

        void CollectVarSupport(const Expr &e, std::vector< uint32_t > &out) {
            if (e.kind == Expr::Kind::kVariable) {
                for (uint32_t idx : out) {
                    if (idx == e.var_index) { return; }
                }
                out.push_back(e.var_index);
                return;
            }
            for (const auto &c : e.children) { CollectVarSupport(*c, out); }
        }

        uint32_t CountNodes(const Expr &e) {
            uint32_t count = 1;
            for (const auto &c : e.children) { count += CountNodes(*c); }
            return count;
        }

        static constexpr uint32_t kMinRepeatSize = 4;

        struct RepeatEntry
        {
            size_t hash;
            std::string rendered;
            const Expr *first_occurrence;
            uint32_t count;
            uint32_t size;
            uint32_t first_preorder;
        };

        void CollectNonLeafSubtrees(
            const Expr &node, uint32_t &preorder_counter,
            const std::vector< std::string > &vars, uint32_t bitwidth,
            std::unordered_map< size_t, std::vector< size_t > > &by_hash,
            std::vector< RepeatEntry > &entries
        ) {
            uint32_t my_order = preorder_counter++;
            bool is_leaf =
                (node.kind == Expr::Kind::kConstant || node.kind == Expr::Kind::kVariable);
            if (!is_leaf) {
                size_t h       = std::hash< Expr >{}(node);
                auto rendered  = Render(node, vars, bitwidth);
                auto node_size = CountNodes(node);

                bool found = false;
                auto hit   = by_hash.find(h);
                if (hit != by_hash.end()) {
                    for (size_t idx : hit->second) {
                        if (entries[idx].rendered == rendered) {
                            entries[idx].count++;
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    by_hash[h].push_back(entries.size());
                    entries.push_back(
                        RepeatEntry{
                            .hash             = h,
                            .rendered         = std::move(rendered),
                            .first_occurrence = &node,
                            .count            = 1,
                            .size             = node_size,
                            .first_preorder   = my_order,
                        }
                    );
                }
            }
            for (const auto &child : node.children) {
                CollectNonLeafSubtrees(
                    *child, preorder_counter, vars, bitwidth, by_hash, entries
                );
            }
        }

        bool IsAncestorOf(const Expr *ancestor, const Expr *descendant) {
            if (ancestor == descendant) { return true; }
            for (const auto &c : ancestor->children) {
                if (IsAncestorOf(c.get(), descendant)) { return true; }
            }
            return false;
        }

        std::unique_ptr< Expr > ReplaceRepeatsWithVirtual(
            const Expr &node, const std::vector< DeduplicatedAtom > &atoms,
            const std::vector< std::string > &vars, uint32_t bitwidth
        ) {
            // Check if this node matches a replacement target.
            if (node.kind != Expr::Kind::kConstant && node.kind != Expr::Kind::kVariable) {
                size_t h = std::hash< Expr >{}(node);
                for (const auto &atom : atoms) {
                    if (atom.hash == h) {
                        auto rendered = Render(node, vars, bitwidth);
                        if (atom.rendered == rendered) {
                            return Expr::Variable(atom.virtual_index);
                        }
                    }
                }
            }

            auto result          = std::make_unique< Expr >();
            result->kind         = node.kind;
            result->constant_val = node.constant_val;
            result->var_index    = node.var_index;

            for (const auto &child : node.children) {
                result->children.push_back(
                    ReplaceRepeatsWithVirtual(*child, atoms, vars, bitwidth)
                );
            }
            return result;
        }

    } // namespace

    Result< PassResult >
    RunLiftArithmeticAtoms(const WorkItem &item, OrchestratorContext &ctx) {
        COBRA_ZONE_N("RunLiftArithmeticAtoms");
        if (!std::holds_alternative< AstPayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &ast               = std::get< AstPayload >(item.payload);
        const auto &vars              = ActiveAstVars(item, ctx);
        const auto &active_eval       = ActiveAstEvaluator(item, ctx);
        const auto original_var_count = static_cast< uint32_t >(vars.size());

        // Collect liftable arithmetic atoms under bitwise parents.
        std::vector< LiftCandidate > candidates;
        bool root_is_bitwise = IsBitwiseKind(ast.expr->kind);
        for (const auto &child : ast.expr->children) {
            CollectLiftableAtoms(*child, root_is_bitwise, vars, ctx.bitwidth, candidates);
        }

        if (candidates.empty()) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        // Deduplicate by structural hash + rendered string equality.
        auto atoms = DeduplicateAtoms(candidates, original_var_count);

        // Check max_vars limit.
        auto total_vars = original_var_count + static_cast< uint32_t >(atoms.size());
        if (total_vars > ctx.opts.max_vars) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kBlocked,
                    .disposition = ItemDisposition::kRetainCurrent,
                    .reason =
                        ReasonDetail{
                            .top = {
                                .code = {
                                    ReasonCategory::kResourceLimit,
                                    ReasonDomain::kOrchestrator,
                                },
                                .message =
                                    "Lifting would exceed max_vars ("
                                    + std::to_string(total_vars)
                                    + " > "
                                    + std::to_string(ctx.opts.max_vars)
                                    + ")",
                            },
                        },
                }
            );
        }

        // Build outer expression by replacing atoms with virtual vars.
        auto outer_expr = ReplaceAtomsWithVirtual(*ast.expr, false, atoms, vars, ctx.bitwidth);

        // Build extended variable list: original vars + virtual vars.
        std::vector< std::string > outer_vars = vars;
        for (size_t i = 0; i < atoms.size(); ++i) {
            outer_vars.push_back("v" + std::to_string(i));
        }

        // Build bindings.
        std::vector< LiftedBinding > bindings;
        bindings.reserve(atoms.size());
        for (const auto &atom : atoms) {
            std::vector< uint32_t > support;
            CollectVarSupport(*atom.subtree, support);

            bindings.push_back(
                LiftedBinding{
                    .kind             = LiftedValueKind::kArithmeticAtom,
                    .outer_var_index  = atom.virtual_index,
                    .subtree          = CloneExpr(*atom.subtree),
                    .structural_hash  = atom.hash,
                    .original_support = std::move(support),
                }
            );
        }

        // Boolean signature for the outer expression.
        auto outer_num_vars = static_cast< uint32_t >(outer_vars.size());
        auto outer_sig = EvaluateBooleanSignature(*outer_expr, outer_num_vars, ctx.bitwidth);

        // Source signature for verification.
        auto source_sig = EvaluateBooleanSignature(*ast.expr, original_var_count, ctx.bitwidth);

        auto baseline_cost = ComputeCost(*ast.expr).cost;

        // Emit the skeleton.
        // Evaluator is std::nullopt — RunBuildSignatureState will
        // recompute from the outer_expr when needed.
        WorkItem skel_item;
        skel_item.payload = LiftedSkeletonPayload{
            .outer_expr = std::move(outer_expr),
            .outer_ctx =
                AstSolveContext{
                                .vars      = std::move(outer_vars),
                                .evaluator = std::nullopt,
                                .input_sig = std::move(outer_sig),
                                },
            .bindings           = std::move(bindings),
            .original_var_count = original_var_count,
            .baseline_cost      = baseline_cost,
            .source_sig         = std::move(source_sig),
            .original_ctx =
                AstSolveContext{
                                .vars      = vars,
                                .evaluator = active_eval,
                                },
        };
        skel_item.features    = item.features;
        skel_item.metadata    = item.metadata;
        skel_item.depth       = item.depth;
        skel_item.rewrite_gen = item.rewrite_gen;
        skel_item.history     = item.history;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kRetainCurrent;
        result.next.push_back(std::move(skel_item));
        return Ok(std::move(result));
    }

    Result< PassResult >
    RunLiftRepeatedSubexpressions(const WorkItem &item, OrchestratorContext &ctx) {
        COBRA_ZONE_N("RunLiftRepeatedSubexpressions");
        if (!std::holds_alternative< AstPayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &ast               = std::get< AstPayload >(item.payload);
        const auto &vars              = ActiveAstVars(item, ctx);
        const auto &active_eval       = ActiveAstEvaluator(item, ctx);
        const auto original_var_count = static_cast< uint32_t >(vars.size());

        // Guard: skip for very large expression trees to avoid
        // stack overflow in recursive helpers.
        constexpr uint32_t kMaxLiftableNodes = 50'000;
        if (CountNodes(*ast.expr) > kMaxLiftableNodes) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        // Candidate discovery: traverse all non-leaf subtrees,
        // group by structural hash + rendered string equality,
        // track occurrence count, size, and first pre-order index.
        std::unordered_map< size_t, std::vector< size_t > > by_hash;
        std::vector< RepeatEntry > entries;
        uint32_t preorder = 0;
        CollectNonLeafSubtrees(*ast.expr, preorder, vars, ctx.bitwidth, by_hash, entries);

        // Filter: keep only candidates with count >= 2 AND size >= 4.
        std::vector< RepeatEntry * > viable;
        for (auto &entry : entries) {
            if (entry.count >= 2 && entry.size >= kMinRepeatSize) { viable.push_back(&entry); }
        }

        if (viable.empty()) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        // Sort by impact: benefit = (count - 1) * size, descending.
        // Tie-break by larger size, then deeper (higher preorder), then
        // higher count.
        std::sort(viable.begin(), viable.end(), [](const auto *a, const auto *b) {
            auto benefit_a = static_cast< uint64_t >(a->count - 1) * a->size;
            auto benefit_b = static_cast< uint64_t >(b->count - 1) * b->size;
            if (benefit_a != benefit_b) { return benefit_a > benefit_b; }
            if (a->size != b->size) { return a->size > b->size; }
            if (a->first_preorder != b->first_preorder) {
                return a->first_preorder > b->first_preorder;
            }
            return a->count > b->count;
        });

        // Greedy non-overlapping selection, bounded by variable budget.
        auto var_budget = ctx.opts.max_vars > original_var_count
            ? ctx.opts.max_vars - original_var_count
            : 0U;

        std::vector< const RepeatEntry * > selected;
        for (const auto *cand : viable) {
            if (selected.size() >= var_budget) { break; }
            bool overlaps = false;
            for (const auto *sel : selected) {
                if (IsAncestorOf(sel->first_occurrence, cand->first_occurrence)
                    || IsAncestorOf(cand->first_occurrence, sel->first_occurrence))
                {
                    overlaps = true;
                    break;
                }
            }
            if (!overlaps) { selected.push_back(cand); }
        }

        if (selected.empty()) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        // Build DeduplicatedAtom vector for replacement.
        std::vector< DeduplicatedAtom > atoms;
        atoms.reserve(selected.size());
        for (size_t i = 0; i < selected.size(); ++i) {
            auto vi = original_var_count + static_cast< uint32_t >(i);
            atoms.push_back(
                DeduplicatedAtom{
                    .subtree       = selected[i]->first_occurrence,
                    .hash          = selected[i]->hash,
                    .rendered      = selected[i]->rendered,
                    .virtual_index = vi,
                }
            );
        }

        // Build outer expression by replacing repeated subtrees.
        auto outer_expr = ReplaceRepeatsWithVirtual(*ast.expr, atoms, vars, ctx.bitwidth);

        // Build extended variable list: original vars + virtual vars.
        std::vector< std::string > outer_vars = vars;
        for (size_t i = 0; i < selected.size(); ++i) {
            outer_vars.push_back("r" + std::to_string(i));
        }

        // Build bindings — use atoms (which have copies of
        // rendered strings) instead of selected for subtree access.
        std::vector< LiftedBinding > bindings;
        bindings.reserve(atoms.size());
        for (const auto &atom : atoms) {
            std::vector< uint32_t > support;
            CollectVarSupport(*atom.subtree, support);

            bindings.push_back(
                LiftedBinding{
                    .kind             = LiftedValueKind::kRepeatedSubexpression,
                    .outer_var_index  = atom.virtual_index,
                    .subtree          = CloneExpr(*atom.subtree),
                    .structural_hash  = atom.hash,
                    .original_support = std::move(support),
                }
            );
        }

        // Boolean signature for the outer expression.
        auto outer_num_vars = static_cast< uint32_t >(outer_vars.size());
        auto outer_sig = EvaluateBooleanSignature(*outer_expr, outer_num_vars, ctx.bitwidth);

        // Source signature for verification.
        auto source_sig = EvaluateBooleanSignature(*ast.expr, original_var_count, ctx.bitwidth);

        auto baseline_cost = ComputeCost(*ast.expr).cost;

        // Emit the skeleton.
        WorkItem skel_item;
        skel_item.payload = LiftedSkeletonPayload{
            .outer_expr = std::move(outer_expr),
            .outer_ctx =
                AstSolveContext{
                                .vars      = std::move(outer_vars),
                                .evaluator = std::nullopt,
                                .input_sig = std::move(outer_sig),
                                },
            .bindings           = std::move(bindings),
            .original_var_count = original_var_count,
            .baseline_cost      = baseline_cost,
            .source_sig         = std::move(source_sig),
            .original_ctx =
                AstSolveContext{
                                .vars      = vars,
                                .evaluator = active_eval,
                                },
        };
        skel_item.features    = item.features;
        skel_item.metadata    = item.metadata;
        skel_item.depth       = item.depth;
        skel_item.rewrite_gen = item.rewrite_gen;
        skel_item.history     = item.history;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kRetainCurrent;
        result.next.push_back(std::move(skel_item));
        return Ok(std::move(result));
    }

} // namespace cobra
