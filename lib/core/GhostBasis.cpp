#include "cobra/core/GhostBasis.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/GhostResidualSolver.h"
#include <cassert>

namespace cobra {

    namespace {

        // Each eval/build pair takes a span sized exactly `arity`.
        // Asserting at entry pins the producer/consumer contract: a
        // future caller that passes a too-small span fails loud instead
        // of reading past the underlying buffer.
        constexpr uint8_t kMulSubAndArity   = 2;
        constexpr uint8_t kMul3SubAnd3Arity = 3;

        static_assert(
            kMulSubAndArity <= kMaxResidualSupport && kMul3SubAnd3Arity <= kMaxResidualSupport,
            "Ghost primitive arity exceeds kMaxResidualSupport — "
            "consumer buffers (combo, args, var_indices) would overflow"
        );

        // mul_sub_and: x*y - (x&y)
        uint64_t EvalMulSubAnd(std::span< const uint64_t > args, uint32_t bw) {
            assert(args.size() == kMulSubAndArity);
            const uint64_t kMask = Bitmask(bw);
            return ((args[0] * args[1]) - (args[0] & args[1])) & kMask;
        }

        std::unique_ptr< Expr > BuildMulSubAnd(std::span< const uint32_t > vars) {
            assert(vars.size() == kMulSubAndArity);
            return Expr::Add(
                Expr::Mul(Expr::Variable(vars[0]), Expr::Variable(vars[1])),
                Expr::Negate(Expr::BitwiseAnd(Expr::Variable(vars[0]), Expr::Variable(vars[1])))
            );
        }

        // mul3_sub_and3: x*y*z - (x&y&z)
        uint64_t EvalMul3SubAnd3(std::span< const uint64_t > args, uint32_t bw) {
            assert(args.size() == kMul3SubAnd3Arity);
            const uint64_t kMask = Bitmask(bw);
            return ((args[0] * args[1] * args[2]) - (args[0] & args[1] & args[2])) & kMask;
        }

        std::unique_ptr< Expr > BuildMul3SubAnd3(std::span< const uint32_t > vars) {
            assert(vars.size() == kMul3SubAnd3Arity);
            return Expr::Add(
                Expr::Mul(
                    Expr::Mul(Expr::Variable(vars[0]), Expr::Variable(vars[1])),
                    Expr::Variable(vars[2])
                ),
                Expr::Negate(
                    Expr::BitwiseAnd(
                        Expr::BitwiseAnd(Expr::Variable(vars[0]), Expr::Variable(vars[1])),
                        Expr::Variable(vars[2])
                    )
                )
            );
        }

    } // namespace

    const std::vector< GhostPrimitive > &GetGhostBasis() {
        static const std::vector< GhostPrimitive > kBasis = {
            {   .name      = "mul_sub_and",
             .arity     = kMulSubAndArity,
             .symmetric = true,
             .eval      = EvalMulSubAnd,
             .build     = BuildMulSubAnd  },
            { .name      = "mul3_sub_and3",
             .arity     = kMul3SubAnd3Arity,
             .symmetric = true,
             .eval      = EvalMul3SubAnd3,
             .build     = BuildMul3SubAnd3 },
        };
        return kBasis;
    }

} // namespace cobra
