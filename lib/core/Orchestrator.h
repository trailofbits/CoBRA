#pragma once

#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/PassContract.h"
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
        kCandidateExpr,
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

    struct AstPayload
    {
        std::unique_ptr< Expr > expr;
        std::optional< Classification > classification;
        Provenance provenance = Provenance::kOriginal;
    };

    struct SignatureStatePayload
    {
        std::vector< uint64_t > sig;
        std::vector< std::string > real_vars;
        EliminationResult elimination;
        std::vector< uint32_t > original_indices;
        bool needs_original_space_verification = true;
    };

    struct CandidatePayload
    {
        std::unique_ptr< Expr > expr;
        std::vector< std::string > real_vars;
        ExprCost cost;
        PassId producing_pass;
        bool needs_original_space_verification = true;
    };

    using StateData = std::variant< AstPayload, SignatureStatePayload, CandidatePayload >;

    // ---------------------------------------------------------------
    // State features and item metadata
    // ---------------------------------------------------------------

    struct StateFeatures
    {
        std::optional< Classification > classification;
        Provenance provenance              = Provenance::kOriginal;
        bool needs_full_width_verification = true;
    };

    struct ItemMetadata
    {
        std::vector< uint64_t > sig_vector;
        VerificationState verification     = VerificationState::kUnverified;
        Route attempted_route              = Route::kBitwiseOnly;
        uint32_t rewrite_rounds            = 0;
        bool rewrite_produced_candidate    = false;
        bool candidate_failed_verification = false;
        std::optional< ReasonCode > reason_code;
        std::vector< ReasonFrame > cause_chain;
        std::optional< DecompositionMeta > decomposition_meta;
        ReasonDetail last_failure;
    };

    // ---------------------------------------------------------------
    // WorkItem — unit of work flowing through the orchestrator
    // ---------------------------------------------------------------

    struct WorkItem
    {
        StateData payload;
        StateFeatures features;
        ItemMetadata metadata;
        uint32_t depth        = 0;
        uint32_t rewrite_gen  = 0;
        uint32_t stage_cursor = 0;
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
        uint32_t stage_cursor;

        bool operator==(const StateFingerprint &) const = default;
    };

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
        bool allow_reroute         = true;
        uint32_t max_expansions    = 64;
        uint32_t max_rewrite_gen   = 3;
        uint32_t max_candidates    = 8;
        bool strict_route_faithful = false;
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

    struct OrchestratorResult
    {
        PassOutcome outcome;
        ItemMetadata metadata;
        RunMetadata run_metadata;
    };

    // ---------------------------------------------------------------
    // Helper declarations
    // ---------------------------------------------------------------

    // Returns the StateKind corresponding to the active variant alternative.
    StateKind GetStateKind(const StateData &data);

    // Computes a deduplication fingerprint for a WorkItem.
    // When normalize_stage_cursor is true, the cursor is zeroed in the key.
    StateFingerprint
    ComputeFingerprint(const WorkItem &item, uint32_t bitwidth, bool normalize_stage_cursor);

    // Deterministic ordering for selecting the best unsupported candidate
    // to surface in the final result.
    bool UnsupportedRankBetter(const UnsupportedCandidate &a, const UnsupportedCandidate &b);

    // ---------------------------------------------------------------
    // StateFingerprintHash — std::unordered_map key support
    // ---------------------------------------------------------------

    struct StateFingerprintHash
    {
        size_t operator()(const StateFingerprint &fp) const;
    };

    // ---------------------------------------------------------------
    // PassAttemptCache — deduplication of pass attempts per fingerprint
    // ---------------------------------------------------------------

    class PassAttemptCache
    {
      public:
        void Record(const StateFingerprint &fp, PassId pass);
        bool HasAttempted(const StateFingerprint &fp, PassId pass) const;

      private:
        std::unordered_map< StateFingerprint, std::vector< PassId >, StateFingerprintHash >
            cache_;
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

    std::vector< PassId > SchedulePasses(
        const WorkItem &item, const OrchestratorPolicy &policy,
        const PassAttemptCache &attempted
    );

    // ---------------------------------------------------------------
    // Orchestrator entry points
    // ---------------------------------------------------------------

    /// Main orchestrator function: seeds the worklist, runs the
    /// pass loop, and returns an OrchestratorResult.
    Result< OrchestratorResult > OrchestrateSimplify(
        const Expr *input_expr, const std::vector< uint64_t > &sig,
        const std::vector< std::string > &vars, const Options &opts,
        const OrchestratorPolicy &policy
    );

    /// Convert an OrchestratorResult to the legacy SimplifyOutcome
    /// format used by the existing test harnesses and CLI.
    SimplifyOutcome AdaptToLegacy(OrchestratorResult result, const Expr *original_expr);

    /// Test-only convenience: runs OrchestrateSimplify then
    /// AdaptToLegacy, returning Result< SimplifyOutcome >.
    Result< SimplifyOutcome > OrchestrateSimplifyForTest(
        const Expr *input_expr, const std::vector< uint64_t > &sig,
        const std::vector< std::string > &vars, const Options &opts,
        const OrchestratorPolicy &policy
    );

} // namespace cobra
