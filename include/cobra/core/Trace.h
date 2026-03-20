#pragma once

#ifdef COBRA_ENABLE_TRACE

    #include "cobra/core/Expr.h"
    #include <cstdint>
    #include <cstdio>
    #include <format>
    #include <print>
    #include <string>
    #include <vector>

    // General trace: [Component] FuncName: key=value ...
    #define COBRA_TRACE(component, ...) \
        do { \
            std::print(stderr, "[" component "] " __VA_ARGS__); \
            std::print(stderr, "\n"); \
        } while (false)

    // Expression trace: renders an Expr and prints it
    #define COBRA_TRACE_EXPR(component, label, expr, vars, bitwidth) \
        do { \
            auto trace_text_ = cobra::Render((expr), (vars), (bitwidth)); \
            std::print(stderr, "[" component "] {}: {}\n", (label), trace_text_); \
        } while (false)

    // Signature trace: prints a vector of uint64_t values
    #define COBRA_TRACE_SIG(component, label, sig) \
        do { \
            std::string trace_buf_ = "["; \
            for (size_t trace_i_ = 0; trace_i_ < (sig).size(); ++trace_i_) { \
                if (trace_i_ > 0) { trace_buf_ += ", "; } \
                trace_buf_ += std::to_string((sig)[trace_i_]); \
            } \
            trace_buf_ += "]"; \
            std::print(stderr, "[" component "] {}: {}\n", (label), trace_buf_); \
        } while (false)

#else

    #define COBRA_TRACE(component, ...)                              ((void) 0)
    #define COBRA_TRACE_EXPR(component, label, expr, vars, bitwidth) ((void) 0)
    #define COBRA_TRACE_SIG(component, label, sig)                   ((void) 0)

#endif
