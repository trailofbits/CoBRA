#include "cobra/core/CompiledExpr.h"
#include "cobra/core/BitWidth.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cobra {

    CompiledExpr CompileExpr(const Expr &expr, uint32_t bitwidth) {
        struct CompileFrame
        {
            const Expr *node;
            bool emit;
        };

        CompiledExpr compiled{
            .bitwidth   = bitwidth,
            .mask       = Bitmask(bitwidth),
            .arity      = 0,
            .stack_size = 1,
            .program    = {},
        };

        std::vector< CompileFrame > frames;
        frames.reserve(64);
        compiled.program.reserve(64);
        frames.push_back({ .node = &expr, .emit = false });

        while (!frames.empty()) {
            const CompileFrame frame = frames.back();
            frames.pop_back();

            const Expr &node = *frame.node;
            if (frame.emit) {
                compiled.program.push_back({ .kind = node.kind, .operand = node.constant_val });
                continue;
            }

            switch (node.kind) {
                case Expr::Kind::kConstant:
                    compiled.program.push_back(
                        { .kind = node.kind, .operand = node.constant_val & compiled.mask }
                    );
                    break;
                case Expr::Kind::kVariable:
                    compiled.arity = std::max(compiled.arity, node.var_index + 1);
                    compiled.program.push_back(
                        { .kind = node.kind, .operand = node.var_index }
                    );
                    break;
                case Expr::Kind::kNot:
                case Expr::Kind::kNeg:
                case Expr::Kind::kShr:
                    frames.push_back({ .node = &node, .emit = true });
                    frames.push_back({ .node = node.children[0].get(), .emit = false });
                    break;
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kAnd:
                case Expr::Kind::kOr:
                case Expr::Kind::kXor:
                    frames.push_back({ .node = &node, .emit = true });
                    frames.push_back({ .node = node.children[1].get(), .emit = false });
                    frames.push_back({ .node = node.children[0].get(), .emit = false });
                    break;
            }
        }

        size_t depth     = 0;
        size_t max_depth = 0;
        for (const auto &instr : compiled.program) {
            switch (instr.kind) {
                case Expr::Kind::kConstant:
                case Expr::Kind::kVariable:
                    ++depth;
                    max_depth = std::max(max_depth, depth);
                    break;
                case Expr::Kind::kNot:
                case Expr::Kind::kNeg:
                case Expr::Kind::kShr:
                    break;
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kAnd:
                case Expr::Kind::kOr:
                case Expr::Kind::kXor:
                    --depth;
                    break;
            }
        }
        compiled.stack_size = max_depth == 0 ? 1 : max_depth;
        return compiled;
    }

    uint64_t EvalCompiledExpr(
        const CompiledExpr &compiled, const std::vector< uint64_t > &var_values,
        std::vector< uint64_t > &stack
    ) {
        if (stack.size() < compiled.stack_size) { stack.resize(compiled.stack_size); }

        size_t sp = 0;
        for (const auto &instr : compiled.program) {
            switch (instr.kind) {
                case Expr::Kind::kConstant:
                    stack[sp++] = instr.operand;
                    break;
                case Expr::Kind::kVariable:
                    stack[sp++] =
                        var_values[static_cast< size_t >(instr.operand)] & compiled.mask;
                    break;
                case Expr::Kind::kNot:
                    stack[sp - 1] = ModNot(stack[sp - 1], compiled.bitwidth);
                    break;
                case Expr::Kind::kNeg:
                    stack[sp - 1] = ModNeg(stack[sp - 1], compiled.bitwidth);
                    break;
                case Expr::Kind::kShr:
                    stack[sp - 1] = ModShr(stack[sp - 1], instr.operand, compiled.bitwidth);
                    break;
                case Expr::Kind::kAdd:
                    stack[sp - 2] = ModAdd(stack[sp - 2], stack[sp - 1], compiled.bitwidth);
                    --sp;
                    break;
                case Expr::Kind::kMul:
                    stack[sp - 2] = ModMul(stack[sp - 2], stack[sp - 1], compiled.bitwidth);
                    --sp;
                    break;
                case Expr::Kind::kAnd:
                    stack[sp - 2] = (stack[sp - 2] & stack[sp - 1]) & compiled.mask;
                    --sp;
                    break;
                case Expr::Kind::kOr:
                    stack[sp - 2] = (stack[sp - 2] | stack[sp - 1]) & compiled.mask;
                    --sp;
                    break;
                case Expr::Kind::kXor:
                    stack[sp - 2] = (stack[sp - 2] ^ stack[sp - 1]) & compiled.mask;
                    --sp;
                    break;
            }
        }

        return stack[sp - 1];
    }

} // namespace cobra
