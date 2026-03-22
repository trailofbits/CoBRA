#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

using namespace cobra;

namespace {

    size_t find_separator(const std::string &line) {
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

    std::string trim(const std::string &s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { return ""; }
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    std::string route_str(Route r) {
        switch (r) {
            case Route::kBitwiseOnly:
                return "BitwiseOnly";
            case Route::kMultilinear:
                return "Multilinear";
            case Route::kPowerRecovery:
                return "PowerRecovery";
            case Route::kMixedRewrite:
                return "MixedRewrite";
            case Route::kUnsupported:
                return "Unsupported";
            default:
                return "Unknown";
        }
    }

    std::string semantic_str(SemanticClass s) {
        switch (s) {
            case SemanticClass::kLinear:
                return "Linear";
            case SemanticClass::kSemilinear:
                return "Semilinear";
            case SemanticClass::kPolynomial:
                return "Polynomial";
            case SemanticClass::kNonPolynomial:
                return "NonPoly";
            default:
                return "Unknown";
        }
    }

    std::string flag_str(StructuralFlag f) {
        std::string s;
        if (HasFlag(f, kSfHasBitwise)) { s += "Bw|"; }
        if (HasFlag(f, kSfHasArithmetic)) { s += "Ar|"; }
        if (HasFlag(f, kSfHasMul)) { s += "Mul|"; }
        if (HasFlag(f, kSfHasMultilinearProduct)) { s += "Mlp|"; }
        if (HasFlag(f, kSfHasSingletonPower)) { s += "Pow|"; }
        if (HasFlag(f, kSfHasMixedProduct)) { s += "MxP|"; }
        if (HasFlag(f, kSfHasBitwiseOverArith)) { s += "BoA|"; }
        if (HasFlag(f, kSfHasArithOverBitwise)) { s += "AoB|"; }
        if (HasFlag(f, kSfHasMultivarHighPower)) { s += "HiP|"; }
        if (!s.empty()) { s.pop_back(); }
        return s;
    }

    enum class OSESFamily {
        kArithmeticUnderBitwise,
        kExpandedPolynomialMBA,
        kPolynomialIdentityWrapping,
        kProductInsideBitwise,
        kLargeConstantPolynomial,
        kUncategorized,
    };

    std::string family_str(OSESFamily family) {
        switch (family) {
            case OSESFamily::kArithmeticUnderBitwise:
                return "ArithmeticUnderBitwise";
            case OSESFamily::kExpandedPolynomialMBA:
                return "ExpandedPolynomialMBA";
            case OSESFamily::kPolynomialIdentityWrapping:
                return "PolynomialIdentityWrapping";
            case OSESFamily::kProductInsideBitwise:
                return "ProductInsideBitwise";
            case OSESFamily::kLargeConstantPolynomial:
                return "LargeConstantPolynomial";
            case OSESFamily::kUncategorized:
                return "Uncategorized";
            default:
                return "Unknown";
        }
    }

    const std::vector< OSESFamily > &family_order() {
        static const std::vector< OSESFamily > kOrder = {
            OSESFamily::kArithmeticUnderBitwise,     OSESFamily::kExpandedPolynomialMBA,
            OSESFamily::kPolynomialIdentityWrapping, OSESFamily::kProductInsideBitwise,
            OSESFamily::kLargeConstantPolynomial,    OSESFamily::kUncategorized,
        };
        return kOrder;
    }

    const std::unordered_set< int > &expanded_poly_lines() {
        static const std::unordered_set< int > kLines = { 122, 124, 125, 126, 127,
                                                          245, 246, 247, 248 };
        return kLines;
    }

    const std::unordered_set< int > &poly_wrap_lines() {
        static const std::unordered_set< int > kLines = { 145, 146, 147, 148, 314 };
        return kLines;
    }

    const std::unordered_set< int > &product_inside_lines() {
        static const std::unordered_set< int > kLines = { 24, 156, 316, 319, 320, 322 };
        return kLines;
    }

    const std::unordered_set< int > &large_constant_lines() {
        static const std::unordered_set< int > kLines = { 135, 371, 373, 374, 404 };
        return kLines;
    }

    OSESFamily classify_family(int line_num, const std::string &reason) {
        if (reason.find("full-width verification") != std::string::npos) {
            return OSESFamily::kArithmeticUnderBitwise;
        }
        if (expanded_poly_lines().count(line_num) != 0) {
            return OSESFamily::kExpandedPolynomialMBA;
        }
        if (poly_wrap_lines().count(line_num) != 0) {
            return OSESFamily::kPolynomialIdentityWrapping;
        }
        if (product_inside_lines().count(line_num) != 0) {
            return OSESFamily::kProductInsideBitwise;
        }
        if (large_constant_lines().count(line_num) != 0) {
            return OSESFamily::kLargeConstantPolynomial;
        }
        return OSESFamily::kUncategorized;
    }

    struct UnsupportedEntry
    {
        int line_num      = 0;
        uint32_t num_vars = 0;
        OSESFamily family = OSESFamily::kUncategorized;
        Classification cls;
        std::string reason;
        std::string obfuscated;
        std::string ground_truth;
        std::vector< std::string > vars;
    };

    std::vector< UnsupportedEntry > scan_oses_all() {
        std::ifstream file(DATASET_DIR "/oses/oses_all.txt");
        EXPECT_TRUE(file.is_open());
        if (!file.is_open()) { return {}; }

        std::vector< UnsupportedEntry > entries;
        std::string line;
        int line_num = 0;
        while (std::getline(file, line)) {
            ++line_num;
            if (line.empty()) { continue; }

            size_t sep = find_separator(line);
            if (sep == std::string::npos) { continue; }

            std::string obfuscated = trim(line.substr(0, sep));
            if (obfuscated.empty()) { continue; }

            auto parse_result = ParseAndEvaluate(obfuscated, 64);
            if (!parse_result.has_value()) { continue; }

            auto ast_result = ParseToAst(obfuscated, 64);
            if (!ast_result.has_value()) { continue; }

            auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
                FoldConstantBitwise(std::move(ast_result.value().expr), 64)
            );

            Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
            opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
                return EvalExpr(**folded_ptr, v, 64);
            };

            auto result = Simplify(
                parse_result.value().sig, parse_result.value().vars, folded_ptr->get(), opts
            );
            if (!result.has_value()) { continue; }
            if (result->kind != SimplifyOutcome::Kind::kUnchangedUnsupported) { continue; }

            auto cls  = ClassifyStructural(**folded_ptr);
            auto elim = EliminateAuxVars(parse_result.value().sig, parse_result.value().vars);
            auto rv   = static_cast< uint32_t >(elim.real_vars.size());
            auto gt   = trim(line.substr(sep + 1));

            entries.push_back(
                UnsupportedEntry{
                    .line_num     = line_num,
                    .num_vars     = rv,
                    .family       = classify_family(line_num, result->diag.reason),
                    .cls          = cls,
                    .reason       = result->diag.reason,
                    .obfuscated   = obfuscated,
                    .ground_truth = gt,
                    .vars         = parse_result.value().vars,
                }
            );
        }
        return entries;
    }

    struct OSESLine
    {
        int line_num = 0;
        std::string obfuscated;
    };

    std::vector< OSESLine > load_oses_lines(const std::unordered_set< int > &target_lines) {
        std::ifstream file(DATASET_DIR "/oses/oses_all.txt");
        EXPECT_TRUE(file.is_open());
        if (!file.is_open()) { return {}; }

        std::vector< OSESLine > lines;
        std::string line;
        int line_num = 0;
        while (std::getline(file, line)) {
            ++line_num;
            if (target_lines.count(line_num) == 0) { continue; }

            size_t sep = find_separator(line);
            if (sep == std::string::npos) { continue; }

            std::string obfuscated = trim(line.substr(0, sep));
            if (obfuscated.empty()) { continue; }
            lines.push_back(OSESLine{ .line_num = line_num, .obfuscated = obfuscated });
        }
        return lines;
    }

    std::string join_counts(const std::map< std::string, int > &counts) {
        std::string out;
        for (const auto &[key, value] : counts) {
            if (!out.empty()) { out += " "; }
            out += key + "=" + std::to_string(value);
        }
        return out.empty() ? "-" : out;
    }

    uint32_t expr_size(const Expr &e) {
        uint32_t total = 1;
        for (const auto &c : e.children) { total += expr_size(*c); }
        return total;
    }

    uint32_t count_kind(const Expr &e, Expr::Kind kind) {
        uint32_t total = (e.kind == kind) ? 1u : 0u;
        for (const auto &c : e.children) { total += count_kind(*c, kind); }
        return total;
    }

    uint32_t count_large_constants(const Expr &e, uint64_t threshold) {
        uint32_t total = 0;
        if (e.kind == Expr::Kind::kConstant && e.constant_val > threshold) { total = 1; }
        for (const auto &c : e.children) { total += count_large_constants(*c, threshold); }
        return total;
    }

    struct SubtreeInfo
    {
        int count     = 0;
        uint32_t size = 0;
        std::string repr;
    };

    void collect_subtrees(
        const Expr &e, const std::vector< std::string > &vars,
        std::map< std::string, SubtreeInfo > &out
    ) {
        if (e.kind != Expr::Kind::kConstant && e.kind != Expr::Kind::kVariable) {
            std::string repr = Render(e, vars, 64);
            auto &info       = out[repr];
            info.count++;
            info.size = expr_size(e);
            if (info.repr.empty()) { info.repr = repr; }
        }
        for (const auto &c : e.children) { collect_subtrees(*c, vars, out); }
    }

} // namespace

TEST(OSESDiagnostic, UnsupportedFamilySnapshot) {
    auto entries = scan_oses_all();
    ASSERT_FALSE(entries.empty());

    std::map< std::string, int > by_family;
    std::map< std::string, std::map< std::string, int > > family_routes;
    std::map< std::string, std::map< std::string, int > > family_semantics;
    std::map< std::string, std::map< std::string, int > > family_reasons;
    std::map< std::string, std::map< std::string, int > > family_vars;
    std::set< int > unsupported_lines;

    for (const auto &entry : entries) {
        const std::string family = family_str(entry.family);
        unsupported_lines.insert(entry.line_num);
        by_family[family]++;
        family_routes[family][route_str(entry.cls.route)]++;
        family_semantics[family][semantic_str(entry.cls.semantic)]++;
        family_reasons[family][entry.reason]++;
        family_vars[family][std::to_string(entry.num_vars) + "v"]++;
    }

    std::cerr << "\n=== OSES Unsupported Family Snapshot (" << entries.size()
              << " cases) ===\n";
    for (const auto family : family_order()) {
        const std::string name = family_str(family);
        std::cerr << "  " << name << ": " << by_family[name] << "\n";
        std::cerr << "    routes:    " << join_counts(family_routes[name]) << "\n";
        std::cerr << "    semantics: " << join_counts(family_semantics[name]) << "\n";
        std::cerr << "    vars:      " << join_counts(family_vars[name]) << "\n";
        std::cerr << "    reasons:   " << join_counts(family_reasons[name]) << "\n";
    }

    const std::vector< std::pair< std::string, const std::unordered_set< int > * > >
        documented = {
            {      "ExpandedPolynomialMBA",  &expanded_poly_lines() },
            { "PolynomialIdentityWrapping",      &poly_wrap_lines() },
            {       "ProductInsideBitwise", &product_inside_lines() },
            {    "LargeConstantPolynomial", &large_constant_lines() },
    };

    std::cerr << "\n--- Documented mixed-family coverage ---\n";
    for (const auto &[name, lines] : documented) {
        std::vector< int > still_unsupported;
        std::vector< int > now_missing;
        for (int line_num : *lines) {
            if (unsupported_lines.count(line_num) != 0) {
                still_unsupported.push_back(line_num);
            } else {
                now_missing.push_back(line_num);
            }
        }
        std::cerr << "  " << name << ": unsupported_now=" << still_unsupported.size() << "/"
                  << lines->size();
        if (!still_unsupported.empty()) {
            std::cerr << " lines=";
            for (size_t i = 0; i < still_unsupported.size(); ++i) {
                if (i > 0) { std::cerr << ","; }
                std::cerr << still_unsupported[i];
            }
        }
        if (!now_missing.empty()) {
            std::cerr << " missing=";
            for (size_t i = 0; i < now_missing.size(); ++i) {
                if (i > 0) { std::cerr << ","; }
                std::cerr << now_missing[i];
            }
        }
        std::cerr << "\n";
    }

    std::cerr << "\n--- Uncategorized unsupported lines ---\n";
    bool any_uncategorized = false;
    for (const auto &entry : entries) {
        if (entry.family != OSESFamily::kUncategorized) { continue; }
        any_uncategorized = true;
        std::cerr << "  L" << entry.line_num << " route=" << route_str(entry.cls.route)
                  << " semantic=" << semantic_str(entry.cls.semantic)
                  << " flags=" << flag_str(entry.cls.flags) << " vars=" << entry.num_vars
                  << " reason=\"" << entry.reason << "\""
                  << " gt=\"" << entry.ground_truth.substr(0, 120) << "\"\n";
    }
    if (!any_uncategorized) { std::cerr << "  (none)\n"; }
}

TEST(OSESDiagnostic, RepeatedSubexpressionTelemetry) {
    const std::vector< std::pair< std::string, const std::unordered_set< int > * > >
        families = {
            { "PolynomialIdentityWrapping",      &poly_wrap_lines() },
            {    "LargeConstantPolynomial", &large_constant_lines() },
    };

    std::cerr << "\n=== OSES Repeated Subexpression Telemetry ===\n";
    for (const auto &[family_name, lines] : families) {
        auto targets = load_oses_lines(*lines);
        ASSERT_FALSE(targets.empty());
        std::cerr << "\n--- " << family_name << " ---\n";
        for (const auto &entry : targets) {
            auto parse_result = ParseAndEvaluate(entry.obfuscated, 64);
            ASSERT_TRUE(parse_result.has_value());
            auto elim = EliminateAuxVars(parse_result->sig, parse_result->vars);
            auto rv   = static_cast< uint32_t >(elim.real_vars.size());

            auto ast_result = ParseToAst(entry.obfuscated, 64);
            ASSERT_TRUE(ast_result.has_value());
            auto folded = FoldConstantBitwise(std::move(ast_result.value().expr), 64);

            auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
            Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
            opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
                return EvalExpr(**folded_ptr, v, 64);
            };
            auto result =
                Simplify(parse_result->sig, parse_result->vars, folded_ptr->get(), opts);
            ASSERT_TRUE(result.has_value());
            bool currently_unsupported =
                result->kind == SimplifyOutcome::Kind::kUnchangedUnsupported;
            auto cls = ClassifyStructural(*folded);

            std::map< std::string, SubtreeInfo > subtrees;
            collect_subtrees(*folded, parse_result->vars, subtrees);

            int repeated_unique       = 0;
            int top_repeat_count      = 0;
            uint32_t top_repeat_size  = 0;
            std::string top_repeat_re = "-";
            for (const auto &[repr, info] : subtrees) {
                if (info.count <= 1 || info.size < 4) { continue; }
                repeated_unique++;
                if (info.count > top_repeat_count
                    || (info.count == top_repeat_count && info.size > top_repeat_size))
                {
                    top_repeat_count = info.count;
                    top_repeat_size  = info.size;
                    top_repeat_re    = repr;
                }
            }

            uint32_t mul_nodes       = count_kind(*folded, Expr::Kind::kMul);
            uint32_t large_constants = count_large_constants(*folded, 0xffffffffULL);

            std::cerr << "  L" << entry.line_num << " vars=" << rv
                      << " semantic=" << semantic_str(cls.semantic)
                      << " unsupported=" << (currently_unsupported ? "Y" : "N")
                      << " mul_nodes=" << mul_nodes << " large_consts=" << large_constants
                      << " repeated_unique=" << repeated_unique
                      << " top_repeat_count=" << top_repeat_count
                      << " top_repeat_size=" << top_repeat_size << " top_repeat=\""
                      << top_repeat_re.substr(0, 120) << "\"\n";
        }
    }
}
