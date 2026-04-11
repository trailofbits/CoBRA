#pragma once

#include "cobra/core/Classification.h"

#include <cstddef>
#include <string>

namespace cobra::test_support {

    inline size_t find_separator(const std::string &line) {
        // Prefer tab separator (unambiguous), fall back to the last
        // top-level comma for legacy two-column datasets.
        int depth         = 0;
        size_t last_comma = std::string::npos;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '(') {
                ++depth;
            } else if (line[i] == ')') {
                --depth;
            } else if (line[i] == '\t' && depth == 0) {
                return i;
            } else if (line[i] == ',' && depth == 0) {
                last_comma = i;
            }
        }
        return last_comma;
    }

    inline std::string trim(const std::string &s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { return ""; }
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    inline std::string semantic_str(SemanticClass sc) {
        switch (sc) {
            case SemanticClass::kLinear:
                return "linear";
            case SemanticClass::kSemilinear:
                return "semilinear";
            case SemanticClass::kPolynomial:
                return "polynomial";
            case SemanticClass::kNonPolynomial:
                return "non-polynomial";
        }
        return "?";
    }

    inline std::string flags_str(StructuralFlag flags) {
        std::string s;
        auto append = [&](StructuralFlag f, const char *name) {
            if (HasFlag(flags, f)) {
                if (!s.empty()) { s += "|"; }
                s += name;
            }
        };
        append(kSfHasBitwise, "BW");
        append(kSfHasArithmetic, "Arith");
        append(kSfHasMul, "Mul");
        append(kSfHasMultilinearProduct, "MultilinProd");
        append(kSfHasSingletonPower, "SingPow");
        append(kSfHasSingletonPowerGt2, "SingPow>2");
        append(kSfHasMixedProduct, "MixedProd");
        append(kSfHasBitwiseOverArith, "BoA");
        append(kSfHasArithOverBitwise, "AoB");
        append(kSfHasMultivarHighPower, "MultivarHiPow");
        append(kSfHasUnknownShape, "Unknown");
        return s.empty() ? "none" : s;
    }

} // namespace cobra::test_support
