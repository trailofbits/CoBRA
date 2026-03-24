#include "cobra/core/PassContract.h"

#include <cassert>
#include <utility>

namespace cobra {

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

    PassOutcome PassOutcome::Inapplicable(ReasonDetail reason) {
        PassOutcome o(OutcomeKind::kInapplicable);
        o.reason_.emplace(std::move(reason));
        return o;
    }

    PassOutcome PassOutcome::Blocked(ReasonDetail reason) {
        PassOutcome o(OutcomeKind::kBlocked);
        o.reason_.emplace(std::move(reason));
        return o;
    }

    PassOutcome PassOutcome::Partial(
        std::unique_ptr< Expr > expr, std::vector< std::string > real_vars,
        VerificationState verification, PendingWork pending, ReasonDetail reason
    ) {
        PassOutcome o(OutcomeKind::kPartial);
        o.expr_         = std::move(expr);
        o.real_vars_    = std::move(real_vars);
        o.verification_ = verification;
        o.pending_.emplace(std::move(pending));
        o.reason_.emplace(std::move(reason));
        return o;
    }

    PassOutcome PassOutcome::VerifyFailed(
        std::unique_ptr< Expr > expr, std::vector< std::string > real_vars, ReasonDetail reason
    ) {
        PassOutcome o(OutcomeKind::kVerifyFailed);
        o.expr_         = std::move(expr);
        o.real_vars_    = std::move(real_vars);
        o.verification_ = VerificationState::kRejected;
        o.reason_.emplace(std::move(reason));
        return o;
    }

    const Expr &PassOutcome::GetExpr() const {
        assert(
            kind_ == OutcomeKind::kSuccess || kind_ == OutcomeKind::kPartial
            || kind_ == OutcomeKind::kVerifyFailed
        );
        return *expr_;
    }

    std::unique_ptr< Expr > PassOutcome::TakeExpr() {
        assert(
            kind_ == OutcomeKind::kSuccess || kind_ == OutcomeKind::kPartial
            || kind_ == OutcomeKind::kVerifyFailed
        );
        return std::move(expr_);
    }

    const std::vector< std::string > &PassOutcome::RealVars() const {
        assert(
            kind_ == OutcomeKind::kSuccess || kind_ == OutcomeKind::kPartial
            || kind_ == OutcomeKind::kVerifyFailed
        );
        return real_vars_;
    }

    VerificationState PassOutcome::Verification() const {
        assert(
            kind_ == OutcomeKind::kSuccess || kind_ == OutcomeKind::kPartial
            || kind_ == OutcomeKind::kVerifyFailed
        );
        return verification_;
    }

    const ReasonDetail &PassOutcome::Reason() const {
        assert(kind_ != OutcomeKind::kSuccess);
        return *reason_;
    }

    const PendingWork &PassOutcome::Pending() const {
        assert(kind_ == OutcomeKind::kPartial);
        return *pending_;
    }

    const std::vector< uint64_t > &PassOutcome::SigVector() const { return sig_vector_; }

    void PassOutcome::SetSigVector(std::vector< uint64_t > sv) { sig_vector_ = std::move(sv); }

} // namespace cobra
