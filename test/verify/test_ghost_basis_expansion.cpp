// Audit-only prototype: test expanded ghost basis against the 15
// GhostResidual search-exhausted cases.
//
// Tests each residual (original - CoB) against candidate primitives
// with EXACT full-width verification.  No solver changes — this is
// measurement only to decide which primitives merit promotion.
//
// Primitive tiers:
//   Tier 1: Single scaled primitive
//     - c * (x_i^2 - x_i)           for each variable i
//     - c * (x_i*x_j - x_i & x_j)  for each pair (existing basis)
//   Tier 2: Sum of two scaled primitives
//     - c1*(x_i^2-x_i) + c2*(x_j^2-x_j)
//     - c1*(x_i^2-x_i) + c2*(x_j*x_k - x_j&x_k)
//   Tier 3: Affine-scaled ghost
//     - (a*x_j + b) * (x_i^2 - x_i)

#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

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
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { return ""; }
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    // Ghost primitive: x^2 - x = x*(x-1)
    uint64_t sq_sub(uint64_t x) { return (x * x) - x; }

    // Ghost primitive: x*y - (x & y)
    uint64_t mul_sub_and(uint64_t x, uint64_t y) { return (x * y) - (x & y); }

    // Modular inverse for odd values (extended Euclidean in Z/2^64)
    uint64_t mod_inverse(uint64_t a) {
        uint64_t x = a; // a * a ≡ a (mod 2) for odd a
        for (int i = 0; i < 6; ++i) { x *= 2 - a * x; }
        return x;
    }

    // Try to solve: residual = c * ghost_fn(vars)
    // Returns (true, c) if exact match across 16 random probes.
    struct ScalarFit
    {
        bool exact = false;
        uint64_t c = 0;
        std::string desc;
    };

    ScalarFit try_scalar_fit(
        const std::function< uint64_t(const std::vector< uint64_t > &) > &residual,
        const std::function< uint64_t(const std::vector< uint64_t > &) > &ghost, uint32_t nv,
        const std::string &desc
    ) {
        // Probe at a diagnostic point with distinct per-variable
        // values to break symmetry between variable-indexed ghosts.
        std::vector< uint64_t > pt(nv);
        for (uint32_t v = 0; v < nv; ++v) { pt[v] = 3 + v * 2; }
        uint64_t g = ghost(pt);
        uint64_t r = residual(pt);

        if (g == 0) { return { false, 0, desc }; }

        // g must be odd for clean division, or we need 2-adic
        auto tz_g = std::countr_zero(g);
        auto tz_r = std::countr_zero(r);
        if (r != 0 && tz_r < tz_g) { return { false, 0, desc }; }

        uint64_t c = 0;
        if (r == 0 && g != 0) {
            // Try another asymmetric point
            std::vector< uint64_t > pt2(nv);
            for (uint32_t v = 0; v < nv; ++v) { pt2[v] = 7 + v * 3; }
            uint64_t g2 = ghost(pt2);
            uint64_t r2 = residual(pt2);
            if (r2 == 0) { return { false, 0, desc }; }
            if (g2 == 0) { return { false, 0, desc }; }
            auto tz_g2 = std::countr_zero(g2);
            auto tz_r2 = std::countr_zero(r2);
            if (tz_r2 < tz_g2) { return { false, 0, desc }; }
            c = (r2 >> tz_g2) * mod_inverse(g2 >> tz_g2);
        } else if (r == 0) {
            c = 0;
        } else {
            c = (r >> tz_g) * mod_inverse(g >> tz_g);
        }

        // Cross-validate at 16 diverse points
        uint64_t rng = 0x12345678DEADBEEFULL;
        for (int probe = 0; probe < 16; ++probe) {
            std::vector< uint64_t > test_pt(nv);
            for (uint32_t v = 0; v < nv; ++v) {
                rng        = rng * 6364136223846793005ULL + 1442695040888963407ULL;
                test_pt[v] = rng;
            }
            uint64_t g_val = ghost(test_pt);
            uint64_t r_val = residual(test_pt);
            if ((c * g_val) != r_val) { return { false, c, desc }; }
        }

        return { true, c, desc };
    }

    // Try: residual = c1*g1 + c2*g2 (sum of two primitives)
    struct SumFit
    {
        bool exact  = false;
        uint64_t c1 = 0;
        uint64_t c2 = 0;
        std::string desc;
    };

    SumFit try_sum_fit(
        const std::function< uint64_t(const std::vector< uint64_t > &) > &residual,
        const std::function< uint64_t(const std::vector< uint64_t > &) > &g1,
        const std::function< uint64_t(const std::vector< uint64_t > &) > &g2, uint32_t nv,
        const std::string &desc
    ) {
        // Solve 2×2 system: r = c1*g1 + c2*g2 at two points.
        // Use distinct per-variable values to break symmetry —
        // uniform points make same-arity ghost terms evaluate
        // identically, collapsing the system.
        std::vector< uint64_t > pt_a(nv);
        std::vector< uint64_t > pt_b(nv);
        for (uint32_t v = 0; v < nv; ++v) {
            pt_a[v] = 3 + v * 2;  // {3, 5, 7, 9, ...}
            pt_b[v] = 11 + v * 3; // {11, 14, 17, 20, ...}
        }

        uint64_t ra = residual(pt_a), rb = residual(pt_b);
        uint64_t a11 = g1(pt_a), a12 = g2(pt_a);
        uint64_t a21 = g1(pt_b), a22 = g2(pt_b);

        // det = a11*a22 - a12*a21
        uint64_t det = a11 * a22 - a12 * a21;
        if (det == 0) { return { false, 0, 0, desc }; }
        if ((det & 1) == 0) { return { false, 0, 0, desc }; } // need odd det

        uint64_t inv_det = mod_inverse(det);
        uint64_t c1      = (a22 * ra - a12 * rb) * inv_det;
        uint64_t c2      = (a11 * rb - a21 * ra) * inv_det;

        // Cross-validate at 16 points
        uint64_t rng = 0xCAFEBABE01234567ULL;
        for (int probe = 0; probe < 16; ++probe) {
            std::vector< uint64_t > test_pt(nv);
            for (uint32_t v = 0; v < nv; ++v) {
                rng        = rng * 6364136223846793005ULL + 1442695040888963407ULL;
                test_pt[v] = rng;
            }
            uint64_t expected = c1 * g1(test_pt) + c2 * g2(test_pt);
            uint64_t actual   = residual(test_pt);
            if (expected != actual) { return { false, c1, c2, desc }; }
        }

        return { true, c1, c2, desc };
    }

    const std::vector< int > kTargetLines = { 14,  85,  95,  96,  102, 107, 115, 131,
                                              148, 152, 155, 186, 218, 246, 278 };

} // namespace

TEST(GhostBasisExpansion, TieredProbe) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< std::string > lines;
    std::string line;
    while (std::getline(file, line)) { lines.push_back(line); }

    int total        = 0;
    int tier1_solved = 0;
    int tier2_solved = 0;
    int tier3_solved = 0;
    int unsolved     = 0;

    std::cerr << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";
    std::cerr << "  GHOST BASIS EXPANSION AUDIT (exact verification)\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";

    for (int target_line : kTargetLines) {
        if (target_line < 1 || target_line > static_cast< int >(lines.size())) { continue; }
        const auto &raw = lines[static_cast< size_t >(target_line) - 1];
        if (raw.empty() || raw[0] == '#') { continue; }

        size_t sep = find_separator(raw);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(raw.substr(0, sep));
        std::string gt_str     = trim(raw.substr(sep + 1));
        if (obfuscated.empty()) { continue; }

        auto parse = ParseAndEvaluate(obfuscated, 64);
        if (!parse.has_value()) { continue; }
        auto ast = ParseToAst(obfuscated, 64);
        if (!ast.has_value()) { continue; }

        auto folded      = FoldConstantBitwise(std::move(ast.value().expr), 64);
        const auto &sig  = parse.value().sig;
        const auto &vars = parse.value().vars;
        auto nv          = static_cast< uint32_t >(vars.size());

        auto bool_elim = EliminateAuxVars(sig, vars);
        auto bool_real = static_cast< uint32_t >(bool_elim.real_vars.size());

        auto folded_ptr     = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
        Evaluator orig_eval = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        // Build reduced-space evaluator
        std::vector< uint32_t > var_map;
        for (const auto &rv : bool_elim.real_vars) {
            for (uint32_t j = 0; j < nv; ++j) {
                if (vars[j] == rv) {
                    var_map.push_back(j);
                    break;
                }
            }
        }
        Evaluator reduced_eval = [&orig_eval, &var_map,
                                  nv](const std::vector< uint64_t > &v) -> uint64_t {
            std::vector< uint64_t > full(nv, 0);
            for (size_t i = 0; i < var_map.size(); ++i) { full[var_map[i]] = v[i]; }
            return orig_eval(full);
        };

        // Build CoB and residual evaluator
        auto coeffs   = InterpolateCoefficients(bool_elim.reduced_sig, bool_real, 64);
        auto cob_expr = BuildCobExpr(coeffs, bool_real, 64);
        auto cob_str  = Render(*cob_expr, bool_elim.real_vars, 64);

        auto cob_shared = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*cob_expr));
        std::function< uint64_t(const std::vector< uint64_t > &) > residual =
            [reduced_eval, cob_shared](const std::vector< uint64_t > &v) -> uint64_t {
            return reduced_eval(v) - EvalExpr(**cob_shared, v, 64);
        };

        ++total;

        std::cerr << "\n────────────────────────────────────────────────────\n";
        std::cerr << "L" << target_line << " vars=" << bool_real
                  << " CoB: " << cob_str.substr(0, 60) << "\n";
        std::cerr << "  GT: " << gt_str << "\n";

        bool solved = false;

        // ══════════════════════════════════════════════════════
        //  TIER 1: Single scaled primitive
        // ══════════════════════════════════════════════════════

        // 1a: c * (x_i^2 - x_i) for each variable
        for (uint32_t i = 0; i < bool_real && !solved; ++i) {
            auto ghost = [i](const std::vector< uint64_t > &v) -> uint64_t {
                return sq_sub(v[i]);
            };
            std::string desc =
                "c*(" + bool_elim.real_vars[i] + "^2-" + bool_elim.real_vars[i] + ")";
            auto fit = try_scalar_fit(residual, ghost, bool_real, desc);
            if (fit.exact) {
                std::cerr << "  TIER1 EXACT: " << fit.c << " * " << fit.desc << "\n";
                solved = true;
                tier1_solved++;
            }
        }

        // 1b: c * (x_i*x_j - x_i&x_j) for each pair
        for (uint32_t i = 0; i < bool_real && !solved; ++i) {
            for (uint32_t j = i + 1; j < bool_real && !solved; ++j) {
                auto ghost = [i, j](const std::vector< uint64_t > &v) -> uint64_t {
                    return mul_sub_and(v[i], v[j]);
                };
                std::string desc = "c*(" + bool_elim.real_vars[i] + "*" + bool_elim.real_vars[j]
                    + "-" + bool_elim.real_vars[i] + "&" + bool_elim.real_vars[j] + ")";
                auto fit = try_scalar_fit(residual, ghost, bool_real, desc);
                if (fit.exact) {
                    std::cerr << "  TIER1 EXACT: " << fit.c << " * " << fit.desc << "\n";
                    solved = true;
                    tier1_solved++;
                }
            }
        }

        if (solved) { continue; }

        // ══════════════════════════════════════════════════════
        //  TIER 2: Sum of two scaled primitives
        // ══════════════════════════════════════════════════════

        // Build all single-variable ghosts as a list
        struct GhostFn
        {
            std::function< uint64_t(const std::vector< uint64_t > &) > fn;
            std::string name;
        };

        std::vector< GhostFn > basis;

        for (uint32_t i = 0; i < bool_real; ++i) {
            basis.push_back(
                {
                    [i](const std::vector< uint64_t > &v) { return sq_sub(v[i]); },
                    bool_elim.real_vars[i] + "^2-" + bool_elim.real_vars[i],
                }
            );
        }
        for (uint32_t i = 0; i < bool_real; ++i) {
            for (uint32_t j = i + 1; j < bool_real; ++j) {
                basis.push_back(
                    {
                        [i, j](const std::vector< uint64_t > &v) {
                            return mul_sub_and(v[i], v[j]);
                        },
                        bool_elim.real_vars[i] + "*" + bool_elim.real_vars[j] + "-"
                            + bool_elim.real_vars[i] + "&" + bool_elim.real_vars[j],
                    }
                );
            }
        }

        // Try all pairs from the basis
        for (size_t a = 0; a < basis.size() && !solved; ++a) {
            for (size_t b = a + 1; b < basis.size() && !solved; ++b) {
                std::string desc = "c1*(" + basis[a].name + ") + c2*(" + basis[b].name + ")";
                auto fit = try_sum_fit(residual, basis[a].fn, basis[b].fn, bool_real, desc);
                if (fit.exact) {
                    std::cerr << "  TIER2 EXACT: " << fit.c1 << "*(" << basis[a].name << ") + "
                              << fit.c2 << "*(" << basis[b].name << ")\n";
                    solved = true;
                    tier2_solved++;
                }
            }
        }

        if (solved) { continue; }

        // ══════════════════════════════════════════════════════
        //  TIER 2b: Weighted poly with product-of-two-ghost weight
        //  r(x) = c * weight(x)  where weight is a product of
        //  two ghost factors from Tier 1 basis
        // ══════════════════════════════════════════════════════

        // Product of two sq_sub
        for (uint32_t i = 0; i < bool_real && !solved; ++i) {
            for (uint32_t j = i; j < bool_real && !solved; ++j) {
                auto ghost = [i, j](const std::vector< uint64_t > &v) -> uint64_t {
                    return sq_sub(v[i]) * sq_sub(v[j]);
                };
                std::string vi   = bool_elim.real_vars[i];
                std::string vj   = bool_elim.real_vars[j];
                std::string desc = "c*(" + vi + "^2-" + vi + ")*(" + vj + "^2-" + vj + ")";
                auto fit         = try_scalar_fit(residual, ghost, bool_real, desc);
                if (fit.exact) {
                    std::cerr << "  TIER2b EXACT: " << fit.c << " * " << fit.desc << "\n";
                    solved = true;
                    tier2_solved++;
                }
            }
        }

        // Product of sq_sub and mul_sub_and
        for (uint32_t i = 0; i < bool_real && !solved; ++i) {
            for (uint32_t j = 0; j < bool_real && !solved; ++j) {
                for (uint32_t k = j + 1; k < bool_real && !solved; ++k) {
                    auto ghost = [i, j, k](const std::vector< uint64_t > &v) -> uint64_t {
                        return sq_sub(v[i]) * mul_sub_and(v[j], v[k]);
                    };
                    std::string vi   = bool_elim.real_vars[i];
                    std::string vj   = bool_elim.real_vars[j];
                    std::string vk   = bool_elim.real_vars[k];
                    std::string desc = "c*(" + vi + "^2-" + vi + ")*(" + vj + "*" + vk + "-"
                        + vj + "&" + vk + ")";
                    auto fit = try_scalar_fit(residual, ghost, bool_real, desc);
                    if (fit.exact) {
                        std::cerr << "  TIER2b EXACT: " << fit.c << " * " << fit.desc << "\n";
                        solved = true;
                        tier2_solved++;
                    }
                }
            }
        }

        // Product of two mul_sub_and (distinct pairs)
        for (uint32_t i = 0; i < bool_real && !solved; ++i) {
            for (uint32_t j = i + 1; j < bool_real && !solved; ++j) {
                for (uint32_t k = i; k < bool_real && !solved; ++k) {
                    uint32_t l_start = (k == i) ? j + 1 : k + 1;
                    for (uint32_t l = l_start; l < bool_real && !solved; ++l) {
                        auto ghost = [i, j, k,
                                      l](const std::vector< uint64_t > &v) -> uint64_t {
                            return mul_sub_and(v[i], v[j]) * mul_sub_and(v[k], v[l]);
                        };
                        std::string desc = "c*(" + bool_elim.real_vars[i] + "*"
                            + bool_elim.real_vars[j] + "-" + bool_elim.real_vars[i] + "&"
                            + bool_elim.real_vars[j] + ")*(" + bool_elim.real_vars[k] + "*"
                            + bool_elim.real_vars[l] + "-" + bool_elim.real_vars[k] + "&"
                            + bool_elim.real_vars[l] + ")";
                        auto fit = try_scalar_fit(residual, ghost, bool_real, desc);
                        if (fit.exact) {
                            std::cerr << "  TIER2b EXACT: " << fit.c << " * " << fit.desc
                                      << "\n";
                            solved = true;
                            tier2_solved++;
                        }
                    }
                }
            }
        }

        if (solved) { continue; }

        // ══════════════════════════════════════════════════════
        //  TIER 3: Ghost × variable (affine-scaled ghost)
        //  r(x) = c * x_j * (x_i^2 - x_i)
        //  r(x) = c * x_j * (x_i*x_k - x_i&x_k)
        // ══════════════════════════════════════════════════════

        int tier3_local = 0;

        for (uint32_t i = 0; i < bool_real && !solved; ++i) {
            for (uint32_t j = 0; j < bool_real && !solved; ++j) {
                // x_j * (x_i^2 - x_i)
                auto ghost = [i, j](const std::vector< uint64_t > &v) -> uint64_t {
                    return v[j] * sq_sub(v[i]);
                };
                std::string vi   = bool_elim.real_vars[i];
                std::string vj   = bool_elim.real_vars[j];
                std::string desc = "c*" + vj + "*(" + vi + "^2-" + vi + ")";
                auto fit         = try_scalar_fit(residual, ghost, bool_real, desc);
                if (fit.exact) {
                    std::cerr << "  TIER3 EXACT: " << fit.c << " * " << fit.desc << "\n";
                    solved = true;
                    tier3_local++;
                }
            }
        }

        for (uint32_t i = 0; i < bool_real && !solved; ++i) {
            for (uint32_t j = i + 1; j < bool_real && !solved; ++j) {
                for (uint32_t k = 0; k < bool_real && !solved; ++k) {
                    // x_k * (x_i*x_j - x_i&x_j)
                    auto ghost = [i, j, k](const std::vector< uint64_t > &v) -> uint64_t {
                        return v[k] * mul_sub_and(v[i], v[j]);
                    };
                    std::string vi = bool_elim.real_vars[i];
                    std::string vj = bool_elim.real_vars[j];
                    std::string vk = bool_elim.real_vars[k];
                    std::string desc =
                        "c*" + vk + "*(" + vi + "*" + vj + "-" + vi + "&" + vj + ")";
                    auto fit = try_scalar_fit(residual, ghost, bool_real, desc);
                    if (fit.exact) {
                        std::cerr << "  TIER3 EXACT: " << fit.c << " * " << fit.desc << "\n";
                        solved = true;
                        tier3_local++;
                    }
                }
            }
        }

        if (tier3_local > 0) { tier3_solved += tier3_local; }

        if (!solved) {
            unsolved++;
            // Report residual at asymmetric points
            std::vector< uint64_t > pt_a(bool_real);
            std::vector< uint64_t > pt_b(bool_real);
            std::vector< uint64_t > pt_c(bool_real);
            for (uint32_t v = 0; v < bool_real; ++v) {
                pt_a[v] = 3 + v * 2;
                pt_b[v] = 5 + v * 3;
                pt_c[v] = 7 + v * 5;
            }
            std::cerr << "  UNSOLVED"
                      << " r(a)=" << residual(pt_a) << " r(b)=" << residual(pt_b)
                      << " r(c)=" << residual(pt_c) << "\n";
        }
    }

    // ── Summary ─────────────────────────────────────────────

    std::cerr << "\n═══════════════════════════════════════════════════════════\n";
    std::cerr << "  RESULTS\n";
    std::cerr << "  Total: " << total << "\n";
    std::cerr << "  Tier 1 (single scalar×prim): " << tier1_solved << "/" << total << "\n";
    std::cerr << "  Tier 2 (sum / product of two): " << tier2_solved << "/" << total << "\n";
    std::cerr << "  Tier 3 (ghost × variable): " << tier3_solved << "/" << total << "\n";
    std::cerr << "  Unsolved: " << unsolved << "/" << total << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";

    EXPECT_EQ(total, 15);
}
