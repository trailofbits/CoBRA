#include "cobra/core/PassContract.h"

#include <cstdlib>
#include <utility>

namespace cobra {

    namespace {
        // Replaces assert() with a release-build runtime check. assert() is
        // compiled out under -DNDEBUG, so a violation in release would
        // dereference an empty optional / null unique_ptr → UB. std::abort
        // matches the death-test contract while failing loud in either
        // build mode.
        [[noreturn]] void ContractAbort() { std::abort(); }
    } // namespace

    PassOutcome::PassOutcome(OutcomeKind kind) : kind_(kind) {}

    PassOutcome PassOutcome::Success(
        std::unique_ptr< Expr > expr, std::vector< std::string > real_vars,
        VerificationState verification
    ) {
        PassOutcome o(OutcomeKind::kSuccess);
        o.expr_         = std::move(expr);
        o.real_vars_    = std::move(real_vars);
        o.verification_ = verification;
        return o;
    }

    PassOutcome PassOutcome::Blocked(ReasonDetail reason) {
        PassOutcome o(OutcomeKind::kBlocked);
        o.reason_.emplace(std::move(reason));
        return o;
    }

    const Expr &PassOutcome::GetExpr() const {
        if (kind_ != OutcomeKind::kSuccess) { ContractAbort(); }
        return *expr_;
    }

    std::unique_ptr< Expr > PassOutcome::TakeExpr() {
        if (kind_ != OutcomeKind::kSuccess) { ContractAbort(); }
        return std::move(expr_);
    }

    const std::vector< std::string > &PassOutcome::RealVars() const {
        if (kind_ != OutcomeKind::kSuccess) { ContractAbort(); }
        return real_vars_;
    }

    VerificationState PassOutcome::Verification() const {
        if (kind_ != OutcomeKind::kSuccess) { ContractAbort(); }
        return verification_;
    }

    const ReasonDetail &PassOutcome::Reason() const {
        if (kind_ == OutcomeKind::kSuccess) { ContractAbort(); }
        return *reason_;
    }

    const std::vector< uint64_t > &PassOutcome::SigVector() const { return sig_vector_; }

    void PassOutcome::SetSigVector(std::vector< uint64_t > sv) { sig_vector_ = std::move(sv); }

    const std::optional< DecompositionMeta > &PassOutcome::DecompositionMetadata() const {
        return decomposition_meta_;
    }

    void PassOutcome::SetDecompositionMeta(DecompositionMeta meta) {
        decomposition_meta_.emplace(std::move(meta));
    }

} // namespace cobra
