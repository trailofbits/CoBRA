// Semantic bitwise audit: detect expressions where the boolean
// signature image is in {0,1} or {c, c+1} (semantically bitwise)
// and report their current classification and pipeline routing.
//
// This evaluates whether RUMBA theorem 19/20 semantic bitwise
// recognition would provide a useful fast off-ramp.
//
// Usage: ./bench_semantic_bitwise_audit (manual, not in CI)

#include "ExprParser.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace cobra;
using Clock = std::chrono::high_resolution_clock;

namespace {

    enum class SigShape {
        kBoolean,       // image ⊆ {0, 1}
        kNegBoolean,    // image ⊆ {-2, -1} mod 2^bw
        kAffineBoolean, // image ⊆ {c, c+1} for some c
        kOther,
    };

    SigShape ClassifySigShape(const std::vector< uint64_t > &sig, uint32_t bitwidth) {
        if (sig.empty()) { return SigShape::kOther; }

        uint64_t mask = Bitmask(bitwidth);

        // Check {0, 1}
        bool is_boolean = true;
        for (auto v : sig) {
            if (v != 0 && v != 1) {
                is_boolean = false;
                break;
            }
        }
        if (is_boolean) { return SigShape::kBoolean; }

        // Check {-2, -1} mod 2^bw
        uint64_t neg1       = mask; // -1 mod 2^bw
        uint64_t neg2       = (mask - 1) & mask;
        bool is_neg_boolean = true;
        for (auto v : sig) {
            if (v != neg1 && v != neg2) {
                is_neg_boolean = false;
                break;
            }
        }
        if (is_neg_boolean) { return SigShape::kNegBoolean; }

        // Check {c, c+1} for any c
        std::set< uint64_t > values(sig.begin(), sig.end());
        if (values.size() <= 2) {
            auto it = values.begin();
            auto lo = *it;
            if (values.size() == 1) { return SigShape::kAffineBoolean; }
            ++it;
            auto hi = *it;
            if (hi == ((lo + 1) & mask)) { return SigShape::kAffineBoolean; }
        }

        return SigShape::kOther;
    }

    const char *SigShapeName(SigShape s) {
        switch (s) {
            case SigShape::kBoolean:
                return "boolean";
            case SigShape::kNegBoolean:
                return "neg_boolean";
            case SigShape::kAffineBoolean:
                return "affine_bool";
            case SigShape::kOther:
                return "other";
        }
        return "?";
    }

    const char *SemanticClassName(SemanticClass c) {
        switch (c) {
            case SemanticClass::kLinear:
                return "Linear";
            case SemanticClass::kSemilinear:
                return "Semilinear";
            case SemanticClass::kPolynomial:
                return "Polynomial";
            case SemanticClass::kNonPolynomial:
                return "NonPoly";
        }
        return "?";
    }

    struct AuditRow
    {
        uint32_t line          = 0;
        SigShape shape         = SigShape::kOther;
        SemanticClass semantic = SemanticClass::kLinear;
        StructuralFlag flags   = static_cast< StructuralFlag >(0);
        uint32_t num_vars      = 0;
        uint32_t node_count    = 0;
        double solve_us        = 0;
        bool simplified        = false;
    };

    uint32_t CountNodes(const Expr &e) {
        uint32_t n = 1;
        for (const auto &c : e.children) { n += CountNodes(*c); }
        return n;
    }

    void RunAudit(const std::string &name, const std::string &path) {
        std::ifstream in(path);
        ASSERT_TRUE(in.good()) << "Cannot open " << path;

        std::vector< AuditRow > rows;
        std::string line;
        uint32_t lineno = 0;

        while (std::getline(in, line)) {
            lineno++;
            if (line.empty() || line[0] == '#') { continue; }
            auto sep = line.find(',');
            if (sep == std::string::npos) { sep = line.find('\t'); }
            std::string expr_str = (sep != std::string::npos) ? line.substr(0, sep) : line;

            auto ast = ParseToAst(expr_str, 64);
            if (!ast.has_value()) { continue; }

            auto nv = static_cast< uint32_t >(ast->vars.size());
            if (nv > 16) { continue; }

            auto sig = EvaluateBooleanSignature(*ast->expr, nv, 64);
            auto cls = ClassifyStructural(*ast->expr);

            AuditRow r;
            r.line       = lineno;
            r.shape      = ClassifySigShape(sig, 64);
            r.semantic   = cls.semantic;
            r.flags      = cls.flags;
            r.num_vars   = nv;
            r.node_count = CountNodes(*ast->expr);

            // Time the solve
            auto parse = ParseAndEvaluate(expr_str, 64);
            if (parse.has_value()) {
                Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
                auto t0     = Clock::now();
                auto result = Simplify(parse->sig, parse->vars, ast->expr.get(), opts);
                auto t1     = Clock::now();
                r.solve_us  = std::chrono::duration< double, std::micro >(t1 - t0).count();
                r.simplified =
                    result.has_value() && result->kind == SimplifyOutcome::Kind::kSimplified;
            }

            rows.push_back(r);
        }

        // Aggregate
        uint32_t total         = static_cast< uint32_t >(rows.size());
        uint32_t n_boolean     = 0;
        uint32_t n_neg_boolean = 0;
        uint32_t n_affine      = 0;
        uint32_t n_other       = 0;

        // Misrouted: semantically bitwise but classified as non-linear
        uint32_t misrouted  = 0;
        double misrouted_us = 0;

        for (const auto &r : rows) {
            switch (r.shape) {
                case SigShape::kBoolean:
                    n_boolean++;
                    break;
                case SigShape::kNegBoolean:
                    n_neg_boolean++;
                    break;
                case SigShape::kAffineBoolean:
                    n_affine++;
                    break;
                case SigShape::kOther:
                    n_other++;
                    break;
            }
            if (r.shape != SigShape::kOther && r.semantic != SemanticClass::kLinear) {
                misrouted++;
                misrouted_us += r.solve_us;
            }
        }

        std::cout << "\n=== " << name << " Semantic Bitwise Audit ===\n";
        std::cout << "total: " << total << "\n";
        std::cout << "\nSignature shape distribution:\n";
        std::cout << "  boolean {0,1}:      " << n_boolean << "\n";
        std::cout << "  neg_boolean {-2,-1}: " << n_neg_boolean << "\n";
        std::cout << "  affine {c,c+1}:     " << n_affine << "\n";
        std::cout << "  other:              " << n_other << "\n";

        std::cout << "\nSemantically bitwise but classified non-linear: " << misrouted << "\n";
        if (misrouted > 0) {
            std::cout << std::fixed << std::setprecision(0);
            std::cout << "  total solve time: " << misrouted_us << " us\n";
        }

        // Cross-tab: shape × semantic class
        std::cout << "\nCross-tab (shape x semantic class):\n";
        std::cout << std::left << std::setw(14) << "shape" << std::setw(10) << "Linear"
                  << std::setw(12) << "Semilinear" << std::setw(12) << "Polynomial"
                  << std::setw(10) << "NonPoly"
                  << "\n";
        std::cout << std::string(58, '-') << "\n";

        for (auto shape : { SigShape::kBoolean, SigShape::kNegBoolean, SigShape::kAffineBoolean,
                            SigShape::kOther })
        {
            uint32_t lin = 0, semi = 0, poly = 0, nonp = 0;
            for (const auto &r : rows) {
                if (r.shape != shape) { continue; }
                switch (r.semantic) {
                    case SemanticClass::kLinear:
                        lin++;
                        break;
                    case SemanticClass::kSemilinear:
                        semi++;
                        break;
                    case SemanticClass::kPolynomial:
                        poly++;
                        break;
                    case SemanticClass::kNonPolynomial:
                        nonp++;
                        break;
                }
            }
            std::cout << std::left << std::setw(14) << SigShapeName(shape) << std::setw(10)
                      << lin << std::setw(12) << semi << std::setw(12) << poly << std::setw(10)
                      << nonp << "\n";
        }

        // Detail for misrouted expressions
        if (misrouted > 0) {
            std::cout << "\nMisrouted detail (semantically bitwise, non-linear class):\n";
            std::cout << std::left << std::setw(8) << "line" << std::setw(14) << "shape"
                      << std::setw(12) << "class" << std::setw(8) << "vars" << std::setw(8)
                      << "nodes" << std::setw(12) << "solve_us" << std::setw(8) << "ok?"
                      << "\n";
            std::cout << std::string(70, '-') << "\n";
            for (const auto &r : rows) {
                if (r.shape == SigShape::kOther || r.semantic == SemanticClass::kLinear) {
                    continue;
                }
                std::cout << std::left << std::setw(8) << r.line << std::setw(14)
                          << SigShapeName(r.shape) << std::setw(12)
                          << SemanticClassName(r.semantic) << std::setw(8) << r.num_vars
                          << std::setw(8) << r.node_count << std::fixed << std::setprecision(0)
                          << std::setw(12) << r.solve_us << std::setw(8)
                          << (r.simplified ? "yes" : "NO") << "\n";
            }
        }

        // P50/P95 solve times by shape
        std::cout << "\nSolve time by shape (us):\n";
        for (auto shape : { SigShape::kBoolean, SigShape::kNegBoolean, SigShape::kAffineBoolean,
                            SigShape::kOther })
        {
            std::vector< double > times;
            for (const auto &r : rows) {
                if (r.shape == shape) { times.push_back(r.solve_us); }
            }
            if (times.empty()) { continue; }
            std::sort(times.begin(), times.end());
            auto p50 = times[times.size() / 2];
            auto p95 = times[static_cast< size_t >(static_cast< double >(times.size()) * 0.95)];
            std::cout << "  " << std::left << std::setw(14) << SigShapeName(shape)
                      << "n=" << std::setw(6) << times.size() << std::fixed
                      << std::setprecision(0) << "p50=" << std::setw(10) << p50
                      << "p95=" << std::setw(10) << p95 << "\n";
        }
    }

} // namespace

TEST(SemanticBitwiseAudit, MSiMBA) { RunAudit("MSiMBA", DATASET_DIR "/msimba.txt"); }

TEST(SemanticBitwiseAudit, QSynthEA) {
    RunAudit("QSynth EA", DATASET_DIR "/gamba/qsynth_ea.txt");
}

TEST(SemanticBitwiseAudit, Syntia) { RunAudit("Syntia", DATASET_DIR "/gamba/syntia.txt"); }
