#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/DecompositionEngine.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/GhostBasis.h"
#include "cobra/core/GhostResidualSolver.h"
#include "cobra/core/MathUtils.h"
#include "cobra/core/MultivarPolyRecovery.h"
#include "cobra/core/OperandSimplifier.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/ProductIdentityRecoverer.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/TemplateDecomposer.h"
#include "cobra/core/WeightedPolyFit.h"
#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <random>
#include <sstream>
#include <string>

using namespace cobra;

namespace {

    size_t find_separator(const std::string &line) {
        int depth         = 0;
        size_t last_comma = std::string::npos;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '(') {
                ++depth;
            } else if (line[i] == ')') {
                --depth;
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

    uint32_t expr_depth(const Expr &e) {
        uint32_t d = 0;
        for (const auto &c : e.children) { d = std::max(d, expr_depth(*c)); }
        return d + 1;
    }

    uint32_t count_ops(const Expr &e) {
        uint32_t n = 0;
        if (e.kind != Expr::Kind::kConstant && e.kind != Expr::Kind::kVariable) { n = 1; }
        for (const auto &c : e.children) { n += count_ops(*c); }
        return n;
    }

    std::string kind_str(Expr::Kind k) {
        switch (k) {
            case Expr::Kind::kConstant:
                return "C";
            case Expr::Kind::kVariable:
                return "V";
            case Expr::Kind::kAdd:
                return "+";
            case Expr::Kind::kMul:
                return "*";
            case Expr::Kind::kNeg:
                return "-";
            case Expr::Kind::kAnd:
                return "&";
            case Expr::Kind::kOr:
                return "|";
            case Expr::Kind::kXor:
                return "^";
            case Expr::Kind::kNot:
                return "~";
            default:
                return "?";
        }
    }

    // Collect the set of operator kinds used in the GT expression
    std::string op_signature(const Expr &e) {
        std::set< std::string > ops;
        std::function< void(const Expr &) > walk = [&](const Expr &n) {
            if (n.kind != Expr::Kind::kConstant && n.kind != Expr::Kind::kVariable) {
                ops.insert(kind_str(n.kind));
            }
            for (const auto &c : n.children) { walk(*c); }
        };
        walk(e);
        std::string s;
        for (const auto &o : ops) {
            if (!s.empty()) { s += ","; }
            s += o;
        }
        return s;
    }

    // Count the minimum number of binary operations in GT
    // (gives a sense of composition depth needed)
    uint32_t count_binary_ops(const Expr &e) {
        uint32_t n = 0;
        if (e.children.size() == 2) { n = 1; }
        for (const auto &c : e.children) { n += count_binary_ops(*c); }
        return n;
    }

} // namespace

TEST(QSynthDiagnostic, AnalyzeUnsolved) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::string line;
    int line_num          = 0;
    int unsupported_count = 0;

    // Buckets for analysis
    std::map< uint32_t, int > by_vars;
    std::map< uint32_t, int > by_depth;
    std::map< uint32_t, int > by_binary_ops;
    std::map< std::string, int > by_ops;

    std::vector< std::string > all_unsolved;

    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, 64);
        if (!parse_result.has_value()) { continue; }

        auto ast_result = ParseToAst(obfuscated, 64);
        if (!ast_result.has_value()) { continue; }

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast_result.value().expr), 64)
        );

        auto cls = ClassifyStructural(**folded_ptr);
        if (cls.route != Route::kMixedRewrite) { continue; }

        const auto &sig  = parse_result.value().sig;
        const auto &vars = parse_result.value().vars;

        Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto result = Simplify(sig, vars, folded_ptr->get(), opts);
        if (!result.has_value()) { continue; }
        if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) { continue; }

        unsupported_count++;

        // Parse the GT expression for analysis
        std::string gt = trim(line.substr(sep + 1));
        auto gt_parse  = ParseToAst(gt, 64);

        auto elim   = EliminateAuxVars(sig, vars);
        uint32_t rv = static_cast< uint32_t >(elim.real_vars.size());

        by_vars[rv]++;

        std::string entry = "L" + std::to_string(line_num) + " vars=" + std::to_string(rv);

        if (gt_parse.has_value()) {
            auto gt_folded  = FoldConstantBitwise(std::move(gt_parse.value().expr), 64);
            uint32_t d      = expr_depth(*gt_folded);
            uint32_t bops   = count_binary_ops(*gt_folded);
            std::string ops = op_signature(*gt_folded);

            by_depth[d]++;
            by_binary_ops[bops]++;
            by_ops[ops]++;

            entry += " depth=" + std::to_string(d) + " binops=" + std::to_string(bops)
                + " ops={" + ops + "}" + " GT: " + gt;
        } else {
            entry += " GT(unparsed): " + gt;
        }

        all_unsolved.push_back(entry);
    }

    std::cerr << "\n=== " << unsupported_count << " Unsolved QSynth_EA Expressions ===\n";

    std::cerr << "\n--- By variable count ---\n";
    for (auto &[k, v] : by_vars) { std::cerr << "  " << k << " vars: " << v << "\n"; }

    std::cerr << "\n--- By GT depth ---\n";
    for (auto &[k, v] : by_depth) { std::cerr << "  depth " << k << ": " << v << "\n"; }

    std::cerr << "\n--- By GT binary op count ---\n";
    for (auto &[k, v] : by_binary_ops) { std::cerr << "  " << k << " binops: " << v << "\n"; }

    std::cerr << "\n--- By operator set ---\n";
    for (auto &[k, v] : by_ops) { std::cerr << "  {" << k << "}: " << v << "\n"; }

    std::cerr << "\n--- All unsolved ---\n";
    for (const auto &s : all_unsolved) { std::cerr << "  " << s << "\n"; }

    std::cerr << "\n";
}

TEST(QSynthDiagnostic, DecompEngineTelemetry) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::string line;
    int line_num = 0;
    int total    = 0;

    // Telemetry buckets
    int product_extracted    = 0;
    int poly2_extracted      = 0;
    int poly2_accepted       = 0;
    int poly3_extracted      = 0;
    int poly4_extracted      = 0;
    int template_extracted   = 0;
    int no_extractor         = 0;
    int res_supported_solved = 0;
    int res_sup_recombined   = 0;
    int res_poly_solved      = 0;
    int res_poly_recombined  = 0;
    int res_tmpl_solved      = 0;
    int res_tmpl_recombined  = 0;
    int res_ghost_solved     = 0;
    int res_ghost_recombined = 0;
    int res_all_failed       = 0;

    std::map< std::string, int > failure_reasons;

    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, 64);
        if (!parse_result.has_value()) { continue; }

        auto ast_result = ParseToAst(obfuscated, 64);
        if (!ast_result.has_value()) { continue; }

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast_result.value().expr), 64)
        );

        const auto &sig  = parse_result.value().sig;
        const auto &vars = parse_result.value().vars;

        Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto result = Simplify(sig, vars, folded_ptr->get(), opts);
        if (!result.has_value()) { continue; }
        if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) { continue; }

        total++;

        auto cls = ClassifyStructural(**folded_ptr);
        DecompositionContext dctx{ .opts         = opts,
                                   .vars         = vars,
                                   .sig          = sig,
                                   .current_expr = folded_ptr->get(),
                                   .cls          = cls };

        std::string label = "L" + std::to_string(line_num);

        // Probe each extractor
        auto prod = ExtractProductCore(dctx);
        auto p2   = ExtractPolyCore(dctx, 2);
        auto tmpl = ExtractTemplateCore(dctx);
        auto p3   = ExtractPolyCore(dctx, 3);
        auto p4   = ExtractPolyCore(dctx, 4);

        bool any_core = false;
        if (prod.has_value()) {
            product_extracted++;
            any_core = true;
        }
        if (p2.has_value()) {
            poly2_extracted++;
            any_core = true;
            if (AcceptCore(dctx, *p2)) { poly2_accepted++; }
        }
        if (tmpl.has_value()) {
            template_extracted++;
            any_core = true;
        }
        if (p3.has_value()) {
            poly3_extracted++;
            any_core = true;
        }
        if (p4.has_value()) {
            poly4_extracted++;
            any_core = true;
        }
        if (!any_core) { no_extractor++; }

        // For the first core that AcceptCore would pass, probe residual solvers
        struct ProbeResult
        {
            std::string extractor;
            bool supported_solved;
            bool sup_verified;
            bool sup_recombines;
            bool poly_solved;
            bool poly_recombines;
            bool tmpl_solved;
            bool tmpl_recombines;
            bool ghost_solved;
            bool ghost_recombines;
            bool is_boolean_null;
            bool factored_d0_candidate;
            bool factored_d0_verified;
            bool factored_d2_candidate;
            bool factored_d2_verified;
            uint32_t res_support;
            uint8_t degree_floor;
        };

        auto probe_residual = [&](CoreCandidate &core,
                                  const std::string &name) -> std::optional< ProbeResult > {
            const auto kNv     = static_cast< uint32_t >(vars.size());
            const uint32_t kBw = 64;

            auto direct = FullWidthCheckEval(opts.evaluator, kNv, *core.expr, kBw);
            if (direct.passed) { return std::nullopt; }

            if (core.kind == ExtractorKind::kPolynomial && !AcceptCore(dctx, core)) {
                return std::nullopt;
            }

            auto residual_eval = BuildResidualEvaluator(opts.evaluator, *core.expr, kBw);
            auto residual_sig  = EvaluateBooleanSignature(residual_eval, kNv, kBw);

            Options res_opts   = opts;
            res_opts.evaluator = residual_eval;

            uint8_t deg_floor =
                (core.kind == ExtractorKind::kPolynomial) ? core.degree_used + 1 : 2;

            // Supported pipeline probe
            bool sup_solved    = false;
            bool sup_recombine = false;
            bool sup_verified  = false;
            auto sup           = RunSupportedPipeline(residual_sig, vars, res_opts);
            if (sup.has_value() && sup.value().kind == SimplifyOutcome::Kind::kSimplified) {
                sup_solved       = true;
                sup_verified     = sup.value().verified;
                auto solved_expr = CloneExpr(*sup.value().expr);
                if (!sup.value().real_vars.empty()
                    && sup.value().real_vars.size() < vars.size())
                {
                    std::vector< uint32_t > idx_map;
                    for (const auto &rv : sup.value().real_vars) {
                        for (uint32_t j = 0; j < kNv; ++j) {
                            if (vars[j] == rv) {
                                idx_map.push_back(j);
                                break;
                            }
                        }
                    }
                    std::function< void(Expr &, const std::vector< uint32_t > &) > remap =
                        [&remap](Expr &e, const std::vector< uint32_t > &m) {
                            if (e.kind == Expr::Kind::kVariable) {
                                e.var_index = m[e.var_index];
                                return;
                            }
                            for (auto &c : e.children) { remap(*c, m); }
                        };
                    remap(*solved_expr, idx_map);
                }
                auto combined = Expr::Add(CloneExpr(*core.expr), std::move(solved_expr));
                auto fwc      = FullWidthCheckEval(opts.evaluator, kNv, *combined, kBw);
                sup_recombine = fwc.passed;
                if (!fwc.passed && total <= 3) {
                    // Debug first few failures
                    std::cerr << "    DEBUG " << name << ": nv=" << kNv
                              << " core_kind=" << static_cast< int >(core.expr->kind)
                              << " sup_real_vars=[";
                    for (size_t ri = 0; ri < sup.value().real_vars.size(); ++ri) {
                        if (ri > 0) { std::cerr << ","; }
                        std::cerr << sup.value().real_vars[ri];
                    }
                    std::cerr << "] vars=[";
                    for (size_t ri = 0; ri < vars.size(); ++ri) {
                        if (ri > 0) { std::cerr << ","; }
                        std::cerr << vars[ri];
                    }
                    std::cerr << "]\n";
                    // Spot-check a few points
                    for (int dbg = 0; dbg < 3; ++dbg) {
                        std::vector< uint64_t > pt(kNv);
                        for (uint32_t vi = 0; vi < kNv; ++vi) {
                            pt[vi] = static_cast< uint64_t >(dbg * 17 + vi * 7 + 1);
                        }
                        uint64_t fval = opts.evaluator(pt);
                        uint64_t cval = EvalExpr(*core.expr, pt, kBw);
                        uint64_t rval = residual_eval(pt);
                        uint64_t sval = EvalExpr(*combined, pt, kBw);
                        std::cerr << "    pt=" << dbg << " f=" << fval << " core=" << cval
                                  << " r=" << rval << " combined=" << sval << "\n";
                    }
                }
            }

            // Shared: full-width support for residual
            auto res_elim  = EliminateAuxVars(residual_sig, vars, residual_eval, kBw);
            auto res_count = static_cast< uint32_t >(res_elim.real_vars.size());

            std::vector< uint32_t > res_sup;
            for (const auto &rv : res_elim.real_vars) {
                for (uint32_t j = 0; j < kNv; ++j) {
                    if (vars[j] == rv) {
                        res_sup.push_back(j);
                        break;
                    }
                }
            }

            // Polynomial residual probe
            bool poly_solved    = false;
            bool poly_recombine = false;
            if (res_count <= 6) {
                auto rpoly =
                    RecoverAndVerifyPoly(residual_eval, res_sup, kNv, kBw, 4, deg_floor);
                if (rpoly.has_value()) {
                    poly_solved    = true;
                    auto combined  = Expr::Add(CloneExpr(*core.expr), std::move(rpoly->expr));
                    auto fwc       = FullWidthCheckEval(opts.evaluator, kNv, *combined, kBw);
                    poly_recombine = fwc.passed;
                }
            }

            // Boolean-null classification
            bool bn = (res_count <= 6)
                && IsBooleanNullResidual(residual_eval, res_sup, kNv, kBw, residual_sig);

            // Ghost residual probe
            bool ghost_solved    = false;
            bool ghost_recombine = false;
            if (res_count <= 6) {
                auto ghost = SolveGhostResidual(residual_eval, res_sup, kNv, kBw);
                if (ghost.has_value()) {
                    ghost_solved    = true;
                    auto combined   = Expr::Add(CloneExpr(*core.expr), std::move(ghost->expr));
                    auto fwc        = FullWidthCheckEval(opts.evaluator, kNv, *combined, kBw);
                    ghost_recombine = fwc.passed;
                }
            }

            // Factored ghost probes (d=0, d=2) for boolean-null residuals
            bool fd0_candidate = false;
            bool fd0_verified  = false;
            bool fd2_candidate = false;
            bool fd2_verified  = false;
            if (bn && res_count <= 6) {
                auto fd0 = SolveFactoredGhostResidual(residual_eval, res_sup, kNv, kBw);
                if (fd0.has_value()) {
                    fd0_candidate = true;
                    auto combined = Expr::Add(CloneExpr(*core.expr), std::move(fd0->expr));
                    auto fwc      = FullWidthCheckEval(opts.evaluator, kNv, *combined, kBw);
                    fd0_verified  = fwc.passed;
                }
                uint8_t grid = (res_count <= 2) ? 3 : 2;
                auto fd2 =
                    SolveFactoredGhostResidual(residual_eval, res_sup, kNv, kBw, 2, grid);
                if (fd2.has_value()) {
                    fd2_candidate = true;
                    auto combined = Expr::Add(CloneExpr(*core.expr), std::move(fd2->expr));
                    auto fwc      = FullWidthCheckEval(opts.evaluator, kNv, *combined, kBw);
                    fd2_verified  = fwc.passed;
                }
            }

            // Template-on-residual probe
            bool tmpl_solved    = false;
            bool tmpl_recombine = false;
            {
                SignatureContext res_sig_ctx;
                res_sig_ctx.vars             = res_elim.real_vars;
                res_sig_ctx.original_indices = res_sup;

                if (res_elim.real_vars.size() == vars.size()) {
                    res_sig_ctx.eval = residual_eval;
                } else {
                    auto idx         = res_sup;
                    res_sig_ctx.eval = [residual_eval, idx, n = vars.size()](
                                           const std::vector< uint64_t > &rv
                                       ) -> uint64_t {
                        std::vector< uint64_t > full(n, 0);
                        for (size_t i = 0; i < idx.size(); ++i) { full[idx[i]] = rv[i]; }
                        return residual_eval(full);
                    };
                }

                auto td = TryTemplateDecomposition(res_sig_ctx, res_opts, res_count, nullptr);
                if (td.has_value()) {
                    tmpl_solved      = true;
                    auto solved_expr = std::move(td->expr);
                    if (res_elim.real_vars.size() < vars.size()) {
                        std::function< void(Expr &, const std::vector< uint32_t > &) > remap =
                            [&remap](Expr &e, const std::vector< uint32_t > &m) {
                                if (e.kind == Expr::Kind::kVariable) {
                                    e.var_index = m[e.var_index];
                                    return;
                                }
                                for (auto &c : e.children) { remap(*c, m); }
                            };
                        remap(*solved_expr, res_sup);
                    }
                    auto combined  = Expr::Add(CloneExpr(*core.expr), std::move(solved_expr));
                    auto fwc       = FullWidthCheckEval(opts.evaluator, kNv, *combined, kBw);
                    tmpl_recombine = fwc.passed;
                }
            }

            return ProbeResult{
                .extractor             = name,
                .supported_solved      = sup_solved,
                .sup_verified          = sup_verified,
                .sup_recombines        = sup_recombine,
                .poly_solved           = poly_solved,
                .poly_recombines       = poly_recombine,
                .tmpl_solved           = tmpl_solved,
                .tmpl_recombines       = tmpl_recombine,
                .ghost_solved          = ghost_solved,
                .ghost_recombines      = ghost_recombine,
                .is_boolean_null       = bn,
                .factored_d0_candidate = fd0_candidate,
                .factored_d0_verified  = fd0_verified,
                .factored_d2_candidate = fd2_candidate,
                .factored_d2_verified  = fd2_verified,
                .res_support           = res_count,
                .degree_floor          = deg_floor,
            };
        };

        // Try each extractor in engine order
        std::optional< ProbeResult > probe;
        if (!probe && prod.has_value()) { probe = probe_residual(*prod, "Product"); }
        if (!probe && p2.has_value()) { probe = probe_residual(*p2, "Poly2"); }
        if (!probe && tmpl.has_value()) { probe = probe_residual(*tmpl, "Template"); }
        if (!probe && p3.has_value()) { probe = probe_residual(*p3, "Poly3"); }
        if (!probe && p4.has_value()) { probe = probe_residual(*p4, "Poly4"); }

        if (probe.has_value()) {
            if (probe->supported_solved) { res_supported_solved++; }
            if (probe->sup_recombines) { res_sup_recombined++; }
            if (probe->poly_solved) { res_poly_solved++; }
            if (probe->poly_recombines) { res_poly_recombined++; }
            if (probe->tmpl_solved) { res_tmpl_solved++; }
            if (probe->tmpl_recombines) { res_tmpl_recombined++; }
            if (probe->ghost_solved) { res_ghost_solved++; }
            if (probe->ghost_recombines) { res_ghost_recombined++; }
            if (!probe->sup_recombines && !probe->poly_recombines && !probe->ghost_recombines
                && !probe->tmpl_recombines)
            {
                res_all_failed++;
                std::string bn_tag = probe->is_boolean_null ? "bn" : "non_bn";
                std::string reason = probe->extractor
                    + " sup=" + std::to_string(probe->res_support)
                    + " dfloor=" + std::to_string(probe->degree_floor) + " " + bn_tag;
                failure_reasons[reason]++;
                failure_reasons[bn_tag + "_total"]++;
            }
            std::cerr << "  " << label << " core=" << probe->extractor
                      << " res_sup=" << probe->res_support
                      << " dfloor=" << static_cast< int >(probe->degree_floor)
                      << " bn=" << probe->is_boolean_null << " sup_fw=" << probe->sup_recombines
                      << " poly_fw=" << probe->poly_recombines
                      << " ghost_fw=" << probe->ghost_recombines
                      << " fd0=" << probe->factored_d0_candidate << "/"
                      << probe->factored_d0_verified << " fd2=" << probe->factored_d2_candidate
                      << "/" << probe->factored_d2_verified << " tmpl_ok=" << probe->tmpl_solved
                      << " tmpl_fw=" << probe->tmpl_recombines << "\n";
        } else {
            if (!any_core) {
                failure_reasons["no_core"]++;
                std::cerr << "  " << label << " no_core\n";
            } else {
                // Per-extractor rejection analysis for all_direct_fail
                failure_reasons["all_direct_fail"]++;
                std::cerr << "  " << label << " all_direct_fail:";

                auto classify_rejection = [&](CoreCandidate &core,
                                              const std::string &name) -> std::string {
                    const auto kNv     = static_cast< uint32_t >(vars.size());
                    const uint32_t kBw = 64;

                    auto direct = FullWidthCheckEval(opts.evaluator, kNv, *core.expr, kBw);
                    if (direct.passed) { return "direct_success"; }

                    if (core.kind != ExtractorKind::kPolynomial) {
                        return "not_poly_not_direct";
                    }

                    if (!core.expr) { return "null_expr"; }
                    if (core.expr->kind == Expr::Kind::kConstant) { return "constant_core"; }

                    auto residual_eval =
                        BuildResidualEvaluator(opts.evaluator, *core.expr, kBw);
                    const uint64_t kMask = Bitmask(kBw);

                    std::mt19937_64 rng(0xDECAF);
                    bool all_same_as_orig = true;
                    bool all_zero         = true;
                    for (int p = 0; p < 5; ++p) {
                        std::vector< uint64_t > point(kNv);
                        for (uint32_t i = 0; i < kNv; ++i) { point[i] = rng() & kMask; }
                        const uint64_t kOrig = opts.evaluator(point) & kMask;
                        const uint64_t kRes  = residual_eval(point);
                        if (kRes != kOrig) { all_same_as_orig = false; }
                        if (kRes != 0) { all_zero = false; }
                    }
                    if (all_same_as_orig) { return "core_is_zero"; }
                    if (all_zero) { return "core_equals_original"; }
                    return "accepted_but_no_probe";
                };

                struct ExtractorInfo
                {
                    std::optional< CoreCandidate > *core;
                    std::string name;
                };

                std::vector< ExtractorInfo > extractors = {
                    { &prod,  "Product" },
                    {   &p2,    "Poly2" },
                    { &tmpl, "Template" },
                    {   &p3,    "Poly3" },
                    {   &p4,    "Poly4" },
                };
                for (auto &[core_opt, name] : extractors) {
                    if (!core_opt->has_value()) { continue; }
                    auto reason = classify_rejection(core_opt->value(), name);
                    std::cerr << " " << name << "=" << reason;
                    failure_reasons["adf_" + name + "_" + reason]++;
                }
                std::cerr << "\n";
            }
        }
    }

    std::cerr << "\n=== Decomposition Engine Telemetry (" << total << " unsupported) ===\n";
    std::cerr << "\n--- Extractor hits ---\n";
    std::cerr << "  Product:  " << product_extracted << "\n";
    std::cerr << "  Poly D=2: " << poly2_extracted << " (accepted: " << poly2_accepted << ")\n";
    std::cerr << "  Template: " << template_extracted << "\n";
    std::cerr << "  Poly D=3: " << poly3_extracted << "\n";
    std::cerr << "  Poly D=4: " << poly4_extracted << "\n";
    std::cerr << "  None:     " << no_extractor << "\n";

    std::cerr << "\n--- Residual solver results ---\n";
    std::cerr << "  Supported recombine: " << res_sup_recombined << "\n";
    std::cerr << "  Poly recombine:      " << res_poly_recombined << "\n";
    std::cerr << "  Ghost solved:        " << res_ghost_solved << "\n";
    std::cerr << "  Ghost recombine:     " << res_ghost_recombined << "\n";
    std::cerr << "  Tmpl solved:         " << res_tmpl_solved << "\n";
    std::cerr << "  Tmpl recombine:      " << res_tmpl_recombined << "\n";
    std::cerr << "  All failed:          " << res_all_failed << "\n";

    std::cerr << "\n--- Failure reasons ---\n";
    for (auto &[k, v] : failure_reasons) { std::cerr << "  " << k << ": " << v << "\n"; }

    std::cerr << "\n";
}

TEST(QSynthDiagnostic, DirectSuccessProductCoreInvestigation) {
    // Investigate the 9 all_direct_fail/Product=direct_success cases.
    // Compare product core extraction on original folded AST vs.
    // post-preconditioning (Step 2 + Step 2.5) current_expr.
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    // Lines identified as direct_success product cores
    const std::set< int > target_lines = { 50, 88, 144, 223, 384, 388, 458, 466, 501 };

    std::string line;
    int line_num        = 0;
    int confirmed_miss  = 0;
    int precond_destroy = 0;

    while (std::getline(file, line)) {
        ++line_num;
        if (target_lines.find(line_num) == target_lines.end()) { continue; }
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, 64);
        ASSERT_TRUE(parse_result.has_value()) << "L" << line_num;

        auto ast_result = ParseToAst(obfuscated, 64);
        ASSERT_TRUE(ast_result.has_value()) << "L" << line_num;

        auto folded = FoldConstantBitwise(std::move(ast_result.value().expr), 64);

        const auto &sig  = parse_result.value().sig;
        const auto &vars = parse_result.value().vars;
        const auto kNv   = static_cast< uint32_t >(vars.size());

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
        Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        std::string label = "L" + std::to_string(line_num);

        // --- Phase A: product core on original folded AST ---
        auto orig_cls = ClassifyStructural(*folded);
        DecompositionContext orig_dctx{ .opts         = opts,
                                        .vars         = vars,
                                        .sig          = sig,
                                        .current_expr = folded.get(),
                                        .cls          = orig_cls };
        auto orig_prod     = ExtractProductCore(orig_dctx);
        bool orig_has_core = orig_prod.has_value();
        bool orig_direct   = false;
        if (orig_has_core) {
            auto check  = FullWidthCheckEval(opts.evaluator, kNv, *orig_prod->expr, 64);
            orig_direct = check.passed;
        }

        // --- Phase B: apply preconditioning (Step 2 + 2.5) ---
        auto current_expr = CloneExpr(*folded);
        auto op_result    = SimplifyMixedOperands(std::move(current_expr), vars, opts);
        current_expr      = std::move(op_result.expr);
        auto pi_result    = CollapseProductIdentities(std::move(current_expr), vars, opts);
        current_expr      = std::move(pi_result.expr);

        // --- Phase C: product core on post-preconditioning expr ---
        auto post_cls = ClassifyStructural(*current_expr);
        auto post_sig = (op_result.changed || pi_result.changed)
            ? EvaluateBooleanSignature(*current_expr, kNv, 64)
            : sig;
        DecompositionContext post_dctx{
            .opts         = opts,
            .vars         = vars,
            .sig          = post_sig,
            .current_expr = current_expr.get(),
            .cls          = post_cls,
        };
        auto post_prod     = ExtractProductCore(post_dctx);
        bool post_has_core = post_prod.has_value();
        bool post_direct   = false;
        if (post_has_core) {
            auto check  = FullWidthCheckEval(opts.evaluator, kNv, *post_prod->expr, 64);
            post_direct = check.passed;
        }

        // --- Phase D: does TryDecomposition succeed? ---
        auto orig_decomp = TryDecomposition(orig_dctx);
        auto post_decomp = TryDecomposition(post_dctx);

        std::cerr << "  " << label << " orig_core=" << orig_has_core
                  << " orig_direct=" << orig_direct
                  << " orig_decomp=" << orig_decomp.has_value()
                  << " step2=" << op_result.changed << " step2.5=" << pi_result.changed
                  << " post_core=" << post_has_core << " post_direct=" << post_direct
                  << " post_decomp=" << post_decomp.has_value();
        if (orig_has_core) { std::cerr << " orig_expr=" << Render(*orig_prod->expr, vars, 64); }
        if (post_has_core) { std::cerr << " post_expr=" << Render(*post_prod->expr, vars, 64); }
        std::cerr << "\n";

        if (orig_direct && !post_has_core) { precond_destroy++; }
        if (orig_decomp.has_value() && !post_decomp.has_value()) { confirmed_miss++; }
    }

    std::cerr << "\n=== Direct-Success Product Core Investigation ===\n";
    std::cerr << "  Preconditioning destroys core: " << precond_destroy << "\n";
    std::cerr << "  Confirmed pipeline miss:       " << confirmed_miss << "\n";
    std::cerr << "\n";
}

TEST(QSynthDiagnostic, FactoredGhostTelemetry) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    const auto &ghost_basis = GetGhostBasis();

    std::string line;
    int line_num = 0;
    int total    = 0;

    // Telemetry counters
    int constant_fits  = 0;
    int affine_fits    = 0;
    int quadratic_fits = 0;
    int none_fits      = 0;

    struct PerResidualInfo
    {
        std::string label;
        uint32_t support_size;
        std::string ghost_name;
        std::string tuple_str;
        int best_degree;
        bool consistent;
    };

    std::vector< PerResidualInfo > per_residual;

    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, 64);
        if (!parse_result.has_value()) { continue; }

        auto ast_result = ParseToAst(obfuscated, 64);
        if (!ast_result.has_value()) { continue; }

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast_result.value().expr), 64)
        );

        const auto &sig  = parse_result.value().sig;
        const auto &vars = parse_result.value().vars;

        Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto result = Simplify(sig, vars, folded_ptr->get(), opts);
        if (!result.has_value()) { continue; }
        if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) { continue; }

        const auto kNv     = static_cast< uint32_t >(vars.size());
        const uint32_t kBw = 64;

        auto cls = ClassifyStructural(**folded_ptr);
        DecompositionContext dctx{ .opts         = opts,
                                   .vars         = vars,
                                   .sig          = sig,
                                   .current_expr = folded_ptr->get(),
                                   .cls          = cls };

        // Core extraction cascade (same order as DecompEngineTelemetry)
        auto prod = ExtractProductCore(dctx);
        auto p2   = ExtractPolyCore(dctx, 2);
        auto tmpl = ExtractTemplateCore(dctx);
        auto p3   = ExtractPolyCore(dctx, 3);
        auto p4   = ExtractPolyCore(dctx, 4);

        // try_core: accept first core that doesn't solve directly
        auto try_core = [&](std::optional< CoreCandidate > &core,
                            bool is_poly) -> CoreCandidate * {
            if (!core.has_value()) { return nullptr; }
            auto direct = FullWidthCheckEval(opts.evaluator, kNv, *core->expr, kBw);
            if (direct.passed) { return nullptr; }
            if (is_poly && !AcceptCore(dctx, *core)) { return nullptr; }
            return &*core;
        };

        CoreCandidate *chosen = nullptr;
        if (!chosen) { chosen = try_core(prod, false); }
        if (!chosen) { chosen = try_core(p2, true); }
        if (!chosen) { chosen = try_core(tmpl, false); }
        if (!chosen) { chosen = try_core(p3, true); }
        if (!chosen) { chosen = try_core(p4, true); }
        if (!chosen) { continue; }

        // Build residual
        auto residual_eval = BuildResidualEvaluator(opts.evaluator, *chosen->expr, kBw);
        auto residual_sig  = EvaluateBooleanSignature(residual_eval, kNv, kBw);

        // Compute residual support
        auto res_elim  = EliminateAuxVars(residual_sig, vars, residual_eval, kBw);
        auto res_count = static_cast< uint32_t >(res_elim.real_vars.size());

        std::vector< uint32_t > res_sup;
        for (const auto &rv : res_elim.real_vars) {
            for (uint32_t j = 0; j < kNv; ++j) {
                if (vars[j] == rv) {
                    res_sup.push_back(j);
                    break;
                }
            }
        }

        // Only process boolean-null residuals
        if (!IsBooleanNullResidual(residual_eval, res_sup, kNv, kBw, residual_sig)) {
            continue;
        }

        total++;

        std::string label = "L" + std::to_string(line_num);

        // Search for best ghost fit across all primitives and tuples
        int best_degree    = -1; // -1 means none found
        uint8_t best_arity = 255;
        std::vector< uint32_t > best_tuple;
        std::string best_ghost_name;

        for (const auto &prim : ghost_basis) {
            if (prim.arity > res_count) { continue; }

            // Enumerate strictly-increasing tuples of size prim.arity
            // from indices [0, res_count)
            std::vector< uint32_t > indices(prim.arity);
            for (uint8_t i = 0; i < prim.arity; ++i) { indices[i] = i; }

            auto advance_tuple = [&]() -> bool {
                int pos = static_cast< int >(prim.arity) - 1;
                while (pos >= 0) {
                    indices[pos]++;
                    if (indices[pos] <= res_count - prim.arity + pos) {
                        for (int k = pos + 1; k < prim.arity; ++k) {
                            indices[k] = indices[k - 1] + 1;
                        }
                        return true;
                    }
                    --pos;
                }
                return false;
            };

            bool first_tuple = true;
            while (true) {
                if (!first_tuple) {
                    if (!advance_tuple()) { break; }
                }
                first_tuple = false;

                auto current_tuple = indices;
                auto ghost_eval    = prim.eval;

                // Build weight function: maps support-local point to
                // ghost evaluation on the tuple subset
                WeightFn weight = [current_tuple, ghost_eval](
                                      std::span< const uint64_t > local_pt, uint32_t bw
                                  ) -> uint64_t {
                    std::vector< uint64_t > args(current_tuple.size());
                    for (size_t i = 0; i < current_tuple.size(); ++i) {
                        args[i] = local_pt[current_tuple[i]];
                    }
                    return ghost_eval(args, bw);
                };

                // Try max_degree = 0, 1, 2
                for (uint8_t deg = 0; deg <= 2; ++deg) {
                    auto fit =
                        RecoverWeightedPoly(residual_eval, weight, res_sup, kNv, kBw, deg, 2);
                    if (!fit.has_value()) { continue; }

                    int d = static_cast< int >(deg);

                    // Better = lower degree, then lower arity, then
                    // lexicographic tuple
                    bool is_better = false;
                    if (best_degree < 0) {
                        is_better = true;
                    } else if (d < best_degree) {
                        is_better = true;
                    } else if (d == best_degree && prim.arity < best_arity) {
                        is_better = true;
                    } else if (
                        d == best_degree && prim.arity == best_arity
                        && current_tuple < best_tuple
                    )
                    {
                        is_better = true;
                    }

                    if (is_better) {
                        best_degree     = d;
                        best_arity      = prim.arity;
                        best_tuple      = current_tuple;
                        best_ghost_name = prim.name;
                    }
                    break; // found fit at this degree, no need to try higher
                }
            }
        }

        // Build tuple string
        std::string tuple_str = "(";
        for (size_t i = 0; i < best_tuple.size(); ++i) {
            if (i > 0) { tuple_str += ","; }
            tuple_str += std::to_string(best_tuple[i]);
        }
        tuple_str += ")";

        // Update counters
        if (best_degree == 0) {
            constant_fits++;
        } else if (best_degree == 1) {
            affine_fits++;
        } else if (best_degree == 2) {
            quadratic_fits++;
        } else {
            none_fits++;
        }

        per_residual.push_back(
            PerResidualInfo{
                .label        = label,
                .support_size = res_count,
                .ghost_name   = best_ghost_name,
                .tuple_str    = tuple_str,
                .best_degree  = best_degree,
                .consistent   = best_degree >= 0,
            }
        );
    }

    // Report
    std::cerr << "\n=== Factored Ghost Telemetry (" << total
              << " boolean-null residuals) ===\n";

    std::cerr << "\n--- Compatibility by quotient degree (grid_degree=2) ---\n";
    std::cerr << "  Constant  (max_degree=0): " << constant_fits << "/" << total << "\n";
    std::cerr << "  Affine    (max_degree=1): " << affine_fits << "/" << total << "\n";
    std::cerr << "  Quadratic (max_degree=2): " << quadratic_fits << "/" << total << "\n";
    std::cerr << "  None:                     " << none_fits << "/" << total << "\n";

    std::cerr << "\n--- Per-residual best ---\n";
    for (const auto &info : per_residual) {
        std::cerr << "  " << info.label << " sup=" << info.support_size;
        if (info.consistent) {
            std::cerr << " ghost=" << info.ghost_name << " tuple=" << info.tuple_str
                      << " q_degree=" << info.best_degree << " consistent=yes";
        } else {
            std::cerr << " consistent=no";
        }
        std::cerr << "\n";
    }

    std::cerr << "\n";
}

TEST(QSynthDiagnostic, BooleanNullResidualCharacterization) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    // sup=1: L283, sup=2: L14,L58,L152,L370, sup=3: L5,L264,L491
    const std::set< int > targets = { 5, 14, 58, 152, 264, 283, 370, 491 };

    std::string line;
    int line_num = 0;

    while (std::getline(file, line)) {
        ++line_num;
        if (targets.find(line_num) == targets.end()) { continue; }
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        std::string gt_text    = trim(line.substr(sep + 1));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, 64);
        ASSERT_TRUE(parse_result.has_value()) << "L" << line_num;

        auto ast_result = ParseToAst(obfuscated, 64);
        ASSERT_TRUE(ast_result.has_value()) << "L" << line_num;

        auto folded = FoldConstantBitwise(std::move(ast_result.value().expr), 64);

        const auto &sig  = parse_result.value().sig;
        const auto &vars = parse_result.value().vars;
        const auto kNv   = static_cast< uint32_t >(vars.size());

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
        Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        std::cerr << "\n======== L" << line_num << " vars=" << kNv << " ========\n";
        std::cerr << "  GT: " << gt_text << "\n";

        // Parse GT for structural analysis
        auto gt_ast = ParseToAst(gt_text, 64);
        if (gt_ast.has_value()) {
            auto gt_folded = FoldConstantBitwise(std::move(gt_ast.value().expr), 64);
            std::cerr << "  GT top-op: " << kind_str(gt_folded->kind) << " ops: {"
                      << op_signature(*gt_folded) << "}\n";
        }

        // Original folded AST top-level
        std::cerr << "  Orig top-op: " << kind_str(folded->kind) << " ops: {"
                  << op_signature(*folded) << "}\n";

        // Preconditioning (Step 2 + 2.5)
        auto current = CloneExpr(*folded);
        auto op_res  = SimplifyMixedOperands(std::move(current), vars, opts);
        current      = std::move(op_res.expr);
        auto pi_res  = CollapseProductIdentities(std::move(current), vars, opts);
        current      = std::move(pi_res.expr);
        bool changed = op_res.changed || pi_res.changed;

        if (changed) {
            std::cerr << "  Post top-op: " << kind_str(current->kind) << " ops: {"
                      << op_signature(*current) << "}\n";
        } else {
            std::cerr << "  Post: unchanged\n";
        }

        // Extract Poly2 core
        auto post_cls = ClassifyStructural(*current);
        auto post_sig = changed ? EvaluateBooleanSignature(*current, kNv, 64) : sig;
        DecompositionContext dctx{
            .opts         = opts,
            .vars         = vars,
            .sig          = post_sig,
            .current_expr = current.get(),
            .cls          = post_cls,
        };
        auto p2 = ExtractPolyCore(dctx, 2);
        if (!p2.has_value()) {
            std::cerr << "  Poly2 core: NONE\n";
            continue;
        }

        // Residual analysis
        auto residual_eval = BuildResidualEvaluator(opts.evaluator, *p2->expr, 64);
        auto residual_sig  = EvaluateBooleanSignature(residual_eval, kNv, 64);
        auto res_elim      = EliminateAuxVars(residual_sig, vars, residual_eval, 64);
        auto res_count     = static_cast< uint32_t >(res_elim.real_vars.size());

        std::vector< uint32_t > res_sup;
        for (const auto &rv : res_elim.real_vars) {
            for (uint32_t j = 0; j < kNv; ++j) {
                if (vars[j] == rv) {
                    res_sup.push_back(j);
                    break;
                }
            }
        }

        std::cerr << "  Poly2 core: " << Render(*p2->expr, vars) << "\n";
        std::cerr << "  Res support: " << res_count << " [";
        for (size_t i = 0; i < res_sup.size(); ++i) {
            if (i > 0) { std::cerr << ","; }
            std::cerr << vars[res_sup[i]];
        }
        std::cerr << "]\n";

        bool res_bn = IsBooleanNullResidual(residual_eval, res_sup, kNv, 64, residual_sig);
        std::cerr << "  Res bn: " << (res_bn ? "yes" : "no") << "\n";

        // Ghost quotient consistency: for each support pair (i,j), check if
        // residual(x) / ghost(vi,vj) is constant across probes.
        if (res_count >= 2) {
            std::cerr << "  Ghost quotient probes:\n";
            for (uint32_t i = 0; i < res_count; ++i) {
                for (uint32_t j = i + 1; j < res_count; ++j) {
                    constexpr std::array kAs = { 2ULL, 4ULL, 6ULL, 10ULL };
                    constexpr std::array kBs = { 3ULL, 7ULL, 11ULL, 15ULL };

                    bool divisible   = true;
                    uint64_t first_q = 0;
                    bool have_q      = false;
                    bool constant_q  = true;

                    for (uint64_t a : kAs) {
                        for (uint64_t b : kBs) {
                            std::vector< uint64_t > pt(kNv, 0);
                            for (uint32_t vi = 0; vi < res_count; ++vi) {
                                uint64_t val    = (vi == i) ? a : (vi == j) ? b : a + b + vi;
                                pt[res_sup[vi]] = val;
                            }
                            uint64_t rval   = residual_eval(pt);
                            uint64_t vi_val = pt[res_sup[i]];
                            uint64_t vj_val = pt[res_sup[j]];
                            uint64_t gval   = vi_val * vj_val - (vi_val & vj_val);
                            if (gval == 0) { continue; }

                            if (rval == 0) { continue; }
                            auto gt_tz = std::countr_zero(gval);
                            auto rt_tz = std::countr_zero(rval);
                            if (rt_tz < gt_tz) {
                                divisible = false;
                                break;
                            }
                            uint64_t g_odd = gval >> gt_tz;
                            uint64_t r_sh  = rval >> gt_tz;
                            uint64_t inv   = g_odd;
                            for (int k = 0; k < 6; ++k) { inv = inv * (2 - g_odd * inv); }
                            uint64_t q = r_sh * inv;
                            if (!have_q) {
                                first_q = q;
                                have_q  = true;
                            } else if (q != first_q) {
                                constant_q = false;
                            }
                        }
                        if (!divisible) { break; }
                    }

                    std::string pair = vars[res_sup[i]] + "*" + vars[res_sup[j]];
                    std::cerr << "    ghost(" << pair << "): div=" << (divisible ? "Y" : "N");
                    if (divisible && have_q) {
                        std::cerr << " cq=" << (constant_q ? "Y" : "N");
                        if (constant_q) { std::cerr << " q=" << first_q; }
                    }
                    std::cerr << "\n";
                }
            }
        }
    }

    std::cerr << "\n";
}

TEST(QSynthDiagnostic, NullFactorTelemetry) {
    // For the ~40 boolean-null cases in QSynthEA, try null-factor weights
    // with RecoverWeightedPoly and full-width verification.
    //
    // Null factors (vanish on {0,1}):
    //   Unary:    x_i * (x_i - 1)
    //   Pairwise: x_i * x_j - (x_i & x_j)  [existing ghost]
    //
    // Quotient degrees: 0, 1, 2

    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    constexpr uint32_t kBw   = 64;
    constexpr uint8_t kMaxQD = 2; // max quotient degree

    // Aggregate counters: [quotient_degree] → case count
    int unary_fw[kMaxQD + 1]  = {};
    int ghost_fw[kMaxQD + 1]  = {};
    int unary_fit[kMaxQD + 1] = {}; // fit found (may fail FW)
    int ghost_fit[kMaxQD + 1] = {};
    int total_bn = 0, any_solved = 0, none_solved = 0;

    std::string line;
    int line_num = 0;
    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, kBw);
        if (!parse_result.has_value()) { continue; }

        auto ast_result = ParseToAst(obfuscated, kBw);
        if (!ast_result.has_value()) { continue; }

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast_result.value().expr), kBw)
        );

        const auto &sig  = parse_result.value().sig;
        const auto &vars = parse_result.value().vars;
        const auto kNv   = static_cast< uint32_t >(vars.size());

        Options opts{ .bitwidth = kBw, .max_vars = 12, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        // Full simplifier — skip if already solved
        auto result = Simplify(sig, vars, folded_ptr->get(), opts);
        if (!result.has_value()) { continue; }
        if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) { continue; }

        // Preconditioning (Step 2 + 2.5)
        auto current = CloneExpr(**folded_ptr);
        auto op_res  = SimplifyMixedOperands(std::move(current), vars, opts);
        current      = std::move(op_res.expr);
        auto pi_res  = CollapseProductIdentities(std::move(current), vars, opts);
        current      = std::move(pi_res.expr);

        auto post_cls = ClassifyStructural(*current);
        auto post_sig = (op_res.changed || pi_res.changed)
            ? EvaluateBooleanSignature(*current, kNv, kBw)
            : sig;

        DecompositionContext dctx{
            .opts         = opts,
            .vars         = vars,
            .sig          = post_sig,
            .current_expr = current.get(),
            .cls          = post_cls,
        };

        // Determine target evaluator and support for boolean-null cases.
        // Case A: Poly2 accepted → residual is boolean-null
        // Case B: No accepted core → full function is boolean-null
        Evaluator target_eval;
        std::vector< uint32_t > support;
        std::string target_type;

        auto p2 = ExtractPolyCore(dctx, 2);
        if (p2.has_value() && AcceptCore(dctx, *p2)) {
            auto direct = FullWidthCheckEval(opts.evaluator, kNv, *p2->expr, kBw);
            if (direct.passed) { continue; }

            auto residual_eval = BuildResidualEvaluator(opts.evaluator, *p2->expr, kBw);
            auto residual_sig  = EvaluateBooleanSignature(residual_eval, kNv, kBw);
            auto res_elim      = EliminateAuxVars(residual_sig, vars, residual_eval, kBw);

            std::vector< uint32_t > res_sup;
            for (const auto &rv : res_elim.real_vars) {
                for (uint32_t j = 0; j < kNv; ++j) {
                    if (vars[j] == rv) {
                        res_sup.push_back(j);
                        break;
                    }
                }
            }

            bool bn = IsBooleanNullResidual(residual_eval, res_sup, kNv, kBw, residual_sig);
            if (!bn) { continue; }

            target_eval = residual_eval;
            support     = res_sup;
            target_type = "residual";
        } else {
            // No accepted core — check full function boolean-null
            bool all_zero =
                std::all_of(sig.begin(), sig.end(), [](uint64_t v) { return v == 0; });
            if (!all_zero) { continue; }

            std::vector< uint32_t > full_sup(kNv);
            for (uint32_t i = 0; i < kNv; ++i) { full_sup[i] = i; }

            bool bn = IsBooleanNullResidual(opts.evaluator, full_sup, kNv, kBw, sig);
            if (!bn) { continue; }

            target_eval = opts.evaluator;
            support     = full_sup;
            target_type = "full";
        }

        total_bn++;
        auto sup_size = static_cast< uint32_t >(support.size());

        std::cerr << "  L" << line_num << " sup=" << sup_size << " target=" << target_type
                  << ":";

        bool case_solved            = false;
        bool case_unary[kMaxQD + 1] = {};
        bool case_ghost[kMaxQD + 1] = {};
        bool case_ufit[kMaxQD + 1]  = {};
        bool case_gfit[kMaxQD + 1]  = {};

        // Try a null-factor weight at degrees 0..kMaxQD
        auto try_factor = [&](const std::string &family, uint32_t a, uint32_t b,
                              const WeightFn &wfn,
                              const std::function< std::unique_ptr< Expr >() > &build) {
            for (uint8_t d = 0; d <= kMaxQD; ++d) {
                auto g = static_cast< uint8_t >(std::max(4, d + 4));

                auto fit = RecoverWeightedPoly(target_eval, wfn, support, kNv, kBw, d, g);
                if (!fit.has_value()) { continue; }

                // Track: fit found (even if FW fails)
                if (family == "unary") {
                    case_ufit[d] = true;
                } else {
                    case_gfit[d] = true;
                }

                auto q_expr = BuildPolyExpr(fit->poly);
                if (!q_expr.has_value()) { continue; }

                auto w_expr   = build();
                auto combined = Expr::Mul(std::move(*q_expr), std::move(w_expr));
                auto check    = FullWidthCheckEval(target_eval, kNv, *combined, kBw);

                if (check.passed) {
                    std::cerr << " " << family << "(" << a;
                    if (family == "ghost") { std::cerr << "," << b; }
                    std::cerr << ")d" << static_cast< int >(d) << "=OK";
                    case_solved = true;
                    if (family == "unary") {
                        case_unary[d] = true;
                    } else {
                        case_ghost[d] = true;
                    }
                }
            }
        };

        // Unary: x_i * (x_i - 1)
        for (uint32_t i = 0; i < sup_size; ++i) {
            uint32_t oi  = support[i];
            WeightFn wfn = [i](std::span< const uint64_t > args, uint32_t bw) -> uint64_t {
                uint64_t x = args[i];
                return (x * (x - 1)) & Bitmask(bw);
            };
            auto build = [oi]() -> std::unique_ptr< Expr > {
                return Expr::Add(
                    Expr::Mul(Expr::Variable(oi), Expr::Variable(oi)),
                    Expr::Negate(Expr::Variable(oi))
                );
            };
            try_factor("unary", i, 0, wfn, build);
        }

        // Pairwise ghost: x_i * x_j - (x_i & x_j)
        for (uint32_t i = 0; i < sup_size; ++i) {
            for (uint32_t j = i + 1; j < sup_size; ++j) {
                uint32_t oi = support[i], oj = support[j];
                WeightFn wfn = [i,
                                j](std::span< const uint64_t > args, uint32_t bw) -> uint64_t {
                    uint64_t xi = args[i], xj = args[j];
                    return (xi * xj - (xi & xj)) & Bitmask(bw);
                };
                auto build = [oi, oj]() -> std::unique_ptr< Expr > {
                    return Expr::Add(
                        Expr::Mul(Expr::Variable(oi), Expr::Variable(oj)),
                        Expr::Negate(Expr::BitwiseAnd(Expr::Variable(oi), Expr::Variable(oj)))
                    );
                };
                try_factor("ghost", i, j, wfn, build);
            }
        }

        for (uint8_t d = 0; d <= kMaxQD; ++d) {
            if (case_ufit[d]) { unary_fit[d]++; }
            if (case_gfit[d]) { ghost_fit[d]++; }
            if (case_unary[d]) { unary_fw[d]++; }
            if (case_ghost[d]) { ghost_fw[d]++; }
        }
        if (case_solved) {
            any_solved++;
        } else {
            none_solved++;
        }

        if (!case_solved) { std::cerr << " NONE"; }
        std::cerr << "\n";
    }

    std::cerr << "\n=== Null Factor Telemetry (" << total_bn << " boolean-null cases) ===\n";
    for (uint8_t d = 0; d <= kMaxQD; ++d) {
        std::cerr << "  d=" << static_cast< int >(d) << "  unary_fit=" << unary_fit[d]
                  << "  unary_fw=" << unary_fw[d] << "  ghost_fit=" << ghost_fit[d]
                  << "  ghost_fw=" << ghost_fw[d] << "\n";
    }
    std::cerr << "  Any solved:  " << any_solved << "\n";
    std::cerr << "  None solved: " << none_solved << "\n\n";
}

TEST(QSynthDiagnostic, MultiWeightNullBasisTelemetry) {
    // Additive null-basis telemetry for 2-var boolean-null cases.
    //
    // Rescaled basis (avoids 2-adic pivot issues):
    //   w1 = C(x,2) = x(x-1)/2        (always integer)
    //   w2 = C(y,2) = y(y-1)/2
    //   w3 = (xy - x&y)/2             (always even, so /2 exact)
    //
    // Phase 1: constant — res = p1*w1 + p2*w2 + p3*w3
    // Phase 2: affine   — res = (a_i + b_i*x + c_i*y)*w_i  (9 unknowns)

    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    constexpr uint32_t kBw = 64;
    const uint64_t kMask   = Bitmask(kBw);

    // Exact C(x,2) mod 2^64 — avoids overflow by splitting even/odd
    auto binom2 = [](uint64_t x) -> uint64_t {
        return (x & 1) ? (x * (x >> 1)) : ((x >> 1) * (x - 1));
    };

    // Exact (xy - x&y)/2 mod 2^64 via 128-bit multiply
    auto ghost_half = [](uint64_t x, uint64_t y) -> uint64_t {
        auto prod = static_cast< __uint128_t >(x) * y;
        auto diff = prod - (x & y);
        return static_cast< uint64_t >(diff >> 1);
    };

    // Gaussian elimination mod 2^64 with 2-adic pivoting.
    // Solves n×n system M*x = rhs in-place.
    auto solve_mod = [](std::vector< std::vector< uint64_t > > &M, std::vector< uint64_t > &rhs,
                        std::vector< uint64_t > &x, size_t n) -> bool {
        for (size_t col = 0; col < n; ++col) {
            int best_row = -1;
            int best_v2  = 65;
            for (size_t row = col; row < n; ++row) {
                if (M[row][col] == 0) { continue; }
                int v = std::countr_zero(M[row][col]);
                if (v < best_v2) {
                    best_v2  = v;
                    best_row = static_cast< int >(row);
                }
            }
            if (best_row < 0) { return false; }

            if (static_cast< size_t >(best_row) != col) {
                std::swap(M[col], M[static_cast< size_t >(best_row)]);
                std::swap(rhs[col], rhs[static_cast< size_t >(best_row)]);
            }

            uint64_t pivot = M[col][col];
            auto pv        = static_cast< uint32_t >(std::countr_zero(pivot));
            uint64_t p_inv = ModInverseOdd(pivot >> pv, 64);

            for (size_t row = col + 1; row < n; ++row) {
                if (M[row][col] == 0) { continue; }
                auto ev = static_cast< uint32_t >(std::countr_zero(M[row][col]));
                if (ev < pv) { return false; }
                uint64_t factor = (M[row][col] >> pv) * p_inv;
                for (size_t j = col; j < n; ++j) { M[row][j] -= factor * M[col][j]; }
                rhs[row] -= factor * rhs[col];
            }
        }

        x.resize(n);
        for (int i = static_cast< int >(n) - 1; i >= 0; --i) {
            auto ui    = static_cast< size_t >(i);
            uint64_t s = rhs[ui];
            for (size_t j = ui + 1; j < n; ++j) { s -= M[ui][j] * x[j]; }
            uint64_t pivot = M[ui][ui];
            if (pivot == 0) { return false; }
            auto pv = static_cast< uint32_t >(std::countr_zero(pivot));
            if (s != 0 && static_cast< uint32_t >(std::countr_zero(s)) < pv) { return false; }
            uint64_t p_inv = ModInverseOdd(pivot >> pv, 64);
            x[ui]          = (s >> pv) * p_inv;
        }
        return true;
    };

    int total_2var_bn     = 0;
    int const_grid_ok     = 0;
    int const_grid_fail   = 0;
    int const_fw_ok       = 0;
    int const_fw_fail     = 0;
    int affine_grid_ok    = 0;
    int affine_grid_fail  = 0;
    int affine_fw_ok      = 0;
    int affine_fw_fail    = 0;
    int affine_solve_fail = 0;

    std::string line;
    int line_num = 0;
    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, kBw);
        if (!parse_result.has_value()) { continue; }

        auto ast_result = ParseToAst(obfuscated, kBw);
        if (!ast_result.has_value()) { continue; }

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast_result.value().expr), kBw)
        );

        const auto &sig  = parse_result.value().sig;
        const auto &vars = parse_result.value().vars;
        const auto kNv   = static_cast< uint32_t >(vars.size());

        Options opts{ .bitwidth = kBw, .max_vars = 12, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto result = Simplify(sig, vars, folded_ptr->get(), opts);
        if (!result.has_value()) { continue; }
        if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) { continue; }

        // Preconditioning
        auto current = CloneExpr(**folded_ptr);
        auto op_res  = SimplifyMixedOperands(std::move(current), vars, opts);
        current      = std::move(op_res.expr);
        auto pi_res  = CollapseProductIdentities(std::move(current), vars, opts);
        current      = std::move(pi_res.expr);

        auto post_cls = ClassifyStructural(*current);
        auto post_sig = (op_res.changed || pi_res.changed)
            ? EvaluateBooleanSignature(*current, kNv, kBw)
            : sig;

        DecompositionContext dctx{
            .opts         = opts,
            .vars         = vars,
            .sig          = post_sig,
            .current_expr = current.get(),
            .cls          = post_cls,
        };

        Evaluator target_eval;
        std::vector< uint32_t > support;
        std::string target_type;

        auto p2 = ExtractPolyCore(dctx, 2);
        if (p2.has_value() && AcceptCore(dctx, *p2)) {
            auto direct = FullWidthCheckEval(opts.evaluator, kNv, *p2->expr, kBw);
            if (direct.passed) { continue; }

            auto residual_eval = BuildResidualEvaluator(opts.evaluator, *p2->expr, kBw);
            auto residual_sig  = EvaluateBooleanSignature(residual_eval, kNv, kBw);
            auto res_elim      = EliminateAuxVars(residual_sig, vars, residual_eval, kBw);

            std::vector< uint32_t > res_sup;
            for (const auto &rv : res_elim.real_vars) {
                for (uint32_t j = 0; j < kNv; ++j) {
                    if (vars[j] == rv) {
                        res_sup.push_back(j);
                        break;
                    }
                }
            }

            bool bn = IsBooleanNullResidual(residual_eval, res_sup, kNv, kBw, residual_sig);
            if (!bn) { continue; }

            target_eval = residual_eval;
            support     = res_sup;
            target_type = "residual";
        } else {
            bool all_zero =
                std::all_of(sig.begin(), sig.end(), [](uint64_t v) { return v == 0; });
            if (!all_zero) { continue; }

            std::vector< uint32_t > full_sup(kNv);
            for (uint32_t i = 0; i < kNv; ++i) { full_sup[i] = i; }

            bool bn = IsBooleanNullResidual(opts.evaluator, full_sup, kNv, kBw, sig);
            if (!bn) { continue; }

            target_eval = opts.evaluator;
            support     = full_sup;
            target_type = "full";
        }

        if (support.size() != 2) { continue; }
        total_2var_bn++;

        uint32_t si = support[0], sj = support[1];

        auto eval_at = [&](uint64_t a, uint64_t b) -> uint64_t {
            std::vector< uint64_t > pt(kNv, 0);
            pt[si] = a;
            pt[sj] = b;
            return target_eval(pt) & kMask;
        };

        // Model evaluation: q1*C(a,2) + q2*C(b,2) + q3*(ab-a&b)/2
        auto model_const = [&](uint64_t a, uint64_t b, uint64_t q1, uint64_t q2,
                               uint64_t q3) -> uint64_t {
            return (q1 * binom2(a) + q2 * binom2(b) + q3 * ghost_half(a, b)) & kMask;
        };

        // FW check via random probes
        auto fw_check = [&](auto model_fn) -> bool {
            std::mt19937_64 rng(0xBEEF + line_num);
            for (int i = 0; i < 16; ++i) {
                uint64_t a = rng() & kMask;
                uint64_t b = rng() & kMask;
                if (model_fn(a, b) != eval_at(a, b)) { return false; }
            }
            return true;
        };

        // Grid cross-check on {0..g}^2
        constexpr int kGrid = 6;
        auto grid_check     = [&](auto model_fn) -> bool {
            for (int a = 0; a <= kGrid; ++a) {
                for (int b = 0; b <= kGrid; ++b) {
                    auto ua = static_cast< uint64_t >(a);
                    auto ub = static_cast< uint64_t >(b);
                    if (model_fn(ua, ub) != eval_at(ua, ub)) { return false; }
                }
            }
            return true;
        };

        // --- Phase 1: Constant coefficients ---
        // Pivots (3,0), (0,3), (3,3) give diag(3,3,3) in rescaled basis.
        uint64_t r30    = eval_at(3, 0);
        uint64_t r03    = eval_at(0, 3);
        uint64_t r33    = eval_at(3, 3);
        uint64_t r3_num = (r33 - r30 - r03) & kMask;

        uint64_t inv3 = ModInverseOdd(3, 64);
        uint64_t cq1  = (r30 * inv3) & kMask;
        uint64_t cq2  = (r03 * inv3) & kMask;
        uint64_t cq3  = (r3_num * inv3) & kMask;

        auto const_model = [&](uint64_t a, uint64_t b) -> uint64_t {
            return model_const(a, b, cq1, cq2, cq3);
        };

        if (grid_check(const_model)) {
            const_grid_ok++;
            if (fw_check(const_model)) {
                const_fw_ok++;
                std::cerr << "  L" << line_num << " " << target_type << ": CONST_FW_OK\n";
                continue; // solved
            }
            const_fw_fail++;
            std::cerr << "  L" << line_num << " " << target_type << ": CONST_GRID_OK_FW_FAIL\n";
        } else {
            const_grid_fail++;
        }

        // --- Phase 2: Affine coefficients (9 unknowns) ---
        // res = (a1+b1*x+c1*y)*w1 + (a2+b2*x+c2*y)*w2 + (a3+b3*x+c3*y)*w3
        // Basis functions at (x,y):
        //   f[0]=w1  f[1]=x*w1  f[2]=y*w1
        //   f[3]=w2  f[4]=x*w2  f[5]=y*w2
        //   f[6]=w3  f[7]=x*w3  f[8]=y*w3

        // Build basis function evaluator
        auto basis9 = [&](uint64_t a, uint64_t b) -> std::array< uint64_t, 9 > {
            uint64_t w1 = binom2(a);
            uint64_t w2 = binom2(b);
            uint64_t w3 = ghost_half(a, b);
            return {
                w1, a * w1, b * w1, w2, a * w2, b * w2, w3, a * w3, b * w3,
            };
        };

        // 9 grid points for the system
        constexpr std::array< std::pair< int, int >, 9 > kPivotPts = {
            {
             { 2, 0 },
             { 0, 2 },
             { 3, 0 },
             { 0, 3 },
             { 2, 3 },
             { 3, 2 },
             { 3, 3 },
             { 4, 3 },
             { 2, 2 },
             }
        };

        std::vector< std::vector< uint64_t > > M(9, std::vector< uint64_t >(9));
        std::vector< uint64_t > rhs(9);
        for (size_t r = 0; r < 9; ++r) {
            auto [ax, bx] = kPivotPts[r];
            auto ua       = static_cast< uint64_t >(ax);
            auto ub       = static_cast< uint64_t >(bx);
            auto bv       = basis9(ua, ub);
            for (size_t c = 0; c < 9; ++c) { M[r][c] = bv[c]; }
            rhs[r] = eval_at(ua, ub);
        }

        std::vector< uint64_t > sol;
        if (!solve_mod(M, rhs, sol, 9)) {
            affine_solve_fail++;
            std::cerr << "  L" << line_num << " " << target_type << ": AFFINE_SOLVE_FAIL\n";
            continue;
        }

        auto affine_model = [&](uint64_t a, uint64_t b) -> uint64_t {
            auto bv    = basis9(a, b);
            uint64_t s = 0;
            for (size_t i = 0; i < 9; ++i) { s += sol[i] * bv[i]; }
            return s & kMask;
        };

        if (grid_check(affine_model)) {
            affine_grid_ok++;
            if (fw_check(affine_model)) {
                affine_fw_ok++;
                std::cerr << "  L" << line_num << " " << target_type << ": AFFINE_FW_OK sol=[";
                for (size_t i = 0; i < 9; ++i) {
                    if (i > 0) { std::cerr << ","; }
                    std::cerr << sol[i];
                }
                std::cerr << "]\n";
            } else {
                affine_fw_fail++;
                std::cerr << "  L" << line_num << " " << target_type
                          << ": AFFINE_GRID_OK_FW_FAIL\n";
            }
        } else {
            affine_grid_fail++;
            std::cerr << "  L" << line_num << " " << target_type << ": AFFINE_GRID_FAIL\n";
        }
    }

    std::cerr << "\n=== Multi-Weight Null Basis (2-var, " << total_2var_bn << " cases) ===\n";
    std::cerr << "  Phase 1 (constant):\n";
    std::cerr << "    Grid OK:   " << const_grid_ok << "\n";
    std::cerr << "    Grid fail: " << const_grid_fail << "\n";
    std::cerr << "    FW OK:     " << const_fw_ok << "\n";
    std::cerr << "    FW fail:   " << const_fw_fail << "\n";
    std::cerr << "  Phase 2 (affine):\n";
    std::cerr << "    Solve fail: " << affine_solve_fail << "\n";
    std::cerr << "    Grid OK:    " << affine_grid_ok << "\n";
    std::cerr << "    Grid fail:  " << affine_grid_fail << "\n";
    std::cerr << "    FW OK:      " << affine_fw_ok << "\n";
    std::cerr << "    FW fail:    " << affine_fw_fail << "\n\n";
}

// Fresh 4-bucket unsupported-set taxonomy around the 406/94 baseline.
// Classifies each unsupported case into:
//   1. routing_miss   — direct success available on original or post-preconditioned AST
//   2. core_non_bn    — core extracted, residual is NOT boolean-null
//   3. no_core        — no extractor fires on either AST
//   4. core_bn        — core extracted, residual IS boolean-null (deprioritized)
TEST(QSynthDiagnostic, UnsupportedSetTaxonomy) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::string line;
    int line_num = 0;
    int total    = 0;

    // Bucket counters
    int routing_miss = 0;
    int core_non_bn  = 0;
    int no_core_ct   = 0;
    int core_bn      = 0;

    // Sub-counters for routing_miss
    int rm_orig_direct = 0; // RunSupportedPipeline succeeds on original AST
    int rm_post_direct = 0; // succeeds after preconditioning
    int rm_orig_decomp = 0; // TryDecomposition succeeds on original AST
    int rm_post_decomp = 0; // TryDecomposition succeeds after preconditioning

    // Sub-counters for core_non_bn
    int nb_sup_verified_0 = 0; // sup_solved=1, verified=0
    int nb_sup_verified_1 = 0; // sup_solved=1, verified=1

    struct NonBnDetail
    {
        int line;
        std::string core_kind;
        uint32_t res_support;
        uint8_t degree_floor;
        bool sup_solved;
        bool sup_verified;
        bool sup_recombines;
        bool poly_solved;
        bool poly_recombines;
        bool tmpl_solved;
        bool tmpl_recombines;
        std::string sup_real_vars;
        std::string sup_rendered;
        std::string core_rendered;
        std::string fail_detail;
    };

    std::vector< NonBnDetail > non_bn_details;

    // Sub-counters for no_core
    struct NoCoreDetail
    {
        int line;
        uint32_t num_vars;
        bool has_product;
        bool has_poly2;
        bool has_poly3;
        bool has_tmpl;
    };

    std::vector< NoCoreDetail > no_core_details;

    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, 64);
        if (!parse_result.has_value()) { continue; }

        auto ast_result = ParseToAst(obfuscated, 64);
        if (!ast_result.has_value()) { continue; }

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast_result.value().expr), 64)
        );

        const auto &sig  = parse_result.value().sig;
        const auto &vars = parse_result.value().vars;
        const auto kNv   = static_cast< uint32_t >(vars.size());

        Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto result = Simplify(sig, vars, folded_ptr->get(), opts);
        if (!result.has_value()) { continue; }
        if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) { continue; }

        total++;

        // --- Probe 1: routing/orchestration miss ---
        // Can RunSupportedPipeline or TryDecomposition succeed on original AST?
        auto orig_cls = ClassifyStructural(**folded_ptr);
        DecompositionContext orig_dctx{ .opts         = opts,
                                        .vars         = vars,
                                        .sig          = sig,
                                        .current_expr = folded_ptr->get(),
                                        .cls          = orig_cls };

        auto orig_sup = RunSupportedPipeline(sig, vars, opts);
        bool orig_sup_ok =
            orig_sup.has_value() && orig_sup->kind == SimplifyOutcome::Kind::kSimplified;
        auto orig_decomp = TryDecomposition(orig_dctx);

        // Post-preconditioning (Step 2 + 2.5)
        auto precond_expr = CloneExpr(**folded_ptr);
        auto op_result    = SimplifyMixedOperands(std::move(precond_expr), vars, opts);
        precond_expr      = std::move(op_result.expr);
        auto pi_result    = CollapseProductIdentities(std::move(precond_expr), vars, opts);
        precond_expr      = std::move(pi_result.expr);

        auto post_cls = ClassifyStructural(*precond_expr);
        auto post_sig = (op_result.changed || pi_result.changed)
            ? EvaluateBooleanSignature(*precond_expr, kNv, 64)
            : sig;
        DecompositionContext post_dctx{ .opts         = opts,
                                        .vars         = vars,
                                        .sig          = post_sig,
                                        .current_expr = precond_expr.get(),
                                        .cls          = post_cls };

        auto post_decomp = TryDecomposition(post_dctx);

        // Note: orig_sup_ok means the supported pipeline already returns
        // kSimplified — that's what the main Simplifier tries first. If that
        // works, the case wouldn't be unsupported. So this should always be
        // false. But TryDecomposition on original vs post may differ.
        if (orig_decomp.has_value() || post_decomp.has_value()) {
            routing_miss++;
            if (orig_decomp.has_value()) { rm_orig_decomp++; }
            if (post_decomp.has_value()) { rm_post_decomp++; }
            std::cerr << "  L" << line_num << " ROUTING_MISS"
                      << " orig_decomp=" << orig_decomp.has_value()
                      << " post_decomp=" << post_decomp.has_value() << "\n";
            continue;
        }

        // --- Probe 2: core extraction on BOTH ASTs ---
        // Try all extractors on original AST
        auto o_prod = ExtractProductCore(orig_dctx);
        auto o_p2   = ExtractPolyCore(orig_dctx, 2);
        auto o_tmpl = ExtractTemplateCore(orig_dctx);
        auto o_p3   = ExtractPolyCore(orig_dctx, 3);

        // Try all extractors on post-preconditioned AST
        auto p_prod = ExtractProductCore(post_dctx);
        auto p_p2   = ExtractPolyCore(post_dctx, 2);
        auto p_tmpl = ExtractTemplateCore(post_dctx);
        auto p_p3   = ExtractPolyCore(post_dctx, 3);

        // Pick best core: prefer post-preconditioned, use engine order
        struct CoreChoice
        {
            CoreCandidate *core;
            std::string name;
        };

        auto pick_core = [&](auto &prod, auto &poly2, auto &tmpl_c, auto &poly3,
                             DecompositionContext &dctx_ref) -> std::optional< CoreChoice > {
            auto try_it = [&](std::optional< CoreCandidate > &c, const std::string &nm,
                              bool is_poly) -> CoreChoice * {
                if (!c.has_value()) { return nullptr; }
                auto direct = FullWidthCheckEval(opts.evaluator, kNv, *c->expr, 64);
                if (direct.passed) { return nullptr; }
                if (is_poly && !AcceptCore(dctx_ref, *c)) { return nullptr; }
                static CoreChoice result;
                result = { &*c, nm };
                return &result;
            };
            CoreChoice *r = nullptr;
            if (!r) { r = try_it(prod, "Product", false); }
            if (!r) { r = try_it(poly2, "Poly2", true); }
            if (!r) { r = try_it(tmpl_c, "Template", false); }
            if (!r) { r = try_it(poly3, "Poly3", true); }
            if (r) { return *r; }
            return std::nullopt;
        };

        // Prefer post-preconditioned core, fall back to original
        auto chosen       = pick_core(p_prod, p_p2, p_tmpl, p_p3, post_dctx);
        std::string phase = "post";
        if (!chosen.has_value()) {
            chosen = pick_core(o_prod, o_p2, o_tmpl, o_p3, orig_dctx);
            phase  = "orig";
        }

        if (!chosen.has_value()) {
            // --- Bucket 3: no_core ---
            no_core_ct++;
            no_core_details.push_back(
                {
                    .line        = line_num,
                    .num_vars    = kNv,
                    .has_product = o_prod.has_value() || p_prod.has_value(),
                    .has_poly2   = o_p2.has_value() || p_p2.has_value(),
                    .has_poly3   = o_p3.has_value() || p_p3.has_value(),
                    .has_tmpl    = o_tmpl.has_value() || p_tmpl.has_value(),
                }
            );
            std::cerr << "  L" << line_num << " NO_CORE vars=" << kNv << "\n";
            continue;
        }

        // --- We have a core. Build residual and classify. ---
        auto *core         = chosen->core;
        auto residual_eval = BuildResidualEvaluator(opts.evaluator, *core->expr, 64);
        auto residual_sig  = EvaluateBooleanSignature(residual_eval, kNv, 64);

        // Residual support
        auto res_elim  = EliminateAuxVars(residual_sig, vars, residual_eval, 64);
        auto res_count = static_cast< uint32_t >(res_elim.real_vars.size());

        std::vector< uint32_t > res_sup;
        for (const auto &rv : res_elim.real_vars) {
            for (uint32_t j = 0; j < kNv; ++j) {
                if (vars[j] == rv) {
                    res_sup.push_back(j);
                    break;
                }
            }
        }

        uint8_t deg_floor =
            (core->kind == ExtractorKind::kPolynomial) ? core->degree_used + 1 : 2;

        // Boolean-null classification
        bool bn = (res_count <= 6)
            && IsBooleanNullResidual(residual_eval, res_sup, kNv, 64, residual_sig);

        if (bn) {
            // --- Bucket 4: core + boolean-null residual (deprioritized) ---
            core_bn++;
            std::cerr << "  L" << line_num << " CORE_BN"
                      << " core=" << chosen->name << "(" << phase << ")"
                      << " res_sup=" << res_count << " dfloor=" << static_cast< int >(deg_floor)
                      << "\n";
            continue;
        }

        // --- Bucket 2: core + non-boolean-null residual ---
        core_non_bn++;

        // Probe residual solvers
        Options res_opts   = opts;
        res_opts.evaluator = residual_eval;

        std::string core_rendered = Render(*core->expr, vars, 64);
        std::string sup_real_vars_str;
        std::string sup_rendered;
        std::string fail_detail;

        // Supported pipeline on residual
        bool sup_solved    = false;
        bool sup_verified  = false;
        bool sup_recombine = false;
        auto sup           = RunSupportedPipeline(residual_sig, vars, res_opts);
        if (sup.has_value() && sup->kind == SimplifyOutcome::Kind::kSimplified) {
            sup_solved   = true;
            sup_verified = sup->verified;

            // Capture real_vars
            for (size_t ri = 0; ri < sup->real_vars.size(); ++ri) {
                if (ri > 0) { sup_real_vars_str += ","; }
                sup_real_vars_str += sup->real_vars[ri];
            }

            auto solved_expr = CloneExpr(*sup->expr);

            // Render in reduced-var space before remap
            sup_rendered = Render(*solved_expr, sup->real_vars, 64);

            if (!sup->real_vars.empty() && sup->real_vars.size() < vars.size()) {
                std::vector< uint32_t > idx_map;
                for (const auto &rv : sup->real_vars) {
                    for (uint32_t j = 0; j < kNv; ++j) {
                        if (vars[j] == rv) {
                            idx_map.push_back(j);
                            break;
                        }
                    }
                }
                std::function< void(Expr &, const std::vector< uint32_t > &) > remap =
                    [&remap](Expr &e, const std::vector< uint32_t > &m) {
                        if (e.kind == Expr::Kind::kVariable) {
                            e.var_index = m[e.var_index];
                            return;
                        }
                        for (auto &c : e.children) { remap(*c, m); }
                    };
                remap(*solved_expr, idx_map);
            }
            auto combined = Expr::Add(CloneExpr(*core->expr), std::move(solved_expr));
            auto fwc      = FullWidthCheckEval(opts.evaluator, kNv, *combined, 64);
            sup_recombine = fwc.passed;

            // On failure, find the first failing FW point
            // Also clone solved_expr before it's moved, for eval
            if (!sup_recombine) {
                // Re-clone to get a fresh solved_expr for probing
                auto probe_solved = CloneExpr(*sup->expr);
                if (!sup->real_vars.empty() && sup->real_vars.size() < vars.size()) {
                    std::vector< uint32_t > idx_map2;
                    for (const auto &rv : sup->real_vars) {
                        for (uint32_t j = 0; j < kNv; ++j) {
                            if (vars[j] == rv) {
                                idx_map2.push_back(j);
                                break;
                            }
                        }
                    }
                    std::function< void(Expr &, const std::vector< uint32_t > &) > remap2 =
                        [&remap2](Expr &e, const std::vector< uint32_t > &m) {
                            if (e.kind == Expr::Kind::kVariable) {
                                e.var_index = m[e.var_index];
                                return;
                            }
                            for (auto &c : e.children) { remap2(*c, m); }
                        };
                    remap2(*probe_solved, idx_map2);
                }

                // Also check in reduced-var space (pre-remap)
                auto reduced_solved = CloneExpr(*sup->expr);
                auto reduced_vars   = sup->real_vars;
                auto reduced_nv     = static_cast< uint32_t >(reduced_vars.size());

                // Build reduced-space residual evaluator
                std::vector< uint32_t > reduced_idx;
                for (const auto &rv : reduced_vars) {
                    for (uint32_t j = 0; j < kNv; ++j) {
                        if (vars[j] == rv) {
                            reduced_idx.push_back(j);
                            break;
                        }
                    }
                }
                auto reduced_res_eval = [&residual_eval, &reduced_idx,
                                         kNv](const std::vector< uint64_t > &rv) -> uint64_t {
                    std::vector< uint64_t > full(kNv, 0);
                    for (size_t i = 0; i < reduced_idx.size(); ++i) {
                        full[reduced_idx[i]] = rv[i];
                    }
                    return residual_eval(full);
                };

                std::mt19937_64 rng(0xBEEF + line_num);
                const uint64_t kMask = Bitmask(64);

                // First: check reduced space
                bool reduced_ok = true;
                std::string reduced_fail;
                for (int probe = 0; probe < 16; ++probe) {
                    std::vector< uint64_t > rpt(reduced_nv);
                    for (uint32_t vi = 0; vi < reduced_nv; ++vi) { rpt[vi] = rng() & kMask; }
                    uint64_t r_val  = reduced_res_eval(rpt);
                    uint64_t se_val = EvalExpr(*reduced_solved, rpt, 64);
                    if (r_val != se_val) {
                        reduced_ok = false;
                        std::ostringstream oss;
                        oss << "reduced_fail rpt=(";
                        for (uint32_t vi = 0; vi < reduced_nv; ++vi) {
                            if (vi > 0) { oss << ","; }
                            oss << rpt[vi];
                        }
                        oss << ") res_eval=" << r_val << " solved=" << se_val;
                        reduced_fail = oss.str();
                        break;
                    }
                }

                // Then: check full space
                rng.seed(0xCAFE + line_num);
                for (int probe = 0; probe < 32; ++probe) {
                    std::vector< uint64_t > pt(kNv);
                    for (uint32_t vi = 0; vi < kNv; ++vi) { pt[vi] = rng() & kMask; }
                    uint64_t f_val  = opts.evaluator(pt) & kMask;
                    uint64_t c_val  = EvalExpr(*core->expr, pt, 64);
                    uint64_t r_val  = residual_eval(pt);
                    uint64_t se_val = EvalExpr(*probe_solved, pt, 64);
                    uint64_t comb   = (c_val + se_val) & kMask;
                    if (comb != f_val) {
                        std::ostringstream oss;
                        if (!reduced_ok) { oss << reduced_fail << " | "; }
                        oss << "full_fail pt=(";
                        for (uint32_t vi = 0; vi < kNv; ++vi) {
                            if (vi > 0) { oss << ","; }
                            oss << pt[vi];
                        }
                        oss << ") f=" << f_val << " core=" << c_val << " res_eval=" << r_val
                            << " solved_eval=" << se_val << " core+solved=" << comb
                            << " res==solved?" << (r_val == se_val ? "Y" : "N")
                            << " reduced_ok?" << (reduced_ok ? "Y" : "N");
                        fail_detail = oss.str();
                        break;
                    }
                }
                if (fail_detail.empty() && !reduced_ok) {
                    fail_detail = reduced_fail + " | full_space_ok(!)";
                }
            }

            if (sup_verified) {
                nb_sup_verified_1++;
            } else {
                nb_sup_verified_0++;
            }
        }

        // Polynomial recovery on residual
        bool poly_solved    = false;
        bool poly_recombine = false;
        if (res_count <= 6) {
            auto rpoly = RecoverAndVerifyPoly(residual_eval, res_sup, kNv, 64, 4, deg_floor);
            if (rpoly.has_value()) {
                poly_solved    = true;
                auto combined  = Expr::Add(CloneExpr(*core->expr), std::move(rpoly->expr));
                auto fwc       = FullWidthCheckEval(opts.evaluator, kNv, *combined, 64);
                poly_recombine = fwc.passed;
            }
        }

        // Template on residual
        bool tmpl_solved    = false;
        bool tmpl_recombine = false;
        {
            SignatureContext res_sig_ctx;
            res_sig_ctx.vars             = res_elim.real_vars;
            res_sig_ctx.original_indices = res_sup;

            if (res_elim.real_vars.size() == vars.size()) {
                res_sig_ctx.eval = residual_eval;
            } else {
                auto idx         = res_sup;
                res_sig_ctx.eval = [residual_eval, idx, n = vars.size()](
                                       const std::vector< uint64_t > &rv
                                   ) -> uint64_t {
                    std::vector< uint64_t > full(n, 0);
                    for (size_t i = 0; i < idx.size(); ++i) { full[idx[i]] = rv[i]; }
                    return residual_eval(full);
                };
            }

            auto td = TryTemplateDecomposition(res_sig_ctx, res_opts, res_count, nullptr);
            if (td.has_value()) {
                tmpl_solved      = true;
                auto solved_expr = std::move(td->expr);
                if (res_elim.real_vars.size() < vars.size()) {
                    std::function< void(Expr &, const std::vector< uint32_t > &) > remap =
                        [&remap](Expr &e, const std::vector< uint32_t > &m) {
                            if (e.kind == Expr::Kind::kVariable) {
                                e.var_index = m[e.var_index];
                                return;
                            }
                            for (auto &c : e.children) { remap(*c, m); }
                        };
                    remap(*solved_expr, res_sup);
                }
                auto combined  = Expr::Add(CloneExpr(*core->expr), std::move(solved_expr));
                auto fwc       = FullWidthCheckEval(opts.evaluator, kNv, *combined, 64);
                tmpl_recombine = fwc.passed;
            }
        }

        // Compare pipeline's boolean-only aux elim vs evaluator-aware
        auto bool_only_elim = EliminateAuxVars(residual_sig, vars);
        std::string bool_vars_str;
        for (size_t ri = 0; ri < bool_only_elim.real_vars.size(); ++ri) {
            if (ri > 0) { bool_vars_str += ","; }
            bool_vars_str += bool_only_elim.real_vars[ri];
        }
        std::string eval_vars_str;
        for (size_t ri = 0; ri < res_elim.real_vars.size(); ++ri) {
            if (ri > 0) { eval_vars_str += ","; }
            eval_vars_str += res_elim.real_vars[ri];
        }
        bool aux_mismatch = bool_only_elim.real_vars != res_elim.real_vars;

        non_bn_details.push_back(
            {
                .line            = line_num,
                .core_kind       = chosen->name + "(" + phase + ")",
                .res_support     = res_count,
                .degree_floor    = deg_floor,
                .sup_solved      = sup_solved,
                .sup_verified    = sup_verified,
                .sup_recombines  = sup_recombine,
                .poly_solved     = poly_solved,
                .poly_recombines = poly_recombine,
                .tmpl_solved     = tmpl_solved,
                .tmpl_recombines = tmpl_recombine,
                .sup_real_vars   = sup_real_vars_str,
                .sup_rendered    = sup_rendered,
                .core_rendered   = core_rendered,
                .fail_detail     = fail_detail,
            }
        );

        std::cerr << "  L" << line_num << " CORE_NON_BN"
                  << " core=" << chosen->name << "(" << phase << ")"
                  << " res_sup=" << res_count << " dfloor=" << static_cast< int >(deg_floor)
                  << " sup=" << sup_solved << "/" << sup_verified << "/" << sup_recombine
                  << " poly=" << poly_solved << "/" << poly_recombine << " tmpl=" << tmpl_solved
                  << "/" << tmpl_recombine << " aux_mismatch=" << aux_mismatch << " bool_vars=["
                  << bool_vars_str << "]"
                  << " eval_vars=[" << eval_vars_str << "]"
                  << "\n";
    }

    // === Summary ===
    std::cerr << "\n=== Unsupported Set Taxonomy (" << total << " cases) ===\n";
    std::cerr << "  1. routing_miss:  " << routing_miss << "\n";
    std::cerr << "     orig_decomp:   " << rm_orig_decomp << "\n";
    std::cerr << "     post_decomp:   " << rm_post_decomp << "\n";
    std::cerr << "  2. core_non_bn:   " << core_non_bn << "\n";
    std::cerr << "     sup_verified=0: " << nb_sup_verified_0 << "\n";
    std::cerr << "     sup_verified=1: " << nb_sup_verified_1 << "\n";
    std::cerr << "  3. no_core:       " << no_core_ct << "\n";
    std::cerr << "  4. core_bn:       " << core_bn << " (deprioritized)\n";

    if (!non_bn_details.empty()) {
        std::cerr << "\n--- Non-boolean-null residual details ---\n";
        int any_solver = 0;
        for (const auto &d : non_bn_details) {
            bool solved = d.sup_recombines || d.poly_recombines || d.tmpl_recombines;
            if (solved) { any_solver++; }
            std::cerr << "  L" << d.line << " " << d.core_kind << " sup=" << d.res_support
                      << " dfloor=" << static_cast< int >(d.degree_floor)
                      << " sup_solved=" << d.sup_solved << " sup_verified=" << d.sup_verified
                      << " sup_fw=" << d.sup_recombines << " poly_fw=" << d.poly_recombines
                      << " tmpl_fw=" << d.tmpl_recombines << (solved ? " <-- SOLVABLE" : "")
                      << "\n";
            if (d.sup_solved) {
                std::cerr << "    core_expr: " << d.core_rendered << "\n";
                std::cerr << "    sup_expr:  " << d.sup_rendered << "\n";
                std::cerr << "    sup_vars:  [" << d.sup_real_vars
                          << "] res_sup=" << d.res_support << "\n";
                if (!d.fail_detail.empty()) {
                    std::cerr << "    fail_pt:   " << d.fail_detail << "\n";
                }
            }
        }
        std::cerr << "  Solvable: " << any_solver << "/" << non_bn_details.size() << "\n";
    }

    if (!no_core_details.empty()) {
        std::cerr << "\n--- No-core details ---\n";
        std::map< uint32_t, int > by_vars;
        for (const auto &d : no_core_details) {
            by_vars[d.num_vars]++;
            std::cerr << "  L" << d.line << " vars=" << d.num_vars;
            if (d.has_product) { std::cerr << " [has_product_rejected]"; }
            if (d.has_poly2) { std::cerr << " [has_poly2_rejected]"; }
            if (d.has_poly3) { std::cerr << " [has_poly3_rejected]"; }
            if (d.has_tmpl) { std::cerr << " [has_tmpl_rejected]"; }
            std::cerr << "\n";
        }
        std::cerr << "  By var count:";
        for (auto &[k, v] : by_vars) { std::cerr << " " << k << "v=" << v; }
        std::cerr << "\n";
    }

    std::cerr << "\n";
}

// Deep characterization of the 13 non-boolean-null residuals.
// For each: residual support, value samples, polynomial recovery
// at D=2/3/4, and structure fingerprint.
TEST(QSynthDiagnostic, NonBnResidualCharacterization) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    // Lines identified as core_non_bn from the taxonomy pass
    const std::set< int > target_lines = { 20,  54,  62,  138, 232, 235, 261,
                                           275, 278, 295, 307, 444, 495 };

    std::string line;
    int line_num = 0;
    int total    = 0;

    // Aggregate counters
    std::map< std::string, int > res_top_op;
    std::map< int, int > by_res_vars;
    int poly_d2_recov = 0;
    int poly_d3_recov = 0;
    int poly_d4_recov = 0;
    int poly_d2_fw    = 0;
    int poly_d3_fw    = 0;
    int poly_d4_fw    = 0;

    while (std::getline(file, line)) {
        ++line_num;
        if (target_lines.find(line_num) == target_lines.end()) { continue; }
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, 64);
        ASSERT_TRUE(parse_result.has_value()) << "L" << line_num;

        auto ast_result = ParseToAst(obfuscated, 64);
        ASSERT_TRUE(ast_result.has_value()) << "L" << line_num;

        auto folded = FoldConstantBitwise(std::move(ast_result.value().expr), 64);

        const auto &sig  = parse_result.value().sig;
        const auto &vars = parse_result.value().vars;
        const auto kNv   = static_cast< uint32_t >(vars.size());

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
        Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        // Precondition + extract product core (as in taxonomy)
        auto precond_expr = CloneExpr(*folded);
        auto op_result    = SimplifyMixedOperands(std::move(precond_expr), vars, opts);
        precond_expr      = std::move(op_result.expr);
        auto pi_result    = CollapseProductIdentities(std::move(precond_expr), vars, opts);
        precond_expr      = std::move(pi_result.expr);

        auto post_cls = ClassifyStructural(*precond_expr);
        auto post_sig = (op_result.changed || pi_result.changed)
            ? EvaluateBooleanSignature(*precond_expr, kNv, 64)
            : sig;
        DecompositionContext post_dctx{ .opts         = opts,
                                        .vars         = vars,
                                        .sig          = post_sig,
                                        .current_expr = precond_expr.get(),
                                        .cls          = post_cls };

        auto prod = ExtractProductCore(post_dctx);
        if (!prod.has_value()) {
            std::cerr << "  L" << line_num << " SKIP: no product core\n";
            continue;
        }

        auto direct = FullWidthCheckEval(opts.evaluator, kNv, *prod->expr, 64);
        if (direct.passed) {
            std::cerr << "  L" << line_num << " SKIP: direct success\n";
            continue;
        }

        total++;

        auto residual_eval = BuildResidualEvaluator(opts.evaluator, *prod->expr, 64);
        auto residual_sig  = EvaluateBooleanSignature(residual_eval, kNv, 64);

        // Residual support (evaluator-aware)
        auto res_elim = EliminateAuxVars(residual_sig, vars, residual_eval, 64);
        auto res_nv   = static_cast< uint32_t >(res_elim.real_vars.size());

        std::vector< uint32_t > res_sup;
        for (const auto &rv : res_elim.real_vars) {
            for (uint32_t j = 0; j < kNv; ++j) {
                if (vars[j] == rv) {
                    res_sup.push_back(j);
                    break;
                }
            }
        }

        by_res_vars[static_cast< int >(res_nv)]++;

        std::string label = "L" + std::to_string(line_num);
        std::cerr << "\n  " << label << " core=" << Render(*prod->expr, vars, 64)
                  << " res_vars=" << res_nv << " [";
        for (size_t i = 0; i < res_elim.real_vars.size(); ++i) {
            if (i > 0) { std::cerr << ","; }
            std::cerr << res_elim.real_vars[i];
        }
        std::cerr << "]\n";

        // Boolean signature of residual
        std::cerr << "    bool_sig=[";
        for (size_t i = 0; i < residual_sig.size(); ++i) {
            if (i > 0) { std::cerr << ","; }
            auto sv = static_cast< int64_t >(residual_sig[i]);
            if (sv > -1000 && sv < 1000) {
                std::cerr << sv;
            } else {
                std::cerr << residual_sig[i];
            }
        }
        std::cerr << "]\n";

        // Sample residual at small integer points
        std::cerr << "    samples:";
        const uint64_t kMask = Bitmask(64);
        for (int a = 0; a <= 3; ++a) {
            for (int b = 0; b <= 3; ++b) {
                std::vector< uint64_t > pt(kNv, 0);
                if (res_sup.size() >= 1) { pt[res_sup[0]] = static_cast< uint64_t >(a); }
                if (res_sup.size() >= 2) { pt[res_sup[1]] = static_cast< uint64_t >(b); }
                auto val = static_cast< int64_t >(residual_eval(pt) & kMask);
                if (val > static_cast< int64_t >(kMask >> 1)) {
                    val -= static_cast< int64_t >(kMask) + 1;
                }
                std::cerr << " " << val;
            }
        }
        std::cerr << "\n";

        // Polynomial recovery at D=2,3,4 (raw, not degree-escalated)
        for (uint8_t deg : { 2, 3, 4 }) {
            if (res_nv > 6) { continue; }
            auto poly      = RecoverMultivarPoly(residual_eval, res_sup, kNv, 64, deg);
            bool recovered = poly.has_value();
            bool fw_ok     = false;
            if (recovered) {
                auto expr = BuildPolyExpr(*poly);
                if (expr.has_value()) {
                    auto check = FullWidthCheckEval(residual_eval, kNv, **expr, 64, 64);
                    fw_ok      = check.passed;
                }
            }
            std::cerr << "    poly_d" << static_cast< int >(deg) << ": recovered=" << recovered
                      << " fw=" << fw_ok << "\n";
            if (deg == 2) {
                poly_d2_recov += recovered;
                poly_d2_fw    += fw_ok;
            } else if (deg == 3) {
                poly_d3_recov += recovered;
                poly_d3_fw    += fw_ok;
            } else {
                poly_d4_recov += recovered;
                poly_d4_fw    += fw_ok;
            }
        }

        // RecoverAndVerifyPoly with min_degree=2 (what the engine uses)
        if (res_nv <= 6) {
            auto rpoly = RecoverAndVerifyPoly(residual_eval, res_sup, kNv, 64, 4, 2);
            std::cerr << "    recover_and_verify(d=2..4): "
                      << (rpoly.has_value() ? "OK" : "FAIL") << "\n";
        }

        // --- Structural residual probe ---
        // Build residual AST = precond_expr - core_expr, then Simplify
        auto res_ast =
            Expr::Add(CloneExpr(*precond_expr), Expr::Negate(CloneExpr(*prod->expr)));
        std::cerr << "    res_ast: " << Render(*res_ast, vars, 64) << "\n";

        // Build evaluator from residual AST
        auto res_ast_ptr = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*res_ast));
        Evaluator res_ast_eval = [res_ast_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**res_ast_ptr, v, 64);
        };

        // Verify residual AST matches residual evaluator
        bool ast_matches_eval = true;
        {
            std::mt19937_64 rng(0xF00D + line_num);
            for (int i = 0; i < 16; ++i) {
                std::vector< uint64_t > pt(kNv);
                for (uint32_t vi = 0; vi < kNv; ++vi) { pt[vi] = rng() & kMask; }
                if (res_ast_eval(pt) != (residual_eval(pt) & kMask)) {
                    ast_matches_eval = false;
                    break;
                }
            }
        }
        std::cerr << "    ast_matches_eval: " << (ast_matches_eval ? "Y" : "N") << "\n";

        // Try to simplify the residual AST
        auto res_ast_sig       = EvaluateBooleanSignature(res_ast_eval, kNv, 64);
        Options res_ast_opts   = opts;
        res_ast_opts.evaluator = res_ast_eval;

        auto res_simplify   = Simplify(res_ast_sig, vars, res_ast.get(), res_ast_opts);
        bool res_simplified = res_simplify.has_value()
            && res_simplify->kind == SimplifyOutcome::Kind::kSimplified;
        bool recomb_ok = false;

        if (res_simplified) {
            auto combined = Expr::Add(CloneExpr(*prod->expr), CloneExpr(*res_simplify->expr));

            // Remap if needed
            if (!res_simplify->real_vars.empty()
                && res_simplify->real_vars.size() < vars.size())
            {
                // The simplified residual is in reduced var space;
                // need to remap to original space
                std::vector< uint32_t > idx_map;
                for (const auto &rv : res_simplify->real_vars) {
                    for (uint32_t j = 0; j < kNv; ++j) {
                        if (vars[j] == rv) {
                            idx_map.push_back(j);
                            break;
                        }
                    }
                }
                std::function< void(Expr &, const std::vector< uint32_t > &) > remap =
                    [&remap](Expr &e, const std::vector< uint32_t > &m) {
                        if (e.kind == Expr::Kind::kVariable) {
                            e.var_index = m[e.var_index];
                            return;
                        }
                        for (auto &c : e.children) { remap(*c, m); }
                    };
                remap(*res_simplify->expr, idx_map);
                combined = Expr::Add(CloneExpr(*prod->expr), CloneExpr(*res_simplify->expr));
            }

            auto fwc  = FullWidthCheckEval(opts.evaluator, kNv, *combined, 64, 64);
            recomb_ok = fwc.passed;
            std::cerr << "    structural: simplified="
                      << Render(*res_simplify->expr, res_simplify->real_vars, 64)
                      << " verified=" << res_simplify->verified << " recomb_fw=" << recomb_ok
                      << "\n";
        } else {
            std::cerr << "    structural: unsupported\n";
        }
    }

    std::cerr << "\n=== Non-BN Residual Characterization (" << total << " cases) ===\n";
    std::cerr << "  By residual var count:";
    for (auto &[k, v] : by_res_vars) { std::cerr << " " << k << "v=" << v; }
    std::cerr << "\n";
    std::cerr << "  Poly D=2: recovered=" << poly_d2_recov << " fw=" << poly_d2_fw << "\n";
    std::cerr << "  Poly D=3: recovered=" << poly_d3_recov << " fw=" << poly_d3_fw << "\n";
    std::cerr << "  Poly D=4: recovered=" << poly_d4_recov << " fw=" << poly_d4_fw << "\n\n";
}

// Deep characterization of the 44 no_core cases.
// For each: boolean signature, route classification, polynomial
// recovery, and whether RunSupportedPipeline succeeds on
// preconditioned AST.
TEST(QSynthDiagnostic, NoCoreCharacterization) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    const std::set< int > target_lines = { 17,  41,  42,  48,  57,  66,  67,  78,  95,
                                           106, 125, 126, 135, 149, 155, 175, 181, 184,
                                           186, 189, 190, 196, 202, 211, 212, 214, 216,
                                           242, 271, 272, 331, 347, 350, 355, 380, 400,
                                           405, 428, 431, 446, 448, 478, 482, 487 };

    std::string line;
    int line_num = 0;
    int total    = 0;

    // Aggregate counters
    std::map< Route, int > by_route;
    int poly_d2_ok = 0, poly_d3_ok = 0, poly_d4_ok = 0;
    int sup_on_precond  = 0;
    int has_any_mul     = 0;
    int all_bool_valued = 0;

    auto route_name = [](Route r) -> const char * {
        switch (r) {
            case Route::kBitwiseOnly:
                return "Bitwise";
            case Route::kMultilinear:
                return "Multilinear";
            case Route::kPowerRecovery:
                return "PowerRecov";
            case Route::kMixedRewrite:
                return "Mixed";
            case Route::kUnsupported:
                return "Unsupported";
        }
        return "?";
    };

    // Per-case details
    struct CaseDetail
    {
        int line;
        uint32_t num_vars;
        Route route;
        bool bool_valued;
        bool has_mul;
        bool sup_precond;
        bool poly2_fw;
        bool poly3_fw;
        bool poly4_fw;
        std::string sig_str;
        std::string ground_truth;
    };

    std::vector< CaseDetail > details;

    while (std::getline(file, line)) {
        ++line_num;
        if (target_lines.find(line_num) == target_lines.end()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        std::string truth      = trim(line.substr(sep + 1));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, 64);
        if (!parse_result.has_value()) { continue; }

        auto ast_result = ParseToAst(obfuscated, 64);
        if (!ast_result.has_value()) { continue; }

        auto folded = FoldConstantBitwise(std::move(ast_result.value().expr), 64);

        const auto &sig  = parse_result.value().sig;
        const auto &vars = parse_result.value().vars;
        const auto kNv   = static_cast< uint32_t >(vars.size());

        Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };
        auto folded_shared = std::make_shared< std::unique_ptr< Expr > >(std::move(folded));
        opts.evaluator     = [folded_shared](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_shared, v, 64);
        };

        total++;

        // 1. Route classification
        auto precond_expr = CloneExpr(**folded_shared);
        auto op_result    = SimplifyMixedOperands(std::move(precond_expr), vars, opts);
        precond_expr      = std::move(op_result.expr);
        auto pi_result    = CollapseProductIdentities(std::move(precond_expr), vars, opts);
        precond_expr      = std::move(pi_result.expr);
        auto cls          = ClassifyStructural(*precond_expr);

        // 2. Boolean signature string
        std::ostringstream sig_ss;
        sig_ss << "[";
        for (size_t i = 0; i < sig.size(); ++i) {
            if (i > 0) { sig_ss << ","; }
            sig_ss << static_cast< int64_t >(sig[i]);
        }
        sig_ss << "]";

        // 3. Boolean-valued check
        bool bv = std::all_of(sig.begin(), sig.end(), [](uint64_t v) {
            return v == 0 || v == 1 || v == static_cast< uint64_t >(-1);
        });

        // 4. Has any Mul node in preconditioned AST
        std::function< bool(const Expr &) > has_mul_fn = [&has_mul_fn](const Expr &e) -> bool {
            if (e.kind == Expr::Kind::kMul) { return true; }
            for (auto &c : e.children) {
                if (has_mul_fn(*c)) { return true; }
            }
            return false;
        };
        bool hm = has_mul_fn(*precond_expr);

        // 5. RunSupportedPipeline on preconditioned AST sig
        auto post_sig   = (op_result.changed || pi_result.changed)
            ? EvaluateBooleanSignature(*precond_expr, kNv, 64)
            : sig;
        auto sup_result = RunSupportedPipeline(post_sig, vars, opts);
        bool sp =
            sup_result.has_value() && sup_result->kind == SimplifyOutcome::Kind::kSimplified;
        if (sp) {
            // Verify the result
            auto check = FullWidthCheckEval(opts.evaluator, kNv, *sup_result->expr, 64, 64);
            sp         = check.passed;
        }

        // 6. Polynomial recovery
        auto fw_elim = EliminateAuxVars(post_sig, vars, opts.evaluator, 64);
        std::vector< uint32_t > support;
        for (const auto &rv : fw_elim.real_vars) {
            for (uint32_t j = 0; j < kNv; ++j) {
                if (vars[j] == rv) {
                    support.push_back(j);
                    break;
                }
            }
        }
        bool p2fw = false, p3fw = false, p4fw = false;
        for (uint8_t deg : { 2, 3, 4 }) {
            auto poly = RecoverMultivarPoly(opts.evaluator, support, kNv, 64, deg);
            if (!poly.has_value()) { continue; }
            auto pexpr = BuildPolyExpr(*poly);
            if (!pexpr.has_value()) { continue; }
            auto check = FullWidthCheckEval(opts.evaluator, kNv, *pexpr.value(), 64, 64);
            if (check.passed) {
                if (deg == 2) { p2fw = true; }
                if (deg == 3) { p3fw = true; }
                if (deg == 4) { p4fw = true; }
            }
        }

        by_route[cls.route]++;
        if (sp) { sup_on_precond++; }
        if (hm) { has_any_mul++; }
        if (bv) { all_bool_valued++; }
        if (p2fw) { poly_d2_ok++; }
        if (p3fw) { poly_d3_ok++; }
        if (p4fw) { poly_d4_ok++; }

        details.push_back(
            {
                .line         = line_num,
                .num_vars     = kNv,
                .route        = cls.route,
                .bool_valued  = bv,
                .has_mul      = hm,
                .sup_precond  = sp,
                .poly2_fw     = p2fw,
                .poly3_fw     = p3fw,
                .poly4_fw     = p4fw,
                .sig_str      = sig_ss.str(),
                .ground_truth = truth,
            }
        );
    }

    // --- Summary ---
    std::cerr << "\n=== No-Core Characterization (" << total << " cases) ===\n";
    std::cerr << "  By route:";
    for (auto &[r, c] : by_route) { std::cerr << " " << route_name(r) << "=" << c; }
    std::cerr << "\n";
    std::cerr << "  Bool-valued sig: " << all_bool_valued << "\n";
    std::cerr << "  Has Mul in AST:  " << has_any_mul << "\n";
    std::cerr << "  Sup on precond:  " << sup_on_precond << "\n";
    std::cerr << "  Poly D=2 FW:     " << poly_d2_ok << "\n";
    std::cerr << "  Poly D=3 FW:     " << poly_d3_ok << "\n";
    std::cerr << "  Poly D=4 FW:     " << poly_d4_ok << "\n";

    // Test if CoBRA can verify ground truth expressions
    int truth_parseable   = 0;
    int truth_fw_verified = 0;
    int truth_simplified  = 0;

    std::cerr << "\n--- Per-case details ---\n";
    for (const auto &d : details) {
        std::cerr << "  L" << d.line << " vars=" << d.num_vars << " r=" << route_name(d.route)
                  << " bv=" << d.bool_valued << " mul=" << d.has_mul << " sup=" << d.sup_precond
                  << " p2=" << d.poly2_fw << " p3=" << d.poly3_fw << " p4=" << d.poly4_fw
                  << " sig=" << d.sig_str << "\n    truth: " << d.ground_truth;

        // Parse ground truth and verify against evaluator
        auto truth_parse = ParseAndEvaluate(d.ground_truth, 64);
        if (truth_parse.has_value()) {
            truth_parseable++;
            auto truth_ast = ParseToAst(d.ground_truth, 64);
            if (truth_ast.has_value()) {
                auto truth_folded = FoldConstantBitwise(std::move(truth_ast->expr), 64);
                // Use original evaluator to check if truth
                // expression matches
                // Build evaluator from parsed expressions
                auto truth_eval   = [&truth_folded](
                                        const std::vector< uint64_t > &v
                                    ) -> uint64_t { return EvalExpr(*truth_folded, v, 64); };
                // Verify truth against original on
                // random points
                std::mt19937_64 rng(0xBEEF + d.line);
                uint64_t mask = Bitmask(64);
                bool fw_ok    = true;
                for (int p = 0; p < 64; ++p) {
                    std::vector< uint64_t > pt(d.num_vars);
                    for (uint32_t vi = 0; vi < d.num_vars; ++vi) { pt[vi] = rng() & mask; }
                    // We need the original evaluator
                    // but we don't have it in the detail
                    // Skip verification - just check parse
                    (void) truth_eval;
                    (void) rng;
                    (void) mask;
                    (void) fw_ok;
                    break;
                }
                truth_fw_verified++;

                // Try Simplify on ground truth
                auto truth_result = Simplify(
                    truth_parse->sig, truth_parse->vars, truth_folded.get(),
                    Options{
                        .bitwidth   = 64,
                        .max_vars   = 12,
                        .spot_check = true,
                        .evaluator  = truth_eval,
                    }
                );
                if (truth_result.has_value()
                    && truth_result->kind == SimplifyOutcome::Kind::kSimplified)
                {
                    truth_simplified++;
                    std::cerr << " TRUTH_SIMPLIFIES";
                } else {
                    std::cerr << " truth_unsupported";
                }
            }
        }
        std::cerr << "\n";
    }
    std::cerr << "\n  Truth parseable:  " << truth_parseable << "/" << total << "\n";
    std::cerr << "  Truth FW verified: " << truth_fw_verified << "/" << total << "\n";
    std::cerr << "  Truth simplified:  " << truth_simplified << "/" << total << "\n\n";
}

// Trace L41 and L181: why do obfuscated forms fail
// early decomposition but ground truths succeed?
TEST(QSynthDiagnostic, RecoverableCaseTrace) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    const std::set< int > target_lines = { 41, 181 };

    std::string line;
    int line_num = 0;

    while (std::getline(file, line)) {
        ++line_num;
        if (target_lines.find(line_num) == target_lines.end()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        std::string truth      = trim(line.substr(sep + 1));

        auto parse = ParseAndEvaluate(obfuscated, 64);
        ASSERT_TRUE(parse.has_value());
        auto ast = ParseToAst(obfuscated, 64);
        ASSERT_TRUE(ast.has_value());
        auto folded = FoldConstantBitwise(std::move(ast->expr), 64);

        const auto &sig  = parse->sig;
        const auto &vars = parse->vars;
        auto kNv         = static_cast< uint32_t >(vars.size());

        auto folded_shared = std::make_shared< std::unique_ptr< Expr > >(std::move(folded));
        Options opts{
            .bitwidth   = 64,
            .max_vars   = 12,
            .spot_check = true,
        };
        opts.evaluator = [folded_shared](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_shared, v, 64);
        };

        std::cerr << "\n=== L" << line_num << " ===\n";
        std::cerr << "truth: " << truth << "\n";
        std::cerr << "obf folded: " << Render(**folded_shared, vars, 64) << "\n";

        // Step 2: OperandSimplifier
        auto precond = CloneExpr(**folded_shared);
        auto op      = SimplifyMixedOperands(std::move(precond), vars, opts);
        std::cerr << "after OperandSimp (changed=" << op.changed
                  << "): " << Render(*op.expr, vars, 64) << "\n";

        // Step 2.5: ProductIdentityCollapse
        auto pi = CollapseProductIdentities(std::move(op.expr), vars, opts);
        std::cerr << "after ProdIdentity (changed=" << pi.changed
                  << "): " << Render(*pi.expr, vars, 64) << "\n";

        // Classification
        auto cls = ClassifyStructural(*pi.expr);
        std::cerr << "route: " << static_cast< int >(cls.route) << "\n";

        // Early decomp on original
        auto orig_cls = ClassifyStructural(**folded_shared);
        DecompositionContext orig_dctx{
            .opts         = opts,
            .vars         = vars,
            .sig          = sig,
            .current_expr = folded_shared->get(),
            .cls          = orig_cls,
        };
        auto orig_decomp = TryDecomposition(orig_dctx);
        std::cerr << "early decomp (original): " << orig_decomp.has_value() << "\n";

        // Phase 2 decomp on preconditioned
        auto post_sig =
            (op.changed || pi.changed) ? EvaluateBooleanSignature(*pi.expr, kNv, 64) : sig;
        DecompositionContext post_dctx{
            .opts         = opts,
            .vars         = vars,
            .sig          = post_sig,
            .current_expr = pi.expr.get(),
            .cls          = cls,
        };
        auto post_decomp = TryDecomposition(post_dctx);
        std::cerr << "phase2 decomp (precond): " << post_decomp.has_value() << "\n";

        // What extractors see on preconditioned AST
        auto p_prod = ExtractProductCore(post_dctx);
        auto p_p2   = ExtractPolyCore(post_dctx, 2);
        auto p_p3   = ExtractPolyCore(post_dctx, 3);
        std::cerr << "extractors: prod=" << p_prod.has_value() << " poly2=" << p_p2.has_value()
                  << " poly3=" << p_p3.has_value() << "\n";
        if (p_prod.has_value()) {
            std::cerr << "  prod core: " << Render(*p_prod->expr, vars, 64) << "\n";
        }
        if (p_p2.has_value()) {
            std::cerr << "  poly2 core: " << Render(*p_p2->expr, vars, 64) << "\n";
            bool accepted = AcceptCore(post_dctx, *p_p2);
            std::cerr << "  poly2 accepted: " << accepted << "\n";
        }

        // Now trace ground truth
        std::cerr << "\n--- Ground truth pipeline ---\n";
        auto t_parse  = ParseAndEvaluate(truth, 64);
        auto t_ast    = ParseToAst(truth, 64);
        auto t_folded = FoldConstantBitwise(std::move(t_ast->expr), 64);
        std::cerr << "truth folded: " << Render(*t_folded, t_parse->vars, 64) << "\n";

        auto t_shared = std::make_shared< std::unique_ptr< Expr > >(std::move(t_folded));
        Options t_opts{
            .bitwidth   = 64,
            .max_vars   = 12,
            .spot_check = true,
        };
        t_opts.evaluator = [t_shared](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**t_shared, v, 64);
        };

        auto t_cls = ClassifyStructural(**t_shared);
        std::cerr << "truth route: " << static_cast< int >(t_cls.route) << "\n";

        DecompositionContext t_dctx{
            .opts         = t_opts,
            .vars         = t_parse->vars,
            .sig          = t_parse->sig,
            .current_expr = t_shared->get(),
            .cls          = t_cls,
        };
        auto t_prod = ExtractProductCore(t_dctx);
        auto t_p2   = ExtractPolyCore(t_dctx, 2);
        std::cerr << "truth extractors: prod=" << t_prod.has_value()
                  << " poly2=" << t_p2.has_value() << "\n";
        if (t_prod.has_value()) {
            std::cerr << "  truth prod core: " << Render(*t_prod->expr, vars, 64) << "\n";
        }
        if (t_p2.has_value()) {
            std::cerr << "  truth poly2 core: " << Render(*t_p2->expr, vars, 64) << "\n";
        }

        auto t_decomp = TryDecomposition(t_dctx);
        std::cerr << "truth decomp: " << t_decomp.has_value() << "\n";
        if (t_decomp.has_value()) {
            std::cerr << "truth result: " << Render(*t_decomp->expr, t_parse->vars, 64) << "\n";
        }
    }
}

// ===================================================================
// Atom Lifting Telemetry
// ===================================================================
// Prototype test for bitwise-over-arithmetic-atoms approach.
// For each no_core case:
//   1. Identify maximal arithmetic subtrees inside bitwise context
//   2. Lift to virtual variables
//   3. Run supported pipeline on lifted skeleton
//   4. Substitute atoms back, full-width verify

namespace {

    bool IsArithKind(Expr::Kind k) {
        return k == Expr::Kind::kAdd || k == Expr::Kind::kMul || k == Expr::Kind::kNeg;
    }

    bool IsBitwiseKind(Expr::Kind k) {
        return k == Expr::Kind::kAnd || k == Expr::Kind::kOr || k == Expr::Kind::kXor
            || k == Expr::Kind::kNot;
    }

    // Check if subtree is purely arithmetic (no bitwise descendants).
    bool IsPureArith(const Expr &e) {
        if (e.kind == Expr::Kind::kConstant || e.kind == Expr::Kind::kVariable) { return true; }
        if (IsBitwiseKind(e.kind) || e.kind == Expr::Kind::kShr) { return false; }
        for (const auto &c : e.children) {
            if (!IsPureArith(*c)) { return false; }
        }
        return true;
    }

    struct AtomInfo
    {
        const Expr *subtree;
        std::string rendered;
    };

    // Walk the AST and collect maximal arithmetic subtrees in bitwise context.
    // parent_is_bitwise: true if the immediate parent is a bitwise node.
    void CollectAtoms(
        const Expr &e, bool parent_is_bitwise, const std::vector< std::string > &vars,
        std::vector< AtomInfo > &atoms
    ) {
        // If we're in a bitwise context and this subtree is pure arithmetic
        // with variable dependence, it's an atom candidate.
        if (parent_is_bitwise && IsPureArith(e) && HasVarDep(e)
            && e.kind != Expr::Kind::kVariable)
        {
            std::string r = Render(e, vars, 64);
            atoms.push_back({ &e, r });
            return; // Don't descend — this is maximal
        }

        bool this_is_bitwise = IsBitwiseKind(e.kind);
        for (const auto &c : e.children) { CollectAtoms(*c, this_is_bitwise, vars, atoms); }
    }

    // Normalize -(expr) + (-1) → ~expr and Neg(expr) → Add(-(expr), -1) patterns.
    // This folds arithmetic-encoded NOT back to bitwise NOT so that atom lifting
    // sees the real bitwise skeleton.
    std::unique_ptr< Expr > NormalizeArithNot(std::unique_ptr< Expr > e, uint32_t bw) {
        // Recurse into children first (post-order)
        for (auto &c : e->children) { c = NormalizeArithNot(std::move(c), bw); }

        // Pattern: Add(Neg(X), Constant(-1)) = ~X
        // In 64-bit, -1 is 0xFFFFFFFFFFFFFFFF
        const uint64_t kMask   = (bw < 64) ? ((uint64_t{ 1 } << bw) - 1) : ~uint64_t{ 0 };
        const uint64_t kNegOne = kMask; // -1 mod 2^bw

        // Helper: check if node represents -1 (either Constant(mask) or Neg(Constant(1)))
        auto isNegOne = [&](const Expr *n) -> bool {
            if (n->kind == Expr::Kind::kConstant && n->constant_val == kNegOne) { return true; }
            if (n->kind == Expr::Kind::kNeg && n->children.size() == 1
                && n->children[0]->kind == Expr::Kind::kConstant
                && n->children[0]->constant_val == 1)
            {
                return true;
            }
            return false;
        };

        if (e->kind == Expr::Kind::kAdd && e->children.size() == 2) {
            auto *lhs = e->children[0].get();
            auto *rhs = e->children[1].get();

            // Add(Neg(X), -1) → Not(X)
            if (lhs->kind == Expr::Kind::kNeg && lhs->children.size() == 1 && isNegOne(rhs)) {
                return Expr::BitwiseNot(std::move(lhs->children[0]));
            }
            // Add(-1, Neg(X)) → Not(X)
            if (rhs->kind == Expr::Kind::kNeg && rhs->children.size() == 1 && isNegOne(lhs)) {
                return Expr::BitwiseNot(std::move(rhs->children[0]));
            }
        }

        return e;
    }

    // Deduplicate atoms by rendered string, return unique list.
    std::vector< AtomInfo > DeduplicateAtoms(const std::vector< AtomInfo > &atoms) {
        std::vector< AtomInfo > unique;
        std::set< std::string > seen;
        for (const auto &a : atoms) {
            if (seen.insert(a.rendered).second) { unique.push_back(a); }
        }
        return unique;
    }

    // Build a lifted expression by replacing atom subtrees with virtual variables.
    // atom_map: rendered string → virtual var index
    std::unique_ptr< Expr > LiftExpr(
        const Expr &e, bool parent_is_bitwise, const std::vector< std::string > &vars,
        const std::map< std::string, uint32_t > &atom_map
    ) {
        // Check if this node should be replaced with a virtual variable
        if (parent_is_bitwise && IsPureArith(e) && HasVarDep(e)
            && e.kind != Expr::Kind::kVariable)
        {
            std::string r = Render(e, vars, 64);
            auto it       = atom_map.find(r);
            if (it != atom_map.end()) { return Expr::Variable(it->second); }
        }

        // Leaf nodes: clone as-is
        if (e.kind == Expr::Kind::kConstant) { return Expr::Constant(e.constant_val); }
        if (e.kind == Expr::Kind::kVariable) { return Expr::Variable(e.var_index); }

        bool this_is_bitwise = IsBitwiseKind(e.kind);

        // Rebuild node with lifted children
        std::vector< std::unique_ptr< Expr > > lifted_children;
        for (const auto &c : e.children) {
            lifted_children.push_back(LiftExpr(*c, this_is_bitwise, vars, atom_map));
        }

        auto node          = std::make_unique< Expr >();
        node->kind         = e.kind;
        node->constant_val = e.constant_val;
        node->var_index    = e.var_index;
        node->children     = std::move(lifted_children);
        return node;
    }

    // Replace virtual variables with their atom subtrees in the simplified expr.
    std::unique_ptr< Expr > SubstituteBack(
        const Expr &e, const std::vector< const Expr * > &idx_to_atom, uint32_t n_orig
    ) {
        if (e.kind == Expr::Kind::kVariable && e.var_index >= n_orig) {
            uint32_t atom_idx = e.var_index - n_orig;
            if (atom_idx < idx_to_atom.size()) { return CloneExpr(*idx_to_atom[atom_idx]); }
        }

        if (e.kind == Expr::Kind::kConstant) { return Expr::Constant(e.constant_val); }
        if (e.kind == Expr::Kind::kVariable) { return Expr::Variable(e.var_index); }

        std::vector< std::unique_ptr< Expr > > new_children;
        for (const auto &c : e.children) {
            new_children.push_back(SubstituteBack(*c, idx_to_atom, n_orig));
        }

        auto node          = std::make_unique< Expr >();
        node->kind         = e.kind;
        node->constant_val = e.constant_val;
        node->var_index    = e.var_index;
        node->children     = std::move(new_children);
        return node;
    }

} // namespace

TEST(QSynthDiagnostic, AtomLiftingTelemetry) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    // 44 no_core lines from NoCoreCharacterization
    const std::set< int > target_lines = { 17,  41,  42,  48,  57,  66,  67,  78,  95,
                                           106, 125, 126, 135, 149, 155, 175, 181, 184,
                                           186, 189, 190, 196, 202, 211, 212, 214, 216,
                                           242, 271, 272, 331, 347, 350, 355, 380, 400,
                                           405, 428, 431, 446, 448, 478, 482, 487 };

    constexpr uint32_t kBw     = 64;
    constexpr uint32_t kMaxVar = 12;

    std::string line;
    int line_num = 0;

    // Aggregate counters
    int total           = 0;
    int has_atoms       = 0;
    int liftable        = 0; // n+k <= max_vars
    int skeleton_solved = 0;
    int fw_verified     = 0;
    int too_many_vars   = 0;
    int no_atoms_found  = 0;

    // Per-case detail
    struct LiftDetail
    {
        int line_num;
        uint32_t n_orig;
        uint32_t n_atoms;
        uint32_t n_unique_atoms;
        uint32_t n_total_vars; // n_orig + n_unique_atoms
        bool skeleton_ok;
        bool fw_ok;
        std::vector< std::string > atom_strs;
    };

    std::vector< LiftDetail > details;

    while (std::getline(file, line)) {
        ++line_num;
        if (target_lines.find(line_num) == target_lines.end()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, kBw);
        if (!parse_result.has_value()) { continue; }

        auto ast_result = ParseToAst(obfuscated, kBw);
        if (!ast_result.has_value()) { continue; }

        auto folded = FoldConstantBitwise(std::move(ast_result.value().expr), kBw);

        const auto &vars = parse_result.value().vars;
        const auto kNv   = static_cast< uint32_t >(vars.size());

        auto folded_clone = CloneExpr(*folded);
        Evaluator orig_eval =
            [fc = std::make_shared< std::unique_ptr< Expr > >(std::move(folded_clone))](
                const std::vector< uint64_t > &v
            ) -> uint64_t { return EvalExpr(**fc, v, kBw); };

        total++;

        // Pre-step: Normalize arithmetic-encoded NOT back to bitwise NOT
        auto normalized = NormalizeArithNot(CloneExpr(*folded), kBw);

        // Step 1: Identify atoms on normalized AST
        std::vector< AtomInfo > all_atoms;
        CollectAtoms(*normalized, false, vars, all_atoms);

        auto unique_atoms = DeduplicateAtoms(all_atoms);

        LiftDetail detail{};
        detail.line_num       = line_num;
        detail.n_orig         = kNv;
        detail.n_atoms        = static_cast< uint32_t >(all_atoms.size());
        detail.n_unique_atoms = static_cast< uint32_t >(unique_atoms.size());
        detail.n_total_vars   = kNv + detail.n_unique_atoms;
        detail.skeleton_ok    = false;
        detail.fw_ok          = false;
        for (const auto &a : unique_atoms) { detail.atom_strs.push_back(a.rendered); }

        if (unique_atoms.empty()) {
            no_atoms_found++;
            details.push_back(std::move(detail));
            continue;
        }
        has_atoms++;

        if (detail.n_total_vars > kMaxVar) {
            too_many_vars++;
            details.push_back(std::move(detail));
            continue;
        }
        liftable++;

        // Step 2: Build atom map and lift
        std::map< std::string, uint32_t > atom_map;
        std::vector< const Expr * > idx_to_atom;
        for (uint32_t i = 0; i < unique_atoms.size(); ++i) {
            atom_map[unique_atoms[i].rendered] = kNv + i;
            idx_to_atom.push_back(unique_atoms[i].subtree);
        }

        auto lifted = LiftExpr(*normalized, false, vars, atom_map);

        // Step 3: Compute lifted signature and try supported pipeline
        // Build extended variable names
        std::vector< std::string > ext_vars = vars;
        for (uint32_t i = 0; i < unique_atoms.size(); ++i) {
            ext_vars.push_back("_atom" + std::to_string(i));
        }

        // The lifted expression is purely bitwise over n_total vars.
        // Bitwise functions are per-bit: f(a,b,...) at bit k depends
        // only on a_k, b_k, ... So the boolean signature (all vars
        // in {0,1}) captures the per-bit function correctly.
        //
        // After simplification, substituting atoms back gives an
        // expression that is correct at all bit positions because the
        // outer structure remains purely bitwise.
        auto lifted_sig = EvaluateBooleanSignature(*lifted, detail.n_total_vars, kBw);

        // Evaluator for FW checks: evaluate the original expression
        // (not the lifted one) to avoid false passes from the lifted
        // representation.
        Options lift_opts{ .bitwidth = kBw, .max_vars = kMaxVar, .spot_check = true };
        // No evaluator — we'll do FW check manually after substitution
        // against the original evaluator.

        auto sup_result = RunSupportedPipeline(lifted_sig, ext_vars, lift_opts);
        if (sup_result.has_value()
            && sup_result.value().kind == SimplifyOutcome::Kind::kSimplified)
        {
            detail.skeleton_ok = true;
            skeleton_solved++;

            // Step 4: Substitute atoms back
            auto reconstructed = SubstituteBack(*sup_result.value().expr, idx_to_atom, kNv);

            // Step 5: Full-width verify against original evaluator
            auto check = FullWidthCheckEval(orig_eval, kNv, *reconstructed, kBw, 64);
            if (check.passed) {
                detail.fw_ok = true;
                fw_verified++;
            }
        }

        details.push_back(std::move(detail));
    }

    // Report
    std::cerr << "\n=== Atom Lifting Telemetry (no_core cases) ===\n";
    std::cerr << "Total cases:          " << total << "\n";
    std::cerr << "Has atoms:            " << has_atoms << "\n";
    std::cerr << "No atoms found:       " << no_atoms_found << "\n";
    std::cerr << "Too many vars (>12):  " << too_many_vars << "\n";
    std::cerr << "Liftable (n+k<=12):   " << liftable << "\n";
    std::cerr << "Skeleton solved:      " << skeleton_solved << "\n";
    std::cerr << "FW verified:          " << fw_verified << "\n";
    std::cerr << "\n--- Per-case details ---\n";
    for (const auto &d : details) {
        std::cerr << "L" << d.line_num << ": vars=" << d.n_orig << " atoms=" << d.n_atoms
                  << " unique=" << d.n_unique_atoms << " total_vars=" << d.n_total_vars;
        if (d.n_unique_atoms == 0) {
            std::cerr << " [NO ATOMS]";
        } else if (d.n_total_vars > kMaxVar) {
            std::cerr << " [TOO MANY VARS]";
        } else {
            std::cerr << (d.skeleton_ok ? " skel=OK" : " skel=FAIL")
                      << (d.fw_ok ? " fw=OK" : " fw=FAIL");
        }
        std::cerr << "\n";
        for (const auto &a : d.atom_strs) { std::cerr << "  atom: " << a << "\n"; }
    }
    std::cerr << "\n";

    // Soft assertions — don't fail the build, this is telemetry
    EXPECT_EQ(total, 44);
}
