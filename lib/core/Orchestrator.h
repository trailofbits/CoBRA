#pragma once

#include "CompetitionGroup.h"
#include "JoinState.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classification.h"
#include "cobra/core/DecompositionEngine.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/SemilinearIR.h"
#include "cobra/core/Simplifier.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace cobra {

    // ---------------------------------------------------------------
    // State kind discriminants
    // ---------------------------------------------------------------

    enum class StateKind {
        kFoldedAst,
        kSignatureState,
        kSignatureCoeffState,
        kCoreCandidate,
        kRemainderState,
        kSemilinearNormalizedIr,
        kSemilinearCheckedIr,
        kSemilinearRewrittenIr,
        kLiftedSkeleton,
        kCandidateExpr,
        kCompetitionResolved,
    };

    enum class Provenance {
        kOriginal,
        kLowered,
        kRewritten,
    };

    enum class PassDecision {
        kNotApplicable,
        kNoProgress,
        kAdvance,
        kSolvedCandidate,
        kBlocked,
    };

    enum class ItemDisposition {
        kRetainCurrent,
        kReplaceCurrent,
        kConsumeCurrent,
    };

    // Forward-declared here; defined in OrchestratorPasses.h
    enum class PassId : uint8_t;

    // ---------------------------------------------------------------
    // Payload types for StateData variant
    // ---------------------------------------------------------------

    struct AstSolveContext
    {
        std::vector< std::string > vars;
        std::optional< Evaluator > evaluator;
        std::vector< uint64_t > input_sig;
    };

    struct AstPayload
    {
        std::unique_ptr< Expr > expr;
        std::optional< Classification > classification;
        Provenance provenance = Provenance::kOriginal;
        std::optional< AstSolveContext > solve_ctx;
    };

    struct SignatureSubproblemContext
    {
        std::vector< uint64_t > sig;
        std::vector< std::string > real_vars;
        EliminationResult elimination;
        std::vector< uint32_t > original_indices;
        bool needs_original_space_verification = true;
    };

    struct SignatureStatePayload
    {
        SignatureSubproblemContext ctx;
    };

    struct SignatureCoeffStatePayload
    {
        SignatureSubproblemContext ctx;
        std::vector< uint64_t > coeffs;
    };

    struct CandidatePayload
    {
        std::unique_ptr< Expr > expr;
        std::vector< std::string > real_vars;
        ExprCost cost;
        PassId producing_pass;
        bool needs_original_space_verification = true;
    };

    enum class RemainderOrigin : uint8_t {
        kDirectBooleanNull,
        kProductCore,
        kPolynomialCore,
        kTemplateCore,
        kSignatureLowering,
        kLiftedOuter,
    };

    struct RemainderTargetContext
    {
        Evaluator eval;
        std::vector< std::string > vars;
        std::vector< uint32_t > remap_support;
    };

    struct CoreCandidatePayload
    {
        std::unique_ptr< Expr > core_expr;
        ExtractorKind extractor_kind;
        uint8_t degree_used = 0;
        std::vector< uint64_t > source_sig;
        RemainderTargetContext target;
    };

    struct RemainderStatePayload
    {
        RemainderOrigin origin;
        std::unique_ptr< Expr > prefix_expr;
        uint8_t prefix_degree = 0;
        Evaluator remainder_eval;
        std::vector< uint64_t > source_sig;
        std::vector< uint64_t > remainder_sig;
        EliminationResult remainder_elim;
        std::vector< uint32_t > remainder_support;
        bool is_boolean_null = false;
        uint8_t degree_floor = 2;
        RemainderTargetContext target;
    };

    struct LiftedSkeletonPayload
    {
        std::unique_ptr< Expr > outer_expr;
        AstSolveContext outer_ctx;
        std::vector< LiftedBinding > bindings;
        uint32_t original_var_count = 0;
        ExprCost baseline_cost;
        std::vector< uint64_t > source_sig;
        // Parent-local context for the expression before adding the
        // new lifted virtuals. Nested lifting must resolve back into
        // this space, not necessarily the global original space.
        AstSolveContext original_ctx;
    };

    struct SemilinearContext
    {
        SemilinearIR ir;
        std::vector< std::string > vars;
        std::optional< Evaluator > evaluator;
    };

    struct NormalizedSemilinearPayload
    {
        SemilinearContext ctx;
    };

    struct CheckedSemilinearPayload
    {
        SemilinearContext ctx;
    };

    struct RewrittenSemilinearPayload
    {
        SemilinearContext ctx;
    };

    struct CompetitionResolvedPayload
    {
        GroupId group_id;
    };

    using StateData = std::variant<
        AstPayload, SignatureStatePayload, SignatureCoeffStatePayload, CoreCandidatePayload,
        RemainderStatePayload, NormalizedSemilinearPayload, CheckedSemilinearPayload,
        RewrittenSemilinearPayload, LiftedSkeletonPayload, CandidatePayload,
        CompetitionResolvedPayload >;

    // ---------------------------------------------------------------
    // State features and item metadata
    // ---------------------------------------------------------------

    struct StateFeatures
    {
        std::optional< Classification > classification;
        Provenance provenance              = Provenance::kOriginal;
        bool needs_full_width_verification = true;
    };

    struct TransformTerminalSignal
    {
        PassId source_pass;
        ReasonCategory category;
    };

    struct ItemMetadata
    {
        std::vector< uint64_t > sig_vector;
        VerificationState verification       = VerificationState::kUnverified;
        uint32_t structural_transform_rounds = 0;
        bool transform_produced_candidate    = false;
        bool candidate_failed_verification   = false;
        std::optional< ReasonCode > reason_code;
        std::vector< ReasonFrame > cause_chain;
        std::optional< DecompositionMeta > decomposition_meta;
        std::vector< ReasonFrame > decomposition_causes;
        ReasonDetail last_failure;
        std::optional< TransformTerminalSignal > structural_transform_terminal;
    };

    // ---------------------------------------------------------------
    // WorkItem — unit of work flowing through the orchestrator
    // ---------------------------------------------------------------

    struct WorkItem
    {
        StateData payload;
        StateFeatures features;
        ItemMetadata metadata;
        uint32_t depth                    = 0;
        uint32_t rewrite_gen              = 0;
        uint64_t attempted_mask           = 0;
        uint8_t signature_recursion_depth = 0;
        std::optional< GroupId > group_id;
        std::optional< Evaluator > evaluator_override;
        uint32_t evaluator_override_arity = 0;
        std::vector< PassId > history;
    };

    // ---------------------------------------------------------------
    // StateFingerprint — deduplication key for the worklist
    // ---------------------------------------------------------------

    struct StateFingerprint
    {
        StateKind kind;
        uint64_t payload_hash;
        std::vector< std::string > vars;
        uint32_t bitwidth;
        Provenance provenance;

        bool operator==(const StateFingerprint &) const = default;
    };

} // namespace cobra

template<>
struct std::hash< cobra::StateFingerprint >
{
    size_t operator()(const cobra::StateFingerprint &fp) const;
};

namespace cobra {

    struct SemilinearFingerprintKey
    {
        uint64_t constant = 0;
        uint32_t bitwidth = 0;

        struct TermKey
        {
            uint64_t coeff = 0;
            std::vector< GlobalVarIdx > support;
            std::vector< uint64_t > truth_table;
            uint64_t structural_hash               = 0;
            OperatorFamily provenance              = OperatorFamily::kMixed;
            bool operator==(const TermKey &) const = default;
        };

        std::vector< TermKey > terms;
        bool operator==(const SemilinearFingerprintKey &) const = default;
    };

    SemilinearFingerprintKey BuildSemilinearFingerprintKey(const SemilinearIR &ir);

} // namespace cobra

template<>
struct std::hash< cobra::SemilinearFingerprintKey >
{
    size_t operator()(const cobra::SemilinearFingerprintKey &key) const;
};

namespace cobra {

    // ---------------------------------------------------------------
    // PassResult — what a pass returns to the orchestrator
    // ---------------------------------------------------------------

    struct PassResult
    {
        PassDecision decision;
        ItemDisposition disposition = ItemDisposition::kConsumeCurrent;
        std::vector< WorkItem > next;
        ReasonDetail reason;
    };

    // ---------------------------------------------------------------
    // Policy and context
    // ---------------------------------------------------------------

    struct OrchestratorPolicy
    {
        uint32_t max_expansions  = 1024;
        uint32_t max_rewrite_gen = 3;
        uint32_t max_candidates  = 8;
    };

    struct OrchestratorTelemetry
    {
        uint32_t total_expansions    = 0;
        uint32_t max_depth_reached   = 0;
        uint32_t candidates_verified = 0;
        uint32_t queue_high_water    = 0;
        std::vector< PassId > passes_attempted;
    };

    struct RunMetadata
    {
        Classification input_classification;
        std::optional< ReasonDetail > semilinear_failure;
    };

    struct OrchestratorContext
    {
        const Options &opts; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
        const std::vector< std::string >
            &original_vars; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
        std::optional< Evaluator > evaluator;
        uint32_t bitwidth = 64;
        RunMetadata run_metadata;
        // Parser-computed signature for the initial expression.
        // Used by RunBuildSignatureState on the first (non-rewritten)
        // pass to match legacy signature computation exactly.
        std::vector< uint64_t > input_sig;
        // Whether the NOT-over-arith lowering fired on the input.
        // When true, RunBuildSignatureState must recompute the
        // signature from the AST because the parser's sig is stale.
        bool lowering_fired = false;
        // Competition group registry for Phase 3 technique competition.
        std::unordered_map< GroupId, CompetitionGroup > competition_groups;
        GroupId next_group_id = 0;
        // Join state registry for multi-operand rewrites.
        std::unordered_map< JoinId, JoinState > join_states;
        JoinId next_join_id = 0;
    };

    // ---------------------------------------------------------------
    // Result types
    // ---------------------------------------------------------------

    struct UnsupportedCandidate
    {
        ItemMetadata metadata;
        uint32_t depth        = 0;
        uint32_t rewrite_gen  = 0;
        uint32_t history_size = 0;
        PassId last_pass{};
        bool is_candidate_state = false;
    };

    // ---------------------------------------------------------------
    // Helper declarations
    // ---------------------------------------------------------------

    // Returns the StateKind corresponding to the active variant alternative.
    StateKind GetStateKind(const StateData &data);

    // Computes a deduplication fingerprint for a WorkItem.
    StateFingerprint ComputeFingerprint(const WorkItem &item, uint32_t bitwidth);

    // Deterministic ordering for selecting the best unsupported candidate
    // to surface in the final result.
    bool UnsupportedRankBetter(const UnsupportedCandidate &a, const UnsupportedCandidate &b);

    // ---------------------------------------------------------------
    // PassAttemptCache — deduplication of pass attempts per fingerprint
    // ---------------------------------------------------------------

    class PassAttemptCache
    {
      public:
        void Record(const StateFingerprint &fp, PassId pass);
        bool HasAttempted(const StateFingerprint &fp, PassId pass) const;

      private:
        std::unordered_map< StateFingerprint, std::vector< PassId > > cache_;
    };

    // ---------------------------------------------------------------
    // Worklist — priority queue over WorkItems
    // ---------------------------------------------------------------

    class Worklist
    {
      public:
        void Push(WorkItem item);
        WorkItem Pop();
        bool Empty() const;
        size_t Size() const;
        size_t HighWaterMark() const;

      private:
        std::vector< WorkItem > items_;
        size_t high_water_ = 0;
    };

    // ---------------------------------------------------------------
    // Scheduler — determines which passes to try for a WorkItem
    // ---------------------------------------------------------------

    std::optional< PassId > SelectNextPass(
        const WorkItem &item, const OrchestratorPolicy &policy, uint32_t verifications_used,
        const PassAttemptCache &cache
    );

    // ---------------------------------------------------------------
    // Competition group handle release
    // ---------------------------------------------------------------

    // Decrements a group's open_handles. Returns a
    // kCompetitionResolved WorkItem when handles reach zero.
    std::optional< WorkItem >
    ReleaseHandle(std::unordered_map< GroupId, CompetitionGroup > &groups, GroupId group_id);

} // namespace cobra
