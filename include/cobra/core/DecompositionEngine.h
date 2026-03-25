#pragma once

#include "cobra/core/Classification.h"
#include "cobra/core/Expr.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/Simplifier.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cobra {

    enum class ExtractorKind : uint8_t {
        kProductAST,
        kPolynomial,
        kTemplate,
        kBooleanNullDirect,
    };

    enum class ResidualSolverKind : uint8_t {
        kSupportedPipeline,
        kPolynomialRecovery,
        kGhostResidual,
        kTemplateDecomposition,
    };

    struct DecompositionContext
    {
        const Options &opts;
        const std::vector< std::string > &vars;
        const std::vector< uint64_t > &sig;
        const Expr *current_expr;
        const Classification &cls;
    };

    struct CoreCandidate
    {
        std::unique_ptr< Expr > expr;
        ExtractorKind kind;
        uint8_t degree_used = 0;
    };

    struct DecompositionResult
    {
        std::unique_ptr< Expr > expr;
        ExtractorKind extractor_kind;
        std::optional< ResidualSolverKind > solver_kind;
        uint8_t core_degree = 0;
    };

    // Core extractor: product/AST. Collects Mul(non-const, non-const) terms.
    SolverResult< CoreCandidate > ExtractProductCore(const DecompositionContext &ctx);

    // Core extractor: polynomial recovery at a specific degree.
    SolverResult< CoreCandidate >
    ExtractPolyCore(const DecompositionContext &ctx, uint8_t degree);

    // Core acceptance screen for polynomial extractors.
    bool AcceptCore(const DecompositionContext &ctx, const CoreCandidate &core);

    // Core extractor: template decomposition with reduced/full var fallback.
    SolverResult< CoreCandidate > ExtractTemplateCore(const DecompositionContext &ctx);

    // Build a residual evaluator: r(x) = (f(x) - EvalExpr(core, x, bw)) & mask.
    // The core expression is cloned internally.
    Evaluator
    BuildResidualEvaluator(const Evaluator &original, const Expr &core, uint32_t bitwidth);

} // namespace cobra
