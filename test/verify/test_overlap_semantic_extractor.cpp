// Semantic extraction for all 18 overlap-sensitive QSynth cases.
//
// For each 2-variable case:
//   1. Test AND-zero vanishing (exhaustive [0..255]^2)
//   2. Try Form 1: r = (x&y) * f(x)  or  r = (x&y) * f(y)
//   3. Try Form 2: r = (x&y) - (mask & ((x&y)*v))  [8 templates]
//   4. Try Form 3: (r + (x&y)) masked by (x&y) = r + (x&y)
//
// For each 3-variable case:
//   Test pairwise AND-zero vanishing per variable pair.

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
#include <vector>

using namespace cobra;

namespace {

    // All 18 overlap-sensitive lines from the frontier analysis
    const std::vector< int > kOverlapLines = { 20,  58,  62,  85,  96,  107, 131, 178, 186,
                                               216, 246, 278, 333, 366, 371, 378, 381, 495 };

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

    // -- Residual evaluator type --

    using ResidualFn = std::function< uint64_t(const std::vector< uint64_t > &) >;

    // -- AND-zero vanishing --

    // 2-var: count violations where r(x,y)!=0 but x&y==0
    int CountVanishingViolations2(const ResidualFn &r, int range) {
        int violations = 0;
        for (int x = 0; x < range; ++x) {
            for (int y = 0; y < range; ++y) {
                if ((x & y) != 0) { continue; }
                std::vector< uint64_t > v = { static_cast< uint64_t >(x),
                                              static_cast< uint64_t >(y) };
                if (r(v) != 0) { ++violations; }
            }
        }
        return violations;
    }

    // 2-var: count total disjoint pairs in range
    int CountDisjointPairs(int range) {
        int count = 0;
        for (int x = 0; x < range; ++x) {
            for (int y = 0; y < range; ++y) {
                if ((x & y) == 0) { ++count; }
            }
        }
        return count;
    }

    // 3-var: count violations for pair (vi,vj), free vk
    int CountVanishingViolations3(const ResidualFn &r, int vi, int vj, int range) {
        int vk         = 3 - vi - vj;
        int violations = 0;
        for (int a = 0; a < range; ++a) {
            for (int b = 0; b < range; ++b) {
                if ((a & b) != 0) { continue; }
                for (int c = 0; c < range; ++c) {
                    std::vector< uint64_t > v(3);
                    v[vi] = static_cast< uint64_t >(a);
                    v[vj] = static_cast< uint64_t >(b);
                    v[vk] = static_cast< uint64_t >(c);
                    if (r(v) != 0) { ++violations; }
                }
            }
        }
        return violations;
    }

    // -- Form 1: r = (x&y) * f(x)  or  f(y) --

    // Cross-multiplication test: for fixed var_idx variable,
    // r(x,y) * ref_overlap == ref_r * (x&y) for all (x,y).
    // var_idx: 0 = f depends on x, 1 = f depends on y.
    bool TestForm1(const ResidualFn &r, int var_idx, int range) {
        for (int fixed = 1; fixed < range; ++fixed) {
            // Find a reference value for the other variable
            uint64_t ref_other = 0;
            for (uint64_t other = 1; other <= static_cast< uint64_t >(fixed); ++other) {
                if ((fixed & other) != 0) {
                    ref_other = other;
                    break;
                }
            }
            if (ref_other == 0) { continue; }

            std::vector< uint64_t > ref_v(2);
            ref_v[var_idx]     = static_cast< uint64_t >(fixed);
            ref_v[1 - var_idx] = ref_other;
            uint64_t r_ref     = r(ref_v);
            uint64_t o_ref     = fixed & ref_other;

            for (int other = 0; other < range; ++other) {
                uint64_t o = static_cast< uint64_t >(fixed) & static_cast< uint64_t >(other);
                if (o == 0) { continue; }
                std::vector< uint64_t > v(2);
                v[var_idx]     = static_cast< uint64_t >(fixed);
                v[1 - var_idx] = static_cast< uint64_t >(other);
                uint64_t r_val = r(v);
                if (r_val * o_ref != r_ref * o) { return false; }
            }
        }
        return true;
    }

    // Try Form 1 on both variable orientations.
    // Returns: 'x' if f(x), 'y' if f(y), 0 if neither.
    char TryForm1(const ResidualFn &r, int range) {
        if (TestForm1(r, 0, range)) { return 'x'; }
        if (TestForm1(r, 1, range)) { return 'y'; }
        return 0;
    }

    // -- Form 2: r = overlap +/- (mask & (overlap * v)) --

    // 8 templates: mask in {-x,-y}, prod in {x,y}, sign in {+,-}
    // Template index encodes: bit0=sign, bit1=prod, bit2=mask
    //   mask: bit2=0 -> -y, bit2=1 -> -x
    //   prod: bit1=0 -> x,  bit1=1 -> y
    //   sign: bit0=0 -> overlap-inner, bit0=1 -> inner-overlap
    int TryForm2(const ResidualFn &r, int range) {
        for (int tmpl = 0; tmpl < 8; ++tmpl) {
            bool match = true;
            for (int ix = 0; ix < range && match; ++ix) {
                for (int iy = 0; iy < range && match; ++iy) {
                    auto x           = static_cast< uint64_t >(ix);
                    auto y           = static_cast< uint64_t >(iy);
                    uint64_t overlap = x & y;

                    uint64_t mask  = ((tmpl & 4) != 0) ? (~x + 1) : (~y + 1);
                    uint64_t prod  = ((tmpl & 2) != 0) ? y : x;
                    uint64_t inner = mask & (overlap * prod);
                    uint64_t expected =
                        ((tmpl & 1) == 0) ? (overlap - inner) : (inner - overlap);

                    std::vector< uint64_t > v = { x, y };
                    if (r(v) != expected) { match = false; }
                }
            }
            if (match) { return tmpl + 1; }
        }
        return 0;
    }

    // -- Form 3: (r + overlap) & ~overlap == 0 --

    bool TryForm3(const ResidualFn &r, int range) {
        for (int ix = 0; ix < range; ++ix) {
            for (int iy = 0; iy < range; ++iy) {
                auto x                    = static_cast< uint64_t >(ix);
                auto y                    = static_cast< uint64_t >(iy);
                uint64_t overlap          = x & y;
                std::vector< uint64_t > v = { x, y };
                uint64_t sum              = r(v) + overlap;
                if ((sum & ~overlap) != 0) { return false; }
            }
        }
        return true;
    }

    // -- Form 1 characterization --

    // Extract f(x) for small x using r(x,1) when x is odd
    void CharacterizeForm1(const ResidualFn &r, int var_idx, std::ostream &os) {
        os << "  f(v" << var_idx << ") samples: ";
        for (int val = 1; val <= 15; val += 2) {
            std::vector< uint64_t > v(2);
            v[var_idx]     = static_cast< uint64_t >(val);
            v[1 - var_idx] = 1;
            // overlap = val & 1 = 1 for odd val, so f(val) = r
            auto fval      = static_cast< int64_t >(r(v));
            os << val << "->" << fval << " ";
        }
        os << "\n";
    }

    // -- Result struct --

    struct ExtractionResult
    {
        int line            = 0;
        int fw_vars         = 0;
        int vanish_failures = 0;
        int vanish_tested   = 0;
        char form1_var      = 0; // 'x','y',0
        int form2_tmpl      = 0; // 1-8 or 0
        bool form3          = false;
        // 3-var pairwise vanishing
        int vanish_01       = -1;
        int vanish_02       = -1;
        int vanish_12       = -1;
    };

} // namespace

TEST(OverlapSemanticExtractor, FullCluster) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< std::string > lines;
    std::string line;
    while (std::getline(file, line)) { lines.push_back(line); }

    std::vector< ExtractionResult > results;

    std::cerr << "\n";
    std::cerr << "═══════════════════════════════════════════════\n";
    std::cerr << "  OVERLAP SEMANTIC EXTRACTOR\n";
    std::cerr << "═══════════════════════════════════════════════\n";

    for (int tl : kOverlapLines) {
        ASSERT_GE(tl, 1);
        ASSERT_LE(tl, static_cast< int >(lines.size()));

        const auto &raw = lines[tl - 1];
        if (raw.empty() || raw[0] == '#') { continue; }

        size_t sep = find_separator(raw);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(raw.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse = ParseAndEvaluate(obfuscated, 64);
        if (!parse.has_value()) { continue; }
        auto ast = ParseToAst(obfuscated, 64);
        if (!ast.has_value()) { continue; }

        auto folded      = FoldConstantBitwise(std::move(ast.value().expr), 64);
        const auto &sig  = parse.value().sig;
        const auto &vars = parse.value().vars;
        auto nv          = static_cast< uint32_t >(vars.size());

        auto elim  = EliminateAuxVars(sig, vars);
        auto nreal = static_cast< uint32_t >(elim.real_vars.size());

        if (nreal < 2 || nreal > 3) {
            std::cerr << "  L" << tl << ": " << nreal << " real vars (skipped)\n";
            continue;
        }

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

        auto cob_shared = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*cob_expr));

        ResidualFn residual = [reduced_eval, cob_shared,
                               nreal](const std::vector< uint64_t > &v) -> uint64_t {
            return reduced_eval(v) - EvalExpr(**cob_shared, v, 64);
        };

        ExtractionResult er;
        er.line    = tl;
        er.fw_vars = static_cast< int >(nreal);

        std::cerr << "\n── L" << tl << " (FV=" << nreal << ", vars:";
        for (const auto &rv : elim.real_vars) { std::cerr << " " << rv; }
        std::cerr << ") ──\n";

        if (nreal == 2) {
            // -- 2-variable: full extraction pipeline --
            constexpr int kRange = 256;

            er.vanish_tested   = CountDisjointPairs(kRange);
            er.vanish_failures = CountVanishingViolations2(residual, kRange);

            double zero_rate =
                1.0 - (static_cast< double >(er.vanish_failures) / er.vanish_tested);
            std::cerr << "  AND-zero vanishing: " << er.vanish_failures << " / "
                      << er.vanish_tested << " violations"
                      << " (" << std::fixed << std::setprecision(1) << (zero_rate * 100)
                      << "% zero)\n";

            if (er.vanish_failures == 0) {
                // Try Form 1
                er.form1_var = TryForm1(residual, kRange);
                if (er.form1_var != 0) {
                    std::cerr << "  Form 1: YES (f depends"
                              << " on v" << (er.form1_var == 'x' ? "0" : "1") << ")\n";
                    CharacterizeForm1(residual, (er.form1_var == 'x') ? 0 : 1, std::cerr);
                } else {
                    std::cerr << "  Form 1: no\n";
                }

                // Try Form 2
                if (er.form1_var == 0) {
                    er.form2_tmpl = TryForm2(residual, kRange);
                    if (er.form2_tmpl != 0) {
                        int t                = er.form2_tmpl - 1;
                        const char *mask_str = ((t & 4) != 0) ? "-x" : "-y";
                        const char *prod_str = ((t & 2) != 0) ? "y" : "x";
                        const char *sign_str = ((t & 1) == 0) ? "o-inner" : "inner-o";
                        std::cerr << "  Form 2: YES (tmpl " << er.form2_tmpl
                                  << ": mask=" << mask_str << " prod=" << prod_str << " "
                                  << sign_str << ")\n";
                    } else {
                        std::cerr << "  Form 2: no\n";
                    }
                }

                // Try Form 3
                if (er.form1_var == 0 && er.form2_tmpl == 0) {
                    er.form3 = TryForm3(residual, kRange);
                    std::cerr << "  Form 3: " << (er.form3 ? "YES" : "no") << "\n";
                }
            }
        } else {
            // -- 3-variable: pairwise vanishing only --
            constexpr int kRange3 = 32;
            for (int vi = 0; vi < 3; ++vi) {
                for (int vj = vi + 1; vj < 3; ++vj) {
                    int failures = CountVanishingViolations3(residual, vi, vj, kRange3);
                    std::cerr << "  pair (" << vi << "," << vj << "): " << failures
                              << " violations\n";
                    if (vi == 0 && vj == 1) { er.vanish_01 = failures; }
                    if (vi == 0 && vj == 2) { er.vanish_02 = failures; }
                    if (vi == 1 && vj == 2) { er.vanish_12 = failures; }
                }
            }
        }

        results.push_back(er);
    }

    // =========================================================
    // Summary table
    // =========================================================

    std::cerr << "\n═══════════════════════════════════════════════\n"
              << "  SUMMARY TABLE\n"
              << "═══════════════════════════════════════════════\n"
              << std::setw(5) << "Line" << std::setw(4) << "FV" << std::setw(10) << "Vanish"
              << std::setw(10) << "Form" << "  Details\n"
              << "  ─────────────────────────────────────────────\n";

    int total_vanish  = 0;
    int total_form1   = 0;
    int total_form2   = 0;
    int total_form3   = 0;
    int total_outside = 0;

    for (const auto &er : results) {
        std::cerr << std::setw(5) << er.line << std::setw(4) << er.fw_vars;

        if (er.fw_vars == 2) {
            if (er.vanish_failures == 0) {
                std::cerr << std::setw(10) << "YES";
                ++total_vanish;

                if (er.form1_var != 0) {
                    std::cerr << std::setw(10) << (std::string("1(") + er.form1_var + ")");
                    ++total_form1;
                } else if (er.form2_tmpl != 0) {
                    std::cerr << std::setw(10) << ("2(" + std::to_string(er.form2_tmpl) + ")");
                    ++total_form2;
                } else if (er.form3) {
                    std::cerr << std::setw(10) << "3";
                    ++total_form3;
                } else {
                    std::cerr << std::setw(10) << "???";
                }
            } else {
                double pct = 100.0
                    * (1.0 - static_cast< double >(er.vanish_failures) / er.vanish_tested);
                std::cerr << std::setw(8) << std::fixed << std::setprecision(0) << pct << "%";
                std::cerr << std::setw(10) << "-";
                ++total_outside;
            }
        } else {
            // 3-var: show pairwise vanishing
            std::cerr << std::setw(10);
            bool any_vanish = false;
            std::string pairs;
            if (er.vanish_01 == 0) {
                pairs      += "(0,1)";
                any_vanish  = true;
            }
            if (er.vanish_02 == 0) {
                if (!pairs.empty()) { pairs += ","; }
                pairs      += "(0,2)";
                any_vanish  = true;
            }
            if (er.vanish_12 == 0) {
                if (!pairs.empty()) { pairs += ","; }
                pairs      += "(1,2)";
                any_vanish  = true;
            }
            if (any_vanish) {
                std::cerr << pairs;
                std::cerr << std::setw(10) << "3v";
                ++total_vanish;
            } else {
                std::cerr << "no";
                std::cerr << std::setw(10) << "-";
                ++total_outside;
            }
        }
        std::cerr << "\n";
    }

    std::cerr << "  ─────────────────────────────────────────────\n"
              << "  AND-zero vanishing: " << total_vanish << " / " << results.size() << "\n"
              << "  Form 1 (overlap*f): " << total_form1 << "\n"
              << "  Form 2 (mask&prod): " << total_form2 << "\n"
              << "  Form 3 (overlap&core): " << total_form3 << "\n"
              << "  Outside family: " << total_outside << "\n"
              << "═══════════════════════════════════════════════\n";

    // At minimum, the 3 anchors must be detected
    int total_detected = total_form1 + total_form2 + total_form3;
    EXPECT_GE(total_detected, 3) << "Expected at least 3 anchor cases detected";
}
