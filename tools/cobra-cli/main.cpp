#include "ExprParser.h"
#include "cobra/core/AtomSimplifier.h"
#include "cobra/core/BitPartitioner.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/MaskedAtomReconstructor.h"
#include "cobra/core/Profile.h"
#include "cobra/core/SelfCheck.h"
#include "cobra/core/SemilinearIR.h"
#include "cobra/core/SemilinearNormalizer.h"
#include "cobra/core/SemilinearSignature.h"
#include "cobra/core/SignatureChecker.h" // FullWidthCheck
#include "cobra/core/Simplifier.h"
#include "cobra/core/StructureRecovery.h"
#include "cobra/core/TermRefiner.h"
#include "cobra/core/Trace.h"
#include <cstdint>
#include <utility>
#ifdef COBRA_HAS_Z3
    #include "cobra/core/CoeffInterpolator.h"
    #include "cobra/verify/Z3Verifier.h"
#endif
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

    void PrintUsage() {
        std::cerr << "Usage: cobra-cli [options]\n"
                  << "\nOptions:\n"
                  << "  --mba <expr>    Simplify an MBA expression\n"
                  << "  --bitwidth <n>  Bit width (default: 64)\n"
                  << "  --max-vars <n>  Max variable count (default: 16)\n"
                  << "  --verify        Enable Z3 verification\n"
                  << "  --strict        Require Z3 for semilinear results\n"
                  << "  --verbose       Print intermediate steps\n"
                  << "  --version       Print version\n"
                  << "  --help          Show this message\n";
    }

    void PrintVersion() { std::cout << "cobra-cli 0.1.0\n"; }

    std::vector< uint64_t >
    EvaluateToSignature(const cobra::Expr &ast, uint32_t num_vars, uint32_t bitwidth) {
        const size_t kLen = size_t{ 1 } << num_vars;
        std::vector< uint64_t > sig(kLen);
        for (size_t i = 0; i < kLen; ++i) {
            std::vector< uint64_t > var_values(num_vars);
            for (uint32_t v = 0; v < num_vars; ++v) { var_values[v] = (i >> v) & 1; }
            sig[i] = cobra::EvalExpr(ast, var_values, bitwidth);
        }
        return sig;
    }

    const char *SemanticClassName(cobra::SemanticClass cls) {
        switch (cls) {
            case cobra::SemanticClass::kLinear:
                return "kLinear";
            case cobra::SemanticClass::kSemilinear:
                return "kSemilinear";
            case cobra::SemanticClass::kPolynomial:
                return "kPolynomial";
            case cobra::SemanticClass::kNonPolynomial:
                return "kNonPolynomial";
        }
        return "Unknown";
    }

    void PrintStructuralFlags(cobra::StructuralFlag flags) {
        bool first = true;
        auto emit  = [&](const char *name) {
            if (!first) { std::cerr << ", "; }
            std::cerr << name;
            first = false;
        };
        if ((flags & cobra::kSfHasBitwise) != 0u) { emit("bitwise"); }
        if ((flags & cobra::kSfHasArithmetic) != 0u) { emit("arithmetic"); }
        if ((flags & cobra::kSfHasMul) != 0u) { emit("mul"); }
        if ((flags & cobra::kSfHasMultilinearProduct) != 0u) { emit("multilinear-product"); }
        if ((flags & cobra::kSfHasSingletonPower) != 0u) { emit("singleton-power"); }
        if ((flags & cobra::kSfHasSingletonPowerGt2) != 0u) { emit("singleton-power-gt2"); }
        if ((flags & cobra::kSfHasMixedProduct) != 0u) { emit("mixed-product"); }
        if ((flags & cobra::kSfHasBitwiseOverArith) != 0u) { emit("bitwise-over-arith"); }
        if ((flags & cobra::kSfHasArithOverBitwise) != 0u) { emit("arith-over-bitwise"); }
        if ((flags & cobra::kSfHasMultivarHighPower) != 0u) { emit("multivar-high-power"); }
        if ((flags & cobra::kSfHasUnknownShape) != 0u) { emit("unknown-shape"); }
        if (first) { std::cerr << "none"; }
    }

    int RunLinearPath(
        const cobra::Expr &ast, const std::vector< std::string > &vars, uint32_t bitwidth,
        uint32_t max_vars, bool verbose, bool verify, const cobra::Expr *original_ast = nullptr
    ) {
        auto num_vars = static_cast< uint32_t >(vars.size());
        COBRA_TRACE(
            "CLI", "RunLinearPath: vars={} bitwidth={} has_original={}", num_vars, bitwidth,
            original_ast != nullptr
        );
        if (verbose) { std::cerr << "Evaluating signature vector...\n"; }
        auto sig = EvaluateToSignature(ast, num_vars, bitwidth);
        COBRA_TRACE_SIG("CLI", "signature vector", sig);

        if (verbose) {
            std::cerr << "Signature vector (" << num_vars << " vars): [";
            for (size_t i = 0; i < sig.size(); ++i) {
                if (i > 0) { std::cerr << ", "; }
                std::cerr << sig[i];
            }
            std::cerr << "]\n";
        }

        cobra::Options opts{ .bitwidth = bitwidth, .max_vars = max_vars, .spot_check = true };

        if (original_ast != nullptr) {
            opts.evaluator = cobra::Evaluator::FromExpr(
                *original_ast, bitwidth, cobra::EvaluatorTraceKind::kCliOriginalAst
            );
        }

        if (verbose) { std::cerr << "Simplifying...\n"; }
        COBRA_TRACE("CLI", "Simplify: calling core simplifier");
        auto result = cobra::Simplify(sig, vars, original_ast, opts);
        COBRA_TRACE("CLI", "Simplify: returned has_value={}", result.has_value());
        if (!result.has_value()) {
            std::cerr << "Error: " << result.error().message << "\n";
            return 1;
        }
        if (result.value().kind == cobra::SimplifyOutcome::Kind::kError) {
            std::cerr << "Error: " << result.value().diag.reason << "\n";
            return 1;
        }
        if (result.value().kind == cobra::SimplifyOutcome::Kind::kUnchangedUnsupported) {
            std::cerr << "Unsupported: " << result.value().diag.reason << "\n";
            if (verbose) {
                std::cerr << "  Flags: ";
                PrintStructuralFlags(result.value().diag.classification.flags);
                std::cerr << ", rewrite rounds: "
                          << result.value().diag.structural_transform_rounds << "\n";
            }
            auto text = cobra::Render(*result.value().expr, vars, bitwidth);
            std::cout << text << "\n";
            return 0;
        }

        if (verbose && result.value().real_vars.size() < vars.size()) {
            auto eliminated = vars.size() - result.value().real_vars.size();
            std::cerr << "Eliminated " << eliminated << " spurious variable"
                      << (eliminated != 1 ? "s" : "") << " (";
            bool first = true; // NOLINT(misc-const-correctness)
            for (const auto &v : vars) {
                bool found = false;
                for (const auto &rv : result.value().real_vars) {
                    if (v == rv) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    if (!first) { std::cerr << ", "; }
                    std::cerr << v;
                    first = false;
                }
            }
            std::cerr << "), reduced to " << result.value().real_vars.size() << " var"
                      << (result.value().real_vars.size() != 1 ? "s" : "") << ": ";
            for (size_t i = 0; i < result.value().real_vars.size(); ++i) {
                if (i > 0) { std::cerr << ", "; }
                std::cerr << result.value().real_vars[i];
            }
            std::cerr << "\n";
        }

        if (verbose && result.value().verified) {
            std::cerr << "Running spot-check... passed\n";
        }

        // Full-width verification for polynomial expressions:
        // The CoB result is {0,1}-correct by construction. For polynomial
        // inputs, verify it also holds at random full-width values.
        if (original_ast != nullptr) {
            // Build mapping from simplified var indices to original var
            // indices (needed when aux var elimination reduced the set).
            std::vector< uint32_t > var_map;
            if (result.value().real_vars.size() < vars.size()) {
                var_map.reserve(result.value().real_vars.size());
                for (const auto &rv : result.value().real_vars) {
                    for (uint32_t j = 0; j < vars.size(); ++j) {
                        if (vars[j] == rv) {
                            var_map.push_back(j);
                            break;
                        }
                    }
                }
            }

            auto fw = cobra::FullWidthCheck(
                *original_ast, num_vars, *result.value().expr, var_map, bitwidth
            );
            COBRA_TRACE("CLI", "FullWidthCheck: passed={}", fw.passed);
            if (!fw.passed) {
                COBRA_TRACE_SIG("CLI", "  failing_input", fw.failing_input);
                if (verbose) {
                    std::cerr << "Verifying full-width equivalence..."
                                 " failed\n";
                    std::cerr << "  Failing inputs:";
                    for (auto v : fw.failing_input) { std::cerr << " " << v; }
                    std::cerr << "\n";
                }
                std::cerr << "Error: CoB result is only correct on "
                             "{0,1} inputs (polynomial target)\n";
                return 1;
            }
            if (verbose) {
                std::cerr << "Verifying full-width equivalence..."
                             " passed\n";
            }
        }

        auto text = cobra::Render(*result.value().expr, result.value().real_vars, bitwidth);
        std::cout << text << "\n";

#ifdef COBRA_HAS_Z3
        if (verify) {
            auto rv = static_cast< uint32_t >(result.value().real_vars.size());
            auto cob_coeffs =
                cobra::InterpolateCoefficients(result.value().sig_vector, rv, bitwidth);
            auto z3r = cobra::Z3Verify(
                cob_coeffs, *result.value().expr, result.value().real_vars, rv, bitwidth
            );
            if (z3r.equivalent) {
                std::cerr << "[Z3] Verified: equivalent\n";
            } else {
                std::cerr << "[Z3] Verification failed: " << z3r.counterexample << "\n";
                return 1;
            }
        }
#else
        if (verify) { std::cerr << "Warning: Z3 not available, --verify ignored\n"; }
#endif

        return 0;
    }

    int RunSemilinearPath(
        const cobra::Expr &original_ast, const std::vector< std::string > &vars,
        uint32_t bitwidth, uint32_t max_vars, bool verbose, bool strict
    ) {
        (void) strict;
        if (vars.size() > max_vars) {
            std::cerr << "Error: expression has " << vars.size() << " variables (max "
                      << max_vars << ")\n";
            return 1;
        }

        COBRA_TRACE("CLI", "RunSemilinearPath: vars={} bitwidth={}", vars.size(), bitwidth);
        if (verbose) { std::cerr << "Normalizing to semilinear IR...\n"; }
        auto ir_result = cobra::NormalizeToSemilinear(original_ast, vars, bitwidth);
        if (!ir_result.has_value()) {
            std::cerr << "Error: " << ir_result.error().message << "\n";
            return 1;
        }
        auto &ir = ir_result.value();

        if (verbose) {
            std::cerr << "kSemilinear IR: " << ir.terms.size() << " terms, "
                      << ir.atom_table.size() << " atoms\n";
        }

        if (verbose) { std::cerr << "Simplifying atom structure...\n"; }
        cobra::SimplifyStructure(ir);

        if (verbose) {
            std::cerr << "After simplification: " << ir.terms.size() << " terms, "
                      << ir.atom_table.size() << " atoms\n";
        }

        // Self-check uses plain reconstruction (no OR-rewrite)
        // to ensure structural round-trip fidelity.
        auto plain = cobra::ReconstructMaskedAtoms(ir, {});
        auto check = cobra::SelfCheckSemilinear(ir, *plain, vars, bitwidth);
        COBRA_TRACE("CLI", "SelfCheck: passed={}", check.passed);
        if (!check.passed) {
            if (verbose) { std::cerr << "Running self-check... failed\n"; }
            std::cerr << "Error: self-check failed: " << check.mismatch_detail << "\n";
            return 1;
        }

        if (verbose) {
            std::cerr << "Running self-check... passed\n";
            std::cerr << "Recovering structure...\n";
        }

        cobra::RecoverStructure(ir);

        if (verbose) {
            std::cerr << "After structure recovery: " << ir.terms.size() << " terms\n";
            std::cerr << "Refining terms...\n";
        }

        cobra::RefineTerms(ir);

        if (verbose) {
            std::cerr << "After refinement: " << ir.terms.size() << " terms\n";
            std::cerr << "Coalescing terms...\n";
        }

        cobra::CoalesceTerms(ir);

        if (verbose) { std::cerr << "After coalescing: " << ir.terms.size() << " terms\n"; }

        // Post-rewrite probe check: evaluate original and reconstructed
        // expressions at random full-width inputs to catch rewrite bugs.
        // Works without Z3; uses 16 samples for high confidence.
        {
            auto probe_expr = cobra::ReconstructMaskedAtoms(ir, {});
            auto num_vars   = static_cast< uint32_t >(vars.size());
            auto probe =
                cobra::FullWidthCheck(original_ast, num_vars, *probe_expr, {}, bitwidth, 16);
            COBRA_TRACE("CLI", "PostRewriteProbe: passed={}", probe.passed);
            if (!probe.passed) {
                if (verbose) { std::cerr << "Running post-rewrite probe check... failed\n"; }
                std::cerr << "Error: post-rewrite equivalence probe failed\n";
                return 1;
            }
            if (verbose) { std::cerr << "Running post-rewrite probe check... passed\n"; }
        }

        // Remove dead intermediate atoms before partitioning.
        cobra::CompactAtomTable(ir);

        if (verbose) { std::cerr << "Computing bit partitions...\n"; }

        auto partitions = cobra::ComputePartitions(ir);

        if (verbose) { std::cerr << "Bit partitions: " << partitions.size() << "\n"; }

        if (verbose) { std::cerr << "Reconstructing with OR-rewrite...\n"; }

        // Final reconstruction uses partitions for OR-rewrite.
        auto simplified = cobra::ReconstructMaskedAtoms(ir, partitions);

        auto text = cobra::Render(*simplified, vars, bitwidth);
        std::cout << text << "\n";

#ifdef COBRA_HAS_Z3
        // Always verify semilinear results when Z3 is available
        auto z3r = cobra::Z3VerifyExprs(original_ast, *simplified, vars, bitwidth);
        if (z3r.equivalent) {
            if (verbose) { std::cerr << "[Z3] Verified: equivalent\n"; }
        } else {
            std::cerr << "[Z3] Verification failed: " << z3r.counterexample << "\n";
            return 1;
        }
#else
        if (strict) {
            std::cerr << "Error: --strict requires Z3 for semilinear verification\n";
            return 1;
        }
        std::cerr << "Warning: semilinear result unverified (Z3 not available)\n";
#endif

        return 0;
    }

} // namespace

int main(int argc, char *argv[]) {
    std::string mba_expr;
    uint32_t bitwidth = 64;
    uint32_t max_vars = 16;
    bool verbose      = false;
    bool verify       = false;
    bool strict       = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
            PrintUsage();
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0) {
            PrintVersion();
            return 0;
        }
        if (std::strcmp(argv[i], "--mba") == 0 && i + 1 < argc) {
            mba_expr = argv[++i];
        } else if (std::strcmp(argv[i], "--bitwidth") == 0 && i + 1 < argc) {
            bitwidth = static_cast< uint32_t >(std::stoul(argv[++i]));
        } else if (std::strcmp(argv[i], "--max-vars") == 0 && i + 1 < argc) {
            max_vars = static_cast< uint32_t >(std::stoul(argv[++i]));
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "--verify") == 0) {
            verify = true;
        } else if (std::strcmp(argv[i], "--strict") == 0) {
            strict = true;
        } else {
            std::cerr << "Error: unknown option '" << argv[i] << "'\n";
            PrintUsage();
            return 1;
        }
    }

    if (mba_expr.empty()) {
        std::cerr << "Error: --mba <expr> is required\n";
        PrintUsage();
        return 1;
    }

    // Parse to AST
    COBRA_TRACE("CLI", "ParseToAst: input=\"{}\" bitwidth={}", mba_expr, bitwidth);
    auto parsed = cobra::ParseToAst(mba_expr, bitwidth);
    if (!parsed.has_value()) {
        std::cerr << "Error: " << parsed.error().message << "\n";
        return 1;
    }

    auto &vars = parsed.value().vars;

    COBRA_TRACE("CLI", "Variables: count={}", vars.size());
    for (size_t i = 0; i < vars.size(); ++i) { COBRA_TRACE("CLI", "  var[{}]={}", i, vars[i]); }

    if (verbose) {
        std::cerr << "Variables: ";
        for (size_t i = 0; i < vars.size(); ++i) {
            if (i > 0) { std::cerr << ", "; }
            std::cerr << vars[i];
        }
        std::cerr << "\n";
    }

    // Fold constant bitwise subtrees
    auto folded = cobra::FoldConstantBitwise(std::move(parsed.value().expr), bitwidth);
    COBRA_TRACE_EXPR("CLI", "after FoldConstantBitwise", *folded, vars, bitwidth);

    auto cls = cobra::ClassifyStructural(*folded);
    COBRA_TRACE(
        "CLI", "ClassifyStructural: semantic={} flags=0x{:x}", SemanticClassName(cls.semantic),
        static_cast< uint32_t >(cls.flags)
    );

    if (verbose) {
        std::cerr << "Classification: " << SemanticClassName(cls.semantic) << "\n";
        std::cerr << "  Flags: ";
        PrintStructuralFlags(cls.flags);
        std::cerr << "\n";
    }

    switch (cls.semantic) {
        case cobra::SemanticClass::kLinear:
            return RunLinearPath(
                *folded, vars, bitwidth, max_vars, verbose, verify, folded.get()
            );

        case cobra::SemanticClass::kSemilinear: {
            // Linear shortcut: if all semi-linear signature rows are
            // identical, the expression is actually linear.
            if (vars.size() <= 12
                && cobra::IsLinearShortcut(
                    *folded, static_cast< uint32_t >(vars.size()), bitwidth
                ))
            {
                if (verbose) {
                    std::cerr << "Linear shortcut: expression is linear in disguise\n";
                }
                // Semilinear-classified: apply semilinear verification contract.
                bool shortcut_verify = verify;
#ifdef COBRA_HAS_Z3
                shortcut_verify = true;
#endif
                const int kRc = RunLinearPath(
                    *folded, vars, bitwidth, max_vars, verbose, shortcut_verify, folded.get()
                );
                if (kRc != 0) { return kRc; }
#ifndef COBRA_HAS_Z3
                if (strict) {
                    std::cerr << "Error: --strict requires Z3 for semilinear verification\n";
                    return 1;
                }
                std::cerr << "Warning: semilinear result unverified (Z3 not available)\n";
#endif
                return 0;
            }

            const int kRc =
                RunSemilinearPath(*folded, vars, bitwidth, max_vars, verbose, strict);
            if (kRc != 0) {
                if (verbose) {
                    std::cerr << "kSemilinear path failed, "
                                 "falling back to linear\n";
                }
                return RunLinearPath(
                    *folded, vars, bitwidth, max_vars, verbose, verify, folded.get()
                );
            }
            return kRc;
        }

        case cobra::SemanticClass::kPolynomial:
        case cobra::SemanticClass::kNonPolynomial:
            return RunLinearPath(
                *folded, vars, bitwidth, max_vars, verbose, verify, folded.get()
            );
    }
}
