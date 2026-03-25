#include "ExprParser.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/OperandSimplifier.h"
#include "cobra/core/ProductIdentityRecoverer.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"
#include <fstream>
#include <gtest/gtest.h>
#include <string>

using namespace cobra;

namespace {

    struct AddMulMulStats
    {
        int count     = 0;
        int max_depth = 0;
    };

    void count_add_mul_mul(const Expr &e, int depth, AddMulMulStats &stats) {
        if (e.kind == Expr::Kind::kAdd && e.children.size() == 2
            && e.children[0]->kind == Expr::Kind::kMul && e.children[0]->children.size() == 2
            && e.children[1]->kind == Expr::Kind::kMul && e.children[1]->children.size() == 2)
        {
            stats.count++;
            if (depth > stats.max_depth) { stats.max_depth = depth; }
        }
        for (const auto &c : e.children) { count_add_mul_mul(*c, depth + 1, stats); }
    }

    std::string render_short(const Expr &e, const std::vector< std::string > &vars) {
        auto s = Render(e, vars, 64);
        if (s.size() > 80) { return s.substr(0, 77) + "..."; }
        return s;
    }

} // namespace

// Parse a specific QSynth_EA expression, check for Add(Mul,Mul)
// structure, and trace what the product identity recoverer does.
TEST(ProductIdentityDiag, SimpleProductEncoding) {
    // Line 483: ~(~(~(b*b)))
    // Obfuscated: - (- (- ((b & b) * (b | b) +
    //   (b & ~ b) * (~ b & b)) - 1) - 1) - 1
    std::string obf = "- (- (- ((b & b) * (b | b) + "
                      "(b & ~ b) * (~ b & b)) - 1) - 1) - 1";

    auto ast = ParseToAst(obf, 64);
    ASSERT_TRUE(ast.has_value()) << ast.error().message;
    auto folded = FoldConstantBitwise(std::move(ast.value().expr), 64);
    auto vars   = ast.value().vars;

    std::cout << "Parsed: " << Render(*folded, vars, 64) << "\n";
    auto cls = ClassifyStructural(*folded);
    std::cout << "Route: " << static_cast< int >(cls.route) << "\n";

    AddMulMulStats stats;
    count_add_mul_mul(*folded, 0, stats);
    std::cout << "Add(Mul,Mul) nodes: " << stats.count << " (max depth " << stats.max_depth
              << ")\n";

    // Run operand simplifier first (like the pipeline does)
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&folded](const std::vector< uint64_t > &v) -> uint64_t {
        return EvalExpr(*folded, v, 64);
    };

    auto original = CloneExpr(*folded);
    auto op       = SimplifyMixedOperands(std::move(folded), vars, opts);
    std::cout << "After operand simp: changed=" << op.changed << "\n";
    std::cout << "  expr: " << render_short(*op.expr, vars) << "\n";

    AddMulMulStats stats2;
    count_add_mul_mul(*op.expr, 0, stats2);
    std::cout << "  Add(Mul,Mul) nodes: " << stats2.count << "\n";

    // Run product identity collapse
    auto pi = CollapseProductIdentities(std::move(op.expr), vars, opts);
    std::cout << "After product collapse: changed=" << pi.changed << "\n";
    std::cout << "  expr: " << render_short(*pi.expr, vars) << "\n";

    if (pi.changed) {
        EXPECT_TRUE(
            FullWidthCheck(*original, static_cast< uint32_t >(vars.size()), *pi.expr, {}, 64)
                .passed
        );
    }
}

TEST(ProductIdentityDiag, NestedProductEncoding) {
    // Line 162: GT = -(((a*d)*d)*a)
    // Read from dataset to avoid string escaping issues
    std::ifstream ds(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(ds.is_open());
    std::string line;
    for (int i = 0; i < 162; ++i) { std::getline(ds, line); }
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
    std::string obf = line.substr(0, last_comma);
    while (!obf.empty() && obf.front() == ' ') { obf.erase(0, 1); }

    auto ast = ParseToAst(obf, 64);
    ASSERT_TRUE(ast.has_value()) << ast.error().message;
    auto folded = FoldConstantBitwise(std::move(ast.value().expr), 64);
    auto vars   = ast.value().vars;

    std::cout << "Vars: ";
    for (const auto &v : vars) { std::cout << v << " "; }
    std::cout << "\n";

    AddMulMulStats stats;
    count_add_mul_mul(*folded, 0, stats);
    std::cout << "Add(Mul,Mul) nodes before: " << stats.count << "\n";
    std::cout << "Cost before: " << ComputeCost(*folded).cost.weighted_size << "\n";

    auto original = CloneExpr(*folded);
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&original](const std::vector< uint64_t > &v) -> uint64_t {
        return EvalExpr(*original, v, 64);
    };

    auto op = SimplifyMixedOperands(CloneExpr(*folded), vars, opts);
    std::cout << "After operand simp: changed=" << op.changed << "\n";

    AddMulMulStats stats2;
    count_add_mul_mul(*op.expr, 0, stats2);
    std::cout << "  Add(Mul,Mul) nodes: " << stats2.count << "\n";

    auto pi = CollapseProductIdentities(std::move(op.expr), vars, opts);
    std::cout << "After product collapse: changed=" << pi.changed << "\n";
    std::cout << "  expr: " << render_short(*pi.expr, vars) << "\n";
    std::cout << "  Cost: " << ComputeCost(*pi.expr).cost.weighted_size << "\n";

    AddMulMulStats stats3;
    count_add_mul_mul(*pi.expr, 0, stats3);
    std::cout << "  Add(Mul,Mul) remaining: " << stats3.count << "\n";
}

std::string read_obf(const std::string &line) {
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
    if (last_comma == std::string::npos) { return ""; }
    auto obf = line.substr(0, last_comma);
    while (!obf.empty() && obf.front() == ' ') { obf.erase(0, 1); }
    return obf;
}

void scan_dataset(const std::string &name, const std::string &path) {
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "Cannot open " << path;

    std::string line;
    int total = 0, has_amm = 0, collapsed = 0;
    int amm_after_ops   = 0;
    int simplified_full = 0, unsupported = 0;
    int collapse_enabled_new = 0;
    uint64_t cost_before_sum = 0, cost_after_sum = 0;

    while (std::getline(file, line)) {
        if (line.empty()) { continue; }
        auto obf = read_obf(line);
        if (obf.empty()) { continue; }

        auto ast = ParseToAst(obf, 64);
        if (!ast.has_value()) { continue; }

        auto folded = FoldConstantBitwise(std::move(ast.value().expr), 64);
        auto vars   = ast.value().vars;
        total++;

        AddMulMulStats stats0;
        count_add_mul_mul(*folded, 0, stats0);
        if (stats0.count > 0) { has_amm++; }

        auto original = CloneExpr(*folded);
        Options opts;
        opts.bitwidth  = 64;
        opts.evaluator = [&original](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(*original, v, 64);
        };

        auto op = SimplifyMixedOperands(CloneExpr(*folded), vars, opts);

        AddMulMulStats stats1;
        count_add_mul_mul(*op.expr, 0, stats1);
        if (stats1.count > 0) { amm_after_ops++; }

        uint32_t cost_pre  = ComputeCost(*op.expr).cost.weighted_size;
        auto pi            = CollapseProductIdentities(std::move(op.expr), vars, opts);
        uint32_t cost_post = ComputeCost(*pi.expr).cost.weighted_size;

        if (pi.changed) {
            collapsed++;
            cost_before_sum += cost_pre;
            cost_after_sum  += cost_post;
        }

        // Full pipeline
        auto parse_result = ParseAndEvaluate(obf, 64);
        if (!parse_result.has_value()) { continue; }
        auto result =
            Simplify(parse_result.value().sig, parse_result.value().vars, folded.get(), opts);
        if (result.has_value()) {
            if (result.value().kind == SimplifyOutcome::Kind::kSimplified) {
                simplified_full++;
            } else {
                unsupported++;
            }
        }
    }

    std::cout << "\n=== " << name << " ===\n"
              << "Total parsed: " << total << "\n"
              << "Has Add(Mul,Mul) before ops: " << has_amm << "\n"
              << "Has Add(Mul,Mul) after ops: " << amm_after_ops << "\n"
              << "Product collapse fired: " << collapsed << "\n";
    if (collapsed > 0) {
        std::cout << "  Avg cost before collapse: " << (cost_before_sum / collapsed) << "\n"
                  << "  Avg cost after collapse: " << (cost_after_sum / collapsed) << "\n";
    }
    std::cout << "Full pipeline simplified: " << simplified_full << "\n"
              << "Full pipeline unsupported: " << unsupported << "\n";
}

TEST(ProductIdentityDiag, QSynthEADatasetScan) {
    scan_dataset("QSynth_EA", DATASET_DIR "/gamba/qsynth_ea.txt");
}

TEST(ProductIdentityDiag, SyntiaDatasetScan) {
    scan_dataset("Syntia", DATASET_DIR "/gamba/syntia.txt");
}

TEST(ProductIdentityDiag, PLDIPolyDatasetScan) {
    scan_dataset("PLDI Poly", DATASET_DIR "/simba/pldi_poly.txt");
}

// Check the classification of post-collapse expressions and
// whether any become polynomial or affine.
TEST(ProductIdentityDiag, PostCollapseClassification) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::string line;
    int total_collapsed = 0;
    int post_bitwise = 0, post_multilinear = 0;
    int post_power = 0, post_mixed = 0, post_unsup = 0;
    int has_mul_of_consts = 0;

    while (std::getline(file, line)) {
        if (line.empty()) { continue; }
        auto obf = read_obf(line);
        if (obf.empty()) { continue; }

        auto ast = ParseToAst(obf, 64);
        if (!ast.has_value()) { continue; }
        auto folded = FoldConstantBitwise(std::move(ast.value().expr), 64);
        auto vars   = ast.value().vars;

        auto original = CloneExpr(*folded);
        Options opts;
        opts.bitwidth  = 64;
        opts.evaluator = [&original](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(*original, v, 64);
        };

        auto op = SimplifyMixedOperands(CloneExpr(*folded), vars, opts);
        auto pi = CollapseProductIdentities(std::move(op.expr), vars, opts);
        if (!pi.changed) { continue; }
        total_collapsed++;

        auto cls = ClassifyStructural(*pi.expr);
        switch (cls.route) {
            case Route::kBitwiseOnly:
                post_bitwise++;
                break;
            case Route::kMultilinear:
                post_multilinear++;
                break;
            case Route::kPowerRecovery:
                post_power++;
                break;
            case Route::kMixedRewrite:
                post_mixed++;
                break;
            case Route::kUnsupported:
                post_unsup++;
                break;
        }
    }

    std::cout << "\n=== Post-collapse classification "
                 "(QSynth_EA) ===\n"
              << "Collapsed: " << total_collapsed << "\n"
              << "  BitwiseOnly: " << post_bitwise << "\n"
              << "  Multilinear: " << post_multilinear << "\n"
              << "  PowerRecovery: " << post_power << "\n"
              << "  MixedRewrite: " << post_mixed << "\n"
              << "  Unsupported: " << post_unsup << "\n";
}

// Trace the 6 Multilinear post-collapse expressions.
TEST(ProductIdentityDiag, MultilinearAfterCollapse) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::string line;
    int line_num = 0;

    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) { continue; }
        auto obf = read_obf(line);
        if (obf.empty()) { continue; }

        auto ast = ParseToAst(obf, 64);
        if (!ast.has_value()) { continue; }
        auto folded = FoldConstantBitwise(std::move(ast.value().expr), 64);
        auto vars   = ast.value().vars;

        auto original = CloneExpr(*folded);
        Options opts;
        opts.bitwidth  = 64;
        opts.evaluator = [&original](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(*original, v, 64);
        };

        auto op = SimplifyMixedOperands(CloneExpr(*folded), vars, opts);
        auto pi = CollapseProductIdentities(std::move(op.expr), vars, opts);
        if (!pi.changed) { continue; }

        auto cls = ClassifyStructural(*pi.expr);
        if (cls.route != Route::kMultilinear) { continue; }

        // Get ground truth
        int depth = 0;
        size_t lc = std::string::npos;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '(') {
                ++depth;
            } else if (line[i] == ')') {
                --depth;
            } else if (line[i] == ',' && depth == 0) {
                lc = i;
            }
        }
        std::string gt = (lc != std::string::npos) ? line.substr(lc + 1) : "?";

        std::cout << "Line " << line_num << ": GT=" << gt << "\n";
        std::cout << "  Post-collapse: " << Render(*pi.expr, vars, 64) << "\n";
        std::cout << "  Cost: " << ComputeCost(*pi.expr).cost.weighted_size << "\n";

        // Try full pipeline
        auto parse_result = ParseAndEvaluate(obf, 64);
        ASSERT_TRUE(parse_result.has_value());
        auto result =
            Simplify(parse_result.value().sig, parse_result.value().vars, folded.get(), opts);
        if (result.has_value()) {
            auto kind = result.value().kind;
            std::cout << "  Pipeline: "
                      << (kind == SimplifyOutcome::Kind::kSimplified ? "Simplified"
                                                                     : "Unsupported")
                      << "\n";
            if (kind == SimplifyOutcome::Kind::kSimplified) {
                std::cout << "  Result: "
                          << Render(
                                 *result.value().expr,
                                 result.value().real_vars.empty() ? vars
                                                                  : result.value().real_vars,
                                 64
                             )
                          << "\n";
            }
            std::cout << "  Route: "
                      << static_cast< int >(result.value().diag.classification.route)
                      << "  Rewrite: " << result.value().diag.transform_produced_candidate
                      << "\n";
        }
        std::cout << "\n";
    }
}
