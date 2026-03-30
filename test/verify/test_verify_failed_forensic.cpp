#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
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
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { return ""; }
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    std::string semantic_str(SemanticClass s) {
        switch (s) {
            case SemanticClass::kLinear:
                return "Linear";
            case SemanticClass::kSemilinear:
                return "Semilinear";
            case SemanticClass::kPolynomial:
                return "Polynomial";
            case SemanticClass::kNonPolynomial:
                return "NonPoly";
        }
        return "?";
    }

    std::string flag_str(StructuralFlag f) {
        std::string s;
        if (HasFlag(f, kSfHasBitwise)) { s += "Bw|"; }
        if (HasFlag(f, kSfHasArithmetic)) { s += "Ar|"; }
        if (HasFlag(f, kSfHasMul)) { s += "Mul|"; }
        if (HasFlag(f, kSfHasMultilinearProduct)) { s += "Mlp|"; }
        if (HasFlag(f, kSfHasSingletonPower)) { s += "Pow|"; }
        if (HasFlag(f, kSfHasMixedProduct)) { s += "MxP|"; }
        if (HasFlag(f, kSfHasBitwiseOverArith)) { s += "BoA|"; }
        if (HasFlag(f, kSfHasArithOverBitwise)) { s += "AoB|"; }
        if (HasFlag(f, kSfHasMultivarHighPower)) { s += "HiP|"; }
        if (!s.empty()) { s.pop_back(); }
        return s;
    }

    std::string kind_str(Expr::Kind k) {
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

    // Classify the "reconstruction shape" of an expression:
    // what operator families does it use?
    struct ReconShape
    {
        bool has_add           = false;
        bool has_mul           = false;
        bool has_and           = false;
        bool has_or            = false;
        bool has_xor           = false;
        bool has_not           = false;
        bool has_neg           = false;
        bool has_shr           = false;
        bool has_nonlinear_mul = false; // var*var
        uint32_t depth         = 0;
        uint32_t nodes         = 0;
    };

    void collect_shape(const Expr &expr, uint32_t depth, ReconShape &shape) {
        shape.nodes++;
        if (depth > shape.depth) { shape.depth = depth; }
        switch (expr.kind) {
            case Expr::Kind::kAdd:
                shape.has_add = true;
                break;
            case Expr::Kind::kMul:
                shape.has_mul = true;
                if (expr.children.size() == 2 && HasVarDep(*expr.children[0])
                    && HasVarDep(*expr.children[1]))
                {
                    shape.has_nonlinear_mul = true;
                }
                break;
            case Expr::Kind::kAnd:
                shape.has_and = true;
                break;
            case Expr::Kind::kOr:
                shape.has_or = true;
                break;
            case Expr::Kind::kXor:
                shape.has_xor = true;
                break;
            case Expr::Kind::kNot:
                shape.has_not = true;
                break;
            case Expr::Kind::kNeg:
                shape.has_neg = true;
                break;
            case Expr::Kind::kShr:
                shape.has_shr = true;
                break;
            default:
                break;
        }
        for (const auto &c : expr.children) { collect_shape(*c, depth + 1, shape); }
    }

    std::string shape_summary(const ReconShape &s) {
        std::string r;
        if (s.has_add) { r += "Add "; }
        if (s.has_neg) { r += "Neg "; }
        if (s.has_mul) { r += "Mul"; }
        if (s.has_nonlinear_mul) {
            r += "(nl) ";
        } else if (s.has_mul) {
            r += "(const) ";
        }
        if (s.has_and) { r += "And "; }
        if (s.has_or) { r += "Or "; }
        if (s.has_xor) { r += "Xor "; }
        if (s.has_not) { r += "Not "; }
        if (s.has_shr) { r += "Shr "; }
        if (!r.empty() && r.back() == ' ') { r.pop_back(); }
        return r;
    }

    // Classify the "reconstruction family":
    //   - purely-bitwise: only And/Or/Xor/Not (CoB shadow)
    //   - semilinear: Add/Neg + bitwise, no nonlinear mul
    //   - polynomial: has nonlinear mul, no bitwise
    //   - mixed: has both nonlinear mul and bitwise
    std::string recon_family(const ReconShape &s) {
        bool has_arith = s.has_add || s.has_neg;
        bool has_bw    = s.has_and || s.has_or || s.has_xor || s.has_not;
        if (s.has_nonlinear_mul && has_bw) { return "mixed-poly"; }
        if (s.has_nonlinear_mul) { return "polynomial"; }
        if (has_arith && has_bw) { return "semilinear"; }
        if (has_bw) { return "purely-bitwise"; }
        if (has_arith) { return "linear"; }
        return "constant";
    }

    // Find a concrete failing input by probing
    struct Witness
    {
        std::vector< uint64_t > input;
        uint64_t original_val;
        uint64_t candidate_val;
    };

    Witness find_failing_witness(
        const Expr &original, const Expr &candidate,
        const std::vector< std::string > &orig_vars,
        const std::vector< std::string > &cand_vars, uint32_t bitwidth
    ) {
        Witness w;
        uint32_t nv = static_cast< uint32_t >(orig_vars.size());

        // Adversarial values
        std::vector< uint64_t > probes = {
            0x0000000000000001ULL,
            0x0000000000000002ULL,
            0x00000000000000FFULL,
            0x5555555555555555ULL,
            0xAAAAAAAAAAAAAAAAULL,
            0x0123456789ABCDEFULL,
            0xFEDCBA9876543210ULL,
            0xFFFFFFFFFFFFFFFFULL,
            0x8000000000000000ULL,
            0x7FFFFFFFFFFFFFFFULL,
            0x0000000100000001ULL,
            0x00FF00FF00FF00FFULL,
            3,
            7,
            13,
            42,
            100,
            255,
            1000,
            65535,
        };

        // Build var index mapping from candidate vars to original vars
        std::vector< uint32_t > cand_to_orig;
        for (const auto &cv : cand_vars) {
            for (uint32_t i = 0; i < orig_vars.size(); ++i) {
                if (orig_vars[i] == cv) {
                    cand_to_orig.push_back(i);
                    break;
                }
            }
        }

        uint64_t mask = (bitwidth == 64) ? UINT64_MAX : ((1ULL << bitwidth) - 1);

        for (const auto &probe : probes) {
            // All vars same value
            std::vector< uint64_t > orig_input(nv, probe);
            uint64_t ov = EvalExpr(original, orig_input, bitwidth);

            std::vector< uint64_t > cand_input(cand_vars.size());
            for (size_t i = 0; i < cand_vars.size(); ++i) {
                if (i < cand_to_orig.size()) { cand_input[i] = orig_input[cand_to_orig[i]]; }
            }
            uint64_t cv = EvalExpr(candidate, cand_input, bitwidth);

            if ((ov & mask) != (cv & mask)) {
                w.input         = orig_input;
                w.original_val  = ov & mask;
                w.candidate_val = cv & mask;
                return w;
            }

            // Try different values per variable
            for (uint32_t v = 0; v < nv; ++v) {
                std::vector< uint64_t > mixed_input(nv, probe);
                mixed_input[v] = probe ^ 0xDEADBEEFCAFEBABEULL;
                uint64_t ov2   = EvalExpr(original, mixed_input, bitwidth);

                std::vector< uint64_t > cand_mixed(cand_vars.size());
                for (size_t i = 0; i < cand_vars.size(); ++i) {
                    if (i < cand_to_orig.size()) {
                        cand_mixed[i] = mixed_input[cand_to_orig[i]];
                    }
                }
                uint64_t cv2 = EvalExpr(candidate, cand_mixed, bitwidth);

                if ((ov2 & mask) != (cv2 & mask)) {
                    w.input         = mixed_input;
                    w.original_val  = ov2 & mask;
                    w.candidate_val = cv2 & mask;
                    return w;
                }
            }
        }

        return w; // empty = no witness found
    }

    // Analyze one target expression
    void analyze_case(const std::string &path, int target_line, const std::string &label) {
        std::ifstream file(path);
        ASSERT_TRUE(file.is_open());

        std::string line;
        int line_num = 0;
        while (std::getline(file, line)) {
            ++line_num;
            if (line_num != target_line) { continue; }

            size_t sep = find_separator(line);
            ASSERT_NE(sep, std::string::npos);

            std::string obfuscated = trim(line.substr(0, sep));
            std::string gt_str     = trim(line.substr(sep + 1));

            auto parse_result = ParseAndEvaluate(obfuscated, 64);
            ASSERT_TRUE(parse_result.has_value());

            auto ast_result = ParseToAst(obfuscated, 64);
            ASSERT_TRUE(ast_result.has_value());

            auto gt_ast = ParseToAst(gt_str, 64);

            auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
                FoldConstantBitwise(std::move(ast_result.value().expr), 64)
            );

            auto cls  = ClassifyStructural(**folded_ptr);
            auto elim = EliminateAuxVars(parse_result.value().sig, parse_result.value().vars);

            std::cerr << "\n══════════════════════════════════════\n";
            std::cerr << label << ": L" << target_line << "\n";
            std::cerr << "══════════════════════════════════════\n";
            std::cerr << "Vars: " << elim.real_vars.size()
                      << " Semantic: " << semantic_str(cls.semantic) << " Flags: {"
                      << flag_str(cls.flags) << "}\n";
            std::cerr << "GT: " << gt_str << "\n";

            if (gt_ast.has_value()) {
                ReconShape gt_shape;
                collect_shape(*gt_ast.value().expr, 0, gt_shape);
                std::cerr << "GT shape: " << shape_summary(gt_shape) << " ("
                          << recon_family(gt_shape) << ", " << gt_shape.nodes << " nodes)\n";
            }

            // ── Run WITHOUT evaluator to get unverified CoB candidate ──
            Options opts_no_eval{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
            auto result_unverified = Simplify(
                parse_result.value().sig, parse_result.value().vars, folded_ptr->get(),
                opts_no_eval
            );
            ASSERT_TRUE(result_unverified.has_value());

            // ── Run WITH evaluator to confirm FW rejection ──
            Options opts_eval{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
            opts_eval.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
                return EvalExpr(**folded_ptr, v, 64);
            };
            auto result_verified = Simplify(
                parse_result.value().sig, parse_result.value().vars, folded_ptr->get(),
                opts_eval
            );
            ASSERT_TRUE(result_verified.has_value());

            bool unverified_simplified =
                result_unverified.value().kind == SimplifyOutcome::Kind::kSimplified;
            bool verified_simplified =
                result_verified.value().kind == SimplifyOutcome::Kind::kSimplified;

            std::cerr << "\nUnverified: "
                      << (unverified_simplified ? "SIMPLIFIED" : "unsupported")
                      << "  Verified: " << (verified_simplified ? "SIMPLIFIED" : "unsupported")
                      << "\n";

            if (unverified_simplified) {
                auto &cand_expr = *result_unverified.value().expr;
                auto &cand_vars = result_unverified.value().real_vars;
                auto rendered   = Render(cand_expr, cand_vars, 64);
                auto cost       = ComputeCost(cand_expr);

                ReconShape cand_shape;
                collect_shape(cand_expr, 0, cand_shape);

                std::cerr << "\nCoB candidate: " << rendered << "\n";
                std::cerr << "Candidate shape: " << shape_summary(cand_shape) << " ("
                          << recon_family(cand_shape) << ")\n";
                std::cerr << "Candidate cost: " << cost.cost.weighted_size
                          << " nodes=" << cand_shape.nodes << " depth=" << cand_shape.depth
                          << "\n";
                std::cerr << "Candidate vars: [";
                for (size_t i = 0; i < cand_vars.size(); ++i) {
                    if (i > 0) { std::cerr << ", "; }
                    std::cerr << cand_vars[i];
                }
                std::cerr << "]\n";

                // Check: does it match at Boolean inputs?
                std::cerr << "\nBoolean-domain check:\n";
                auto &vars   = parse_result.value().vars;
                uint32_t nv  = static_cast< uint32_t >(vars.size());
                bool bool_ok = true;
                for (uint32_t bits = 0; bits < (1U << nv); ++bits) {
                    std::vector< uint64_t > bools(nv);
                    for (uint32_t v = 0; v < nv; ++v) { bools[v] = (bits >> v) & 1; }
                    uint64_t orig_val = EvalExpr(**folded_ptr, bools, 64);

                    // Map to candidate vars
                    std::vector< uint64_t > cand_bools(cand_vars.size());
                    for (size_t i = 0; i < cand_vars.size(); ++i) {
                        for (uint32_t j = 0; j < vars.size(); ++j) {
                            if (vars[j] == cand_vars[i]) {
                                cand_bools[i] = bools[j];
                                break;
                            }
                        }
                    }
                    uint64_t cand_val = EvalExpr(cand_expr, cand_bools, 64);

                    if (orig_val != cand_val) {
                        std::cerr << "  MISMATCH at bools=" << bits << " orig=0x" << std::hex
                                  << orig_val << " cand=0x" << cand_val << std::dec << "\n";
                        bool_ok = false;
                    }
                }
                if (bool_ok) {
                    std::cerr << "  All " << (1U << nv)
                              << " Boolean points match (as expected)\n";
                }

                // Find a full-width failing witness
                std::cerr << "\nFull-width witness search:\n";
                auto witness =
                    find_failing_witness(**folded_ptr, cand_expr, vars, cand_vars, 64);
                if (!witness.input.empty()) {
                    std::cerr << "  DIVERGES at input=[";
                    for (size_t i = 0; i < witness.input.size(); ++i) {
                        if (i > 0) { std::cerr << ", "; }
                        std::cerr << "0x" << std::hex << witness.input[i];
                    }
                    std::cerr << std::dec << "]\n";
                    std::cerr << "  Original:  0x" << std::hex << witness.original_val
                              << std::dec << "\n";
                    std::cerr << "  Candidate: 0x" << std::hex << witness.candidate_val
                              << std::dec << "\n";
                    std::cerr << "  XOR diff:  0x" << std::hex
                              << (witness.original_val ^ witness.candidate_val) << std::dec
                              << "\n";
                } else {
                    std::cerr << "  No witness found in probe set\n";
                }

                // Structural comparison: is the candidate a "boolean shadow"?
                std::cerr << "\nDiagnostic: ";
                if (!cand_shape.has_nonlinear_mul && gt_ast.has_value()) {
                    ReconShape gt_s;
                    collect_shape(*gt_ast.value().expr, 0, gt_s);
                    if (gt_s.has_nonlinear_mul) {
                        std::cerr << "BOOLEAN SHADOW — candidate has no var*var mul "
                                  << "but GT requires it\n";
                    } else {
                        std::cerr << "GT also has no nonlinear mul\n";
                    }
                } else if (cand_shape.has_nonlinear_mul) {
                    std::cerr << "Candidate has nonlinear mul\n";
                } else {
                    std::cerr << "No GT AST for comparison\n";
                }
            } else {
                std::cerr << "\nNo unverified candidate surfaced through API\n";
                std::cerr << "Verified reason: " << result_verified.value().diag.reason << "\n";
            }

            // ── Direct CoB reconstruction ──────────────────────────
            // Replicate the CoB pipeline to see what it would produce.
            std::cerr << "\n── Direct CoB Reconstruction ──\n";
            {
                auto &vars        = parse_result.value().vars;
                auto elim2        = EliminateAuxVars(parse_result.value().sig, vars);
                auto &real_vars   = elim2.real_vars;
                auto &reduced_sig = elim2.reduced_sig;
                uint32_t nv       = static_cast< uint32_t >(real_vars.size());

                std::cerr << "Real vars: [";
                for (size_t i = 0; i < real_vars.size(); ++i) {
                    if (i > 0) { std::cerr << ", "; }
                    std::cerr << real_vars[i];
                }
                std::cerr << "] (" << nv << ")\n";

                // Signature vector
                std::cerr << "Signature (" << reduced_sig.size() << " entries): [";
                for (size_t i = 0; i < reduced_sig.size() && i < 16; ++i) {
                    if (i > 0) { std::cerr << ", "; }
                    std::cerr << "0x" << std::hex << reduced_sig[i] << std::dec;
                }
                if (reduced_sig.size() > 16) { std::cerr << ", ..."; }
                std::cerr << "]\n";

                // Interpolate coefficients
                auto coeffs = InterpolateCoefficients(reduced_sig, nv, 64);
                std::cerr << "CoB coefficients (" << coeffs.size() << "):\n";
                for (size_t i = 0; i < coeffs.size(); ++i) {
                    if (coeffs[i] == 0) { continue; }
                    std::cerr << "  c[" << i << "] = 0x" << std::hex << coeffs[i] << std::dec;
                    // Decode which AND-product this is
                    std::cerr << "  (";
                    if (i == 0) {
                        std::cerr << "constant";
                    } else {
                        bool first = true;
                        for (uint32_t b = 0; b < nv; ++b) {
                            if ((i >> b) & 1) {
                                if (!first) { std::cerr << "&"; }
                                std::cerr << real_vars[b];
                                first = false;
                            }
                        }
                    }
                    std::cerr << ")\n";
                }

                // Build CoB expression
                auto cob_expr     = BuildCobExpr(coeffs, nv, 64);
                auto cob_rendered = Render(*cob_expr, real_vars, 64);
                std::cerr << "\nCoB expr: " << cob_rendered << "\n";

                ReconShape cob_shape;
                collect_shape(*cob_expr, 0, cob_shape);
                std::cerr << "CoB shape: " << shape_summary(cob_shape) << " ("
                          << recon_family(cob_shape) << ", " << cob_shape.nodes << " nodes)\n";

                // Boolean check
                bool bool_ok = true;
                for (uint32_t bits = 0; bits < (1U << nv); ++bits) {
                    std::vector< uint64_t > bools(nv);
                    for (uint32_t v = 0; v < nv; ++v) { bools[v] = (bits >> v) & 1; }
                    // Evaluate original with full var list
                    std::vector< uint64_t > full_bools(vars.size(), 0);
                    for (size_t i = 0; i < real_vars.size(); ++i) {
                        for (size_t j = 0; j < vars.size(); ++j) {
                            if (vars[j] == real_vars[i]) {
                                full_bools[j] = bools[i];
                                break;
                            }
                        }
                    }
                    uint64_t orig = EvalExpr(**folded_ptr, full_bools, 64);
                    uint64_t cob  = EvalExpr(*cob_expr, bools, 64);
                    if (orig != cob) {
                        std::cerr << "  Boolean MISMATCH at " << bits << ": orig=0x" << std::hex
                                  << orig << " cob=0x" << cob << std::dec << "\n";
                        bool_ok = false;
                    }
                }
                if (bool_ok) {
                    std::cerr << "Boolean check: all " << (1U << nv) << " points match\n";
                }

                // Full-width divergence witness
                std::cerr << "\nFull-width witness:\n";
                auto w = find_failing_witness(**folded_ptr, *cob_expr, vars, real_vars, 64);
                if (!w.input.empty()) {
                    std::cerr << "  DIVERGES at [";
                    for (size_t i = 0; i < w.input.size(); ++i) {
                        if (i > 0) { std::cerr << ", "; }
                        std::cerr << "0x" << std::hex << w.input[i];
                    }
                    std::cerr << std::dec << "]\n";
                    std::cerr << "  Original:  0x" << std::hex << w.original_val << std::dec
                              << "\n";
                    std::cerr << "  CoB:       0x" << std::hex << w.candidate_val << std::dec
                              << "\n";
                    std::cerr << "  XOR diff:  0x" << std::hex
                              << (w.original_val ^ w.candidate_val) << std::dec << "\n";

                    // Diagnostic: which AND-product terms contribute
                    // to the mismatch?
                    std::cerr << "\n  Term-by-term at divergence point:\n";
                    // Map input to reduced vars
                    std::vector< uint64_t > red_input(nv);
                    for (size_t i = 0; i < real_vars.size(); ++i) {
                        for (size_t j = 0; j < vars.size(); ++j) {
                            if (vars[j] == real_vars[i]) {
                                red_input[i] = w.input[j];
                                break;
                            }
                        }
                    }
                    for (size_t i = 0; i < coeffs.size(); ++i) {
                        if (coeffs[i] == 0) { continue; }
                        // Compute AND-product value
                        uint64_t and_prod = UINT64_MAX;
                        for (uint32_t b = 0; b < nv; ++b) {
                            if ((i >> b) & 1) { and_prod &= red_input[b]; }
                        }
                        uint64_t term_val = (coeffs[i] * and_prod);
                        std::cerr << "    c[" << i << "]*AND=0x" << std::hex << term_val
                                  << std::dec;
                        // Show what the REAL product would be
                        if (std::popcount(static_cast< unsigned >(i)) >= 2) {
                            uint64_t real_prod = 1;
                            for (uint32_t b = 0; b < nv; ++b) {
                                if ((i >> b) & 1) { real_prod *= red_input[b]; }
                            }
                            uint64_t real_term = coeffs[i] * real_prod;
                            if (real_term != term_val) {
                                std::cerr << "  (MUL would be 0x" << std::hex << real_term
                                          << std::dec << " — DIVERGENT)";
                            }
                        }
                        std::cerr << "\n";
                    }
                } else {
                    std::cerr << "  No witness found\n";
                }
            }

            return;
        }

        FAIL() << "Line " << target_line << " not found in dataset";
    }

} // namespace

// ── Lifted outer analysis ───────────────────────────────────

namespace {

    // Forward-declare from first anonymous namespace (redeclared here)
    std::string category_str2(ReasonCategory cat) {
        switch (cat) {
            case ReasonCategory::kSearchExhausted:
                return "search-exhausted";
            case ReasonCategory::kVerifyFailed:
                return "verify-failed";
            case ReasonCategory::kRepresentationGap:
                return "representation-gap";
            case ReasonCategory::kNoSolution:
                return "no-solution";
            case ReasonCategory::kResourceLimit:
                return "resource-limit";
            case ReasonCategory::kGuardFailed:
                return "guard-failed";
            case ReasonCategory::kInapplicable:
                return "inapplicable";
            case ReasonCategory::kCostRejected:
                return "cost-rejected";
            case ReasonCategory::kInternalInvariant:
                return "internal";
            case ReasonCategory::kNone:
                return "none";
        }
        return "unknown";
    }

    bool IsBitwiseKind(Expr::Kind k) {
        return k == Expr::Kind::kAnd || k == Expr::Kind::kOr || k == Expr::Kind::kXor
            || k == Expr::Kind::kNot;
    }

    uint32_t CountNodes(const Expr &e) {
        uint32_t n = 1;
        for (const auto &c : e.children) { n += CountNodes(*c); }
        return n;
    }

    struct LiftRepeatEntry
    {
        size_t hash;
        std::string rendered;
        const Expr *first_occurrence;
        uint32_t count;
        uint32_t size;
        uint32_t first_preorder;
    };

    void CollectRepeats(
        const Expr &node, uint32_t &preorder, const std::vector< std::string > &vars,
        uint32_t bitwidth, std::unordered_map< size_t, std::vector< size_t > > &by_hash,
        std::vector< LiftRepeatEntry > &entries
    ) {
        uint32_t my_order = preorder++;
        bool is_leaf =
            (node.kind == Expr::Kind::kConstant || node.kind == Expr::Kind::kVariable);
        if (!is_leaf) {
            size_t h       = std::hash< Expr >{}(node);
            auto rendered  = Render(node, vars, bitwidth);
            auto node_size = CountNodes(node);

            bool found = false;
            auto hit   = by_hash.find(h);
            if (hit != by_hash.end()) {
                for (size_t idx : hit->second) {
                    if (entries[idx].rendered == rendered) {
                        entries[idx].count++;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                by_hash[h].push_back(entries.size());
                entries.push_back(
                    LiftRepeatEntry{
                        .hash             = h,
                        .rendered         = std::move(rendered),
                        .first_occurrence = &node,
                        .count            = 1,
                        .size             = node_size,
                        .first_preorder   = my_order,
                    }
                );
            }
        }
        for (const auto &child : node.children) {
            CollectRepeats(*child, preorder, vars, bitwidth, by_hash, entries);
        }
    }

    bool IsAncestorOf(const Expr *ancestor, const Expr *descendant) {
        if (ancestor == descendant) { return true; }
        for (const auto &c : ancestor->children) {
            if (IsAncestorOf(c.get(), descendant)) { return true; }
        }
        return false;
    }

    std::unique_ptr< Expr > ReplaceWithVirtual(
        const Expr &node, const std::vector< std::pair< size_t, std::string > > &targets,
        const std::vector< uint32_t > &virtual_indices, const std::vector< std::string > &vars,
        uint32_t bitwidth
    ) {
        if (node.kind != Expr::Kind::kConstant && node.kind != Expr::Kind::kVariable) {
            size_t h      = std::hash< Expr >{}(node);
            auto rendered = Render(node, vars, bitwidth);
            for (size_t i = 0; i < targets.size(); ++i) {
                if (targets[i].first == h && targets[i].second == rendered) {
                    return Expr::Variable(virtual_indices[i]);
                }
            }
        }

        auto result          = std::make_unique< Expr >();
        result->kind         = node.kind;
        result->constant_val = node.constant_val;
        result->var_index    = node.var_index;
        for (const auto &child : node.children) {
            result->children.push_back(
                ReplaceWithVirtual(*child, targets, virtual_indices, vars, bitwidth)
            );
        }
        return result;
    }

    // Replicate the budgeted lifting and show the outer expression
    void analyze_lifted(const std::string &path, int target_line, const std::string &label) {
        std::ifstream file(path);
        ASSERT_TRUE(file.is_open());

        std::string line;
        int line_num = 0;
        while (std::getline(file, line)) {
            ++line_num;
            if (line_num != target_line) { continue; }

            size_t sep = find_separator(line);
            ASSERT_NE(sep, std::string::npos);

            std::string obfuscated = trim(line.substr(0, sep));
            std::string gt_str     = trim(line.substr(sep + 1));

            auto parse_result = ParseAndEvaluate(obfuscated, 64);
            ASSERT_TRUE(parse_result.has_value());

            auto ast_result = ParseToAst(obfuscated, 64);
            ASSERT_TRUE(ast_result.has_value());

            auto folded      = FoldConstantBitwise(std::move(ast_result.value().expr), 64);
            const auto &vars = parse_result.value().vars;
            auto original_var_count = static_cast< uint32_t >(vars.size());

            std::cerr << "\n══════════════════════════════════════\n";
            std::cerr << label << ": L" << target_line << " (Lifted Outer Analysis)\n";
            std::cerr << "══════════════════════════════════════\n";
            std::cerr << "GT: " << gt_str << "\n";
            std::cerr << "Original: " << CountNodes(*folded) << " nodes, " << original_var_count
                      << " vars\n";

            // Collect repeated subtrees
            std::unordered_map< size_t, std::vector< size_t > > by_hash;
            std::vector< LiftRepeatEntry > entries;
            uint32_t preorder = 0;
            CollectRepeats(*folded, preorder, vars, 64, by_hash, entries);

            // Filter viable
            std::vector< LiftRepeatEntry * > viable;
            for (auto &entry : entries) {
                if (entry.count >= 2 && entry.size >= 4) { viable.push_back(&entry); }
            }

            // Sort by benefit
            std::sort(viable.begin(), viable.end(), [](const auto *a, const auto *b) {
                auto benefit_a = static_cast< uint64_t >(a->count - 1) * a->size;
                auto benefit_b = static_cast< uint64_t >(b->count - 1) * b->size;
                if (benefit_a != benefit_b) { return benefit_a > benefit_b; }
                if (a->size != b->size) { return a->size > b->size; }
                if (a->first_preorder != b->first_preorder) {
                    return a->first_preorder > b->first_preorder;
                }
                return a->count > b->count;
            });

            // Budgeted selection
            uint32_t var_budget = 16 > original_var_count ? 16 - original_var_count : 0;

            std::vector< const LiftRepeatEntry * > selected;
            for (const auto *cand : viable) {
                if (selected.size() >= var_budget) { break; }
                bool overlaps = false;
                for (const auto *sel : selected) {
                    if (IsAncestorOf(sel->first_occurrence, cand->first_occurrence)
                        || IsAncestorOf(cand->first_occurrence, sel->first_occurrence))
                    {
                        overlaps = true;
                        break;
                    }
                }
                if (!overlaps) { selected.push_back(cand); }
            }

            std::cerr << "Lifting: " << viable.size() << " viable, " << selected.size()
                      << " selected (budget=" << var_budget << ")\n";

            if (selected.empty()) {
                std::cerr << "No subtrees selected for lifting\n";
                return;
            }

            // Show selected subtrees
            for (size_t i = 0; i < selected.size(); ++i) {
                auto benefit = (selected[i]->count - 1) * selected[i]->size;
                std::cerr << "  r" << i << ": " << selected[i]->count << "x "
                          << selected[i]->size << "nodes benefit=" << benefit << " → "
                          << selected[i]->rendered.substr(0, 60) << "\n";
            }

            // Build replacement targets
            std::vector< std::pair< size_t, std::string > > targets;
            std::vector< uint32_t > virtual_indices;
            for (size_t i = 0; i < selected.size(); ++i) {
                targets.emplace_back(selected[i]->hash, selected[i]->rendered);
                virtual_indices.push_back(original_var_count + static_cast< uint32_t >(i));
            }

            // Build outer expression
            auto outer       = ReplaceWithVirtual(*folded, targets, virtual_indices, vars, 64);
            auto outer_nodes = CountNodes(*outer);

            // Build outer var list
            std::vector< std::string > outer_vars = vars;
            for (size_t i = 0; i < selected.size(); ++i) {
                outer_vars.push_back("r" + std::to_string(i));
            }

            auto outer_cls      = ClassifyStructural(*outer);
            auto outer_rendered = Render(*outer, outer_vars, 64);

            std::cerr << "\nLifted outer: " << outer_nodes << " nodes, " << outer_vars.size()
                      << " vars\n";
            std::cerr << "Outer classification: " << semantic_str(outer_cls.semantic)
                      << " flags={" << flag_str(outer_cls.flags) << "}\n";
            std::cerr << "Outer expr: " << outer_rendered.substr(0, 200) << "\n";

            // Analyze outer shape
            ReconShape outer_shape;
            collect_shape(*outer, 0, outer_shape);
            std::cerr << "Outer shape: " << shape_summary(outer_shape) << " ("
                      << recon_family(outer_shape) << ")\n";

            // CoB on the outer expression
            auto outer_nv   = static_cast< uint32_t >(outer_vars.size());
            auto outer_sig  = EvaluateBooleanSignature(*outer, outer_nv, 64);
            auto outer_elim = EliminateAuxVars(outer_sig, outer_vars);

            std::cerr << "\nOuter aux-var elimination: " << outer_elim.real_vars.size()
                      << " real vars from " << outer_nv << "\n";
            std::cerr << "Outer real vars: [";
            for (size_t i = 0; i < outer_elim.real_vars.size(); ++i) {
                if (i > 0) { std::cerr << ", "; }
                std::cerr << outer_elim.real_vars[i];
            }
            std::cerr << "]\n";

            auto outer_rnv    = static_cast< uint32_t >(outer_elim.real_vars.size());
            auto outer_coeffs = InterpolateCoefficients(outer_elim.reduced_sig, outer_rnv, 64);

            // Count nonlinear CoB terms
            int linear_terms    = 0;
            int nonlinear_terms = 0;
            for (size_t i = 1; i < outer_coeffs.size(); ++i) {
                if (outer_coeffs[i] == 0) { continue; }
                auto pop = std::popcount(static_cast< unsigned >(i));
                if (pop == 1) {
                    linear_terms++;
                } else {
                    nonlinear_terms++;
                }
            }
            std::cerr << "Outer CoB: " << linear_terms << " linear terms, " << nonlinear_terms
                      << " nonlinear AND terms";
            if (outer_coeffs[0] != 0) {
                std::cerr << ", constant=0x" << std::hex << outer_coeffs[0] << std::dec;
            }
            std::cerr << "\n";

            // Show nonlinear terms
            if (nonlinear_terms > 0) {
                std::cerr << "Nonlinear CoB terms:\n";
                for (size_t i = 1; i < outer_coeffs.size(); ++i) {
                    if (outer_coeffs[i] == 0) { continue; }
                    if (std::popcount(static_cast< unsigned >(i)) < 2) { continue; }
                    std::cerr << "  c[" << i << "] = 0x" << std::hex << outer_coeffs[i]
                              << std::dec << "  (";
                    bool first = true;
                    for (uint32_t b = 0; b < outer_rnv; ++b) {
                        if ((i >> b) & 1) {
                            if (!first) { std::cerr << "&"; }
                            std::cerr << outer_elim.real_vars[b];
                            first = false;
                        }
                    }
                    std::cerr << ")\n";
                }
            }

            // Try Simplify on the outer expression alone
            std::cerr << "\n── Simplify outer expression directly ──\n";
            auto outer_folded = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*outer));
            Options outer_opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
            outer_opts.evaluator = [outer_folded](
                                       const std::vector< uint64_t > &v
                                   ) -> uint64_t { return EvalExpr(**outer_folded, v, 64); };

            auto outer_result =
                Simplify(outer_sig, outer_vars, outer_folded->get(), outer_opts);
            if (outer_result.has_value()) {
                if (outer_result.value().kind == SimplifyOutcome::Kind::kSimplified) {
                    auto simplified_rendered =
                        Render(*outer_result.value().expr, outer_result.value().real_vars, 64);
                    std::cerr << "Outer SIMPLIFIED: " << simplified_rendered << "\n";
                    std::cerr << "Verified: " << (outer_result.value().verified ? "yes" : "no")
                              << "\n";
                } else {
                    std::cerr << "Outer UNSUPPORTED\n";
                    std::cerr << "Reason: " << outer_result.value().diag.reason << "\n";
                    if (outer_result.value().diag.reason_code.has_value()) {
                        std::cerr
                            << "Category: "
                            << category_str2(outer_result.value().diag.reason_code->category)
                            << "\n";
                    }
                    std::cerr << "Expansions: "
                              << outer_result.value().telemetry.total_expansions
                              << " verified: "
                              << outer_result.value().telemetry.candidates_verified << "\n";
                }
            }

            return;
        }

        FAIL() << "Line " << target_line << " not found";
    }

} // namespace

TEST(LiftedOuterAnalysis, L58) {
    analyze_lifted(DATASET_DIR "/gamba/qsynth_ea.txt", 58, "Case 1");
}

TEST(LiftedOuterAnalysis, L126) {
    analyze_lifted(DATASET_DIR "/gamba/qsynth_ea.txt", 126, "Case 2");
}

TEST(LiftedOuterAnalysis, L283) {
    analyze_lifted(DATASET_DIR "/gamba/qsynth_ea.txt", 283, "Case 3");
}

TEST(LiftedOuterAnalysis, L307) {
    analyze_lifted(DATASET_DIR "/gamba/qsynth_ea.txt", 307, "Case 4");
}

TEST(VerifyFailedForensic, L58) {
    analyze_case(DATASET_DIR "/gamba/qsynth_ea.txt", 58, "Case 1");
}

TEST(VerifyFailedForensic, L126) {
    analyze_case(DATASET_DIR "/gamba/qsynth_ea.txt", 126, "Case 2");
}

TEST(VerifyFailedForensic, L283) {
    analyze_case(DATASET_DIR "/gamba/qsynth_ea.txt", 283, "Case 3");
}

TEST(VerifyFailedForensic, L307) {
    analyze_case(DATASET_DIR "/gamba/qsynth_ea.txt", 307, "Case 4");
}
