#pragma once

#include "cobra/core/Classification.h"
#include "cobra/core/Expr.h"
#include "cobra/core/PassContract.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cobra {

    struct Diagnostic
    {
        Classification classification        = { .semantic = SemanticClass::kLinear,
                                                 .flags    = kSfNone };
        uint32_t structural_transform_rounds = 0;
        bool transform_produced_candidate    = false;
        bool candidate_failed_verification   = false;
        std::string reason;

        std::optional< ReasonCode > reason_code;
        std::vector< ReasonFrame > cause_chain;
    };

    struct SimplifyTelemetry
    {
        uint32_t total_expansions    = 0;
        uint32_t max_depth_reached   = 0;
        uint32_t candidates_verified = 0;
        uint32_t queue_high_water    = 0;
    };

    struct SimplifyOutcome
    {
        enum class Kind { kSimplified, kUnchangedUnsupported, kError };

        Kind kind = Kind::kSimplified;
        std::unique_ptr< Expr > expr;
        std::vector< uint64_t > sig_vector;
        std::vector< std::string > real_vars;
        bool verified = false;
        Diagnostic diag;
        SimplifyTelemetry telemetry;
    };

} // namespace cobra
