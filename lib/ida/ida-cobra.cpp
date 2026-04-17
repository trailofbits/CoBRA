#include <cobra/core/ExprCost.h>
#include <cobra/core/Simplifier.h>

#include "MicrocodeConverter.h"
#include "MicrocodeDetector.h"
#include "Verifier.h"

#define ACTION_NAME "ida_cobra:run"

struct plugin_ctx_t;

struct run_ah_t : public action_handler_t
{
    plugin_ctx_t *ctx;

    explicit run_ah_t(plugin_ctx_t *c) : ctx(c) {}

    int idaapi activate(action_activation_ctx_t *act_ctx) override;

    action_state_t idaapi update(action_update_ctx_t *upd_ctx) override {
        return upd_ctx->widget_type == BWN_PSEUDOCODE ? AST_ENABLE_FOR_WIDGET
                                                      : AST_DISABLE_FOR_WIDGET;
    }
};

struct plugin_ctx_t : public plugmod_t
{
    bool run_automatically = false;
    bool active            = false;

    run_ah_t action_handler;

    plugin_ctx_t();

    ~plugin_ctx_t() { term_hexrays_plugin(); }

    bool idaapi run(size_t) override { return true; }
};

static ssize_t idaapi hex_callback(void *ud, hexrays_event_t event, va_list va) {
    auto *ctx = static_cast< plugin_ctx_t * >(ud);

    switch (event) {
        case hxe_microcode: {
            auto *mba = va_arg(va, mba_t *);
            if (ctx->run_automatically) { ctx->active = true; }
            if (ctx->active) { mba->set_mba_flags2(MBA2_PROP_COMPLEX); }
            break;
        }
        case hxe_populating_popup: {
            auto *widget = va_arg(va, TWidget *);
            auto *popup  = va_arg(va, TPopupMenu *);
            attach_action_to_popup(widget, popup, ACTION_NAME);
            break;
        }
        case hxe_glbopt: {
            auto *mba = va_arg(va, mba_t *);

            if (!ctx->active) { return MERR_OK; }

            auto candidates = ida_cobra::DetectMbaCandidatesCrossBlock(*mba);
            int improved    = 0;

            for (auto &cand : candidates) {
                auto expr = ida_cobra::BuildExprFromMinsn(*cand.root, cand);
                if (!expr) { continue; }

                cobra::Options opts;
                opts.bitwidth = cand.bitwidth;

                auto result = cobra::Simplify(cand.sig, cand.var_names, expr.get(), opts);
                if (!result.has_value()) { continue; }

                auto &outcome = result.value();
                if (outcome.kind != cobra::SimplifyOutcome::Kind::kSimplified) { continue; }
                if (!outcome.expr) { continue; }

                auto original_cost   = cobra::ComputeCost(*expr);
                auto simplified_cost = cobra::ComputeCost(*outcome.expr);
                if (!cobra::IsBetter(simplified_cost.cost, original_cost.cost)) { continue; }

                if (!ida_cobra::ProbablyEquivalent(*cand.root, *outcome.expr, cand)) {
                    continue;
                }

                minsn_t *replacement =
                    ida_cobra::ReconstructMinsn(*outcome.expr, cand, outcome.real_vars);
                if (replacement == nullptr) { continue; }

                replacement->d.swap(cand.root->d);
                cand.root->swap(*replacement);
                delete replacement;

                improved++;
            }

            ctx->active = false;
            mba->clr_mba_flags2(MBA2_PROP_COMPLEX);

            if (improved > 0) {
                mba->verify(true);
                msg("ida-cobra: simplified %d MBA expression(s) in %a\n", improved,
                    mba->entry_ea);
                // Tag the function so scripts can query which functions were simplified.
                netnode n(mba->entry_ea);
                n.altset(0, improved, 'C');
                return MERR_LOOP;
            }
            return MERR_OK;
        }
        default:
            break;
    }
    return 0;
}

int idaapi run_ah_t::activate(action_activation_ctx_t *act_ctx) {
    vdui_t *vu = get_widget_vdui(act_ctx->widget);
    if (vu != nullptr) {
        ctx->active = true;
        vu->refresh_view(true);
        return 1;
    }
    return 0;
}

plugin_ctx_t::plugin_ctx_t() : action_handler(this) {
    install_hexrays_callback(hex_callback, this);
    register_action(ACTION_DESC_LITERAL_PLUGMOD(
        ACTION_NAME, "Run CoBRA Optimizer", &action_handler, this, nullptr,
        "Simplify MBA-obfuscated expressions using CoBRA", -1
    ));
}

static plugmod_t *idaapi init() {
    if (!init_hexrays_plugin()) { return nullptr; }

    const char *hxver = get_hexrays_version();
    msg("ida-cobra: Hex-Rays %s detected, CoBRA MBA optimizer ready\n", hxver);

    auto *ctx = new plugin_ctx_t;

    const cfgopt_t cfgopts[] = {
        cfgopt_t("COBRA_RUN_AUTOMATICALLY", &ctx->run_automatically, 1),
    };
    read_config_file("ida-cobra", cfgopts, qnumber(cfgopts), nullptr);

    return ctx;
}

static char comment[] = "CoBRA MBA deobfuscation plugin for Hex-Rays decompiler";

plugin_t PLUGIN = {
    IDP_INTERFACE_VERSION,
    PLUGIN_MULTI | PLUGIN_HIDE,
    init,
    nullptr,
    nullptr,
    comment,
    nullptr,
    "ida-cobra plugin",
    nullptr,
};
