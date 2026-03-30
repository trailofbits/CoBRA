#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>

using namespace cobra;

namespace {

    // ── Mask shape classification ───────────────────────────────

    enum class MaskShape {
        kLowContiguous, // (1 << k) - 1 for some k in [1, 63]
        kByteAligned,   // union of whole byte lanes (0xFF, 0xFFFF, etc.)
        kOtherConstant, // arbitrary constant mask
        kSymbolic,      // mask side contains variables
    };

    bool is_low_contiguous(uint64_t v) {
        // v == (1 << k) - 1 iff v != 0 and (v & (v+1)) == 0
        return v != 0 && (v & (v + 1)) == 0;
    }

    bool is_byte_aligned(uint64_t v) {
        // Every byte is either 0x00 or 0xFF
        if (v == 0) { return false; }
        for (int i = 0; i < 8; ++i) {
            uint8_t byte = static_cast< uint8_t >((v >> (i * 8)) & 0xFF);
            if (byte != 0x00 && byte != 0xFF) { return false; }
        }
        return true;
    }

    MaskShape classify_mask(const Expr &mask_expr) {
        if (!IsConstantSubtree(mask_expr)) { return MaskShape::kSymbolic; }
        uint64_t val = EvalConstantExpr(mask_expr, 64);
        if (is_low_contiguous(val)) { return MaskShape::kLowContiguous; }
        if (is_byte_aligned(val)) { return MaskShape::kByteAligned; }
        return MaskShape::kOtherConstant;
    }

    const char *mask_shape_str(MaskShape s) {
        switch (s) {
            case MaskShape::kLowContiguous:
                return "low-contiguous";
            case MaskShape::kByteAligned:
                return "byte-aligned";
            case MaskShape::kOtherConstant:
                return "other-const";
            case MaskShape::kSymbolic:
                return "symbolic";
        }
        return "?";
    }

    // ── Arithmetic kind under the mask ──────────────────────────

    enum class ArithKind {
        kNone,   // purely bitwise (no arithmetic)
        kAddSub, // contains Add or Neg but no var*var Mul
        kMul,    // contains var*var Mul
        kMixed,  // both Add/Neg and var*var Mul
    };

    // Classify the "arithmetic content" of a subtree, stopping at
    // bitwise boundaries (AND/OR/XOR/NOT). We only care about
    // arithmetic that is *above* or *at* the masked site.
    ArithKind classify_arith(const Expr &expr) {
        bool has_add = false;
        bool has_mul = false;

        switch (expr.kind) {
            case Expr::Kind::kConstant:
            case Expr::Kind::kVariable:
                return ArithKind::kNone;

            case Expr::Kind::kAdd:
            case Expr::Kind::kNeg:
                has_add = true;
                break;

            case Expr::Kind::kMul:
                // var*var Mul vs const*var Mul
                if (expr.children.size() == 2 && HasVarDep(*expr.children[0])
                    && HasVarDep(*expr.children[1]))
                {
                    has_mul = true;
                } else {
                    has_add = true; // const*var is effectively scaling
                }
                break;

            case Expr::Kind::kShr:
                // shift of arithmetic is also interesting but rare
                break;

            // Bitwise ops: stop recursing into children for arith
            case Expr::Kind::kAnd:
            case Expr::Kind::kOr:
            case Expr::Kind::kXor:
            case Expr::Kind::kNot:
                return ArithKind::kNone;
        }

        for (const auto &child : expr.children) {
            auto child_kind = classify_arith(*child);
            if (child_kind == ArithKind::kAddSub || child_kind == ArithKind::kMixed) {
                has_add = true;
            }
            if (child_kind == ArithKind::kMul || child_kind == ArithKind::kMixed) {
                has_mul = true;
            }
        }

        if (has_add && has_mul) { return ArithKind::kMixed; }
        if (has_mul) { return ArithKind::kMul; }
        if (has_add) { return ArithKind::kAddSub; }
        return ArithKind::kNone;
    }

    const char *arith_kind_str(ArithKind k) {
        switch (k) {
            case ArithKind::kNone:
                return "none";
            case ArithKind::kAddSub:
                return "add/sub";
            case ArithKind::kMul:
                return "mul";
            case ArithKind::kMixed:
                return "mixed";
        }
        return "?";
    }

    // ── Combined site classification ────────────────────────────

    // Taxonomy from research plan:
    //   A: add/sub under constant low mask
    //   B: add/sub under other constant mask (including byte-aligned)
    //   C: add/sub under symbolic mask
    //   D: mul under constant low mask
    //   E: mul under other constant mask
    //   F: mul under symbolic mask
    //   G: mixed arith under any mask
    //   H: no masked-arithmetic site found
    enum class SiteCategory { kA, kB, kC, kD, kE, kF, kG, kH };

    SiteCategory categorize(ArithKind arith, MaskShape mask) {
        if (arith == ArithKind::kMixed) { return SiteCategory::kG; }

        if (arith == ArithKind::kAddSub) {
            if (mask == MaskShape::kLowContiguous) { return SiteCategory::kA; }
            if (mask == MaskShape::kSymbolic) { return SiteCategory::kC; }
            return SiteCategory::kB; // byte-aligned or other-const
        }

        if (arith == ArithKind::kMul) {
            if (mask == MaskShape::kLowContiguous) { return SiteCategory::kD; }
            if (mask == MaskShape::kSymbolic) { return SiteCategory::kF; }
            return SiteCategory::kE;
        }

        return SiteCategory::kH;
    }

    const char *category_str(SiteCategory c) {
        switch (c) {
            case SiteCategory::kA:
                return "A: add/sub + low-mask";
            case SiteCategory::kB:
                return "B: add/sub + other-const-mask";
            case SiteCategory::kC:
                return "C: add/sub + symbolic-mask";
            case SiteCategory::kD:
                return "D: mul + low-mask";
            case SiteCategory::kE:
                return "E: mul + other-const-mask";
            case SiteCategory::kF:
                return "F: mul + symbolic-mask";
            case SiteCategory::kG:
                return "G: mixed arith";
            case SiteCategory::kH:
                return "H: no masked-arith";
        }
        return "?";
    }

    // ── AST site finder ─────────────────────────────────────────

    struct MaskedArithSite
    {
        ArithKind arith;
        MaskShape mask;
        SiteCategory category;
        Expr::Kind bitwise_op; // AND, OR, or XOR
        uint64_t mask_val;     // 0 if symbolic
        int depth;             // depth in AST where site was found
    };

    // Walk the AST and collect all masked-arithmetic sites.
    // A site is a bitwise node (AND/OR/XOR) where at least one child
    // contains arithmetic operations (Add/Mul/Neg).
    void find_masked_arith_sites(
        const Expr &expr, int depth, std::vector< MaskedArithSite > &sites
    ) {
        // Recurse into children first (post-order — find deepest sites first)
        for (const auto &child : expr.children) {
            find_masked_arith_sites(*child, depth + 1, sites);
        }

        // Only interested in binary bitwise ops
        if (expr.kind != Expr::Kind::kAnd && expr.kind != Expr::Kind::kOr
            && expr.kind != Expr::Kind::kXor)
        {
            return;
        }
        if (expr.children.size() != 2) { return; }

        const auto &lhs = *expr.children[0];
        const auto &rhs = *expr.children[1];

        auto lhs_arith = classify_arith(lhs);
        auto rhs_arith = classify_arith(rhs);

        // At least one side must have arithmetic
        if (lhs_arith == ArithKind::kNone && rhs_arith == ArithKind::kNone) { return; }

        // Determine which side is "arithmetic" and which is "mask"
        // If both have arithmetic, the site is category G regardless
        ArithKind arith   = ArithKind::kNone;
        MaskShape mask    = MaskShape::kSymbolic;
        uint64_t mask_val = 0;

        if (lhs_arith != ArithKind::kNone && rhs_arith != ArithKind::kNone) {
            // Both sides have arithmetic — mixed
            arith = ArithKind::kMixed;
            mask  = MaskShape::kSymbolic;
        } else if (lhs_arith != ArithKind::kNone) {
            arith = lhs_arith;
            mask  = classify_mask(rhs);
            if (mask != MaskShape::kSymbolic) { mask_val = EvalConstantExpr(rhs, 64); }
        } else {
            arith = rhs_arith;
            mask  = classify_mask(lhs);
            if (mask != MaskShape::kSymbolic) { mask_val = EvalConstantExpr(lhs, 64); }
        }

        auto cat = categorize(arith, mask);
        sites.push_back(
            { .arith      = arith,
              .mask       = mask,
              .category   = cat,
              .bitwise_op = expr.kind,
              .mask_val   = mask_val,
              .depth      = depth }
        );
    }

    // For an expression, find the "best" (most tractable) site category.
    // Priority: A > D > B > E > C > F > G > H
    SiteCategory best_site_category(const std::vector< MaskedArithSite > &sites) {
        if (sites.empty()) { return SiteCategory::kH; }
        return std::min_element(
                   sites.begin(), sites.end(),
                   [](const MaskedArithSite &a, const MaskedArithSite &b) {
                       return static_cast< int >(a.category) < static_cast< int >(b.category);
                   }
        )->category;
    }

    // ── Dataset utilities (shared with other diagnostics) ───────

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
        }
        return "?";
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

    // ── Per-expression record ───────────────────────────────────

    struct ExprRecord
    {
        int line_num;
        std::string dataset;
        std::string ground_truth;
        Classification cls;
        uint32_t num_vars;
        SiteCategory best_category;
        std::vector< MaskedArithSite > sites;
        std::string reason;
    };

    void scan_dataset(
        const std::string &path, const std::string &dataset_name,
        std::vector< ExprRecord > &records, int &total_parsed, int &total_simplified,
        int &total_unsupported
    ) {
        std::ifstream file(path);
        ASSERT_TRUE(file.is_open()) << "Cannot open " << path;

        std::string line;
        int line_num = 0;
        while (std::getline(file, line)) {
            ++line_num;
            if (line.empty() || line[0] == '#') { continue; }

            size_t sep = find_separator(line);
            if (sep == std::string::npos) { continue; }

            std::string obfuscated = trim(line.substr(0, sep));
            if (obfuscated.empty()) { continue; }

            auto parse_result = ParseAndEvaluate(obfuscated, 64);
            if (!parse_result.has_value()) { continue; }

            auto ast_result = ParseToAst(obfuscated, 64);
            if (!ast_result.has_value()) { continue; }

            total_parsed++;

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

            if (result.value().kind == SimplifyOutcome::Kind::kSimplified) {
                total_simplified++;
                continue;
            }
            if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) {
                continue;
            }

            total_unsupported++;

            auto cls  = ClassifyStructural(**folded_ptr);
            auto elim = EliminateAuxVars(parse_result.value().sig, parse_result.value().vars);
            auto rv   = static_cast< uint32_t >(elim.real_vars.size());

            // Find masked-arithmetic sites in the folded AST
            std::vector< MaskedArithSite > sites;
            find_masked_arith_sites(**folded_ptr, 0, sites);

            std::string reason;
            if (result.value().diag.reason_code.has_value()) {
                auto cat = result.value().diag.reason_code->category;
                switch (cat) {
                    case ReasonCategory::kVerifyFailed:
                        reason = "verify-failed";
                        break;
                    case ReasonCategory::kSearchExhausted:
                        reason = "search-exhausted";
                        break;
                    case ReasonCategory::kRepresentationGap:
                        reason = "representation-gap";
                        break;
                    case ReasonCategory::kNoSolution:
                        reason = "no-solution";
                        break;
                    default:
                        reason = result.value().diag.reason.substr(0, 50);
                        break;
                }
            } else {
                reason = result.value().diag.reason.substr(0, 50);
            }

            std::string gt = trim(line.substr(sep + 1));
            records.push_back(
                { .line_num      = line_num,
                  .dataset       = dataset_name,
                  .ground_truth  = gt.substr(0, 60),
                  .cls           = cls,
                  .num_vars      = rv,
                  .best_category = best_site_category(sites),
                  .sites         = std::move(sites),
                  .reason        = reason }
            );
        }
    }

} // namespace

// Main audit: classify every unsupported expression by masked-arithmetic
// subfamily to determine the first implementation target.
TEST(MaskedArithAudit, ClassifyUnsupported) {
    struct DatasetInfo
    {
        std::string path;
        std::string name;
    };

    std::vector< DatasetInfo > datasets = {
        { DATASET_DIR "/gamba/qsynth_ea.txt", "QSynth" },
        {    DATASET_DIR "/gamba/syntia.txt", "Syntia" },
    };

    std::vector< ExprRecord > all_records;

    for (const auto &ds : datasets) {
        int parsed = 0, simplified = 0, unsupported = 0;
        std::cerr << "\n=== Scanning " << ds.name << " ===\n";
        scan_dataset(ds.path, ds.name, all_records, parsed, simplified, unsupported);
        std::cerr << "  parsed=" << parsed << " simplified=" << simplified
                  << " unsupported=" << unsupported << "\n";
    }

    // ── Aggregate by best-site category ─────────────────────
    std::map< SiteCategory, int > by_category;
    std::map< SiteCategory, std::map< std::string, int > > category_by_dataset;
    std::map< SiteCategory, std::map< std::string, int > > category_by_reason;

    for (const auto &rec : all_records) {
        by_category[rec.best_category]++;
        category_by_dataset[rec.best_category][rec.dataset]++;
        category_by_reason[rec.best_category][rec.reason]++;
    }

    std::cerr << "\n============================================\n";
    std::cerr << "Masked-Arithmetic Site Audit\n";
    std::cerr << "============================================\n";
    std::cerr << "Total unsupported expressions: " << all_records.size() << "\n";

    std::cerr << "\n--- By best-site category ---\n";
    for (auto &[cat, count] : by_category) {
        std::cerr << "  " << category_str(cat) << ": " << count << "\n";
        for (auto &[ds, n] : category_by_dataset[cat]) {
            std::cerr << "    " << ds << ": " << n << "\n";
        }
    }

    std::cerr << "\n--- Category breakdown by failure reason ---\n";
    for (auto &[cat, reasons] : category_by_reason) {
        std::cerr << "  " << category_str(cat) << ":\n";
        for (auto &[reason, n] : reasons) {
            std::cerr << "    " << reason << ": " << n << "\n";
        }
    }

    // ── Detailed: all AND-specific sites ────────────────────
    std::map< MaskShape, int > and_mask_shapes;
    std::map< ArithKind, int > and_arith_kinds;
    std::map< uint64_t, int > common_mask_values;
    int total_and_sites = 0;

    for (const auto &rec : all_records) {
        for (const auto &site : rec.sites) {
            if (site.bitwise_op != Expr::Kind::kAnd) { continue; }
            total_and_sites++;
            and_mask_shapes[site.mask]++;
            and_arith_kinds[site.arith]++;
            if (site.mask != MaskShape::kSymbolic && site.mask_val != 0) {
                common_mask_values[site.mask_val]++;
            }
        }
    }

    std::cerr << "\n--- AND-specific site breakdown ---\n";
    std::cerr << "Total AND sites across all unsupported: " << total_and_sites << "\n";

    std::cerr << "\n  Mask shapes:\n";
    for (auto &[shape, n] : and_mask_shapes) {
        std::cerr << "    " << mask_shape_str(shape) << ": " << n << "\n";
    }

    std::cerr << "\n  Arith kinds:\n";
    for (auto &[kind, n] : and_arith_kinds) {
        std::cerr << "    " << arith_kind_str(kind) << ": " << n << "\n";
    }

    std::cerr << "\n  Most common constant mask values:\n";
    // Sort by frequency
    std::vector< std::pair< uint64_t, int > > mask_freq(
        common_mask_values.begin(), common_mask_values.end()
    );
    std::sort(mask_freq.begin(), mask_freq.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });
    int shown = 0;
    for (const auto &[val, count] : mask_freq) {
        if (shown >= 20) { break; }
        std::cerr << "    0x" << std::hex << val << std::dec << ": " << count;
        if (is_low_contiguous(val)) { std::cerr << " [low-contiguous]"; }
        if (is_byte_aligned(val)) { std::cerr << " [byte-aligned]"; }
        std::cerr << "\n";
        shown++;
    }

    // ── Per-category detailed listings (first 5 per category) ──
    for (auto cat : { SiteCategory::kA, SiteCategory::kB, SiteCategory::kC, SiteCategory::kD,
                      SiteCategory::kE, SiteCategory::kF, SiteCategory::kG })
    {
        std::cerr << "\n--- Sample expressions: " << category_str(cat) << " ---\n";
        int count = 0;
        for (const auto &rec : all_records) {
            if (rec.best_category != cat) { continue; }
            if (count >= 5) { break; }
            std::cerr << "  " << rec.dataset << " L" << rec.line_num << " vars=" << rec.num_vars
                      << " semantic=" << semantic_str(rec.cls.semantic) << " flags={"
                      << flag_str(rec.cls.flags) << "}"
                      << " reason=" << rec.reason << "\n";
            std::cerr << "    GT: " << rec.ground_truth << "\n";
            std::cerr << "    Sites (" << rec.sites.size() << "):\n";
            for (const auto &site : rec.sites) {
                std::cerr << "      depth=" << site.depth << " "
                          << (site.bitwise_op == Expr::Kind::kAnd      ? "AND"
                                  : site.bitwise_op == Expr::Kind::kOr ? "OR"
                                                                       : "XOR")
                          << "(" << arith_kind_str(site.arith) << ", "
                          << mask_shape_str(site.mask);
                if (site.mask != MaskShape::kSymbolic && site.mask_val != 0) {
                    std::cerr << "=0x" << std::hex << site.mask_val << std::dec;
                }
                std::cerr << ")\n";
            }
            count++;
        }
        if (count == 0) { std::cerr << "  (none)\n"; }
    }

    // ── Coverage estimate ───────────────────────────────────
    int cat_a = by_category.count(SiteCategory::kA) ? by_category[SiteCategory::kA] : 0;
    int cat_b = by_category.count(SiteCategory::kB) ? by_category[SiteCategory::kB] : 0;
    int cat_d = by_category.count(SiteCategory::kD) ? by_category[SiteCategory::kD] : 0;
    int cat_h = by_category.count(SiteCategory::kH) ? by_category[SiteCategory::kH] : 0;

    int total = static_cast< int >(all_records.size());
    std::cerr << "\n============================================\n";
    std::cerr << "Coverage estimate\n";
    std::cerr << "============================================\n";
    std::cerr << "Category A (add/sub + low-mask):      " << cat_a << " / " << total << "\n";
    std::cerr << "Category A+B (add/sub + any const):   " << (cat_a + cat_b) << " / " << total
              << "\n";
    std::cerr << "Category A+B+D (+ mul low-mask):      " << (cat_a + cat_b + cat_d) << " / "
              << total << "\n";
    std::cerr << "No masked-arith sites (H):            " << cat_h << " / " << total << "\n";
    std::cerr << "\n";
}
