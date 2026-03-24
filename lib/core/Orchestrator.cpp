#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "SimplifierInternal.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/SignatureEval.h"
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
                } else {
                    return StateKind::kCandidateExpr;
                }
            },
            data
        );
    }

    namespace {
        uint64_t HashExpr(const Expr &e) {
            uint64_t h = 14695981039346656037ULL;
            auto mix   = [&h](uint64_t val) {
                h ^= val;
                h *= 1099511628211ULL;
            };
            mix(static_cast< uint64_t >(e.kind));
            mix(e.constant_val);
            mix(e.var_index);
            for (const auto &child : e.children) { mix(HashExpr(*child)); }
            return h;
        }

        WorkItem CloneWorkItem(const WorkItem &item) {
            WorkItem clone;
            std::visit(
                [&clone](const auto &p) {
                    using T = std::decay_t< decltype(p) >;
                    if constexpr (std::is_same_v< T, AstPayload >) {
                        clone.payload = AstPayload{
                            .expr           = CloneExpr(*p.expr),
                            .classification = p.classification,
                            .provenance     = p.provenance,
                        };
                    } else if constexpr (std::is_same_v< T, SignatureStatePayload >) {
                        clone.payload = p;
                    } else {
                        clone.payload = CandidatePayload{
                            .expr           = CloneExpr(*p.expr),
                            .real_vars      = p.real_vars,
                            .cost           = p.cost,
                            .producing_pass = p.producing_pass,
                            .needs_original_space_verification =
                                p.needs_original_space_verification,
                        };
                    }
                },
                item.payload
            );
            clone.features     = item.features;
            clone.metadata     = item.metadata;
            clone.depth        = item.depth;
            clone.rewrite_gen  = item.rewrite_gen;
            clone.stage_cursor = item.stage_cursor;
            clone.history      = item.history;
            return clone;
        }

        bool IsStrictMixedRewriteItem(const WorkItem &item, const OrchestratorPolicy &policy) {
            if (!policy.strict_route_faithful) { return false; }
            if (!std::holds_alternative< AstPayload >(item.payload)) { return false; }
            auto prov = item.features.provenance;
            if (prov != Provenance::kLowered && prov != Provenance::kRewritten) {
                return false;
            }
            if (!item.features.classification) { return false; }
            return item.features.classification->route == Route::kMixedRewrite;
        }
    } // namespace

    StateFingerprint
    ComputeFingerprint(const WorkItem &item, uint32_t bitwidth, bool normalize_stage_cursor) {
        StateFingerprint fp;
        fp.kind         = GetStateKind(item.payload);
        fp.bitwidth     = bitwidth;
        fp.provenance   = item.features.provenance;
        fp.stage_cursor = normalize_stage_cursor ? 0 : item.stage_cursor;

        std::visit(
            [&fp](const auto &payload) {
                using T = std::decay_t< decltype(payload) >;
                if constexpr (std::is_same_v< T, AstPayload >) {
                    fp.payload_hash = HashExpr(*payload.expr);
                    fp.vars         = {};
                } else if constexpr (std::is_same_v< T, SignatureStatePayload >) {
                    uint64_t h = 14695981039346656037ULL;
                    for (uint64_t v : payload.sig) {
                        h ^= v;
                        h *= 1099511628211ULL;
                    }
                    fp.payload_hash = h;
                    fp.vars         = payload.real_vars;
                } else {
                    // CandidatePayload
                    uint64_t h  = HashExpr(*payload.expr);
                    h          ^= payload.needs_original_space_verification ? 0x1ULL : 0x0ULL;
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

    size_t StateFingerprintHash::operator()(const StateFingerprint &fp) const {
        size_t h = std::hash< uint64_t >{}(fp.payload_hash);
        h ^= std::hash< int >{}(static_cast< int >(fp.kind)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash< uint32_t >{}(fp.bitwidth) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash< int >{}(static_cast< int >(fp.provenance)) + 0x9e3779b9 + (h << 6)
            + (h >> 2);
        h ^= std::hash< uint32_t >{}(fp.stage_cursor) + 0x9e3779b9 + (h << 6) + (h >> 2);
        for (const auto &v : fp.vars) {
            h ^= std::hash< std::string >{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }

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
    // Scheduler
    // ---------------------------------------------------------------

    namespace {
        // Stage-gated passes for strict MixedRewrite pipeline.
        // Maps stage_cursor -> PassId for stages 0..6.
        constexpr int kMixedRewriteMaxStage = 6;

        PassId MixedRewriteStagePass(uint32_t stage) {
            switch (stage) {
                case 0:
                    return PassId::kBuildSignatureState;
                case 1:
                    return PassId::kDecompose;
                case 2:
                    return PassId::kOperandSimplify;
                case 3:
                    return PassId::kProductIdentityCollapse;
                case 4:
                    return PassId::kDecompose;
                case 5:
                    return PassId::kXorLowering;
                case 6:
                    return PassId::kBuildSignatureState;
                default:
                    return PassId::kBuildSignatureState;
            }
        }

        std::vector< PassId > FullBand1Passes() {
            return {
                PassId::kBuildSignatureState, PassId::kDecompose,
                PassId::kOperandSimplify,     PassId::kProductIdentityCollapse,
                PassId::kXorLowering,
            };
        }

        void ScheduleFoldedAst(
            const WorkItem &item, const OrchestratorPolicy &policy,
            std::vector< PassId > &passes
        ) {
            auto prov  = item.features.provenance;
            auto route = item.features.classification ? item.features.classification->route
                                                      : Route::kBitwiseOnly;

            if (prov == Provenance::kOriginal) {
                if (item.features.classification
                    && item.features.classification->semantic == SemanticClass::kSemilinear)
                {
                    passes.push_back(PassId::kTrySemilinearPass);
                }
                return;
            }

            if (prov == Provenance::kLowered) {
                if (route == Route::kUnsupported) { return; }

                if (route != Route::kMixedRewrite || !policy.strict_route_faithful) {
                    if (policy.allow_reroute && route == Route::kMixedRewrite) {
                        passes = FullBand1Passes();
                    } else {
                        passes.push_back(PassId::kBuildSignatureState);
                    }
                    return;
                }

                // Strict MixedRewrite: stage cursor gates
                if (item.stage_cursor <= static_cast< uint32_t >(kMixedRewriteMaxStage)) {
                    passes.push_back(MixedRewriteStagePass(item.stage_cursor));
                }
                return;
            }

            // Provenance::kRewritten
            if (policy.allow_reroute) {
                passes = FullBand1Passes();
                return;
            }

            // Strict MixedRewrite: kRewritten items need a
            // supported-solve reentry (BuildSignatureState) first.
            // Beyond stage 5, no more passes are applicable.
            if (policy.strict_route_faithful && route == Route::kMixedRewrite) {
                if (item.stage_cursor <= 5) { passes.push_back(PassId::kBuildSignatureState); }
                return;
            }

            // Non-MixedRewrite kRewritten: supported solve only
            passes.push_back(PassId::kBuildSignatureState);
        }
    } // namespace

    std::vector< PassId > SchedulePasses(
        const WorkItem &item, const OrchestratorPolicy &policy,
        const PassAttemptCache &attempted
    ) {
        std::vector< PassId > passes;
        auto kind = GetStateKind(item.payload);

        switch (kind) {
            case StateKind::kCandidateExpr: {
                const auto &cand = std::get< CandidatePayload >(item.payload);
                if (cand.needs_original_space_verification) {
                    passes.push_back(PassId::kVerifyCandidate);
                }
                break;
            }
            case StateKind::kSignatureState:
                passes.push_back(PassId::kSupportedSolve);
                break;
            case StateKind::kFoldedAst:
                ScheduleFoldedAst(item, policy, passes);
                break;
        }

        auto fp = ComputeFingerprint(item, 64, policy.allow_reroute);
        std::erase_if(passes, [&](PassId id) { return attempted.HasAttempted(fp, id); });

        return passes;
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
        return false;
    }

    // ---------------------------------------------------------------
    // PassId hash for unordered_map keying
    // ---------------------------------------------------------------

    namespace {
        struct PassIdHash
        {
            size_t operator()(PassId id) const {
                return std::hash< uint8_t >{}(static_cast< uint8_t >(id));
            }
        };
    } // namespace

    // ---------------------------------------------------------------
    // OrchestrateSimplify — main orchestrator loop
    // ---------------------------------------------------------------

    namespace {

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

    } // namespace

    Result< OrchestratorResult > OrchestrateSimplify(
        const Expr *input_expr, const std::vector< uint64_t > &sig,
        const std::vector< std::string > &vars, const Options &opts,
        const OrchestratorPolicy &policy
    ) {
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
                auto early      = std::move(*seed_result.value());
                early.telemetry = std::move(telemetry);
                return Ok(std::move(early));
            }
        } else {
            auto seed_result = SeedWithAst(*input_expr, context, worklist);
            if (!seed_result.has_value()) { return std::unexpected(seed_result.error()); }
            if (seed_result.value().has_value()) {
                auto early      = std::move(*seed_result.value());
                early.telemetry = std::move(telemetry);
                return Ok(std::move(early));
            }
        }

        // Build registry lookup map
        const auto &registry = GetPassRegistry();
        std::unordered_map< PassId, const PassDescriptor *, PassIdHash > registry_map;
        for (const auto &desc : registry) { registry_map[desc.id] = &desc; }
        PassAttemptCache cache;
        uint32_t expansions    = 0;
        uint32_t verifications = 0;
        std::optional< UnsupportedCandidate > best_unsupported;
        std::vector< ReasonFrame > decomp_causes;
        // XOR lowering terminal state tracked independently so a
        // later best_unsupported replacement cannot erase it.
        std::optional< ReasonCategory > xor_lowering_terminal;

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

            // Candidate acceptance: verified candidates are immediately returned
            if (auto *cand = std::get_if< CandidatePayload >(&item.payload)) {
                if (!cand->needs_original_space_verification) {
                    telemetry.queue_high_water = worklist.HighWaterMark();
                    return Ok(
                        OrchestratorResult{
                            .outcome = PassOutcome::Success(
                                CloneExpr(*cand->expr), cand->real_vars,
                                item.metadata.verification
                            ),
                            .metadata     = std::move(item.metadata),
                            .run_metadata = context.run_metadata,
                            .telemetry    = std::move(telemetry),
                        }
                    );
                }
            }

            // Schedule and execute passes
            auto passes = SchedulePasses(item, policy, cache);

            bool progressed         = false;
            bool retained_for_stage = false;

            for (auto pass_id : passes) {
                if (pass_id == PassId::kVerifyCandidate
                    && verifications >= policy.max_candidates)
                {
                    continue;
                }

                auto it = registry_map.find(pass_id);
                if (it == registry_map.end()) { continue; }

                telemetry.passes_attempted.push_back(pass_id);
                auto result = it->second->run(item, context);
                if (pass_id == PassId::kVerifyCandidate) {
                    ++verifications;
                    telemetry.candidates_verified = verifications;
                }
                if (!result.has_value()) { return std::unexpected(result.error()); }

                auto fp = ComputeFingerprint(item, context.bitwidth, policy.allow_reroute);
                cache.Record(fp, pass_id);

                auto &pr = result.value();
                if (pr.decision == PassDecision::kSolvedCandidate
                    || pr.decision == PassDecision::kAdvance)
                {
                    for (auto &next : pr.next) {
                        next.depth = item.depth + 1;
                        next.history.push_back(pass_id);
                        worklist.Push(std::move(next));
                    }

                    // Stage advancement: when a pass retains the source
                    // item, re-push it at the next stage so the pipeline
                    // continues if the emitted children fail. Depth is
                    // set to item.depth+2 so the children (at depth+1)
                    // are processed first — the fallback stage only runs
                    // if no child solves the expression.
                    if (pr.disposition == ItemDisposition::kRetainCurrent
                        && IsStrictMixedRewriteItem(item, policy)
                        && item.stage_cursor < static_cast< uint32_t >(kMixedRewriteMaxStage))
                    {
                        auto retained         = CloneWorkItem(item);
                        retained.stage_cursor = item.stage_cursor + 1;
                        retained.depth        = item.depth + 2;
                        worklist.Push(std::move(retained));
                        retained_for_stage = true;
                    }

                    progressed = true;
                    break; // first-progress-yields
                }

                // On blocked/no-progress, update last_failure tracking
                if (pr.decision == PassDecision::kBlocked
                    || pr.decision == PassDecision::kNoProgress)
                {
                    if (!pr.reason.top.message.empty()) {
                        item.metadata.last_failure              = pr.reason;
                        best_unsupported->metadata.last_failure = pr.reason;
                    }
                    // Track XOR lowering terminal states for
                    // MixedRewrite reason-code derivation. Stored
                    // in a loop-scoped variable so a later
                    // best_unsupported replacement cannot erase it.
                    if (pass_id == PassId::kXorLowering) {
                        auto cat              = pr.reason.top.code.category;
                        xor_lowering_terminal = cat;
                        if (cat == ReasonCategory::kRepresentationGap) {
                            item.metadata.rewrite_produced_candidate              = true;
                            best_unsupported->metadata.rewrite_produced_candidate = true;
                        } else if (cat == ReasonCategory::kVerifyFailed) {
                            item.metadata.rewrite_produced_candidate                 = true;
                            item.metadata.candidate_failed_verification              = true;
                            best_unsupported->metadata.rewrite_produced_candidate    = true;
                            best_unsupported->metadata.candidate_failed_verification = true;
                        }
                    }
                    // Accumulate decomposition failure causes for
                    // the cause chain, matching the legacy's
                    // decomp_causes accumulation.
                    if (pass_id == PassId::kDecompose) {
                        decomp_causes.push_back(pr.reason.top);
                        for (const auto &c : pr.reason.causes) { decomp_causes.push_back(c); }
                    }
                }
            }

            // All scheduled passes exhausted without progress:
            // advance the stage cursor so the next MixedRewrite step
            // fires on the next iteration.
            if (!progressed && !retained_for_stage && IsStrictMixedRewriteItem(item, policy)
                && item.stage_cursor < static_cast< uint32_t >(kMixedRewriteMaxStage))
            {
                item.stage_cursor++;
                worklist.Push(std::move(item));
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

        // For MixedRewrite routes, derive the reason code from the
        // pipeline terminal state to match legacy behavior. The
        // xor_lowering_terminal variable is authoritative — it
        // survives best_unsupported replacements that could erase
        // the per-item metadata flags.
        bool is_mixed_route =
            context.run_metadata.input_classification.route == Route::kMixedRewrite;
        if (is_mixed_route && !final_meta.reason_code.has_value()) {
            if (xor_lowering_terminal == ReasonCategory::kVerifyFailed) {
                final_meta.reason_code =
                    ReasonCode{ ReasonCategory::kVerifyFailed, ReasonDomain::kMixedRewrite, 0 };
                final_meta.candidate_failed_verification = true;
                final_meta.rewrite_produced_candidate    = true;
            } else if (xor_lowering_terminal == ReasonCategory::kRepresentationGap) {
                final_meta.reason_code = ReasonCode{ ReasonCategory::kRepresentationGap,
                                                     ReasonDomain::kMixedRewrite, 0 };
                final_meta.rewrite_produced_candidate = true;
            } else if (xor_lowering_terminal == ReasonCategory::kSearchExhausted) {
                final_meta.reason_code = ReasonCode{ ReasonCategory::kSearchExhausted,
                                                     ReasonDomain::kMixedRewrite, 0 };
            } else {
                final_meta.reason_code = ReasonCode{ ReasonCategory::kSearchExhausted,
                                                     ReasonDomain::kMixedRewrite, 0 };
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
        if (final_meta.cause_chain.empty() && !is_mixed_route
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
        return Ok(
            OrchestratorResult{
                .outcome      = PassOutcome::Blocked(std::move(exhaustion_reason)),
                .metadata     = std::move(final_meta),
                .run_metadata = context.run_metadata,
                .telemetry    = std::move(telemetry),
            }
        );
    }

    // ---------------------------------------------------------------
    // AdaptToLegacy — convert OrchestratorResult to SimplifyOutcome
    // ---------------------------------------------------------------

    SimplifyOutcome AdaptToLegacy(OrchestratorResult result, const Expr *original_expr) {
        SimplifyOutcome outcome;

        if (result.outcome.Succeeded()) {
            outcome.kind       = SimplifyOutcome::Kind::kSimplified;
            outcome.expr       = result.outcome.TakeExpr();
            outcome.real_vars  = result.outcome.RealVars();
            outcome.verified   = result.metadata.verification == VerificationState::kVerified;
            outcome.sig_vector = std::move(result.metadata.sig_vector);
        } else {
            outcome.kind = SimplifyOutcome::Kind::kUnchangedUnsupported;
            outcome.expr = original_expr != nullptr ? CloneExpr(*original_expr) : nullptr;
        }

        outcome.diag.classification             = result.run_metadata.input_classification;
        outcome.diag.attempted_route            = result.metadata.attempted_route;
        outcome.diag.rewrite_rounds             = result.metadata.rewrite_rounds;
        outcome.diag.rewrite_produced_candidate = result.metadata.rewrite_produced_candidate;
        outcome.diag.candidate_failed_verification =
            result.metadata.candidate_failed_verification;
        outcome.diag.reason_code = result.metadata.reason_code;
        outcome.diag.cause_chain = std::move(result.metadata.cause_chain);

        if (!result.outcome.Succeeded()) {
            outcome.diag.reason = result.outcome.Reason().top.message;
        }

        return outcome;
    }

    // ---------------------------------------------------------------
    // OrchestrateSimplifyForTest — convenience wrapper
    // ---------------------------------------------------------------

    Result< SimplifyOutcome > OrchestrateSimplifyForTest(
        const Expr *input_expr, const std::vector< uint64_t > &sig,
        const std::vector< std::string > &vars, const Options &opts,
        const OrchestratorPolicy &policy
    ) {
        auto result = OrchestrateSimplify(input_expr, sig, vars, opts, policy);
        if (!result.has_value()) { return std::unexpected(result.error()); }
        return Ok(AdaptToLegacy(std::move(result.value()), input_expr));
    }

} // namespace cobra
