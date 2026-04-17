#include <cobra/core/CompiledExpr.h>

#include <random>

#include "MicrocodeDetector.h"
#include "Verifier.h"

namespace ida_cobra {
    namespace {

        constexpr int kNumTests = 256;

        std::mt19937_64 &Rng() {
            thread_local std::mt19937_64 rng{ std::random_device{}() };
            return rng;
        }

        constexpr uint64_t kSpecial[] = { 0, 1, 0xFFFFFFFFFFFFFFFF };
        constexpr int kNumSpecial     = 3;

        uint64_t RandValue() {
            if (std::uniform_int_distribution< int >{ 0, 4 }(Rng()) == 0) {
                return kSpecial[std::uniform_int_distribution< int >{ 0,
                                                                      kNumSpecial - 1 }(Rng())];
            }
            return Rng()();
        }

    } // anonymous namespace

    bool ProbablyEquivalent(
        const minsn_t &original, const cobra::Expr &simplified, const MBACandidate &candidate
    ) {
        uint64_t mask = candidate.bitwidth >= 64 ? ~uint64_t{ 0 }
                                                 : (uint64_t{ 1 } << candidate.bitwidth) - 1;

        cobra::CompiledExpr compiled = cobra::CompileExpr(simplified, candidate.bitwidth);
        std::vector< uint64_t > stack(compiled.stack_size);

        for (int test = 0; test < kNumTests; ++test) {
            std::vector< uint64_t > vals(candidate.leaves.size());

            for (size_t i = 0; i < candidate.leaves.size(); ++i) {
                vals[i] = RandValue() & mask;
            }

            uint64_t original_result =
                ida_cobra::EvalMinsn(original, candidate.leaves, vals, mask);

            uint64_t simplified_result = cobra::EvalCompiledExpr(compiled, vals, stack) & mask;

            if (original_result != simplified_result) {
                msg("ida-cobra: verification FAILED on test %d\n", test);
                msg("  original=%llx simplified=%llx\n",
                    static_cast< unsigned long long >(original_result),
                    static_cast< unsigned long long >(simplified_result));
                return false;
            }
        }

        return true;
    }

    int CountNodes(const minsn_t &insn) {
        struct NodeCounter : public minsn_visitor_t
        {
            int count = 0;

            int idaapi visit_minsn() override {
                count++;
                return 0;
            }
        };

        NodeCounter counter;
        const_cast< minsn_t & >(insn).for_all_insns(counter);
        return counter.count;
    }

} // namespace ida_cobra
