/// Audit diagnostic for the semantic polynomial MBA pass proposal.
///
/// Three tests:
///   1. QSynthUnsupportedPopulation — full population shape analysis
///   2. L278FactorSurrogateReconstruction — factor-level bitwise surrogate
///      experiment (proves surrogates are FW-correct individually but
///      additive skeleton diverges at full width)
///   3. L278WholeExprBitwiseReconstruction — whole-expression bitwise
///      reconstruction falsification (confirms Boolean-shadow is
///      non-reconstructive for this expression class)
///
/// Conclusion: paper-derived semantic polynomial MBA passes have no
/// target population in QSynth unsupported. The remaining problem is
/// structural recovery of product-of-bitwise functions, not classification.

#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/SimplifyOutcome.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace cobra;

namespace {

    constexpr uint32_t kBw = 64;

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

    // ── AST analysis helpers (non-destructive, operate on const Expr&) ──

    void collect_var_indices(const Expr &e, std::set< uint32_t > &out) {
        if (e.kind == Expr::Kind::kVariable) {
            out.insert(e.var_index);
            return;
        }
        for (const auto &c : e.children) { collect_var_indices(*c, out); }
    }

    uint32_t support_size(const Expr &e) {
        std::set< uint32_t > vars;
        collect_var_indices(e, vars);
        return static_cast< uint32_t >(vars.size());
    }

    bool contains_shr(const Expr &e) {
        if (e.kind == Expr::Kind::kShr) { return true; }
        for (const auto &c : e.children) {
            if (contains_shr(*c)) { return true; }
        }
        return false;
    }

    bool contains_mul(const Expr &e) {
        if (e.kind == Expr::Kind::kMul) { return true; }
        for (const auto &c : e.children) {
            if (contains_mul(*c)) { return true; }
        }
        return false;
    }

    bool is_bitwise_op(Expr::Kind k) {
        return k == Expr::Kind::kAnd || k == Expr::Kind::kOr || k == Expr::Kind::kXor
            || k == Expr::Kind::kNot;
    }

    bool is_purely_bitwise(const Expr &e) {
        if (e.kind == Expr::Kind::kConstant || e.kind == Expr::Kind::kVariable) { return true; }
        if (!is_bitwise_op(e.kind)) { return false; }
        for (const auto &c : e.children) {
            if (!is_purely_bitwise(*c)) { return false; }
        }
        return true;
    }

    // ── Additive term decomposition (non-destructive) ──
    //
    // Collects pointers to additive children by walking Add chains.
    // Does NOT move/consume — just observes.

    void collect_additive_terms(const Expr &e, std::vector< const Expr * > &terms) {
        if (e.kind == Expr::Kind::kAdd) {
            for (const auto &c : e.children) { collect_additive_terms(*c, terms); }
        } else {
            terms.push_back(&e);
        }
    }

    // ── Multiplicative factor decomposition (non-destructive) ──

    void collect_mul_factors(const Expr &e, std::vector< const Expr * > &factors) {
        if (e.kind == Expr::Kind::kMul) {
            for (const auto &c : e.children) { collect_mul_factors(*c, factors); }
        } else {
            factors.push_back(&e);
        }
    }

    // ── Per-term analysis ──

    struct TermInfo
    {
        uint32_t factor_count       = 0; // non-constant factors
        uint32_t const_factor_count = 0;
        uint32_t max_factor_support = 0;
        uint32_t term_support       = 0;
        bool all_factors_bitwise    = true;
        bool has_shr_factor         = false;
        bool is_bare_factor         = false; // single non-product term
    };

    TermInfo analyze_term(const Expr &term) {
        TermInfo info;

        std::vector< const Expr * > factors;
        collect_mul_factors(term, factors);

        if (factors.size() == 1 && term.kind != Expr::Kind::kMul) {
            info.is_bare_factor = true;
        }

        for (const Expr *f : factors) {
            if (IsConstantSubtree(*f)) {
                info.const_factor_count++;
                continue;
            }
            info.factor_count++;
            uint32_t fsup            = support_size(*f);
            info.max_factor_support  = std::max(info.max_factor_support, fsup);
            info.term_support       += fsup; // upper bound (vars may overlap)

            if (!is_purely_bitwise(*f)) { info.all_factors_bitwise = false; }
            if (contains_shr(*f)) { info.has_shr_factor = true; }
        }

        // Correct term_support to actual unique vars
        std::set< uint32_t > vars;
        collect_var_indices(term, vars);
        info.term_support = static_cast< uint32_t >(vars.size());

        return info;
    }

    // ── Expression-level shape analysis ──

    struct ShapeAnalysis
    {
        uint32_t real_var_count           = 0;
        uint32_t additive_term_count      = 0;
        uint32_t max_mul_degree           = 0; // max non-constant factors in any term
        uint32_t max_factor_support       = 0; // max support of any single factor
        uint32_t mul_term_count           = 0; // terms with degree >= 2
        uint32_t bitwise_factor_count     = 0; // factors that are purely bitwise
        uint32_t non_bitwise_factor_count = 0;
        bool has_shr                      = false;
        bool has_mul                      = false;
        bool matches_narrow_shape         = true; // sum of (coeff * product) terms
        uint32_t narrow_shape_terms       = 0;    // terms matching narrow shape

        // PCT admission candidates
        bool pct_admissible = false; // t<=3, d<=3, d*t<=9, no shr

        // Semantic bitwise check on whole expression
        bool sig_image_01   = false; // image ⊆ {0, 1}
        bool sig_image_neg1 = false; // image ⊆ {-2, -1} (i.e. 0xFFFE, 0xFFFF)

        // Structural classification
        SemanticClass semantic_class = SemanticClass::kLinear;
        StructuralFlag flags         = kSfNone;

        // Reason category from simplifier
        std::string reason_category;
    };

    std::string flags_str(StructuralFlag flags) {
        std::string s;
        auto append = [&](StructuralFlag f, const char *name) {
            if (HasFlag(flags, f)) {
                if (!s.empty()) { s += "|"; }
                s += name;
            }
        };
        append(kSfHasBitwise, "BW");
        append(kSfHasArithmetic, "Arith");
        append(kSfHasMul, "Mul");
        append(kSfHasMultilinearProduct, "MultilinProd");
        append(kSfHasSingletonPower, "SingPow");
        append(kSfHasSingletonPowerGt2, "SingPow>2");
        append(kSfHasMixedProduct, "MixedProd");
        append(kSfHasBitwiseOverArith, "BWoArith");
        append(kSfHasArithOverBitwise, "ArithoBW");
        append(kSfHasMultivarHighPower, "MultivarHiPow");
        append(kSfHasUnknownShape, "Unknown");
        return s.empty() ? "none" : s;
    }

    std::string semantic_str(SemanticClass sc) {
        switch (sc) {
            case SemanticClass::kLinear:
                return "linear";
            case SemanticClass::kSemilinear:
                return "semilinear";
            case SemanticClass::kPolynomial:
                return "polynomial";
            case SemanticClass::kNonPolynomial:
                return "non-polynomial";
        }
        return "?";
    }

} // namespace

// ── Iterative ProductIdentityCollapse diagnostic ──
//
// Tests whether CoBRA can already solve MBA-obfuscated multiplications at
// various nesting depths, and where it fails.

TEST(SemanticPolyAudit, IterativeCollapseDepth) {
    std::cerr << "\n╔═══════════════════════════════════════════════╗\n";
    std::cerr << "║  Iterative ProductIdentityCollapse Diagnostic  ║\n";
    std::cerr << "╚═══════════════════════════════════════════════╝\n\n";

    struct TestCase
    {
        const char *name;
        const char *expr;
        const char *expected_gt;
    };

    // Layer 0: no MBA obfuscation
    // Layer 1: one layer (inner c*a obfuscated)
    // Layer 2: two layers (inner c*a + outer c*(c*a^a))
    // Layer 3: full L278 (two layers + OR with c|a, further obfuscated)

    TestCase cases[] = {
        {                                                          "L0: c*a (direct)","c * a",           "a * c"                                                          },
        {                                "L1: MBA(c*a) = (c&a)*(c|a) + (c&~a)*(~c&a)",
         "(c & a) * (c | a) + (c & ~a) * (~c & a)",           "a * c"                },
        {                                            "L2: c * (MBA(c*a) ^ a) via MBA",
         "(c & ((c & a) * (c | a) + (c & ~a) * (~c & a) ^ a))"
         " * (c | ((c & a) * (c | a) + (c & ~a) * (~c & a) ^ a))"
         " + (c & ~((c & a) * (c | a) + (c & ~a) * (~c & a) ^ a))"
         " * (~c & ((c & a) * (c | a) + (c & ~a) * (~c & a) ^ a))", "c * (c * a ^ a)" },
        { "L2b: c * (MBA(c*a) XOR-via-sub a) via MBA — XOR obfuscated as (x|a)-(x&a)",
         "(c & (((c & a) * (c | a) + (c & ~a) * (~c & a) | a)"
         " - ((c & a) * (c | a) + (c & ~a) * (~c & a) & a)))"
         " * (c | (((c & a) * (c | a) + (c & ~a) * (~c & a) | a)"
         " - ((c & a) * (c | a) + (c & ~a) * (~c & a) & a)))"
         " + (c & ~(((c & a) * (c | a) + (c & ~a) * (~c & a) | a)"
         " - ((c & a) * (c | a) + (c & ~a) * (~c & a) & a)))"
         " * (~c & (((c & a) * (c | a) + (c & ~a) * (~c & a) | a)"
         " - ((c & a) * (c | a) + (c & ~a) * (~c & a) & a)))", "c * (c * a ^ a)"     },
    };

    for (const auto &tc : cases) {
        auto parse = ParseAndEvaluate(tc.expr, kBw);
        if (!parse.has_value()) {
            std::cerr << tc.name << ": PARSE FAILED\n";
            continue;
        }
        auto ast = ParseToAst(tc.expr, kBw);
        if (!ast.has_value()) {
            std::cerr << tc.name << ": AST PARSE FAILED\n";
            continue;
        }

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast.value().expr), kBw)
        );

        Options opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, kBw);
        };

        auto result = Simplify(parse.value().sig, parse.value().vars, folded_ptr->get(), opts);

        std::cerr << tc.name << ":\n";
        std::cerr << "  Cost: " << ComputeCost(**folded_ptr).cost.weighted_size;

        if (!result.has_value()) {
            std::cerr << " → ERROR\n";
            continue;
        }

        if (result.value().kind == SimplifyOutcome::Kind::kSimplified) {
            auto &sr = result.value();
            std::cerr << " → SIMPLIFIED: " << Render(*sr.expr, sr.real_vars) << " (cost "
                      << ComputeCost(*sr.expr).cost.weighted_size
                      << ", verified=" << sr.verified << ")\n";
        } else {
            std::cerr << " → UNSUPPORTED\n";
            auto &diag = result.value().diag;
            std::cerr << "  struct_transforms=" << diag.structural_transform_rounds
                      << " candidate_produced=" << diag.transform_produced_candidate
                      << " candidate_failed_verify=" << diag.candidate_failed_verification
                      << "\n";
            auto &tel = result.value().telemetry;
            std::cerr << "  expansions=" << tel.total_expansions
                      << " depth=" << tel.max_depth_reached
                      << " candidates_verified=" << tel.candidates_verified
                      << " queue_hw=" << tel.queue_high_water << "\n";
            if (diag.reason_code.has_value()) {
                std::cerr << "  reason: " << diag.reason << "\n";
            }
        }
        std::cerr << "  Expected: " << tc.expected_gt << "\n\n";
    }

    // ── AST structure analysis for L278 ──
    // Count Add(Mul, Mul) sites and show what blocks ProductIdentityCollapse.

    auto count_add_mul_mul = [](const Expr &root, auto &self) -> int {
        int count = 0;
        if (root.kind == Expr::Kind::kAdd && root.children.size() == 2) {
            bool lhs_mul = root.children[0]->kind == Expr::Kind::kMul
                && root.children[0]->children.size() == 2;
            bool rhs_mul = root.children[1]->kind == Expr::Kind::kMul
                && root.children[1]->children.size() == 2;
            if (lhs_mul && rhs_mul) { count++; }
        }
        for (const auto &c : root.children) { count += self(*c, self); }
        return count;
    };

    // Show top-level structure of an AST
    auto show_structure = [](const Expr &root, const std::vector< std::string > &vars,
                             int depth, auto &self) -> void {
        if (depth > 4) {
            std::cerr << "...";
            return;
        }
        std::string indent(depth * 2, ' ');
        switch (root.kind) {
            case Expr::Kind::kAdd:
                std::cerr << "Add";
                break;
            case Expr::Kind::kMul:
                std::cerr << "Mul";
                break;
            case Expr::Kind::kAnd:
                std::cerr << "And";
                break;
            case Expr::Kind::kOr:
                std::cerr << "Or";
                break;
            case Expr::Kind::kXor:
                std::cerr << "Xor";
                break;
            case Expr::Kind::kNot:
                std::cerr << "Not";
                break;
            case Expr::Kind::kNeg:
                std::cerr << "Neg";
                break;
            case Expr::Kind::kShr:
                std::cerr << "Shr";
                break;
            case Expr::Kind::kConstant:
                std::cerr << static_cast< int64_t >(root.constant_val);
                return;
            case Expr::Kind::kVariable:
                if (root.var_index < vars.size()) {
                    std::cerr << vars[root.var_index];
                } else {
                    std::cerr << "v" << root.var_index;
                }
                return;
        }
        std::cerr << "(";
        for (size_t i = 0; i < root.children.size(); ++i) {
            if (i > 0) { std::cerr << ", "; }
            self(*root.children[i], vars, depth + 1, self);
        }
        std::cerr << ")";
    };

    // L278 from dataset file
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());
    std::string line;
    int line_num = 0;
    while (std::getline(file, line)) {
        ++line_num;
        if (line_num != 278) { continue; }
        size_t sep = find_separator(line);
        ASSERT_NE(sep, std::string::npos);
        std::string obfuscated = trim(line.substr(0, sep));
        std::string gt_str     = trim(line.substr(sep + 1));

        auto parse = ParseAndEvaluate(obfuscated, kBw);
        ASSERT_TRUE(parse.has_value());
        auto ast = ParseToAst(obfuscated, kBw);
        ASSERT_TRUE(ast.has_value());

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast.value().expr), kBw)
        );

        Options opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, kBw);
        };

        auto result = Simplify(parse.value().sig, parse.value().vars, folded_ptr->get(), opts);

        std::cerr << "L3: Full L278 from dataset:\n";
        std::cerr << "  Cost: " << ComputeCost(**folded_ptr).cost.weighted_size;

        if (result.has_value() && result.value().kind == SimplifyOutcome::Kind::kSimplified) {
            auto &sr = result.value();
            std::cerr << " → SIMPLIFIED: " << Render(*sr.expr, sr.real_vars) << " (cost "
                      << ComputeCost(*sr.expr).cost.weighted_size
                      << ", verified=" << sr.verified << ")\n";
        } else if (result.has_value()) {
            std::cerr << " → UNSUPPORTED\n";
            auto &diag = result.value().diag;
            std::cerr << "  struct_transforms=" << diag.structural_transform_rounds
                      << " candidate_produced=" << diag.transform_produced_candidate
                      << " candidate_failed_verify=" << diag.candidate_failed_verification
                      << "\n";
            auto &tel = result.value().telemetry;
            std::cerr << "  expansions=" << tel.total_expansions
                      << " depth=" << tel.max_depth_reached
                      << " candidates_verified=" << tel.candidates_verified
                      << " queue_hw=" << tel.queue_high_water << "\n";
            if (diag.reason_code.has_value()) {
                std::cerr << "  reason: " << diag.reason << "\n";
            }
        }
        std::cerr << "  Expected: " << gt_str << "\n";

        int sites = count_add_mul_mul(**folded_ptr, count_add_mul_mul);
        std::cerr << "  Add(Mul,Mul) sites in folded AST: " << sites << "\n";

        // Show each Add(Mul,Mul) site's factor signatures
        auto dump_sites = [&](const Expr &root, const std::vector< std::string > &vars,
                              auto &self) -> void {
            if (root.kind == Expr::Kind::kAdd && root.children.size() == 2) {
                bool lhs_mul = root.children[0]->kind == Expr::Kind::kMul
                    && root.children[0]->children.size() == 2;
                bool rhs_mul = root.children[1]->kind == Expr::Kind::kMul
                    && root.children[1]->children.size() == 2;
                if (lhs_mul && rhs_mul) {
                    auto nv_local = static_cast< uint32_t >(vars.size());
                    auto s0 =
                        EvaluateBooleanSignature(*root.children[0]->children[0], nv_local, kBw);
                    auto s1 =
                        EvaluateBooleanSignature(*root.children[0]->children[1], nv_local, kBw);
                    auto s2 =
                        EvaluateBooleanSignature(*root.children[1]->children[0], nv_local, kBw);
                    auto s3 =
                        EvaluateBooleanSignature(*root.children[1]->children[1], nv_local, kBw);

                    // Check pairwise-disjoint sigs for i=0,o=1,l=2,r=3
                    bool disjoint = true;
                    for (size_t j = 0; j < s0.size(); ++j) {
                        uint64_t m  = Bitmask(kBw);
                        uint64_t mi = s0[j] & m, ml = s2[j] & m, mr = s3[j] & m;
                        if (((mi & ml) | (mi & mr) | (ml & mr)) != 0u) {
                            disjoint = false;
                            break;
                        }
                    }

                    std::string f0 = Render(*root.children[0]->children[0], vars);
                    std::string f1 = Render(*root.children[0]->children[1], vars);
                    if (f0.size() > 50) { f0 = f0.substr(0, 47) + "..."; }
                    if (f1.size() > 50) { f1 = f1.substr(0, 47) + "..."; }
                    std::cerr << "  Site: Mul(" << f0 << ", " << f1 << ") + Mul(...)"
                              << " disjoint=" << disjoint << " sigs=[" << s0[0] << "," << s0[1]
                              << "," << s0[2] << "," << s0[3] << "]x[" << s2[0] << "," << s2[1]
                              << "," << s2[2] << "," << s2[3] << "]\n";
                }
            }
            for (const auto &c : root.children) { self(*c, vars, self); }
        };
        dump_sites(**folded_ptr, parse.value().vars, dump_sites);
        std::cerr << "  Top-level structure: ";
        show_structure(**folded_ptr, parse.value().vars, 0, show_structure);
        std::cerr << "\n\n";
        break;
    }

    // Also show L2b's structure for comparison
    {
        std::string l2b_expr = "(c & (((c & a) * (c | a) + (c & ~a) * (~c & a) | a)"
                               " - ((c & a) * (c | a) + (c & ~a) * (~c & a) & a)))"
                               " * (c | (((c & a) * (c | a) + (c & ~a) * (~c & a) | a)"
                               " - ((c & a) * (c | a) + (c & ~a) * (~c & a) & a)))"
                               " + (c & ~(((c & a) * (c | a) + (c & ~a) * (~c & a) | a)"
                               " - ((c & a) * (c | a) + (c & ~a) * (~c & a) & a)))"
                               " * (~c & (((c & a) * (c | a) + (c & ~a) * (~c & a) | a)"
                               " - ((c & a) * (c | a) + (c & ~a) * (~c & a) & a)))";
        auto l2b_ast         = ParseToAst(l2b_expr, kBw);
        if (l2b_ast.has_value()) {
            auto l2b_folded = FoldConstantBitwise(std::move(l2b_ast.value().expr), kBw);
            int l2b_sites   = count_add_mul_mul(*l2b_folded, count_add_mul_mul);
            std::cerr << "L2b comparison:\n";
            std::cerr << "  Add(Mul,Mul) sites: " << l2b_sites << "\n";
            std::cerr << "  Top-level structure: ";
            show_structure(*l2b_folded, l2b_ast.value().vars, 0, show_structure);
            std::cerr << "\n\n";
        }
    }
}

// ── Minimal Boolean-shadow reproducer ──
//
// The atomic phenomenon: (x&y)*(x|y) + (x&~y)*(~x&y) is the standard MBA
// identity for x*y. On {0,1}, multiplication collapses to AND, so this
// evaluates as x&y — a bitwise function. At full width, it evaluates as x*y,
// which is not bitwise. This is the building block of all 41 unsupported
// QSynth cases and explains why Boolean-signature-based reconstruction fails.
//
// The L278 ground truth c*(c*a^a)|(c|a) contains this pattern nested inside
// its obfuscated form, making it impossible to reconstruct from Boolean
// signatures alone.

TEST(SemanticPolyAudit, MinimalBooleanShadowReproducer) {
    std::cerr << "\n╔═══════════════════════════════════════════════╗\n";
    std::cerr << "║  Minimal Boolean-Shadow Divergence Reproducer  ║\n";
    std::cerr << "╚═══════════════════════════════════════════════╝\n\n";

    // The MBA identity for multiplication: (x&y)*(x|y) + (x&~y)*(~x&y) = x*y
    // On {0,1}: equals x&y (bitwise)
    // At full width: equals x*y (arithmetic)

    const std::vector< std::string > vars = { "x", "y" };
    constexpr uint32_t kNv                = 2;

    // Build: (x&y)*(x|y) + (x&~y)*(~x&y)
    auto mba_mul = Expr::Add(
        Expr::Mul(
            Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
            Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1))
        ),
        Expr::Mul(
            Expr::BitwiseAnd(Expr::Variable(0), Expr::BitwiseNot(Expr::Variable(1))),
            Expr::BitwiseAnd(Expr::BitwiseNot(Expr::Variable(0)), Expr::Variable(1))
        )
    );

    std::cerr << "MBA identity: " << Render(*mba_mul, vars) << "\n";

    // Verify it equals x*y at full width
    auto x_times_y = Expr::Mul(Expr::Variable(0), Expr::Variable(1));

    std::cerr << "\n── Full-width equivalence to x*y ──\n";
    auto mul_eval = Evaluator::FromExpr(*x_times_y, kBw);
    auto fw       = FullWidthCheckEval(mul_eval, kNv, *mba_mul, kBw, 256);
    std::cerr << "  MBA == x*y at full width (256 samples): " << (fw.passed ? "YES" : "NO")
              << "\n";

    // Verify it equals x&y on {0,1}
    auto x_and_y = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    auto mba_sig = EvaluateBooleanSignature(*mba_mul, kNv, kBw);
    auto and_sig = EvaluateBooleanSignature(*x_and_y, kNv, kBw);

    bool bool_match = (mba_sig == and_sig);
    std::cerr << "  MBA == x&y on {0,1}: " << (bool_match ? "YES" : "NO") << "\n";

    // Verify x&y does NOT equal x*y at full width
    auto and_eval = Evaluator::FromExpr(*x_and_y, kBw);
    auto fw_and   = FullWidthCheckEval(mul_eval, kNv, *x_and_y, kBw, 256);
    std::cerr << "  x&y == x*y at full width: " << (fw_and.passed ? "YES" : "NO") << "\n";

    if (!fw_and.passed && !fw_and.failing_input.empty()) {
        uint64_t x = fw_and.failing_input[0];
        uint64_t y = fw_and.failing_input[1];
        std::cerr << "  Divergence at x=" << x << " y=" << y
                  << ": x*y=" << ((x * y) & Bitmask(kBw)) << " x&y=" << (x & y) << "\n";
    }

    // Show the Boolean shadow explicitly
    std::cerr << "\n── Boolean shadow ──\n";
    std::cerr << "  Truth table (x=0,1; y=0,1):\n";
    for (uint32_t i = 0; i < 4; ++i) {
        uint64_t x                 = i & 1;
        uint64_t y                 = (i >> 1) & 1;
        std::vector< uint64_t > pt = { x, y };
        uint64_t mba_val           = EvalExpr(*mba_mul, pt, kBw);
        uint64_t and_val           = EvalExpr(*x_and_y, pt, kBw);
        uint64_t mul_val           = EvalExpr(*x_times_y, pt, kBw);
        std::cerr << "    x=" << x << " y=" << y << "  MBA=" << mba_val << "  x&y=" << and_val
                  << "  x*y=" << mul_val << "\n";
    }

    std::cerr << "\n  Full-width examples:\n";
    std::vector< std::pair< uint64_t, uint64_t > > examples = {
        {     2,     3 },
        {     5,     7 },
        {   255,   256 },
        { 0x100, 0x200 }
    };
    for (auto [x, y] : examples) {
        std::vector< uint64_t > pt = { x, y };
        uint64_t mba_val           = EvalExpr(*mba_mul, pt, kBw);
        uint64_t and_val           = EvalExpr(*x_and_y, pt, kBw);
        uint64_t mul_val           = EvalExpr(*x_times_y, pt, kBw);
        std::cerr << "    x=" << x << " y=" << y << "  MBA=" << mba_val << "  x&y=" << and_val
                  << "  x*y=" << mul_val << (mba_val == mul_val ? "  ✓ MBA==x*y" : "  BUG")
                  << (mba_val != and_val ? "  ✗ MBA≠x&y" : "") << "\n";
    }

    // Now demonstrate the compound failure: nest this inside a GT-like structure
    // GT: c*(c*a^a) | (c|a)
    // Using MBA identity for each multiplication gives an expression that
    // looks bitwise on {0,1} but isn't at full width.

    std::cerr << "\n── Compound: GT structure with MBA-obfuscated multiply ──\n";

    // Build c*a using MBA identity (reusing x=c=var1, y=a=var0)
    auto mba_ca = Expr::Add(
        Expr::Mul(
            Expr::BitwiseAnd(Expr::Variable(1), Expr::Variable(0)),
            Expr::BitwiseOr(Expr::Variable(1), Expr::Variable(0))
        ),
        Expr::Mul(
            Expr::BitwiseAnd(Expr::Variable(1), Expr::BitwiseNot(Expr::Variable(0))),
            Expr::BitwiseAnd(Expr::BitwiseNot(Expr::Variable(1)), Expr::Variable(0))
        )
    );

    // c*a ^ a
    auto ca_xor_a = Expr::BitwiseXor(CloneExpr(*mba_ca), Expr::Variable(0));

    // c * (c*a ^ a) using MBA identity
    auto mba_outer = Expr::Add(
        Expr::Mul(
            Expr::BitwiseAnd(Expr::Variable(1), CloneExpr(*ca_xor_a)),
            Expr::BitwiseOr(Expr::Variable(1), CloneExpr(*ca_xor_a))
        ),
        Expr::Mul(
            Expr::BitwiseAnd(Expr::Variable(1), Expr::BitwiseNot(CloneExpr(*ca_xor_a))),
            Expr::BitwiseAnd(Expr::BitwiseNot(Expr::Variable(1)), CloneExpr(*ca_xor_a))
        )
    );

    // c*(c*a^a) | (c|a)
    auto compound = Expr::BitwiseOr(
        std::move(mba_outer), Expr::BitwiseOr(Expr::Variable(1), Expr::Variable(0))
    );

    const std::vector< std::string > ac_vars = { "a", "c" };

    std::cerr << "  Compound cost: " << ComputeCost(*compound).cost.weighted_size << "\n";

    // Build true GT: c*(c*a^a) | (c|a) directly
    auto true_gt = Expr::BitwiseOr(
        Expr::Mul(
            Expr::Variable(1),
            Expr::BitwiseXor(Expr::Mul(Expr::Variable(1), Expr::Variable(0)), Expr::Variable(0))
        ),
        Expr::BitwiseOr(Expr::Variable(1), Expr::Variable(0))
    );

    std::cerr << "  True GT: " << Render(*true_gt, ac_vars) << "\n";

    // Verify compound == true GT at full width
    auto gt_eval     = Evaluator::FromExpr(*true_gt, kBw);
    auto compound_fw = FullWidthCheckEval(gt_eval, kNv, *compound, kBw, 256);
    std::cerr << "  MBA-compound == true GT at full width: "
              << (compound_fw.passed ? "PASSED" : "FAILED") << "\n";

    // Check Boolean signature
    auto compound_sig = EvaluateBooleanSignature(*compound, kNv, kBw);
    bool sig_01       = std::all_of(compound_sig.begin(), compound_sig.end(), [](uint64_t v) {
        return v <= 1;
    });
    std::cerr << "  Compound Boolean image ⊆ {0,1}: " << (sig_01 ? "YES" : "NO") << "\n";

    // Try to reconstruct from Boolean signature — should fail FW
    auto bitwise_candidate_sig = EvaluateBooleanSignature(*compound, kNv, kBw);
    Options recon_opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };
    auto recon = Simplify(bitwise_candidate_sig, ac_vars, nullptr, recon_opts);
    if (recon.has_value() && recon.value().kind == SimplifyOutcome::Kind::kSimplified) {
        auto &rr  = recon.value();
        auto cand = CloneExpr(*rr.expr);
        if (!rr.real_vars.empty() && rr.real_vars.size() < ac_vars.size()) {
            std::vector< uint32_t > idx_map;
            for (const auto &rv : rr.real_vars) {
                for (uint32_t j = 0; j < kNv; ++j) {
                    if (ac_vars[j] == rv) {
                        idx_map.push_back(j);
                        break;
                    }
                }
            }
            RemapVarIndices(*cand, idx_map);
        }
        std::cerr << "  Boolean-reconstructed: " << Render(*cand, ac_vars) << "\n";
        auto recon_fw = FullWidthCheckEval(gt_eval, kNv, *cand, kBw, 256);
        std::cerr << "  Reconstruction == GT at full width: "
                  << (recon_fw.passed ? "PASSED" : "FAILED") << "\n";
        if (!recon_fw.passed) {
            std::cerr << "  → Boolean-shadow confirmed: reconstruction is "
                         "Boolean-correct but FW-incorrect\n";
        }
    }

    std::cerr << "\n── Summary ──\n";
    std::cerr << "The MBA identity (x&y)*(x|y)+(x&~y)*(~x&y) = x*y\n"
              << "collapses to x&y on {0,1}, creating a Boolean shadow.\n"
              << "Any expression built from nested MBA-obfuscated multiplications\n"
              << "inherits this shadow: it looks bitwise on Boolean inputs but\n"
              << "diverges at full width wherever multiplication carries.\n\n";
}

// ── L278 Factor-Level Bitwise Surrogate Reconstruction ──
//
// The central experiment: can CoBRA solve L278 by treating each multiplicative
// factor as a bitwise function (based on its Boolean signature), reconstructing
// each factor individually via Simplify(), then reassembling the original
// additive/product skeleton with bitwise surrogates?
//
// This tests the core hypothesis of Section 5.2: semantic bitwise recognition
// at the factor level, not whole-expression reclassification.

TEST(SemanticPolyAudit, L278FactorSurrogateReconstruction) {
    // Read line 278 from QSynth dataset
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::string line;
    int line_num = 0;
    std::string obfuscated;
    std::string gt_str;
    while (std::getline(file, line)) {
        ++line_num;
        if (line_num != 278) { continue; }
        size_t sep = find_separator(line);
        ASSERT_NE(sep, std::string::npos);
        obfuscated = trim(line.substr(0, sep));
        gt_str     = trim(line.substr(sep + 1));
        break;
    }
    ASSERT_FALSE(obfuscated.empty());

    std::cerr << "\n╔══════════════════════════════════════════════════════╗\n";
    std::cerr << "║  L278 Factor-Level Bitwise Surrogate Reconstruction  ║\n";
    std::cerr << "╚══════════════════════════════════════════════════════╝\n\n";
    std::cerr << "GT: " << gt_str << "\n\n";

    // Parse and fold
    auto parse_result = ParseAndEvaluate(obfuscated, kBw);
    ASSERT_TRUE(parse_result.has_value());
    auto ast_result = ParseToAst(obfuscated, kBw);
    ASSERT_TRUE(ast_result.has_value());

    auto folded      = FoldConstantBitwise(std::move(ast_result.value().expr), kBw);
    const auto &sig  = parse_result.value().sig;
    const auto &vars = parse_result.value().vars;
    auto nv          = static_cast< uint32_t >(vars.size());

    auto original_eval = [&folded](const std::vector< uint64_t > &v) -> uint64_t {
        return EvalExpr(*folded, v, kBw);
    };

    std::cerr << "Variables: [";
    for (uint32_t i = 0; i < nv; ++i) {
        if (i > 0) { std::cerr << ", "; }
        std::cerr << vars[i];
    }
    std::cerr << "]  (nv=" << nv << ")\n\n";

    // ── Step 1: Factor inventory ──

    std::vector< const Expr * > additive_terms;
    collect_additive_terms(*folded, additive_terms);

    std::cerr << "── Step 1: Factor Inventory ──\n";
    std::cerr << "Additive terms: " << additive_terms.size() << "\n\n";

    const uint64_t kMask = Bitmask(kBw);

    struct FactorRecord
    {
        const Expr *original = nullptr;
        bool is_constant     = false;
        uint64_t const_val   = 0;
        bool sig_01          = false;
        bool sig_neg1        = false;
        uint32_t support     = 0;
        std::vector< uint64_t > bool_sig;
    };

    struct TermRecord
    {
        std::vector< FactorRecord > factors;
        bool all_factors_semantic_bw = true;
        bool sig_01                  = false;
        bool sig_neg1                = false;
    };

    std::vector< TermRecord > term_records;

    for (size_t ti = 0; ti < additive_terms.size(); ++ti) {
        const Expr *term = additive_terms[ti];
        TermRecord tr;

        // Term-level Boolean signature
        auto term_sig = EvaluateBooleanSignature(*term, nv, kBw);
        tr.sig_01     = std::all_of(term_sig.begin(), term_sig.end(), [&](uint64_t v) {
            return (v & kMask) <= 1;
        });
        tr.sig_neg1   = std::all_of(term_sig.begin(), term_sig.end(), [&](uint64_t v) {
            uint64_t val = v & kMask;
            return val == kMask || val == (kMask - 1);
        });

        // Factor-level analysis
        std::vector< const Expr * > factors;
        collect_mul_factors(*term, factors);

        for (const Expr *f : factors) {
            FactorRecord fr;
            fr.original = f;

            if (IsConstantSubtree(*f)) {
                fr.is_constant = true;
                fr.const_val   = EvalConstantExpr(*f, kBw);
                tr.factors.push_back(fr);
                continue;
            }

            fr.bool_sig = EvaluateBooleanSignature(*f, nv, kBw);
            fr.support  = support_size(*f);

            fr.sig_01   = std::all_of(fr.bool_sig.begin(), fr.bool_sig.end(), [&](uint64_t v) {
                return (v & kMask) <= 1;
            });
            fr.sig_neg1 = std::all_of(fr.bool_sig.begin(), fr.bool_sig.end(), [&](uint64_t v) {
                uint64_t val = v & kMask;
                return val == kMask || val == (kMask - 1);
            });

            if (!fr.sig_01 && !fr.sig_neg1) { tr.all_factors_semantic_bw = false; }

            tr.factors.push_back(fr);
        }

        std::cerr << "  term[" << ti << "]"
                  << " sig01=" << tr.sig_01 << " sig-1=" << tr.sig_neg1
                  << " all_fac_bw=" << tr.all_factors_semantic_bw
                  << " factors=" << tr.factors.size() << "\n";

        for (size_t fi = 0; fi < tr.factors.size(); ++fi) {
            const auto &fr = tr.factors[fi];
            if (fr.is_constant) {
                std::cerr << "    factor[" << fi
                          << "] CONST=" << static_cast< int64_t >(fr.const_val) << "\n";
            } else {
                std::string rendered = Render(*fr.original, vars);
                if (rendered.size() > 100) { rendered = rendered.substr(0, 97) + "..."; }
                std::cerr << "    factor[" << fi << "]"
                          << " sig01=" << fr.sig_01 << " sig-1=" << fr.sig_neg1
                          << " support=" << fr.support << "  " << rendered << "\n";
            }
        }

        term_records.push_back(std::move(tr));
    }

    // ── Step 2: Per-factor bitwise surrogate reconstruction ──

    std::cerr << "\n── Step 2: Per-Factor Bitwise Surrogate Reconstruction ──\n";

    // For each non-constant factor whose Boolean signature is in {0,1} or
    // {-2,-1}, run Simplify() on that factor's Boolean signature to get
    // a bitwise surrogate expression.

    struct SurrogateResult
    {
        bool attempted  = false;
        bool simplified = false;
        bool verified   = false;
        std::unique_ptr< Expr > expr;
        std::vector< std::string > real_vars;
    };

    // Indexed [term_idx][factor_idx]
    std::vector< std::vector< SurrogateResult > > surrogates(term_records.size());

    for (size_t ti = 0; ti < term_records.size(); ++ti) {
        surrogates[ti].resize(term_records[ti].factors.size());

        for (size_t fi = 0; fi < term_records[ti].factors.size(); ++fi) {
            const auto &fr = term_records[ti].factors[fi];
            auto &sr       = surrogates[ti][fi];

            if (fr.is_constant) { continue; }
            if (!fr.sig_01 && !fr.sig_neg1) { continue; }

            sr.attempted = true;

            // For neg-1 based factors, shift to {0,1} range:
            // f' = f + 1, which maps {-2,-1} → {-1,0} ... no.
            // Actually: f maps to {0xFFFE, 0xFFFF}. Add 2 to get {0, 1}.
            // Then reconstruct surrogate, then subtract 2 at the end.
            // For simplicity, handle neg1 by noting ~f has image in {0,1}.
            std::vector< uint64_t > working_sig;
            bool is_neg1 = fr.sig_neg1 && !fr.sig_01;

            if (is_neg1) {
                // ~f maps {-2,-1} → {0, 1} (bitwise NOT)
                working_sig.resize(fr.bool_sig.size());
                for (size_t i = 0; i < fr.bool_sig.size(); ++i) {
                    working_sig[i] = (~fr.bool_sig[i]) & kMask;
                }
            } else {
                working_sig = fr.bool_sig;
            }

            Options factor_opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };

            auto factor_result = Simplify(working_sig, vars, nullptr, factor_opts);
            if (!factor_result.has_value()) {
                std::cerr << "  term[" << ti << "].factor[" << fi
                          << "] Simplify error: " << factor_result.error().message << "\n";
                continue;
            }

            if (factor_result.value().kind == SimplifyOutcome::Kind::kSimplified) {
                sr.simplified = true;
                sr.verified   = factor_result.value().verified;
                sr.real_vars  = factor_result.value().real_vars;

                if (is_neg1) {
                    // Undo the NOT: surrogate = ~(simplified)
                    sr.expr = Expr::BitwiseNot(std::move(factor_result.value().expr));
                } else {
                    sr.expr = std::move(factor_result.value().expr);
                }

                // Remap variables: surrogate may use reduced var set
                if (!sr.real_vars.empty() && sr.real_vars.size() < vars.size()) {
                    std::vector< uint32_t > idx_map;
                    for (const auto &rv : sr.real_vars) {
                        for (uint32_t j = 0; j < nv; ++j) {
                            if (vars[j] == rv) {
                                idx_map.push_back(j);
                                break;
                            }
                        }
                    }
                    RemapVarIndices(*sr.expr, idx_map);
                }

                std::string rendered = Render(*sr.expr, vars);
                std::cerr << "  term[" << ti << "].factor[" << fi << "] → " << rendered
                          << (sr.verified ? " [FW-verified]" : " [bool-only]")
                          << (is_neg1 ? " (via NOT inversion)" : "") << "\n";
            } else {
                std::cerr << "  term[" << ti << "].factor[" << fi
                          << "] unsupported by Simplify()\n";
            }
        }
    }

    // ── Step 3: Reassemble expression from surrogates ──

    std::cerr << "\n── Step 3: Structure-Preserving Reassembly ──\n";

    // Build: Σ (coeff * surrogate_f1 * surrogate_f2 * ...)
    // If any factor wasn't simplified, fall back to original factor.

    std::unique_ptr< Expr > reassembled;
    bool all_surrogates_available = true;

    for (size_t ti = 0; ti < term_records.size(); ++ti) {
        const auto &tr = term_records[ti];

        // Build product of factors for this term
        std::unique_ptr< Expr > term_expr;

        for (size_t fi = 0; fi < tr.factors.size(); ++fi) {
            const auto &fr = tr.factors[fi];
            const auto &sr = surrogates[ti][fi];

            std::unique_ptr< Expr > factor_expr;

            if (fr.is_constant) {
                factor_expr = Expr::Constant(fr.const_val);
            } else if (sr.simplified) {
                factor_expr = CloneExpr(*sr.expr);
            } else {
                // Fall back to original
                factor_expr = CloneExpr(*fr.original);
                if (sr.attempted) { all_surrogates_available = false; }
            }

            if (!term_expr) {
                term_expr = std::move(factor_expr);
            } else {
                term_expr = Expr::Mul(std::move(term_expr), std::move(factor_expr));
            }
        }

        if (!term_expr) { term_expr = Expr::Constant(0); }

        if (!reassembled) {
            reassembled = std::move(term_expr);
        } else {
            reassembled = Expr::Add(std::move(reassembled), std::move(term_expr));
        }
    }

    std::cerr << "All surrogates available: " << (all_surrogates_available ? "YES" : "NO")
              << "\n";

    std::string reassembled_str = Render(*reassembled, vars);
    if (reassembled_str.size() > 200) {
        reassembled_str = reassembled_str.substr(0, 197) + "...";
    }
    std::cerr << "Reassembled: " << reassembled_str << "\n";

    auto reassembled_cost = ComputeCost(*reassembled).cost;
    auto original_cost    = ComputeCost(*folded).cost;
    std::cerr << "Cost: original=" << original_cost.weighted_size
              << " reassembled=" << reassembled_cost.weighted_size << "\n";

    // ── Step 4: Full-width verification ──

    std::cerr << "\n── Step 4: Full-Width Verification ──\n";

    // Check reassembled expression against original at full width
    auto reassembled_eval = Evaluator::FromExpr(*reassembled, kBw);
    auto fw_check = FullWidthCheckEval(Evaluator(original_eval), nv, *reassembled, kBw, 64);

    std::cerr << "FW verification (64 samples): " << (fw_check.passed ? "PASSED" : "FAILED")
              << "\n";

    if (!fw_check.passed && !fw_check.failing_input.empty()) {
        std::cerr << "  Failing input: [";
        for (size_t i = 0; i < fw_check.failing_input.size(); ++i) {
            if (i > 0) { std::cerr << ", "; }
            std::cerr << fw_check.failing_input[i];
        }
        std::cerr << "]\n";
        uint64_t orig_val  = original_eval(fw_check.failing_input);
        uint64_t reasm_val = EvalExpr(*reassembled, fw_check.failing_input, kBw);
        std::cerr << "  original=" << orig_val << " reassembled=" << reasm_val << "\n";
    }

    // Also check with exhaustive Boolean check
    auto bool_check_sig = EvaluateBooleanSignature(*reassembled, nv, kBw);
    bool bool_match     = (bool_check_sig == sig);
    std::cerr << "Boolean signature match: " << (bool_match ? "YES" : "NO") << "\n";

    // ── Step 5: Compare with whole-expression direct reconstruction ──

    std::cerr << "\n── Step 5: Whole-Expression Direct Reconstruction ──\n";

    // Try Simplify() on the whole expression's signature directly
    Options whole_opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };
    whole_opts.evaluator = Evaluator(original_eval);
    auto whole_result    = Simplify(sig, vars, folded.get(), whole_opts);

    if (whole_result.has_value()) {
        if (whole_result.value().kind == SimplifyOutcome::Kind::kSimplified) {
            std::cerr << "Whole-expr Simplify: SOLVED\n";
            std::cerr << "  Result: "
                      << Render(*whole_result.value().expr, whole_result.value().real_vars)
                      << "\n";
            std::cerr << "  Verified: " << whole_result.value().verified << "\n";
        } else {
            std::cerr << "Whole-expr Simplify: UNSUPPORTED\n";
            if (whole_result.value().diag.reason_code.has_value()) {
                auto cat = whole_result.value().diag.reason_code->category;
                std::cerr << "  Reason: cat=" << static_cast< int >(cat) << "\n";
            }
        }
    } else {
        std::cerr << "Whole-expr Simplify: ERROR\n";
    }

    // ── Step 6: Try Simplify on reassembled expression ──

    std::cerr << "\n── Step 6: Simplify Reassembled Expression ──\n";

    // The reassembled expression has bitwise surrogates in place of the
    // original mixed factors. Can CoBRA simplify THIS expression?
    auto reasm_ast_result = ParseToAst(reassembled_str, kBw);
    // Instead of parsing the rendered form (fragile), just use the
    // reassembled Expr directly via its signature.

    auto reasm_sig = EvaluateBooleanSignature(*reassembled, nv, kBw);
    Options reasm_opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };
    reasm_opts.evaluator = Evaluator::FromExpr(*reassembled, kBw);

    auto reasm_result = Simplify(reasm_sig, vars, reassembled.get(), reasm_opts);
    if (reasm_result.has_value()) {
        if (reasm_result.value().kind == SimplifyOutcome::Kind::kSimplified) {
            std::cerr << "Reassembled Simplify: SOLVED\n";

            // Remap vars if needed
            auto &rr        = reasm_result.value();
            auto final_expr = CloneExpr(*rr.expr);
            if (!rr.real_vars.empty() && rr.real_vars.size() < vars.size()) {
                std::vector< uint32_t > idx_map;
                for (const auto &rv : rr.real_vars) {
                    for (uint32_t j = 0; j < nv; ++j) {
                        if (vars[j] == rv) {
                            idx_map.push_back(j);
                            break;
                        }
                    }
                }
                RemapVarIndices(*final_expr, idx_map);
            }

            std::cerr << "  Result: " << Render(*final_expr, vars) << "\n";
            std::cerr << "  Verified: " << rr.verified << "\n";
            auto final_cost = ComputeCost(*final_expr).cost;
            std::cerr << "  Cost: " << final_cost.weighted_size
                      << " (original: " << original_cost.weighted_size << ")\n";

            // Final FW check of the simplified reassembled form
            auto final_fw =
                FullWidthCheckEval(Evaluator(original_eval), nv, *final_expr, kBw, 64);
            std::cerr << "  Final FW check (64 samples): "
                      << (final_fw.passed ? "PASSED" : "FAILED") << "\n";
        } else {
            std::cerr << "Reassembled Simplify: UNSUPPORTED\n";
            if (reasm_result.value().diag.reason_code.has_value()) {
                auto cat = reasm_result.value().diag.reason_code->category;
                std::cerr << "  Reason: cat=" << static_cast< int >(cat) << "\n";
            }
        }
    } else {
        std::cerr << "Reassembled Simplify: ERROR\n";
    }

    std::cerr << "\n── Summary ──\n";
    std::cerr << "GT:          " << gt_str << "\n";
    std::cerr << "Factor-level surrogate reconstruction: "
              << (all_surrogates_available ? "complete" : "partial") << "\n";
    std::cerr << "FW verification of reassembled: " << (fw_check.passed ? "PASSED" : "FAILED")
              << "\n";
    std::cerr << "\n";
}

// ── L278 Whole-Expression Direct Bitwise Reconstruction ──
//
// Binary falsification test: can a bitwise expression reconstructed directly
// from the whole-expression Boolean signature match L278 at full width?
//
// If FW-fails: retire Section 5.2 as an implementation target for this
//              population. Boolean bitwise appearance is diagnostic only.
// If FW-verifies: the additive decomposition was the problem, not the
//                 semantic bitwise hypothesis.

TEST(SemanticPolyAudit, L278WholeExprBitwiseReconstruction) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::string line;
    int line_num = 0;
    std::string obfuscated;
    std::string gt_str;
    while (std::getline(file, line)) {
        ++line_num;
        if (line_num != 278) { continue; }
        size_t sep = find_separator(line);
        ASSERT_NE(sep, std::string::npos);
        obfuscated = trim(line.substr(0, sep));
        gt_str     = trim(line.substr(sep + 1));
        break;
    }
    ASSERT_FALSE(obfuscated.empty());

    std::cerr << "\n╔══════════════════════════════════════════════════════╗\n";
    std::cerr << "║  L278 Whole-Expression Direct Bitwise Reconstruction  ║\n";
    std::cerr << "╚══════════════════════════════════════════════════════╝\n\n";
    std::cerr << "GT: " << gt_str << "\n\n";

    auto parse_result = ParseAndEvaluate(obfuscated, kBw);
    ASSERT_TRUE(parse_result.has_value());
    auto ast_result = ParseToAst(obfuscated, kBw);
    ASSERT_TRUE(ast_result.has_value());

    auto folded      = FoldConstantBitwise(std::move(ast_result.value().expr), kBw);
    const auto &sig  = parse_result.value().sig;
    const auto &vars = parse_result.value().vars;
    auto nv          = static_cast< uint32_t >(vars.size());

    auto original_eval = [&folded](const std::vector< uint64_t > &v) -> uint64_t {
        return EvalExpr(*folded, v, kBw);
    };

    // ── Approach 1: Treat the Boolean signature as a bitwise truth table ──
    //
    // The sig has 2^nv entries, each the expression's value on a Boolean
    // input. If the expression is truly bitwise, then sig fully determines
    // it (bitwise functions are determined by their Boolean behavior).
    // Feed this sig into Simplify() with NO input AST (signature-only mode)
    // to force pure signature-based reconstruction.

    std::cerr << "── Approach 1: Signature-only reconstruction ──\n";

    Options sig_opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };
    // No evaluator — force Boolean-signature-only path
    auto sig_result = Simplify(sig, vars, nullptr, sig_opts);

    if (sig_result.has_value() && sig_result.value().kind == SimplifyOutcome::Kind::kSimplified)
    {
        auto &sr = sig_result.value();

        // Remap variables to original indices
        auto candidate = CloneExpr(*sr.expr);
        if (!sr.real_vars.empty() && sr.real_vars.size() < vars.size()) {
            std::vector< uint32_t > idx_map;
            for (const auto &rv : sr.real_vars) {
                for (uint32_t j = 0; j < nv; ++j) {
                    if (vars[j] == rv) {
                        idx_map.push_back(j);
                        break;
                    }
                }
            }
            RemapVarIndices(*candidate, idx_map);
        }

        std::string rendered = Render(*candidate, vars);
        std::cerr << "  Simplified: " << rendered << "\n";
        std::cerr << "  Cost: " << ComputeCost(*candidate).cost.weighted_size
                  << " (original: " << ComputeCost(*folded).cost.weighted_size << ")\n";
        std::cerr << "  Boolean-verified: " << sr.verified << "\n";

        // THE critical check: does this FW-verify against original?
        auto fw = FullWidthCheckEval(Evaluator(original_eval), nv, *candidate, kBw, 256);
        std::cerr << "  FW verification (256 samples): " << (fw.passed ? "PASSED" : "FAILED")
                  << "\n";

        if (!fw.passed && !fw.failing_input.empty()) {
            std::cerr << "  Failing input: [";
            for (size_t i = 0; i < fw.failing_input.size(); ++i) {
                if (i > 0) { std::cerr << ", "; }
                std::cerr << fw.failing_input[i];
            }
            std::cerr << "]\n";
            uint64_t orig_val = original_eval(fw.failing_input);
            uint64_t cand_val = EvalExpr(*candidate, fw.failing_input, kBw);
            std::cerr << "  original=" << orig_val << " candidate=" << cand_val << "\n";
        }
    } else {
        std::cerr << "  Simplify: UNSUPPORTED or ERROR\n";
    }

    // ── Approach 2: Evaluate GT and compare ──
    //
    // Parse the ground truth expression and verify it matches the original
    // at full width. This confirms the GT is correct, and tells us what
    // the "right answer" looks like structurally.

    std::cerr << "\n── Approach 2: Ground truth verification ──\n";

    auto gt_ast = ParseToAst(gt_str, kBw);
    if (gt_ast.has_value()) {
        auto gt_folded = FoldConstantBitwise(std::move(gt_ast.value().expr), kBw);
        auto gt_cost   = ComputeCost(*gt_folded).cost;
        std::cerr << "  GT rendered: " << Render(*gt_folded, gt_ast.value().vars) << "\n";
        std::cerr << "  GT cost: " << gt_cost.weighted_size << "\n";

        // Verify GT matches original at full width
        // GT may use different variable names — need to align
        const auto &gt_vars = gt_ast.value().vars;
        std::cerr << "  GT vars: [";
        for (size_t i = 0; i < gt_vars.size(); ++i) {
            if (i > 0) { std::cerr << ", "; }
            std::cerr << gt_vars[i];
        }
        std::cerr << "]  original vars: [";
        for (size_t i = 0; i < vars.size(); ++i) {
            if (i > 0) { std::cerr << ", "; }
            std::cerr << vars[i];
        }
        std::cerr << "]\n";

        // Build var mapping from GT var space to original var space
        std::vector< uint32_t > gt_to_orig;
        bool mapping_ok = true;
        for (const auto &gv : gt_vars) {
            bool found = false;
            for (uint32_t j = 0; j < nv; ++j) {
                if (vars[j] == gv) {
                    gt_to_orig.push_back(j);
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << "  WARNING: GT var '" << gv << "' not found in original vars\n";
                mapping_ok = false;
            }
        }

        if (mapping_ok) {
            auto gt_clone = CloneExpr(*gt_folded);
            RemapVarIndices(*gt_clone, gt_to_orig);

            auto gt_fw = FullWidthCheckEval(Evaluator(original_eval), nv, *gt_clone, kBw, 256);
            std::cerr << "  GT FW verification (256 samples): "
                      << (gt_fw.passed ? "PASSED" : "FAILED") << "\n";

            if (!gt_fw.passed && !gt_fw.failing_input.empty()) {
                uint64_t orig_val = original_eval(gt_fw.failing_input);
                uint64_t gt_val   = EvalExpr(*gt_clone, gt_fw.failing_input, kBw);
                std::cerr << "  original=" << orig_val << " gt=" << gt_val << "\n";
            }

            // Also check: does the GT have the same Boolean signature?
            auto gt_sig    = EvaluateBooleanSignature(*gt_clone, nv, kBw);
            bool sig_match = (gt_sig == sig);
            std::cerr << "  GT Boolean signature match: " << (sig_match ? "YES" : "NO") << "\n";
        }
    } else {
        std::cerr << "  GT parse failed\n";
    }

    // ── Approach 3: Build bitwise expr directly from truth table ──
    //
    // For 2-variable expressions with Boolean image in {0,1}, there are
    // only 16 possible bitwise functions. We can identify which one by
    // looking at the 4-entry truth table.

    std::cerr << "\n── Approach 3: Direct truth-table identification ──\n";

    if (nv <= 4) {
        std::cerr << "  Boolean truth table (nv=" << nv << ", " << sig.size()
                  << " entries):\n    ";
        for (size_t i = 0; i < sig.size(); ++i) { std::cerr << (sig[i] & 1); }
        std::cerr << "\n";

        // For 2 vars: inputs are (0,0), (1,0), (0,1), (1,1)
        // sig[0]=f(0,0), sig[1]=f(1,0), sig[2]=f(0,1), sig[3]=f(1,1)
        if (nv == 2 && sig.size() == 4) {
            // Identify the Boolean function
            uint8_t tt = 0;
            for (size_t i = 0; i < 4; ++i) {
                if (sig[i] & 1) { tt |= (1 << i); }
            }
            std::cerr << "  Truth table index: 0x" << std::hex << static_cast< int >(tt)
                      << std::dec << "\n";

            // Known 2-var bitwise functions
            const char *names[] = { "0",        "a & c",    "a & ~c",   "a",
                                    "~a & c",   "c",        "a ^ c",    "a | c",
                                    "~(a | c)", "~(a ^ c)", "~c",       "a | ~c",
                                    "~a",       "~a | c",   "~(a & c)", "1" };
            std::cerr << "  Identified as: " << names[tt] << "\n";

            // Build the identified bitwise expression and FW-verify
            // This is the maximally direct test: we know the Boolean
            // function exactly, so build it and check full width.
            std::unique_ptr< Expr > direct_bw;
            switch (tt) {
                case 0x0:
                    direct_bw = Expr::Constant(0);
                    break;
                case 0x1:
                    direct_bw = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
                    break;
                case 0x6:
                    direct_bw = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
                    break;
                case 0x7:
                    direct_bw = Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1));
                    break;
                case 0x8:
                    direct_bw =
                        Expr::BitwiseNot(Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1)));
                    break;
                case 0xE:
                    direct_bw = Expr::BitwiseOr(
                        Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1)),
                        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1))
                    );
                    break;
                case 0xF:
                    direct_bw = Expr::Constant(Bitmask(kBw));
                    break;
                default:
                    // For any 2-var truth table, build from ANF:
                    // f = c0 ^ (c1 & a) ^ (c2 & c) ^ (c3 & a & c)
                    // where c_i are the ANF coefficients
                    {
                        uint8_t c0 = tt & 1;
                        uint8_t c1 = ((tt >> 1) ^ tt) & 1;
                        uint8_t c2 = ((tt >> 2) ^ tt) & 1;
                        uint8_t c3 = ((tt >> 3) ^ (tt >> 2) ^ (tt >> 1) ^ tt) & 1;
                        // Build expression from ANF
                        std::unique_ptr< Expr > result;
                        auto xor_in = [&](std::unique_ptr< Expr > term) {
                            if (!result) {
                                result = std::move(term);
                            } else {
                                result = Expr::BitwiseXor(std::move(result), std::move(term));
                            }
                        };
                        if (c0) { xor_in(Expr::Constant(Bitmask(kBw))); }
                        if (c1) { xor_in(Expr::Variable(0)); }
                        if (c2) { xor_in(Expr::Variable(1)); }
                        if (c3) {
                            xor_in(Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)));
                        }
                        if (!result) { result = Expr::Constant(0); }
                        direct_bw = std::move(result);
                    }
                    break;
            }

            if (direct_bw) {
                std::cerr << "  Direct bitwise expr: " << Render(*direct_bw, vars) << "\n";

                auto direct_fw =
                    FullWidthCheckEval(Evaluator(original_eval), nv, *direct_bw, kBw, 256);
                std::cerr << "  Direct bitwise FW verification (256 samples): "
                          << (direct_fw.passed ? "PASSED ★" : "FAILED") << "\n";

                if (!direct_fw.passed && !direct_fw.failing_input.empty()) {
                    uint64_t orig_val = original_eval(direct_fw.failing_input);
                    uint64_t bw_val   = EvalExpr(*direct_bw, direct_fw.failing_input, kBw);
                    std::cerr << "  original=" << orig_val << " direct_bw=" << bw_val
                              << " at input=[";
                    for (size_t i = 0; i < direct_fw.failing_input.size(); ++i) {
                        if (i > 0) { std::cerr << ", "; }
                        std::cerr << direct_fw.failing_input[i];
                    }
                    std::cerr << "]\n";
                }
            }
        }
    }

    std::cerr << "\n── Conclusion ──\n";
    std::cerr << "If all approaches FW-fail: Boolean bitwise appearance is\n"
              << "diagnostic only, not reconstructive, for this expression class.\n"
              << "Section 5.2 is not a sound reconstruction mechanism here.\n\n";
}

TEST(SemanticPolyAudit, QSynthUnsupportedPopulation) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::string line;
    int line_num = 0;

    std::vector< std::pair< int, ShapeAnalysis > > results;

    // Aggregate counters
    int total_unsupported    = 0;
    int has_mul_count        = 0;
    int has_shr_count        = 0;
    int narrow_shape_count   = 0;
    int pct_admissible_count = 0;
    int sig_bitwise_count    = 0;

    std::map< uint32_t, int > by_real_vars;
    std::map< uint32_t, int > by_max_degree;
    std::map< uint32_t, int > by_max_factor_sup;
    std::map< uint32_t, int > by_additive_terms;
    std::map< std::string, int > by_reason;
    std::map< std::string, int > by_flags;

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

        Options opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, kBw);
        };

        auto result = Simplify(sig, vars, folded_ptr->get(), opts);
        if (!result.has_value()) { continue; }
        if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) { continue; }

        total_unsupported++;

        // ── Classification ──
        auto cls = ClassifyStructural(**folded_ptr);

        // ── Aux var elimination for real var count ──
        auto elim      = EliminateAuxVars(sig, vars);
        uint32_t nreal = static_cast< uint32_t >(elim.real_vars.size());

        // ── Shape analysis ──
        ShapeAnalysis sa;
        sa.real_var_count = nreal;
        sa.semantic_class = cls.semantic;
        sa.flags          = cls.flags;
        sa.has_shr        = contains_shr(**folded_ptr);
        sa.has_mul        = contains_mul(**folded_ptr);

        // Reason category
        if (result.value().diag.reason_code.has_value()) {
            auto cat = result.value().diag.reason_code->category;
            switch (cat) {
                case ReasonCategory::kVerifyFailed:
                    sa.reason_category = "verify-failed";
                    break;
                case ReasonCategory::kGuardFailed:
                    sa.reason_category = "guard-failed";
                    break;
                case ReasonCategory::kSearchExhausted:
                    sa.reason_category = "search-exhausted";
                    break;
                case ReasonCategory::kRepresentationGap:
                    sa.reason_category = "repr-gap";
                    break;
                case ReasonCategory::kNoSolution:
                    sa.reason_category = "no-solution";
                    break;
                case ReasonCategory::kResourceLimit:
                    sa.reason_category = "resource-limit";
                    break;
                case ReasonCategory::kInapplicable:
                    sa.reason_category = "inapplicable";
                    break;
                default:
                    sa.reason_category = "other";
                    break;
            }
        } else {
            sa.reason_category = "no-code";
        }

        // ── Additive decomposition ──
        std::vector< const Expr * > additive_terms;
        collect_additive_terms(**folded_ptr, additive_terms);
        sa.additive_term_count = static_cast< uint32_t >(additive_terms.size());

        // ── Per-term analysis ──
        for (const Expr *term : additive_terms) {
            TermInfo ti = analyze_term(*term);

            sa.max_mul_degree     = std::max(sa.max_mul_degree, ti.factor_count);
            sa.max_factor_support = std::max(sa.max_factor_support, ti.max_factor_support);

            if (ti.factor_count >= 2) { sa.mul_term_count++; }
            if (ti.has_shr_factor) { sa.has_shr = true; }

            sa.bitwise_factor_count     += (ti.all_factors_bitwise ? ti.factor_count : 0);
            sa.non_bitwise_factor_count += (ti.all_factors_bitwise ? 0 : ti.factor_count);

            // Narrow shape: each term is coeff * product-of-non-const-factors
            // where non-constant factors exist and are all purely bitwise.
            bool term_narrow =
                (ti.factor_count >= 1) && ti.all_factors_bitwise && !ti.has_shr_factor;
            if (term_narrow) {
                sa.narrow_shape_terms++;
            } else if (!ti.is_bare_factor || !IsConstantSubtree(*term)) {
                // A non-matching term that isn't just a bare constant
                // disqualifies the whole expression from narrow shape.
                sa.matches_narrow_shape = false;
            }
        }

        // A bare constant-only additive term is fine (it's the constant offset).
        // But we need at least one non-trivial narrow term.
        if (sa.narrow_shape_terms == 0) { sa.matches_narrow_shape = false; }

        // ── PCT admission ──
        sa.pct_admissible = sa.matches_narrow_shape && sa.real_var_count <= 6
            && sa.max_mul_degree <= 3 && sa.max_factor_support <= 6
            && (sa.max_mul_degree * sa.real_var_count) <= 9 && !sa.has_shr;

        // ── Semantic bitwise check on whole expression ──
        // Check if Boolean signature image lands in {0,1} or {-2,-1}.
        if (nreal <= 12) {
            const uint64_t mask = Bitmask(kBw);
            const uint64_t neg1 = mask;     // 0xFFFFFFFFFFFFFFFF
            const uint64_t neg2 = mask - 1; // 0xFFFFFFFFFFFFFFFE

            bool all_01   = true;
            bool all_neg1 = true;
            for (uint64_t val : sig) {
                uint64_t v = val & mask;
                if (v != 0 && v != 1) { all_01 = false; }
                if (v != neg1 && v != neg2) { all_neg1 = false; }
                if (!all_01 && !all_neg1) { break; }
            }
            sa.sig_image_01   = all_01;
            sa.sig_image_neg1 = all_neg1;
        }

        // ── Aggregate ──
        if (sa.has_mul) { has_mul_count++; }
        if (sa.has_shr) { has_shr_count++; }
        if (sa.matches_narrow_shape) { narrow_shape_count++; }
        if (sa.pct_admissible) { pct_admissible_count++; }
        if (sa.sig_image_01 || sa.sig_image_neg1) { sig_bitwise_count++; }

        by_real_vars[sa.real_var_count]++;
        by_max_degree[sa.max_mul_degree]++;
        by_max_factor_sup[sa.max_factor_support]++;
        by_additive_terms[sa.additive_term_count]++;
        by_reason[sa.reason_category]++;
        by_flags[flags_str(sa.flags)]++;

        results.emplace_back(line_num, sa);
    }

    // ── Print summary ──

    std::cerr << "\n╔══════════════════════════════════════════════╗\n";
    std::cerr << "║  SEMANTIC POLY MBA AUDIT — QSynth Unsupported ║\n";
    std::cerr << "╚══════════════════════════════════════════════╝\n\n";

    std::cerr << "Total unsupported: " << total_unsupported << "\n\n";

    std::cerr << "── Key population counts ──\n";
    std::cerr << "  Has Mul:             " << has_mul_count << "\n";
    std::cerr << "  Has LShr:            " << has_shr_count << "\n";
    std::cerr << "  Narrow shape match:  " << narrow_shape_count << "\n";
    std::cerr << "  PCT admissible:      " << pct_admissible_count << "\n";
    std::cerr << "  Sig bitwise (0/1 or -2/-1): " << sig_bitwise_count << "\n";
    std::cerr << "\n";

    std::cerr << "── By real variable count ──\n";
    for (auto &[k, v] : by_real_vars) { std::cerr << "  " << k << " vars: " << v << "\n"; }

    std::cerr << "\n── By max multiplicative degree ──\n";
    for (auto &[k, v] : by_max_degree) { std::cerr << "  degree " << k << ": " << v << "\n"; }

    std::cerr << "\n── By max per-factor support ──\n";
    for (auto &[k, v] : by_max_factor_sup) {
        std::cerr << "  support " << k << ": " << v << "\n";
    }

    std::cerr << "\n── By additive term count ──\n";
    for (auto &[k, v] : by_additive_terms) {
        std::cerr << "  " << k << " terms: " << v << "\n";
    }

    std::cerr << "\n── By failure reason ──\n";
    for (auto &[k, v] : by_reason) { std::cerr << "  " << k << ": " << v << "\n"; }

    std::cerr << "\n── By structural flags ──\n";
    for (auto &[k, v] : by_flags) { std::cerr << "  {" << k << "}: " << v << "\n"; }

    // ── Per-expression detail ──

    std::cerr << "\n── Per-expression detail ──\n";
    for (auto &[ln, sa] : results) {
        std::cerr << "  L" << ln << " vars=" << sa.real_var_count
                  << " deg=" << sa.max_mul_degree << " terms=" << sa.additive_term_count
                  << " mul_terms=" << sa.mul_term_count << " max_fsup=" << sa.max_factor_support
                  << " bw_factors=" << sa.bitwise_factor_count
                  << " non_bw=" << sa.non_bitwise_factor_count
                  << " shr=" << (sa.has_shr ? "Y" : "N")
                  << " narrow=" << (sa.matches_narrow_shape ? "Y" : "N")
                  << " pct=" << (sa.pct_admissible ? "Y" : "N")
                  << " sig01=" << (sa.sig_image_01 ? "Y" : "N")
                  << " sig-1=" << (sa.sig_image_neg1 ? "Y" : "N")
                  << " reason=" << sa.reason_category
                  << " class=" << semantic_str(sa.semantic_class) << " flags={"
                  << flags_str(sa.flags) << "}"
                  << "\n";
    }

    std::cerr << "\n── Decision gate summary ──\n";
    std::cerr << "  Narrow shape match >= 8?  " << (narrow_shape_count >= 8 ? "YES" : "NO")
              << " (" << narrow_shape_count << ")\n";
    std::cerr << "  PCT admissible >= 5?      " << (pct_admissible_count >= 5 ? "YES" : "NO")
              << " (" << pct_admissible_count << ")\n";
    std::cerr << "  Sig bitwise >= 3?         " << (sig_bitwise_count >= 3 ? "YES" : "NO")
              << " (" << sig_bitwise_count << ")\n";
    std::cerr << "\n";
}
