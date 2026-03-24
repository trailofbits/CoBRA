#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/MixedProductRewriter.h"
#include "cobra/core/MultivarPolyRecovery.h"
#include "cobra/core/OperandSimplifier.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/ProductIdentityRecoverer.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/TemplateDecomposer.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <print>
#include <string>
#include <vector>

using namespace cobra;

namespace {

    // 0-var: L67, L165, L196, L211, L212, L478, L495
    // 1-var: L154, L223, L271, L275, L278, L283, L370, L378, L476
    constexpr std::array kTargetLines = { 67,  165, 196, 211, 212, 478, 495, 154,
                                          223, 271, 275, 278, 283, 370, 378, 476 };

    size_t find_separator(const std::string &line) {
        int depth         = 0;
        size_t last_comma = std::string::npos;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '(') {
                ++depth;
            } else if (line[i] == ')') {
                --depth;
            } else if (line[i] == '\t' && depth == 0) {
                return i;
            } else if (line[i] == ',' && depth == 0) {
                last_comma = i;
            }
        }
        return last_comma;
    }

    std::string trim(const std::string &s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { return ""; }
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    bool is_target(int line_num) {
        return std::ranges::any_of(kTargetLines, [line_num](int t) { return t == line_num; });
    }

    std::string route_name(Route r) {
        switch (r) {
            case Route::kBitwiseOnly:
                return "BitwiseOnly";
            case Route::kMultilinear:
                return "Multilinear";
            case Route::kPowerRecovery:
                return "PowerRecovery";
            case Route::kMixedRewrite:
                return "MixedRewrite";
            case Route::kUnsupported:
                return "Unsupported";
        }
        return "Unknown";
    }

    std::string sig_to_hex(const std::vector< uint64_t > &sig) {
        std::string s = "[";
        for (size_t i = 0; i < sig.size(); ++i) {
            if (i > 0) { s += ", "; }
            s += std::format("0x{:X}", sig[i]);
        }
        s += "]";
        return s;
    }

    std::string expr_str(const Expr &e) {
        switch (e.kind) {
            case Expr::Kind::kConstant:
                return std::format("{}", e.constant_val);
            case Expr::Kind::kVariable:
                return std::format("v{}", e.var_index);
            case Expr::Kind::kAdd:
                return "(" + expr_str(*e.children[0]) + "+" + expr_str(*e.children[1]) + ")";
            case Expr::Kind::kMul:
                return "(" + expr_str(*e.children[0]) + "*" + expr_str(*e.children[1]) + ")";
            case Expr::Kind::kNeg:
                return "(-" + expr_str(*e.children[0]) + ")";
            case Expr::Kind::kAnd:
                return "(" + expr_str(*e.children[0]) + "&" + expr_str(*e.children[1]) + ")";
            case Expr::Kind::kOr:
                return "(" + expr_str(*e.children[0]) + "|" + expr_str(*e.children[1]) + ")";
            case Expr::Kind::kXor:
                return "(" + expr_str(*e.children[0]) + "^" + expr_str(*e.children[1]) + ")";
            case Expr::Kind::kNot:
                return "(~" + expr_str(*e.children[0]) + ")";
            default:
                return "?";
        }
    }

    // Build a SignatureContext and mapped evaluator for reduced vars
    SignatureContext build_ctx(
        const EliminationResult &elim, const std::vector< std::string > &vars,
        const Evaluator &eval
    ) {
        SignatureContext ctx;
        ctx.vars = elim.real_vars;
        ctx.original_indices.reserve(elim.real_vars.size());
        for (const auto &rv : elim.real_vars) {
            for (size_t j = 0; j < vars.size(); ++j) {
                if (vars[j] == rv) {
                    ctx.original_indices.push_back(static_cast< uint32_t >(j));
                    break;
                }
            }
        }
        if (elim.real_vars.size() == vars.size()) {
            ctx.eval = eval;
        } else {
            auto idx = ctx.original_indices;
            ctx.eval = [eval, idx,
                        n = vars.size()](const std::vector< uint64_t > &rv) -> uint64_t {
                std::vector< uint64_t > full(n, 0);
                for (size_t i = 0; i < idx.size(); ++i) { full[idx[i]] = rv[i]; }
                return eval(full);
            };
        }
        return ctx;
    }

    // Replicate RunSupportedPipeline using public APIs
    std::optional< SimplifyOutcome > try_supported_pipeline(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        const Options &opts, const char *label
    ) {
        const auto kNv = static_cast< uint32_t >(vars.size());

        // Pattern match
        auto pm = MatchPattern(sig, kNv, opts.bitwidth);
        if (pm && (*pm)->kind == Expr::Kind::kConstant) {
            std::print(
                stderr, "    {}: pattern match constant={}\n", label, (*pm)->constant_val
            );
            SimplifyOutcome outcome;
            outcome.kind     = SimplifyOutcome::Kind::kSimplified;
            outcome.expr     = std::move(*pm);
            outcome.verified = true;
            return outcome;
        }

        // Aux var elimination
        auto elim = EliminateAuxVars(sig, vars);
        std::print(
            stderr, "    {}: real_vars={} reduced_sig={}\n", label, elim.real_vars.size(),
            sig_to_hex(elim.reduced_sig)
        );

        // SignatureSimplifier
        auto ctx = build_ctx(elim, vars, opts.evaluator);
        auto sub = SimplifyFromSignature(elim.reduced_sig, ctx, opts, 0);
        if (sub.has_value()) {
            std::print(
                stderr, "    {}: SigSimplifier => {} verified={}\n", label,
                expr_str(*sub->expr), sub->verified
            );
            SimplifyOutcome outcome;
            outcome.kind       = SimplifyOutcome::Kind::kSimplified;
            outcome.expr       = std::move(sub->expr);
            outcome.sig_vector = elim.reduced_sig;
            outcome.real_vars  = std::move(elim.real_vars);
            outcome.verified   = sub->verified;
            return outcome;
        }

        std::print(stderr, "    {}: SigSimplifier => no result\n", label);
        return std::nullopt;
    }

    void trace_expression(
        int line_num, const std::string &gt, const std::vector< uint64_t > &sig,
        const std::vector< std::string > &vars, const Expr &folded_expr, uint32_t bitwidth
    ) {
        std::print(stderr, "\n========================================");
        std::print(stderr, "================================\n");
        std::print(stderr, "L{} | vars={} | GT: {}\n", line_num, vars.size(), gt);
        std::print(stderr, "  AST: {}\n", expr_str(folded_expr));

        auto cls = ClassifyStructural(folded_expr);
        std::print(
            stderr, "  Route: {} | Semantic: {} | Flags: 0x{:X}\n", route_name(cls.route),
            static_cast< int >(cls.semantic), static_cast< uint32_t >(cls.flags)
        );

        // Aux var elimination (top-level)
        auto elim = EliminateAuxVars(sig, vars);
        std::print(
            stderr, "  AuxVarElim: {} real, {} spurious\n", elim.real_vars.size(),
            elim.spurious_vars.size()
        );
        for (const auto &rv : elim.real_vars) { std::print(stderr, "    real: {}\n", rv); }
        for (const auto &sv : elim.spurious_vars) {
            std::print(stderr, "    spurious: {}\n", sv);
        }
        std::print(stderr, "  Reduced sig: {}\n", sig_to_hex(elim.reduced_sig));

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(folded_expr));
        Options opts{ .bitwidth = bitwidth, .max_vars = 16, .spot_check = true };
        opts.evaluator = [folded_ptr, bitwidth](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, bitwidth);
        };

        // === Step 1: Opportunistic supported pipeline ===
        std::print(stderr, "\n  [Step 1] Opportunistic supported pipeline\n");
        auto step1 = try_supported_pipeline(sig, vars, opts, "Step1");
        if (step1.has_value()) {
            // Verify in original space
            auto check = FullWidthCheckEval(
                opts.evaluator, static_cast< uint32_t >(vars.size()), *step1->expr, bitwidth
            );
            std::print(stderr, "    VerifyOrigSpace: {}\n", check.passed ? "PASS" : "FAIL");
            if (!check.passed && !check.failing_input.empty()) {
                std::print(stderr, "    failing_input: [");
                for (size_t i = 0; i < check.failing_input.size(); ++i) {
                    if (i > 0) { std::print(stderr, ", "); }
                    std::print(stderr, "0x{:X}", check.failing_input[i]);
                }
                std::print(stderr, "]\n");
            }
            if (check.passed) {
                std::print(stderr, "    => SOLVED at Step 1\n");
                return;
            }
        }

        auto current_expr = CloneExpr(folded_expr);

        // === Step 2: Operand simplification ===
        std::print(stderr, "\n  [Step 2] OperandSimplifier\n");
        auto op_result = SimplifyMixedOperands(std::move(current_expr), vars, opts);
        current_expr   = std::move(op_result.expr);
        std::print(stderr, "    changed: {}\n", op_result.changed ? "yes" : "no");
        if (op_result.changed) {
            std::print(stderr, "    result: {}\n", expr_str(*current_expr));
            auto new_sig = EvaluateBooleanSignature(
                *current_expr, static_cast< uint32_t >(vars.size()), bitwidth
            );
            auto reentry = try_supported_pipeline(new_sig, vars, opts, "Step2-re");
            if (reentry.has_value()) {
                auto check = FullWidthCheckEval(
                    opts.evaluator, static_cast< uint32_t >(vars.size()), *reentry->expr,
                    bitwidth
                );
                std::print(stderr, "    VerifyOrigSpace: {}\n", check.passed ? "PASS" : "FAIL");
                if (check.passed) {
                    std::print(stderr, "    => SOLVED at Step 2\n");
                    return;
                }
            }
        }

        // === Step 2.5: Product identity collapse ===
        std::print(stderr, "\n  [Step 2.5] ProductIdentityCollapse\n");
        auto pi_result = CollapseProductIdentities(std::move(current_expr), vars, opts);
        current_expr   = std::move(pi_result.expr);
        std::print(stderr, "    changed: {}\n", pi_result.changed ? "yes" : "no");
        if (pi_result.changed) {
            std::print(stderr, "    result: {}\n", expr_str(*current_expr));
        }

        // === Step 2.75: Template decomposition ===
        std::print(stderr, "\n  [Step 2.75] TemplateDecomposition\n");
        {
            auto rv_count = static_cast< uint32_t >(elim.real_vars.size());
            auto ctx      = build_ctx(elim, vars, opts.evaluator);

            auto td = TryTemplateDecomposition(ctx, opts, rv_count, nullptr);
            std::print(
                stderr, "    reduced ({} vars): {}\n", rv_count,
                td.has_value() ? "FOUND" : "none"
            );
            if (td.has_value()) {
                std::print(
                    stderr, "    candidate: {} verified={}\n", expr_str(*td->expr), td->verified
                );
                // Verify in original space with all vars
                auto check = FullWidthCheckEval(
                    opts.evaluator, static_cast< uint32_t >(vars.size()), *td->expr, bitwidth
                );
                std::print(stderr, "    VerifyOrigSpace: {}\n", check.passed ? "PASS" : "FAIL");
                if (check.passed) {
                    std::print(stderr, "    => SOLVED at Step 2.75 (reduced)\n");
                    return;
                }
            }

            // Full variables attempt
            if (elim.real_vars.size() < vars.size()) {
                SignatureContext full_ctx;
                full_ctx.vars = vars;
                full_ctx.original_indices.resize(vars.size());
                for (uint32_t vi = 0; vi < vars.size(); ++vi) {
                    full_ctx.original_indices[vi] = vi;
                }
                full_ctx.eval = opts.evaluator;
                auto all_vars = static_cast< uint32_t >(vars.size());
                auto td2      = TryTemplateDecomposition(full_ctx, opts, all_vars, nullptr);
                std::print(
                    stderr, "    full ({} vars): {}\n", all_vars,
                    td2.has_value() ? "FOUND" : "none"
                );
                if (td2.has_value()) {
                    std::print(
                        stderr, "    candidate: {} verified={}\n", expr_str(*td2->expr),
                        td2->verified
                    );
                }
            }
        }

        // === Step 2.8: Evaluator-based polynomial recovery ===
        std::print(stderr, "\n  [Step 2.8] EvaluatorPolyRecovery\n");
        {
            auto fw_elim = EliminateAuxVars(sig, vars, opts.evaluator, bitwidth);
            std::print(
                stderr, "    full-width elim: {} real, {} spurious\n", fw_elim.real_vars.size(),
                fw_elim.spurious_vars.size()
            );

            auto fw_rv_count = static_cast< uint32_t >(fw_elim.real_vars.size());
            if (fw_rv_count <= 6) {
                std::vector< uint32_t > support;
                for (const auto &rv : fw_elim.real_vars) {
                    for (uint32_t j = 0; j < vars.size(); ++j) {
                        if (vars[j] == rv) {
                            support.push_back(j);
                            break;
                        }
                    }
                }

                auto all_vars = static_cast< uint32_t >(vars.size());
                auto poly = RecoverMultivarPoly(opts.evaluator, support, all_vars, bitwidth);
                std::print(
                    stderr, "    poly recovery: {}\n", poly.Succeeded() ? "FOUND" : "none"
                );

                if (poly.Succeeded()) {
                    auto poly_expr = BuildPolyExpr(poly.Payload());
                    if (poly_expr.has_value()) {
                        std::print(stderr, "    candidate: {}\n", expr_str(*poly_expr.value()));
                        auto check = FullWidthCheckEval(
                            opts.evaluator, all_vars, *poly_expr.value(), bitwidth
                        );
                        std::print(stderr, "    verify: {}\n", check.passed ? "PASS" : "FAIL");
                        if (check.passed) {
                            std::print(stderr, "    => SOLVED at Step 2.8\n");
                            return;
                        }
                    } else {
                        std::print(stderr, "    BuildPolyExpr: failed\n");
                    }
                }
            } else {
                std::print(stderr, "    skipped (>6 vars)\n");
            }
        }

        // === Step 3: XOR lowering ===
        std::print(stderr, "\n  [Step 3] XOR lowering (RewriteMixedProducts)\n");
        RewriteOptions rw_opts;
        rw_opts.max_rounds      = 2;
        rw_opts.max_node_growth = 3;
        rw_opts.bitwidth        = bitwidth;

        auto rewritten = RewriteMixedProducts(std::move(current_expr), rw_opts);
        std::print(
            stderr, "    rounds: {} | changed: {}\n", rewritten.rounds_applied,
            rewritten.structure_changed ? "yes" : "no"
        );

        if (rewritten.structure_changed) {
            std::print(stderr, "    rewritten: {}\n", expr_str(*rewritten.expr));
            auto new_cls = ClassifyStructural(*rewritten.expr);
            std::print(stderr, "    post-rewrite route: {}\n", route_name(new_cls.route));

            if (new_cls.route != Route::kMixedRewrite && new_cls.route != Route::kUnsupported) {
                auto new_sig = EvaluateBooleanSignature(
                    *rewritten.expr, static_cast< uint32_t >(vars.size()), bitwidth
                );
                auto reentry = try_supported_pipeline(new_sig, vars, opts, "Step3-re");
                if (reentry.has_value()) {
                    auto check = FullWidthCheckEval(
                        opts.evaluator, static_cast< uint32_t >(vars.size()), *reentry->expr,
                        bitwidth
                    );
                    std::print(
                        stderr, "    VerifyOrigSpace: {}\n", check.passed ? "PASS" : "FAIL"
                    );
                }
            }
        }

        std::print(stderr, "    => UNSOLVED\n");
    }

} // namespace

TEST(QSynthTraceDive, ZeroAndOneVarExpressions) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::string line;
    int line_num = 0;
    int traced   = 0;

    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) { continue; }
        if (!is_target(line_num)) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        std::string gt         = trim(line.substr(sep + 1));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, 64);
        if (!parse_result.has_value()) {
            std::print(stderr, "L{}: PARSE FAILED\n", line_num);
            continue;
        }

        auto ast_result = ParseToAst(obfuscated, 64);
        if (!ast_result.has_value()) { continue; }

        auto folded = FoldConstantBitwise(std::move(ast_result.value().expr), 64);

        trace_expression(
            line_num, gt, parse_result.value().sig, parse_result.value().vars, *folded, 64
        );
        traced++;
    }

    std::print(stderr, "\n========================================");
    std::print(stderr, "================================\n");
    std::print(
        stderr, "Traced {} / {} target expressions\n", traced,
        static_cast< int >(kTargetLines.size())
    );
    EXPECT_EQ(traced, static_cast< int >(kTargetLines.size()));
}
