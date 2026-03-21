#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ProductIdentityRecoverer.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <set>
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

    uint32_t expr_depth(const Expr &e) {
        uint32_t d = 0;
        for (const auto &c : e.children) { d = std::max(d, expr_depth(*c)); }
        return d + 1;
    }

    uint32_t count_binary_ops(const Expr &e) {
        uint32_t n = 0;
        if (e.children.size() == 2) { n = 1; }
        for (const auto &c : e.children) { n += count_binary_ops(*c); }
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

    std::string route_str(Route r) {
        switch (r) {
            case Route::kBitwiseOnly:
                return "BitwiseOnly";
            case Route::kMixedRewrite:
                return "MixedRewrite";
            default:
                return "Unknown";
        }
    }

} // namespace

TEST(PLDIPolyDiagnostic, AnalyzeUnsolved) {
    std::ifstream file(DATASET_DIR "/simba/pldi_poly.txt");
    ASSERT_TRUE(file.is_open());

    std::string line;
    int line_num          = 0;
    int total_parsed      = 0;
    int simplified_count  = 0;
    int unsupported_count = 0;

    std::map< uint32_t, int > by_vars;
    std::map< uint32_t, int > by_depth;
    std::map< uint32_t, int > by_binary_ops;
    std::map< std::string, int > by_ops;
    std::map< std::string, int > by_route;

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

        total_parsed++;

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast_result.value().expr), 64)
        );

        Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto result = Simplify(
            parse_result.value().sig, parse_result.value().vars, folded_ptr->get(), opts
        );

        if (!result.has_value()) { continue; }
        if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) {
            simplified_count++;
            continue;
        }

        unsupported_count++;

        std::string gt = trim(line.substr(sep + 1));
        auto gt_parse  = ParseToAst(gt, 64);

        auto elim   = EliminateAuxVars(parse_result.value().sig, parse_result.value().vars);
        uint32_t rv = static_cast< uint32_t >(elim.real_vars.size());

        auto cls          = ClassifyStructural(**folded_ptr);
        std::string route = route_str(cls.route);
        by_route[route]++;
        by_vars[rv]++;

        // Trace product identity collapse
        auto pi_expr = CloneExpr(**folded_ptr);
        auto pi =
            CollapseProductIdentities(std::move(pi_expr), parse_result.value().vars, opts);

        std::vector< std::string > var_names;
        for (uint32_t vi = 0; vi < rv; ++vi) {
            var_names.push_back(
                elim.real_vars.empty() ? std::string(1, 'a' + vi) : elim.real_vars[vi]
            );
        }

        std::string collapse_info = pi.changed
            ? "COLLAPSED -> " + Render(*pi.expr, parse_result.value().vars, 64)
            : "no collapse";

        // After collapse, try simplify on the new AST
        std::string reentry_info = "n/a";
        if (pi.changed) {
            auto new_sig = EvaluateBooleanSignature(
                *pi.expr, static_cast< uint32_t >(parse_result.value().vars.size()), 64
            );
            auto new_cls = ClassifyStructural(*pi.expr);
            reentry_info = "route=" + route_str(new_cls.route);

            auto reentry = Simplify(new_sig, parse_result.value().vars, pi.expr.get(), opts);
            if (reentry.has_value()
                && reentry.value().kind == SimplifyOutcome::Kind::kSimplified)
            {
                reentry_info += " -> SIMPLIFIED: "
                    + Render(*reentry.value().expr, parse_result.value().vars, 64);
            } else {
                reentry_info += " -> STILL UNSUPPORTED";
            }
        }

        std::string entry =
            "L" + std::to_string(line_num) + " vars=" + std::to_string(rv) + " route=" + route;

        if (gt_parse.has_value()) {
            auto gt_folded  = FoldConstantBitwise(std::move(gt_parse.value().expr), 64);
            uint32_t d      = expr_depth(*gt_folded);
            uint32_t bops   = count_binary_ops(*gt_folded);
            std::string ops = op_signature(*gt_folded);

            by_depth[d]++;
            by_binary_ops[bops]++;
            by_ops[ops]++;

            entry += " depth=" + std::to_string(d) + " binops=" + std::to_string(bops)
                + " ops={" + ops + "}";
        }

        entry += "\n       OBF: " + obfuscated.substr(0, 80);
        entry += "\n       GT:  " + gt;
        entry += "\n       Collapse: " + collapse_info;
        entry += "\n       Reentry:  " + reentry_info;

        all_unsolved.push_back(entry);
    }

    std::cerr << "\n=== PLDIPoly: " << simplified_count << "/" << total_parsed
              << " simplified, " << unsupported_count << " unsolved ===\n";

    std::cerr << "\n--- By route ---\n";
    for (auto &[k, v] : by_route) { std::cerr << "  " << k << ": " << v << "\n"; }

    std::cerr << "\n--- By variable count ---\n";
    for (auto &[k, v] : by_vars) { std::cerr << "  " << k << " vars: " << v << "\n"; }

    std::cerr << "\n--- By GT depth ---\n";
    for (auto &[k, v] : by_depth) { std::cerr << "  depth " << k << ": " << v << "\n"; }

    std::cerr << "\n--- By GT binary op count ---\n";
    for (auto &[k, v] : by_binary_ops) { std::cerr << "  " << k << " binops: " << v << "\n"; }

    std::cerr << "\n--- By operator set (GT) ---\n";
    for (auto &[k, v] : by_ops) { std::cerr << "  {" << k << "}: " << v << "\n"; }

    std::cerr << "\n--- All unsolved ---\n";
    for (const auto &s : all_unsolved) { std::cerr << "  " << s << "\n"; }

    std::cerr << "\n";
}
