#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "SimplifierInternal.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/SimplifyOutcome.h"

#include <algorithm>
#include <unordered_map>

namespace cobra {

    StateKind GetStateKind(const StateData &data) {
        return std::visit(
            []< typename T >(const T &) -> StateKind {
                if constexpr (std::is_same_v< T, AstPayload >) {
                    return StateKind::kFoldedAst;
                } else if constexpr (std::is_same_v< T, SignatureStatePayload >) {
                    return StateKind::kSignatureState;
                } else if constexpr (std::is_same_v< T, CoreCandidatePayload >) {
                    return StateKind::kCoreCandidate;
                } else if constexpr (std::is_same_v< T, ResidualStatePayload >) {
                    return StateKind::kResidualState;
                } else {
                    return StateKind::kCandidateExpr;
                }
            },
            data
        );
    }

    StateFingerprint ComputeFingerprint(const WorkItem &item, uint32_t bitwidth) {
        StateFingerprint fp;
        fp.kind           = GetStateKind(item.payload);
        fp.bitwidth       = bitwidth;
        fp.provenance     = item.features.provenance;
        fp.attempted_mask = item.attempted_mask;

        std::visit(
            [&fp](const auto &payload) {
                using T = std::decay_t< decltype(payload) >;
                if constexpr (std::is_same_v< T, AstPayload >) {
                    fp.payload_hash = std::hash< Expr >{}(*payload.expr);
                    fp.vars         = {};
                } else if constexpr (std::is_same_v< T, SignatureStatePayload >) {
                    size_t h = std::hash< size_t >{}(payload.sig.size());
                    for (uint64_t v : payload.sig) {
                        h = detail::hash_combine(h, std::hash< uint64_t >{}(v));
                    }
                    fp.payload_hash = h;
                    fp.vars         = payload.real_vars;
                } else if constexpr (std::is_same_v< T, CoreCandidatePayload >) {
                    size_t h = std::hash< Expr >{}(*payload.core_expr);
                    h        = detail::hash_combine(
                        h,
                        std::hash< uint8_t >{}(static_cast< uint8_t >(payload.extractor_kind))
                    );
                    h = detail::hash_combine(h, std::hash< uint8_t >{}(payload.degree_used));
                    fp.payload_hash = h;
                    fp.vars         = {};
                } else if constexpr (std::is_same_v< T, ResidualStatePayload >) {
                    size_t h = std::hash< uint8_t >{}(static_cast< uint8_t >(payload.origin));
                    if (payload.core_expr) {
                        h = detail::hash_combine(h, std::hash< Expr >{}(*payload.core_expr));
                    } else {
                        h = detail::hash_combine(h, std::hash< uint64_t >{}(0xDEAD));
                    }
                    for (uint64_t v : payload.residual_sig) {
                        h = detail::hash_combine(h, std::hash< uint64_t >{}(v));
                    }
                    for (uint32_t s : payload.residual_support) {
                        h = detail::hash_combine(h, std::hash< uint32_t >{}(s));
                    }
                    h = detail::hash_combine(h, std::hash< bool >{}(payload.is_boolean_null));
                    h = detail::hash_combine(h, std::hash< uint8_t >{}(payload.degree_floor));
                    fp.payload_hash = h;
                    fp.vars         = {};
                } else {
                    // CandidatePayload
                    size_t h = detail::hash_combine(
                        std::hash< Expr >{}(*payload.expr),
                        std::hash< bool >{}(payload.needs_original_space_verification)
                    );
                    fp.payload_hash = h;
                    fp.vars         = payload.real_vars;
                }
            },
            item.payload
        );

        return fp;
    }

    namespace {
        int BandOf(const WorkItem &item) {
            return GetStateKind(item.payload) == StateKind::kCandidateExpr ? 0 : 1;
        }

        bool IsBetterPriority(const WorkItem &a, const WorkItem &b) {
            int band_a = BandOf(a);
            int band_b = BandOf(b);
            if (band_a != band_b) { return band_a < band_b; }
            if (a.depth != b.depth) { return a.depth < b.depth; }
            if (a.features.provenance != b.features.provenance) {
                return a.features.provenance < b.features.provenance;
            }
            if (a.history.size() != b.history.size()) {
                return a.history.size() < b.history.size();
            }
            return false;
        }
    } // namespace

} // namespace cobra

size_t
std::hash< cobra::StateFingerprint >::operator()(const cobra::StateFingerprint &fp) const {
    using cobra::detail::hash_combine;
    size_t h = std::hash< uint64_t >{}(fp.payload_hash);
    h        = hash_combine(h, std::hash< int >{}(static_cast< int >(fp.kind)));
    h        = hash_combine(h, std::hash< uint32_t >{}(fp.bitwidth));
    h        = hash_combine(h, std::hash< int >{}(static_cast< int >(fp.provenance)));
    h        = hash_combine(h, std::hash< uint64_t >{}(fp.attempted_mask));
    for (const auto &v : fp.vars) { h = hash_combine(h, std::hash< std::string >{}(v)); }
    return h;
}

namespace cobra {

    void PassAttemptCache::Record(const StateFingerprint &fp, PassId pass) {
        cache_[fp].push_back(pass);
    }

    bool PassAttemptCache::HasAttempted(const StateFingerprint &fp, PassId pass) const {
        auto it = cache_.find(fp);
        if (it == cache_.end()) { return false; }
        const auto &passes = it->second;
        return std::find(passes.begin(), passes.end(), pass) != passes.end();
    }

    void Worklist::Push(WorkItem item) {
        items_.push_back(std::move(item));
        high_water_ = std::max(high_water_, items_.size());
    }

    WorkItem Worklist::Pop() {
        size_t best = 0;
        for (size_t i = 1; i < items_.size(); ++i) {
            if (IsBetterPriority(items_[i], items_[best])) { best = i; }
        }
        WorkItem result = std::move(items_[best]);
        items_.erase(items_.begin() + static_cast< ptrdiff_t >(best));
        return result;
    }

    bool Worklist::Empty() const { return items_.empty(); }

    size_t Worklist::Size() const { return items_.size(); }

    size_t Worklist::HighWaterMark() const { return high_water_; }

    // ---------------------------------------------------------------
    // SelectNextPass — DAG-aware priority scheduler
    // ---------------------------------------------------------------

    namespace {

        bool IsFoldedAstExplorationCandidate(const Classification &cls) {
            if (HasFlag(cls.flags, kSfHasUnknownShape)) { return false; }
            return HasFlag(cls.flags, kSfHasMixedProduct)
                || HasFlag(cls.flags, kSfHasBitwiseOverArith);
        }

        struct FoldedAstPassEntry
        {
            PassId id;
            uint64_t prereq_mask;
            uint8_t priority;
            bool is_structural_transform;
        };

        constexpr uint64_t Bit(PassId p) {
            return static_cast< uint64_t >(1) << static_cast< uint8_t >(p);
        }

        constexpr FoldedAstPassEntry kFoldedAstPasses[] = {
            {     PassId::kBuildSignatureState,                                0, 0, false },
            {      PassId::kExtractProductCore,                                0, 1, false },
            {         PassId::kOperandSimplify, Bit(PassId::kExtractProductCore), 2,  true },
            { PassId::kProductIdentityCollapse, Bit(PassId::kExtractProductCore), 3,  true },
            {             PassId::kXorLowering,                                0, 4,  true },
        };

    } // namespace

    std::optional< PassId > SelectNextPass(
        const WorkItem &item, const OrchestratorPolicy &policy, uint32_t verifications_used,
        const PassAttemptCache &cache
    ) {
        auto kind = GetStateKind(item.payload);
        auto fp   = ComputeFingerprint(item, 64);

        // 1. Candidate → kVerifyCandidate
        if (kind == StateKind::kCandidateExpr) {
            auto pass = PassId::kVerifyCandidate;
            if (verifications_used >= policy.max_candidates) { return std::nullopt; }
            if ((item.attempted_mask & Bit(pass)) != 0) { return std::nullopt; }
            if (cache.HasAttempted(fp, pass)) { return std::nullopt; }
            return pass;
        }

        // 2. SignatureState → kSupportedSolve
        if (kind == StateKind::kSignatureState) {
            auto pass = PassId::kSupportedSolve;
            if ((item.attempted_mask & Bit(pass)) != 0) { return std::nullopt; }
            if (cache.HasAttempted(fp, pass)) { return std::nullopt; }
            return pass;
        }

        // 3. Original provenance + semilinear → kTrySemilinearPass
        if (item.features.provenance == Provenance::kOriginal) {
            if (item.features.classification
                && item.features.classification->semantic == SemanticClass::kSemilinear)
            {
                auto pass = PassId::kTrySemilinearPass;
                if ((item.attempted_mask & Bit(pass)) != 0) { return std::nullopt; }
                if (cache.HasAttempted(fp, pass)) { return std::nullopt; }
                return pass;
            }
            return std::nullopt;
        }

        // 4. Non-original: must have classification and no unknown shape
        if (!item.features.classification) { return std::nullopt; }
        const auto &cls = *item.features.classification;
        if (HasFlag(cls.flags, kSfHasUnknownShape)) { return std::nullopt; }

        // 5. Non-exploration candidates → only kBuildSignatureState
        if (!IsFoldedAstExplorationCandidate(cls)) {
            auto pass = PassId::kBuildSignatureState;
            if ((item.attempted_mask & Bit(pass)) != 0) { return std::nullopt; }
            if (cache.HasAttempted(fp, pass)) { return std::nullopt; }
            return pass;
        }

        // 6. Exploration candidates → iterate the pass table
        for (const auto &entry : kFoldedAstPasses) {
            if ((item.attempted_mask & Bit(entry.id)) != 0) { continue; }
            if ((item.attempted_mask & entry.prereq_mask) != entry.prereq_mask) { continue; }
            if (entry.is_structural_transform && item.rewrite_gen >= policy.max_rewrite_gen) {
                continue;
            }
            if (cache.HasAttempted(fp, entry.id)) { continue; }
            return entry.id;
        }

        // 7. No eligible pass
        return std::nullopt;
    }

    bool UnsupportedRankBetter(const UnsupportedCandidate &a, const UnsupportedCandidate &b) {
        // 1. Candidates (verification-failed) rank highest
        if (a.is_candidate_state != b.is_candidate_state) { return a.is_candidate_state; }
        // 2. Deeper depth
        if (a.depth != b.depth) { return a.depth > b.depth; }
        // 3. More rewrites
        if (a.rewrite_gen != b.rewrite_gen) { return a.rewrite_gen > b.rewrite_gen; }
        // 4. More passes attempted
        if (a.history_size != b.history_size) { return a.history_size > b.history_size; }
        // 5. Last PassId enum order
        if (a.last_pass != b.last_pass) { return a.last_pass > b.last_pass; }
        // 6. Prefer items with structural transform terminal evidence
        bool a_has_terminal = a.metadata.structural_transform_terminal.has_value();
        bool b_has_terminal = b.metadata.structural_transform_terminal.has_value();
        if (a_has_terminal != b_has_terminal) { return a_has_terminal; }
        if (a_has_terminal && b_has_terminal) {
            auto rank = [](ReasonCategory c) -> int {
                if (c == ReasonCategory::kVerifyFailed) { return 2; }
                if (c == ReasonCategory::kRepresentationGap) { return 1; }
                return 0;
            };
            int ra = rank(a.metadata.structural_transform_terminal->category);
            int rb = rank(b.metadata.structural_transform_terminal->category);
            if (ra != rb) { return ra > rb; }
        }
        return false;
    }

    // ---------------------------------------------------------------
    // Simplify — main orchestrator loop
    // ---------------------------------------------------------------

    namespace {

        struct OrchestratorResult
        {
            PassOutcome outcome;
            ItemMetadata metadata;
            RunMetadata run_metadata;
        };

        // Seed the worklist for the no-AST path (signature only).
        Result< std::optional< OrchestratorResult > > SeedNoAst(
            const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
            OrchestratorContext &ctx, Worklist &worklist
        ) {
            const auto num_vars = static_cast< uint32_t >(vars.size());

            // Constant pruning
            auto pm = MatchPattern(sig, num_vars, ctx.bitwidth);
            if (pm && (*pm)->kind == Expr::Kind::kConstant) {
                ItemMetadata meta;
                meta.sig_vector   = sig;
                meta.verification = VerificationState::kVerified;

                return Ok(
                    std::optional< OrchestratorResult >(OrchestratorResult{
                        .outcome = PassOutcome::Success(
                            std::move(*pm), {}, VerificationState::kVerified
                        ),
                        .metadata     = std::move(meta),
                        .run_metadata = ctx.run_metadata,
                    })
                );
            }

            // Eliminate auxiliary variables
            auto elim                 = EliminateAuxVars(sig, vars);
            const auto real_var_count = static_cast< uint32_t >(elim.real_vars.size());

            if (real_var_count > ctx.opts.max_vars) {
                return Err< std::optional< OrchestratorResult > >(
                    CobraError::kTooManyVariables,
                    "Variable count after elimination (" + std::to_string(real_var_count)
                        + ") exceeds max_vars (" + std::to_string(ctx.opts.max_vars) + ")"
                );
            }

            auto original_indices = BuildVarSupport(vars, elim.real_vars);

            WorkItem sig_item;
            sig_item.payload = SignatureStatePayload{
                .sig                               = sig,
                .real_vars                         = elim.real_vars,
                .elimination                       = std::move(elim),
                .original_indices                  = std::move(original_indices),
                .needs_original_space_verification = false,
            };
            sig_item.features.provenance = Provenance::kOriginal;
            worklist.Push(std::move(sig_item));

            return Ok(std::optional< OrchestratorResult >(std::nullopt));
        }

        // Seed the worklist for the with-AST path.
        Result< std::optional< OrchestratorResult > >
        SeedWithAst(const Expr &input_expr, OrchestratorContext &ctx, Worklist &worklist) {
            // Create initial AST item
            WorkItem seed;
            seed.payload = AstPayload{
                .expr       = CloneExpr(input_expr),
                .provenance = Provenance::kOriginal,
            };
            seed.features.provenance = Provenance::kOriginal;

            // Prerequisite: lower NOT-over-arith
            auto lower_result = RunLowerNotOverArith(seed, ctx);
            if (!lower_result.has_value()) { return std::unexpected(lower_result.error()); }

            auto &lr = lower_result.value();

            WorkItem *classify_target = nullptr;
            std::optional< WorkItem > lowered_item;

            if (lr.decision == PassDecision::kAdvance && !lr.next.empty()) {
                lowered_item       = std::move(lr.next[0]);
                classify_target    = &*lowered_item;
                ctx.lowering_fired = true;
            } else {
                classify_target = &seed;
            }

            // Classify the target
            auto cls_result = RunClassifyAst(*classify_target, ctx);
            if (!cls_result.has_value()) { return std::unexpected(cls_result.error()); }

            auto &cr        = cls_result.value();
            auto classified = std::move(cr.next[0]);
            auto cls        = classified.features.classification;

            // Copy classification to the original seed and set
            // attempted_route so unsupported outcomes carry the
            // correct route in diagnostics.
            seed.features.classification = cls;
            if (auto *ast = std::get_if< AstPayload >(&seed.payload)) {
                ast->classification = cls;
            }
            if (cls) {
                classified.metadata.attempted_route = cls->route;
                seed.metadata.attempted_route       = cls->route;
            }

            // Push items to the worklist.
            // Both branches ensure a kLowered item enters the worklist
            // so the scheduler routes it into kBuildSignatureState.
            if (lowered_item) {
                // Lowering fired: classified item is already kLowered.
                worklist.Push(std::move(classified));
                // Push the original for the semilinear path.
                worklist.Push(std::move(seed));
            } else {
                // Lowering was a no-op: promote to kLowered so the
                // scheduler treats it identically to the lowered case.
                if (auto *ast = std::get_if< AstPayload >(&classified.payload)) {
                    ast->provenance = Provenance::kLowered;
                }
                classified.features.provenance = Provenance::kLowered;
                worklist.Push(std::move(classified));
                // Push the original for semilinear if applicable.
                if (cls && cls->semantic == SemanticClass::kSemilinear) {
                    worklist.Push(std::move(seed));
                }
            }

            return Ok(std::optional< OrchestratorResult >(std::nullopt));
        }

        SimplifyOutcome ToSimplifyOutcome(
            OrchestratorResult result, const Expr *original_expr,
            const OrchestratorTelemetry &telemetry
        ) {
            SimplifyOutcome outcome;

            if (result.outcome.Succeeded()) {
                outcome.kind      = SimplifyOutcome::Kind::kSimplified;
                outcome.expr      = result.outcome.TakeExpr();
                outcome.real_vars = result.outcome.RealVars();
                outcome.verified = result.metadata.verification == VerificationState::kVerified;
                outcome.sig_vector = std::move(result.metadata.sig_vector);
            } else {
                outcome.kind = SimplifyOutcome::Kind::kUnchangedUnsupported;
                outcome.expr = original_expr != nullptr ? CloneExpr(*original_expr) : nullptr;
            }

            outcome.diag.classification  = result.run_metadata.input_classification;
            outcome.diag.attempted_route = result.metadata.attempted_route;
            outcome.diag.structural_transform_rounds =
                result.metadata.structural_transform_rounds;
            outcome.diag.transform_produced_candidate =
                result.metadata.transform_produced_candidate;
            outcome.diag.candidate_failed_verification =
                result.metadata.candidate_failed_verification;
            outcome.diag.reason_code = result.metadata.reason_code;
            outcome.diag.cause_chain = std::move(result.metadata.cause_chain);

            if (!result.outcome.Succeeded()) {
                outcome.diag.reason = result.outcome.Reason().top.message;
            }

            outcome.telemetry = {
                .total_expansions    = telemetry.total_expansions,
                .max_depth_reached   = telemetry.max_depth_reached,
                .candidates_verified = telemetry.candidates_verified,
                .queue_high_water    = telemetry.queue_high_water,
            };

            return outcome;
        }

    } // namespace

    Result< SimplifyOutcome > Simplify(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        const Expr *input_expr, const Options &opts
    ) {
        OrchestratorPolicy policy;
        OrchestratorContext context{
            .opts          = opts,
            .original_vars = vars,
            .evaluator =
                opts.evaluator ? std::optional< Evaluator >(opts.evaluator) : std::nullopt,
            .bitwidth     = opts.bitwidth,
            .run_metadata = {},
            .input_sig    = sig,
        };

        Worklist worklist;
        OrchestratorTelemetry telemetry;

        // Seeding
        if (input_expr == nullptr) {
            auto seed_result = SeedNoAst(sig, vars, context, worklist);
            if (!seed_result.has_value()) { return std::unexpected(seed_result.error()); }
            if (seed_result.value().has_value()) {
                return Ok(
                    ToSimplifyOutcome(std::move(*seed_result.value()), input_expr, telemetry)
                );
            }
        } else {
            auto seed_result = SeedWithAst(*input_expr, context, worklist);
            if (!seed_result.has_value()) { return std::unexpected(seed_result.error()); }
            if (seed_result.value().has_value()) {
                return Ok(
                    ToSimplifyOutcome(std::move(*seed_result.value()), input_expr, telemetry)
                );
            }
        }

        // Build registry lookup map
        const auto &registry = GetPassRegistry();
        std::unordered_map< PassId, const PassDescriptor * > registry_map;
        for (const auto &desc : registry) { registry_map[desc.id] = &desc; }
        PassAttemptCache cache;
        uint32_t expansions    = 0;
        uint32_t verifications = 0;
        std::optional< UnsupportedCandidate > best_unsupported;
        std::vector< ReasonFrame > decomp_causes;
        // Strongest structural-transform terminal observed across
        // all lineages. Survives best_unsupported replacements.
        std::optional< TransformTerminalSignal > strongest_transform_terminal;

        // Main loop
        while (!worklist.Empty() && expansions < policy.max_expansions) {
            auto item = worklist.Pop();
            ++expansions;
            telemetry.total_expansions  = expansions;
            telemetry.max_depth_reached = std::max(telemetry.max_depth_reached, item.depth);

            // Track best unsupported
            UnsupportedCandidate current{
                .metadata           = item.metadata,
                .depth              = item.depth,
                .rewrite_gen        = item.rewrite_gen,
                .history_size       = static_cast< uint32_t >(item.history.size()),
                .last_pass          = item.history.empty() ? PassId{} : item.history.back(),
                .is_candidate_state = std::holds_alternative< CandidatePayload >(item.payload),
            };
            if (!best_unsupported || UnsupportedRankBetter(current, *best_unsupported)) {
                best_unsupported = current;
            }
            // Promote lineage-local terminal to loop-level tracker
            if (item.metadata.structural_transform_terminal) {
                auto &sig  = *item.metadata.structural_transform_terminal;
                auto trank = [](ReasonCategory c) -> int {
                    if (c == ReasonCategory::kVerifyFailed) { return 2; }
                    if (c == ReasonCategory::kRepresentationGap) { return 1; }
                    return 0;
                };
                if (!strongest_transform_terminal
                    || trank(sig.category) > trank(strongest_transform_terminal->category))
                {
                    strongest_transform_terminal = sig;
                }
            }

            // Candidate acceptance: verified candidates are immediately returned
            if (auto *cand = std::get_if< CandidatePayload >(&item.payload)) {
                if (!cand->needs_original_space_verification) {
                    // Stamp transform_produced_candidate if any rewrite
                    // pass is in this candidate's lineage.
                    for (auto h : item.history) {
                        if (h == PassId::kOperandSimplify
                            || h == PassId::kProductIdentityCollapse
                            || h == PassId::kXorLowering)
                        {
                            item.metadata.transform_produced_candidate = true;
                            break;
                        }
                    }
                    telemetry.queue_high_water = worklist.HighWaterMark();
                    return Ok(ToSimplifyOutcome(
                        OrchestratorResult{
                            .outcome = PassOutcome::Success(
                                CloneExpr(*cand->expr), cand->real_vars,
                                item.metadata.verification
                            ),
                            .metadata     = std::move(item.metadata),
                            .run_metadata = context.run_metadata,
                        },
                        input_expr, telemetry
                    ));
                }
            }

            // Select one pass
            auto pass_id = SelectNextPass(item, policy, verifications, cache);
            if (!pass_id) { continue; }

            // Compute pre-attempt fingerprint, record attempt
            auto fp              = ComputeFingerprint(item, context.bitwidth);
            item.attempted_mask |= Bit(*pass_id);

            // Run the pass
            auto it = registry_map.find(*pass_id);
            if (it == registry_map.end()) { continue; }
            telemetry.passes_attempted.push_back(*pass_id);
            auto result = it->second->run(item, context);
            if (*pass_id == PassId::kVerifyCandidate) {
                ++verifications;
                telemetry.candidates_verified = verifications;
            }
            if (!result.has_value()) { return std::unexpected(result.error()); }
            cache.Record(fp, *pass_id);

            auto &pr = result.value();
            if (pr.decision == PassDecision::kAdvance
                || pr.decision == PassDecision::kSolvedCandidate)
            {
                for (auto &next : pr.next) {
                    next.depth = item.depth + 1;
                    next.history.push_back(*pass_id);
                    worklist.Push(std::move(next));
                }
                if (pr.disposition == ItemDisposition::kRetainCurrent) {
                    item.depth = item.depth + 2;
                    worklist.Push(std::move(item));
                }
            } else {
                // kBlocked / kNoProgress
                if (!pr.reason.top.message.empty()) {
                    item.metadata.last_failure = pr.reason;
                    if (best_unsupported) {
                        best_unsupported->metadata.last_failure = pr.reason;
                    }
                }
                // XOR lowering terminal attribution (lineage-local)
                if (*pass_id == PassId::kXorLowering) {
                    auto cat = pr.reason.top.code.category;
                    item.metadata.structural_transform_terminal =
                        TransformTerminalSignal{ *pass_id, cat };
                    if (cat == ReasonCategory::kRepresentationGap) {
                        item.metadata.transform_produced_candidate = true;
                    } else if (cat == ReasonCategory::kVerifyFailed) {
                        item.metadata.transform_produced_candidate  = true;
                        item.metadata.candidate_failed_verification = true;
                    }
                    if (best_unsupported) {
                        best_unsupported->metadata.transform_produced_candidate =
                            item.metadata.transform_produced_candidate;
                        best_unsupported->metadata.candidate_failed_verification =
                            item.metadata.candidate_failed_verification;
                    }
                }
                // Verify failure after XOR lowering (lineage-local)
                if (*pass_id == PassId::kVerifyCandidate
                    && pr.reason.top.code.category == ReasonCategory::kVerifyFailed)
                {
                    for (auto h : item.history) {
                        if (h == PassId::kXorLowering) {
                            item.metadata.structural_transform_terminal =
                                TransformTerminalSignal{ PassId::kXorLowering,
                                                         ReasonCategory::kVerifyFailed };
                            item.metadata.transform_produced_candidate  = true;
                            item.metadata.candidate_failed_verification = true;
                            if (best_unsupported) {
                                best_unsupported->metadata.transform_produced_candidate  = true;
                                best_unsupported->metadata.candidate_failed_verification = true;
                            }
                            break;
                        }
                    }
                }
                // Accumulate decomposition cause chain
                if (IsDecompositionFamilyPass(*pass_id)) {
                    decomp_causes.push_back(pr.reason.top);
                    for (const auto &c : pr.reason.causes) { decomp_causes.push_back(c); }
                }
                // Requeue if retained (SelectNextPass will find next eligible)
                if (pr.disposition == ItemDisposition::kRetainCurrent) {
                    worklist.Push(std::move(item));
                }
            }
        }

        // Exhaustion: build the final unsupported result
        ReasonDetail exhaustion_reason;
        if (best_unsupported && !best_unsupported->metadata.last_failure.top.message.empty()) {
            exhaustion_reason = best_unsupported->metadata.last_failure;
        } else {
            exhaustion_reason = ReasonDetail{
                .top = { .code    = { ReasonCategory::kSearchExhausted,
                                      ReasonDomain::kOrchestrator },
                        .message = "Worklist exhausted" },
            };
        }

        ItemMetadata final_meta =
            best_unsupported ? std::move(best_unsupported->metadata) : ItemMetadata{};

        // Derive structural-transform terminal reason code from
        // lineage-local metadata instead of the old loop-global variable.
        bool used_folded_ast_exploration =
            IsFoldedAstExplorationCandidate(context.run_metadata.input_classification)
            || final_meta.structural_transform_rounds > 0
            || final_meta.transform_produced_candidate
            || strongest_transform_terminal.has_value();

        if (used_folded_ast_exploration && !final_meta.reason_code.has_value()) {
            if (strongest_transform_terminal) {
                auto cat = strongest_transform_terminal->category;
                final_meta.reason_code =
                    ReasonCode{ cat, ReasonDomain::kStructuralTransform, 0 };
                if (cat == ReasonCategory::kVerifyFailed) {
                    final_meta.candidate_failed_verification = true;
                    final_meta.transform_produced_candidate  = true;
                } else if (cat == ReasonCategory::kRepresentationGap) {
                    final_meta.transform_produced_candidate = true;
                }
            }
        }

        // Propagate reason_code from the last failure if not already set
        if (!final_meta.reason_code.has_value()
            && exhaustion_reason.top.code.category != ReasonCategory::kNone)
        {
            final_meta.reason_code = exhaustion_reason.top.code;
        }

        // Propagate accumulated decomposition cause chain
        if (final_meta.cause_chain.empty() && !decomp_causes.empty()) {
            final_meta.cause_chain = std::move(decomp_causes);
        }
        // For non-MixedRewrite routes, propagate semilinear failure
        // as the cause chain (matching legacy behavior where the
        // supported pipeline's verification failure includes the
        // semilinear pipeline's failure reason).
        if (final_meta.cause_chain.empty()
            && context.run_metadata.input_classification.route != Route::kMixedRewrite
            && context.run_metadata.semilinear_failure.has_value())
        {
            const auto &sf = *context.run_metadata.semilinear_failure;
            final_meta.cause_chain.push_back(sf.top);
            for (const auto &c : sf.causes) { final_meta.cause_chain.push_back(c); }
        }
        // Fallback: collect cause chain from exhaustion reason
        if (final_meta.cause_chain.empty() && !exhaustion_reason.causes.empty()) {
            final_meta.cause_chain = exhaustion_reason.causes;
        }

        telemetry.queue_high_water = worklist.HighWaterMark();
        return Ok(ToSimplifyOutcome(
            OrchestratorResult{
                .outcome      = PassOutcome::Blocked(std::move(exhaustion_reason)),
                .metadata     = std::move(final_meta),
                .run_metadata = context.run_metadata,
            },
            input_expr, telemetry
        ));
    }

} // namespace cobra
