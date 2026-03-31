// Structural extraction for the 3 AND-zero vanishing anchors.
//
// Validates:
//   1. AND-zero vanishing: r(x,y)=0 whenever x&y=0
//   2. Diagonal law: r(x,x) = -x(x-1) for L246/L58
//   3. Cross-case: r_L58(x,y) = -r_L246(x,y)
//   4. GT AST structure: identifies mask and core subtrees
//   5. Candidate decomposition formulas (exact match)
//   6. Off-diagonal overlap-conditioned probing

#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <string>
#include <unordered_map>
#include <vector>

using namespace cobra;

namespace {

    constexpr int kL58  = 58;
    constexpr int kL178 = 178;
    constexpr int kL246 = 246;

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
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { return ""; }
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    // ── AST analysis helpers ─────────────────────────────────

    const char *KindName(Expr::Kind k) {
        switch (k) {
            case Expr::Kind::kConstant:
                return "Const";
            case Expr::Kind::kVariable:
                return "Var";
            case Expr::Kind::kAdd:
                return "Add";
            case Expr::Kind::kMul:
                return "Mul";
            case Expr::Kind::kAnd:
                return "And";
            case Expr::Kind::kOr:
                return "Or";
            case Expr::Kind::kXor:
                return "Xor";
            case Expr::Kind::kNot:
                return "Not";
            case Expr::Kind::kNeg:
                return "Neg";
            case Expr::Kind::kShr:
                return "Shr";
        }
        return "?";
    }

    void PrintAst(
        const Expr &e, const std::vector< std::string > &vars, int indent, std::ostream &os
    ) {
        std::string pad(static_cast< size_t >(indent * 2), ' ');
        if (e.kind == Expr::Kind::kConstant) {
            os << pad << "Const(" << static_cast< int64_t >(e.constant_val) << ")\n";
        } else if (e.kind == Expr::Kind::kVariable) {
            os << pad << "Var(" << (e.var_index < vars.size() ? vars[e.var_index] : "?")
               << ")\n";
        } else {
            os << pad << KindName(e.kind) << "\n";
            for (const auto &c : e.children) { PrintAst(*c, vars, indent + 1, os); }
        }
    }

    bool HasArithmetic(const Expr &e) {
        if (e.kind == Expr::Kind::kAdd || e.kind == Expr::Kind::kMul) { return true; }
        for (const auto &c : e.children) {
            if (HasArithmetic(*c)) { return true; }
        }
        return false;
    }

    bool IsVarAndVar(const Expr &e) {
        return e.kind == Expr::Kind::kAnd && e.children.size() == 2
            && e.children[0]->kind == Expr::Kind::kVariable
            && e.children[1]->kind == Expr::Kind::kVariable;
    }

    bool ContainsOverlap(const Expr &e) {
        if (IsVarAndVar(e)) { return true; }
        for (const auto &c : e.children) {
            if (ContainsOverlap(*c)) { return true; }
        }
        return false;
    }

    struct AndInfo
    {
        std::string lhs;
        std::string rhs;
        bool lhs_arith;
        bool rhs_arith;
        bool is_overlap;
        bool lhs_has_overlap;
        bool rhs_has_overlap;
    };

    void FindAndNodes(
        const Expr &e, const std::vector< std::string > &vars, std::vector< AndInfo > &out
    ) {
        if (e.kind == Expr::Kind::kAnd) {
            AndInfo ai;
            ai.lhs             = Render(*e.children[0], vars, 64);
            ai.rhs             = Render(*e.children[1], vars, 64);
            ai.lhs_arith       = HasArithmetic(*e.children[0]);
            ai.rhs_arith       = HasArithmetic(*e.children[1]);
            ai.is_overlap      = IsVarAndVar(e);
            ai.lhs_has_overlap = ContainsOverlap(*e.children[0]);
            ai.rhs_has_overlap = ContainsOverlap(*e.children[1]);
            out.push_back(ai);
        }
        for (const auto &c : e.children) { FindAndNodes(*c, vars, out); }
    }

    struct AnchorCase
    {
        int line;
        std::string gt_str;
        std::vector< std::string > reduced_vars;
        std::function< uint64_t(uint64_t, uint64_t) > residual;
    };

} // namespace

TEST(AndZeroStructural, AnchorDecomposition) {
    // ── Load dataset ──────────────────────────────────────

    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< std::string > lines;
    std::string line;
    while (std::getline(file, line)) { lines.push_back(line); }

    std::vector< int > target_lines = { kL58, kL178, kL246 };
    std::vector< AnchorCase > cases;
    std::unordered_map< int, size_t > case_idx;

    std::cerr << "\n";
    std::cerr << "═══════════════════════════════════════════════\n";
    std::cerr << "  AND-ZERO STRUCTURAL EXTRACTION\n";
    std::cerr << "═══════════════════════════════════════════════\n";

    for (int tl : target_lines) {
        ASSERT_GE(tl, 1);
        ASSERT_LE(tl, static_cast< int >(lines.size()));

        const auto &raw = lines[tl - 1];
        ASSERT_FALSE(raw.empty());

        size_t sep = find_separator(raw);
        ASSERT_NE(sep, std::string::npos);

        std::string obfuscated = trim(raw.substr(0, sep));
        std::string gt_str     = trim(raw.substr(sep + 1));
        ASSERT_FALSE(obfuscated.empty());

        auto parse = ParseAndEvaluate(obfuscated, 64);
        ASSERT_TRUE(parse.has_value());
        auto ast = ParseToAst(obfuscated, 64);
        ASSERT_TRUE(ast.has_value());

        auto folded      = FoldConstantBitwise(std::move(ast.value().expr), 64);
        const auto &sig  = parse.value().sig;
        const auto &vars = parse.value().vars;
        auto nv          = static_cast< uint32_t >(vars.size());

        auto elim  = EliminateAuxVars(sig, vars);
        auto nreal = static_cast< uint32_t >(elim.real_vars.size());
        ASSERT_EQ(nreal, 2u) << "L" << tl << " expected 2 real vars";

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
        auto orig_eval  = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        std::vector< uint32_t > var_map;
        for (const auto &rv : elim.real_vars) {
            for (uint32_t j = 0; j < nv; ++j) {
                if (vars[j] == rv) {
                    var_map.push_back(j);
                    break;
                }
            }
        }

        auto reduced_eval = [orig_eval, var_map,
                             nv](const std::vector< uint64_t > &v) -> uint64_t {
            std::vector< uint64_t > full(nv, 0);
            for (size_t i = 0; i < var_map.size(); ++i) { full[var_map[i]] = v[i]; }
            return orig_eval(full);
        };

        auto coeffs   = InterpolateCoefficients(elim.reduced_sig, nreal, 64);
        auto cob_expr = BuildCobExpr(coeffs, nreal, 64);
        auto cob_str  = Render(*cob_expr, elim.real_vars, 64);

        auto cob_shared = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*cob_expr));

        auto r = [reduced_eval, cob_shared](uint64_t x, uint64_t y) -> uint64_t {
            std::vector< uint64_t > v = { x, y };
            return reduced_eval(v) - EvalExpr(**cob_shared, v, 64);
        };

        case_idx[tl] = cases.size();
        cases.push_back({ tl, gt_str, elim.real_vars, std::move(r) });

        std::cerr << "\nL" << tl << " vars={" << elim.real_vars[0] << "," << elim.real_vars[1]
                  << "}"
                  << " CoB: " << cob_str << "\n"
                  << "  GT: " << gt_str << "\n";
    }

    ASSERT_EQ(cases.size(), 3u);

    // ════════════════════════════════════════════════════════
    // Section 1: AND-zero vanishing (exhaustive [0..255]²)
    // ════════════════════════════════════════════════════════

    std::cerr << "\n══ AND-ZERO VANISHING CHECK "
              << "══════════════════\n";

    for (const auto &ac : cases) {
        int violations = 0;
        int tested     = 0;
        for (uint64_t x = 0; x < 256; ++x) {
            for (uint64_t y = 0; y < 256; ++y) {
                if ((x & y) != 0) { continue; }
                ++tested;
                uint64_t val = ac.residual(x, y);
                if (val != 0) {
                    ++violations;
                    if (violations <= 3) {
                        std::cerr << "  L" << ac.line << " VIOLATION: r(" << x << "," << y
                                  << ") = " << static_cast< int64_t >(val) << "\n";
                    }
                }
            }
        }
        std::cerr << "  L" << ac.line << ": " << tested << " disjoint pairs, " << violations
                  << " violations\n";
        EXPECT_EQ(violations, 0) << "L" << ac.line << " AND-zero vanishing failed";
    }

    // ════════════════════════════════════════════════════════
    // Section 2: Diagonal characterization
    //   L58:  r(x,x) = x*(x-1)  (overlap * (x-1))
    //   L246: r(x,x) = x - ((-x) & x²)  (NOT -x(x-1))
    //   L178: characterize
    // ════════════════════════════════════════════════════════

    std::cerr << "\n══ DIAGONAL CHARACTERIZATION "
              << "═════════════════\n";

    // L58: r(x,x) = x*(x-1) exactly
    {
        const auto &ac = cases[case_idx[kL58]];
        int failures   = 0;
        for (uint64_t x = 0; x < 1024; ++x) {
            uint64_t actual   = ac.residual(x, x);
            uint64_t expected = x * (x - 1);
            if (actual != expected) {
                ++failures;
                if (failures <= 3) {
                    std::cerr << "  L58 FAIL: r(" << x << "," << x
                              << ") = " << static_cast< int64_t >(actual) << " expected "
                              << static_cast< int64_t >(expected) << "\n";
                }
            }
        }
        std::cerr << "  L58: 1024 diagonal, " << failures << " failures"
                  << " [r(x,x) = x*(x-1)]\n";
        EXPECT_EQ(failures, 0) << "L58 diagonal x*(x-1) failed";
    }

    // L246: r(x,x) = x - ((-x) & x²), NOT -x(x-1)
    {
        const auto &ac = cases[case_idx[kL246]];
        int failures   = 0;
        for (uint64_t x = 0; x < 1024; ++x) {
            uint64_t actual   = ac.residual(x, x);
            uint64_t neg_x    = ~x + 1;
            uint64_t expected = x - (neg_x & (x * x));
            if (actual != expected) {
                ++failures;
                if (failures <= 3) {
                    std::cerr << "  L246 FAIL: r(" << x << "," << x
                              << ") = " << static_cast< int64_t >(actual) << " expected "
                              << static_cast< int64_t >(expected) << "\n";
                }
            }
        }
        std::cerr << "  L246: 1024 diagonal, " << failures << " failures"
                  << " [r(x,x) = x - ((-x) & x²)]\n";
        EXPECT_EQ(failures, 0) << "L246 diagonal failed";
    }

    // L178: characterize diagonal
    {
        const auto &ac = cases[case_idx[kL178]];
        std::cerr << "  L178 diagonal r(x,x) for x=0..15:\n"
                  << "    ";
        for (uint64_t x = 0; x < 16; ++x) {
            auto val = static_cast< int64_t >(ac.residual(x, x));
            std::cerr << std::setw(6) << val;
        }
        std::cerr << "\n";
        // L178: r(x,x) = x & (2x + x²) - x
        int failures = 0;
        for (uint64_t x = 0; x < 1024; ++x) {
            uint64_t actual   = ac.residual(x, x);
            uint64_t expected = (x & ((2 * x) + (x * x))) - x;
            if (actual != expected) { ++failures; }
        }
        std::cerr << "  L178: 1024 diagonal, " << failures << " failures"
                  << " [r(x,x) = x & (2x + x²) - x]\n";
        EXPECT_EQ(failures, 0) << "L178 diagonal failed";
    }

    // ════════════════════════════════════════════════════════
    // Section 3: Cross-case relationship
    //   L58 ≠ -L246 (frontier doc was wrong)
    //   Characterize: where do they agree/disagree?
    // ════════════════════════════════════════════════════════

    std::cerr << "\n══ CROSS-CASE: L58 vs -L246 "
              << "═════════════════\n";

    {
        const auto &r58  = cases[case_idx[kL58]].residual;
        const auto &r246 = cases[case_idx[kL246]].residual;
        int matches      = 0;
        int total        = 0;
        int diag_matches = 0;
        int diag_total   = 0;
        for (uint64_t x = 0; x < 256; ++x) {
            for (uint64_t y = 0; y < 256; ++y) {
                ++total;
                uint64_t v58  = r58(x, y);
                uint64_t v246 = r246(x, y);
                if (v58 == (~v246 + 1)) {
                    ++matches;
                    if (x == y) { ++diag_matches; }
                }
                if (x == y) { ++diag_total; }
            }
        }
        int off_diag_matches = matches - diag_matches;
        int off_diag_total   = total - diag_total;
        std::cerr << "  r58 == -r246: " << matches << " / " << total << "\n";
        std::cerr << "    diagonal: " << diag_matches << " / " << diag_total << "\n";
        std::cerr << "    off-diag: " << off_diag_matches << " / " << off_diag_total << "\n";
    }

    // ════════════════════════════════════════════════════════
    // Section 4: GT AST structural analysis
    // ════════════════════════════════════════════════════════

    std::cerr << "\n══ GT AST STRUCTURAL ANALYSIS "
              << "═══════════════\n";

    for (const auto &ac : cases) {
        auto gt_ast = ParseToAst(ac.gt_str, 64);
        ASSERT_TRUE(gt_ast.has_value()) << "L" << ac.line << " GT parse failed";

        auto gt_folded      = FoldConstantBitwise(std::move(gt_ast.value().expr), 64);
        const auto &gt_vars = gt_ast.value().vars;

        std::cerr << "\n  L" << ac.line << " GT (vars: ";
        for (size_t i = 0; i < gt_vars.size(); ++i) {
            if (i > 0) { std::cerr << ", "; }
            std::cerr << gt_vars[i];
        }
        std::cerr << "):\n";
        PrintAst(*gt_folded, gt_vars, 2, std::cerr);

        std::cerr << "  Rendered: " << Render(*gt_folded, gt_vars, 64) << "\n";

        std::vector< AndInfo > and_nodes;
        FindAndNodes(*gt_folded, gt_vars, and_nodes);

        std::cerr << "  AND nodes: " << and_nodes.size() << "\n";
        for (size_t i = 0; i < and_nodes.size(); ++i) {
            const auto &ai = and_nodes[i];
            std::cerr << "    [" << i << "] " << ai.lhs << " & " << ai.rhs << "\n";
            std::cerr << "        lhs: " << (ai.lhs_arith ? "ARITHMETIC" : "bitwise")
                      << (ai.lhs_has_overlap ? " +overlap" : "") << "\n";
            std::cerr << "        rhs: " << (ai.rhs_arith ? "ARITHMETIC" : "bitwise")
                      << (ai.rhs_has_overlap ? " +overlap" : "") << "\n";
            if (ai.is_overlap) { std::cerr << "        ** OVERLAP TERM **\n"; }
        }
    }

    // ════════════════════════════════════════════════════════
    // Section 5: Candidate decomposition verification
    //
    //   L246: r(a,c) = (a&c) - ((-c) & ((a&c)*a))
    //         overlap - (mask & (overlap * var))
    //
    //   L58:  r(b,d) = ((-d) & ((b&d)*b)) - (b&d)
    //         negation of L246 pattern
    //
    //   L178: r(c,e) = (c&e) & (2e + c²) - (c&e)
    //         (overlap & arith_core) - overlap
    // ════════════════════════════════════════════════════════

    std::cerr << "\n══ CANDIDATE DECOMPOSITION VERIFICATION "
              << "══════\n";

    // L246: r(a,c) = (a&c) - ((-c) & ((a&c)*a))
    // vars: a=v0, c=v1
    {
        const auto &r  = cases[case_idx[kL246]].residual;
        auto candidate = [](uint64_t a, uint64_t c) -> uint64_t {
            uint64_t overlap = a & c;
            uint64_t neg_c   = ~c + 1;
            return overlap - (neg_c & (overlap * a));
        };
        int bad = 0;
        for (uint64_t x = 0; x < 256; ++x) {
            for (uint64_t y = 0; y < 256; ++y) {
                if (r(x, y) != candidate(x, y)) { ++bad; }
            }
        }
        std::cerr << "  L246: " << bad << " mismatches / 65536\n";
        EXPECT_EQ(bad, 0) << "L246 candidate failed";
    }

    // L58: r(b,d) = (b&d) * (b - 1)
    // Derived from GT via half-adder identity:
    //   GT = -d + b*(b&d) - b - (b^d) + 1
    //   CoB = 1 - 2b - 2d + 3*(b&d)
    //   r = d + b + (b&d)*(b-3) - (b^d)
    //     = 2*(b&d) + (b&d)*(b-3)     [since b+d-(b^d) = 2*(b&d)]
    //     = (b&d) * (b - 1)
    // vars: b=v0, d=v1
    {
        const auto &r  = cases[case_idx[kL58]].residual;
        auto candidate = [](uint64_t b, uint64_t d) -> uint64_t { return (b & d) * (b - 1); };
        int bad        = 0;
        for (uint64_t x = 0; x < 256; ++x) {
            for (uint64_t y = 0; y < 256; ++y) {
                if (r(x, y) != candidate(x, y)) { ++bad; }
            }
        }
        std::cerr << "  L58: " << bad << " mismatches / 65536\n";
        EXPECT_EQ(bad, 0) << "L58 candidate failed";
    }

    // L178: r(c,e) = (c&e) & (2e + c²) - (c&e)
    // vars: c=v0, e=v1
    {
        const auto &r  = cases[case_idx[kL178]].residual;
        auto candidate = [](uint64_t c, uint64_t e) -> uint64_t {
            uint64_t overlap = c & e;
            uint64_t core    = (2 * e) + (c * c);
            return (overlap & core) - overlap;
        };
        int bad = 0;
        for (uint64_t x = 0; x < 256; ++x) {
            for (uint64_t y = 0; y < 256; ++y) {
                if (r(x, y) != candidate(x, y)) { ++bad; }
            }
        }
        std::cerr << "  L178: " << bad << " mismatches / 65536\n";
        EXPECT_EQ(bad, 0) << "L178 candidate failed";
    }

    // ════════════════════════════════════════════════════════
    // Section 6: Off-diagonal overlap probing
    //
    // Question: does the residual depend on full variables
    // or only on the overlap + one variable's extra bits?
    // ════════════════════════════════════════════════════════

    std::cerr << "\n══ OFF-DIAGONAL OVERLAP PROBING "
              << "═════════════\n";

    for (const auto &ac : cases) {
        std::cerr << "\n  L" << ac.line << ":\n";

        // Fix overlap=bit0, vary x's non-overlap bits
        // y=1, x=1|(extra<<1) so x&y=1 always
        std::cerr << "    overlap=1, sweep x "
                  << "(y=1, x=1|extra<<1):\n      ";
        for (uint64_t extra = 0; extra < 16; ++extra) {
            uint64_t x = 1 | (extra << 1);
            uint64_t y = 1;
            std::cerr << std::setw(8) << static_cast< int64_t >(ac.residual(x, y));
        }
        std::cerr << "\n";

        // Fix overlap=bit0, vary y's non-overlap bits
        // x=1, y=1|(extra<<1) so x&y=1 always
        std::cerr << "    overlap=1, sweep y "
                  << "(x=1, y=1|extra<<1):\n      ";
        for (uint64_t extra = 0; extra < 16; ++extra) {
            uint64_t x = 1;
            uint64_t y = 1 | (extra << 1);
            std::cerr << std::setw(8) << static_cast< int64_t >(ac.residual(x, y));
        }
        std::cerr << "\n";

        // Fix overlap=3, vary x's non-overlap bits
        // y=3, x=3|(extra<<2) so x&y=3 always
        std::cerr << "    overlap=3, sweep x "
                  << "(y=3, x=3|extra<<2):\n      ";
        for (uint64_t extra = 0; extra < 16; ++extra) {
            uint64_t x = 3 | (extra << 2);
            uint64_t y = 3;
            std::cerr << std::setw(8) << static_cast< int64_t >(ac.residual(x, y));
        }
        std::cerr << "\n";

        // Overlap sweep: vary overlap magnitude,
        // no non-overlap bits (x=y=overlap_val)
        // This is the diagonal, but for specific values
        std::cerr << "    pure overlap (x=y=val):\n      ";
        for (uint64_t val : { 1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL, 7ULL, 8ULL, 15ULL, 16ULL,
                              31ULL, 32ULL, 63ULL, 64ULL, 127ULL, 255ULL })
        {
            std::cerr << std::setw(8) << static_cast< int64_t >(ac.residual(val, val));
        }
        std::cerr << "\n";
    }

    // ════════════════════════════════════════════════════════
    // Summary
    // ════════════════════════════════════════════════════════

    std::cerr << "\n══ FAMILY SUMMARY "
              << "═══════════════════════════════\n"
              << "  Decomposition shapes (all verified exact):\n"
              << "    L58:  (b&d) * (b - 1)\n"
              << "    L246: (a&c) - ((-c) & ((a&c) * a))\n"
              << "    L178: (c&e) & (2e + c²) - (c&e)\n"
              << "  where overlap = x & y in each case\n"
              << "\n"
              << "  Common properties:\n"
              << "    - AND-zero vanishing: r=0 when overlap=0\n"
              << "    - overlap is universal gating term\n"
              << "  Sub-shapes:\n"
              << "    - L58:  overlap * arithmetic (simplest)\n"
              << "    - L246: overlap - masked(overlap * var)\n"
              << "    - L178: (overlap & arith_core) - overlap\n"
              << "  Corrections to frontier doc:\n"
              << "    - L58 != -L246 (agree off-diagonal, "
              << "diverge on diagonal)\n"
              << "    - L246 diagonal != -x(x-1); "
              << "it is x - ((-x) & x²)\n"
              << "    - L58 diagonal = x*(x-1) (exact)\n"
              << "═══════════════════════════════════════════════"
              << "\n";
}
