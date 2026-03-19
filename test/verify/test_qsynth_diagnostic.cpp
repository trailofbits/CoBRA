#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/TemplateDecomposer.h"
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
