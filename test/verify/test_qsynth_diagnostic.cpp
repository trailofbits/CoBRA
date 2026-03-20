#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/DecompositionEngine.h"
#include "cobra/core/Expr.h"
#include "cobra/core/GhostBasis.h"
#include "cobra/core/GhostResidualSolver.h"
#include "cobra/core/MultivarPolyRecovery.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/TemplateDecomposer.h"
#include "cobra/core/WeightedPolyFit.h"
#include <fstream>
#include <gtest/gtest.h>
#include <map>
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
                .extractor        = name,
                .supported_solved = sup_solved,
                .sup_verified     = sup_verified,
                .sup_recombines   = sup_recombine,
                .poly_solved      = poly_solved,
                .poly_recombines  = poly_recombine,
                .tmpl_solved      = tmpl_solved,
                .tmpl_recombines  = tmpl_recombine,
                .ghost_solved     = ghost_solved,
                .ghost_recombines = ghost_recombine,
                .res_support      = res_count,
                .degree_floor     = deg_floor,
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
                std::string reason = probe->extractor
                    + " sup=" + std::to_string(probe->res_support)
                    + " dfloor=" + std::to_string(probe->degree_floor);
                failure_reasons[reason]++;
            }
            std::cerr << "  " << label << " core=" << probe->extractor
                      << " res_sup=" << probe->res_support
                      << " dfloor=" << static_cast< int >(probe->degree_floor)
                      << " sup_fw=" << probe->sup_recombines
                      << " poly_fw=" << probe->poly_recombines
                      << " ghost_fw=" << probe->ghost_recombines
                      << " tmpl_ok=" << probe->tmpl_solved
                      << " tmpl_fw=" << probe->tmpl_recombines << "\n";
        } else {
            std::string reason = any_core ? "all_direct_fail" : "no_core";
            failure_reasons[reason]++;
            std::cerr << "  " << label << " " << reason << "\n";
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
