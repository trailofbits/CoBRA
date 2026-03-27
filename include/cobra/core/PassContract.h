#pragma once

#include "cobra/core/Classification.h"
#include "cobra/core/Expr.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cobra {

    enum class OutcomeKind {
        kSuccess,
        kInapplicable,
        kBlocked,
        kPartial,
        kVerifyFailed,
    };

    enum class ReasonCategory {
        kNone,
        kGuardFailed,
        kInapplicable,
        kRepresentationGap,
        kNoSolution,
        kSearchExhausted,
        kVerifyFailed,
        kResourceLimit,
        kCostRejected,
        kInternalInvariant,
    };

    enum class ReasonDomain {
        kOrchestrator,
        kSemilinear,
        kSignature,
        kStructuralTransform,
        kDecomposition,
        kTemplateDecomposer,
        kWeightedPolyFit,
        kMultivarPoly,
        kPolynomialRecovery,
        kBitwiseDecomposer,
        kHybridDecomposer,
        kGhostResidual,
        kOperandSimplifier,
        kLifting,
        kVerifier,
    };

    struct ReasonCode
    {
        ReasonCategory category = ReasonCategory::kNone;
        ReasonDomain domain     = ReasonDomain::kOrchestrator;
        uint16_t subcode        = 0;
    };

    struct DiagField
    {
        std::string key;
        std::string value;
    };

    struct ReasonFrame
    {
        ReasonCode code;
        std::string message;
        std::vector< DiagField > fields;
    };

    struct ReasonDetail
    {
        ReasonFrame top;
        std::vector< ReasonFrame > causes;
    };

    // ---------------------------------------------------------------
    // SolverResult<T> — non-fatal solver outcome with optional payload
    // ---------------------------------------------------------------

    template< typename T >
    class [[nodiscard]] SolverResult
    {
      public:
        SolverResult() = delete;

        static SolverResult Success(T payload) {
            SolverResult r(OutcomeKind::kSuccess);
            r.payload_.emplace(std::move(payload));
            return r;
        }

        static SolverResult Inapplicable(ReasonDetail reason) {
            SolverResult r(OutcomeKind::kInapplicable);
            r.reason_.emplace(std::move(reason));
            return r;
        }

        static SolverResult Blocked(ReasonDetail reason) {
            SolverResult r(OutcomeKind::kBlocked);
            r.reason_.emplace(std::move(reason));
            return r;
        }

        static SolverResult VerifyFailed(T payload, ReasonDetail reason) {
            SolverResult r(OutcomeKind::kVerifyFailed);
            r.payload_.emplace(std::move(payload));
            r.reason_.emplace(std::move(reason));
            return r;
        }

        OutcomeKind Kind() const { return kind_; }

        bool Succeeded() const { return kind_ == OutcomeKind::kSuccess; }

        const T &Payload() const {
            assert(kind_ == OutcomeKind::kSuccess || kind_ == OutcomeKind::kVerifyFailed);
            return *payload_;
        }

        T TakePayload() {
            assert(kind_ == OutcomeKind::kSuccess || kind_ == OutcomeKind::kVerifyFailed);
            return std::move(*payload_);
        }

        const ReasonDetail &Reason() const {
            assert(kind_ != OutcomeKind::kSuccess);
            return *reason_;
        }

      private:
        explicit SolverResult(OutcomeKind kind) : kind_(kind) {}

        OutcomeKind kind_;
        std::optional< T > payload_;
        std::optional< ReasonDetail > reason_;
    };

    // ---------------------------------------------------------------
    // DecompositionMeta — set via SetDecompositionMeta() when
    // Decomposition extractor/solver passes produce a result.
    // ---------------------------------------------------------------

    struct DecompositionMeta
    {
        uint8_t extractor_kind = 0; // Cast of ExtractorKind
        uint8_t solver_kind    = 0; // Cast of ResidualSolverKind (0 = none)
        bool has_solver        = false;
        uint8_t core_degree    = 0;
    };

    // ---------------------------------------------------------------
    // PassOutcome — concrete outcome for simplification passes
    // ---------------------------------------------------------------

    enum class VerificationState {
        kUnverified,
        kVerified,
        kRejected,
    };

    struct PendingWork
    {
        std::unique_ptr< Expr > residual;
        Classification residual_classification;
    };

    class [[nodiscard]] PassOutcome
    {
      public:
        PassOutcome() = delete;

        static PassOutcome Success(
            std::unique_ptr< Expr > expr, std::vector< std::string > real_vars,
            VerificationState verification
        );

        static PassOutcome Inapplicable(ReasonDetail reason);
        static PassOutcome Blocked(ReasonDetail reason);

        static PassOutcome Partial(
            std::unique_ptr< Expr > expr, std::vector< std::string > real_vars,
            VerificationState verification, PendingWork pending, ReasonDetail reason
        );

        static PassOutcome VerifyFailed(
            std::unique_ptr< Expr > expr, std::vector< std::string > real_vars,
            ReasonDetail reason
        );

        OutcomeKind Kind() const { return kind_; }

        bool Succeeded() const { return kind_ == OutcomeKind::kSuccess; }

        const Expr &GetExpr() const;
        std::unique_ptr< Expr > TakeExpr();

        const std::vector< std::string > &RealVars() const;
        VerificationState Verification() const;

        const ReasonDetail &Reason() const;
        const PendingWork &Pending() const;

        const std::vector< uint64_t > &SigVector() const;
        void SetSigVector(std::vector< uint64_t > sv);

        const std::optional< DecompositionMeta > &DecompositionMetadata() const;
        void SetDecompositionMeta(DecompositionMeta meta);

      private:
        explicit PassOutcome(OutcomeKind kind);

        OutcomeKind kind_;
        std::unique_ptr< Expr > expr_;
        std::vector< std::string > real_vars_;
        VerificationState verification_ = VerificationState::kUnverified;
        std::optional< ReasonDetail > reason_;
        std::optional< PendingWork > pending_;
        std::vector< uint64_t > sig_vector_;
        std::optional< DecompositionMeta > decomposition_meta_;
    };

} // namespace cobra
