#include "cobra/core/SelfCheck.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SemilinearIR.h"
#include "cobra/core/SemilinearNormalizer.h"
#include "cobra/core/Trace.h"
#include <cstdint>
#include <ios>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cobra {

    namespace {

        using CoeffMap = std::unordered_map< AtomKey, uint64_t, AtomKeyHash >;

        CoeffMap BuildCoeffMap(const SemilinearIR &ir, uint64_t mask) {
            CoeffMap result;
            for (const auto &term : ir.terms) {
                const AtomKey &key = ir.atom_table[term.atom_id].key;
                result[key]        = (result[key] + term.coeff) & mask;
            }
            // Remove zero-coefficient entries
            for (auto it = result.begin(); it != result.end();) {
                if (it->second == 0) {
                    it = result.erase(it);
                } else {
                    ++it;
                }
            }
            return result;
        }

    } // namespace

    SelfCheckResult SelfChecSemilinear(
        const SemilinearIR &original_ir, const Expr &reconstructed,
        const std::vector< std::string > &vars, uint32_t bitwidth
    ) {
        COBRA_TRACE(
            "SelfCheck", "SelfChecSemilinear: vars={} bitwidth={}", vars.size(), bitwidth
        );
        auto re_result = NormalizeToSemilinear(reconstructed, vars, bitwidth);
        if (!re_result.has_value()) {
            return { .passed = false,
                     .mismatch_detail =
                         "re-normalization failed: " + re_result.error().message };
        }
        const SemilinearIR &re = re_result.value();

        const uint64_t kMask = Bitmask(bitwidth);

        if ((original_ir.constant & kMask) != (re.constant & kMask)) {
            std::ostringstream oss;
            oss << "constant mismatch: original=0x" << std::hex
                << (original_ir.constant & kMask) << " re=0x" << (re.constant & kMask);
            return { .passed = false, .mismatch_detail = oss.str() };
        }

        const CoeffMap kOrigMap = BuildCoeffMap(original_ir, kMask);
        CoeffMap re_map         = BuildCoeffMap(re, kMask);

        if (kOrigMap.size() != re_map.size()) {
            std::ostringstream oss;
            oss << "term count mismatch: original=" << kOrigMap.size()
                << " re=" << re_map.size();
            return { .passed = false, .mismatch_detail = oss.str() };
        }

        for (const auto &[key, coeff] : kOrigMap) {
            auto it = re_map.find(key);
            if (it == re_map.end()) {
                return { .passed = false,
                         .mismatch_detail =
                             "atom present in original but missing in re-normalized" };
            }
            if (it->second != coeff) {
                std::ostringstream oss;
                oss << "coefficient mismatch for atom: original=0x" << std::hex << coeff
                    << " re=0x" << it->second;
                return { .passed = false, .mismatch_detail = oss.str() };
            }
        }

        auto result = SelfCheckResult{ .passed = true, .mismatch_detail = "" };
        COBRA_TRACE("SelfCheck", "SelfChecSemilinear: passed={}", result.passed);
        return result;
    }

} // namespace cobra
