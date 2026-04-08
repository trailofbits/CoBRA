#!/usr/bin/env python3
"""Generate NPN4 lookup table for 4-variable Boolean function matching.

Produces a C++ include file with:
1. NPN equivalence class lookup table (65536 entries)
2. Canonical expression builders for all NPN4 classes

Usage: python3 scripts/gen_npn4_table.py > lib/core/Npn4Table.inc
"""

import itertools
import sys
from collections import defaultdict

MASK = 0xFFFF
NUM_TT = 65536

# Variable truth tables for packed sig encoding:
# bit i of tt = f(x0=bit0(i), x1=bit1(i), x2=bit2(i), x3=bit3(i))
VAR_TT = [0xAAAA, 0xCCCC, 0xF0F0, 0xFF00]

ALL_PERMS = list(itertools.permutations(range(4)))


def make_perm_bit_tables():
    """For each permutation, precompute the bit-index mapping.

    perm_bit[p][j] gives the source bit index when evaluating
    the permuted function g at input index j:
      g(x0,...,x3) = f(x_{perm[0]}, ..., x_{perm[3]})
      g[j] = f[perm_bit[p][j]]
    """
    tables = []
    for perm in ALL_PERMS:
        table = []
        for j in range(16):
            i = 0
            for k in range(4):
                if (j >> perm[k]) & 1:
                    i |= 1 << k
            table.append(i)
        tables.append(table)
    return tables


PERM_BIT = make_perm_bit_tables()


def apply_transform_tt(tt, perm_idx, neg_in, neg_out):
    """Apply NPN transform to a truth table.

    Steps (forward):
      1. Negate selected inputs (flip bits in the index)
      2. Permute input variables
      3. Negate output (complement truth table)
    """
    perm_table = PERM_BIT[perm_idx]
    result = 0
    for j in range(16):
        src = perm_table[j] ^ neg_in
        if (tt >> src) & 1:
            result |= 1 << j
    if neg_out:
        result ^= MASK
    return result


def canonicalize_all():
    """Compute NPN canonical forms for all 65536 4-variable truth tables.

    Returns:
        canonical_list: sorted list of unique canonical truth tables
        entries: list of (class_id, perm_idx, neg_in, neg_out) per truth table
    """
    canonical = [0] * NUM_TT
    transforms = [None] * NUM_TT

    for tt in range(NUM_TT):
        best = NUM_TT
        best_xform = (0, 0, False)

        for p_idx in range(24):
            perm_table = PERM_BIT[p_idx]
            for neg_in in range(16):
                # Compute transformed truth table
                result = 0
                for j in range(16):
                    if (tt >> (perm_table[j] ^ neg_in)) & 1:
                        result |= 1 << j

                if result < best:
                    best = result
                    best_xform = (p_idx, neg_in, False)

                comp = result ^ MASK
                if comp < best:
                    best = comp
                    best_xform = (p_idx, neg_in, True)

        canonical[tt] = best
        transforms[tt] = best_xform

    unique_canonical = sorted(set(canonical))
    class_map = {c: i for i, c in enumerate(unique_canonical)}

    entries = []
    for tt in range(NUM_TT):
        c = canonical[tt]
        cls = class_map[c]
        p_idx, neg_in, neg_out = transforms[tt]
        entries.append((cls, p_idx, neg_in, 1 if neg_out else 0))

    return unique_canonical, entries


def bfs_minimal_expressions(target_tts):
    """BFS to find minimal-cost Boolean expressions.

    Cost = number of operations (AND, OR, XOR, NOT).
    Variables have cost 0.

    Returns dict mapping truth table -> expression tree.
    Expression tree: ('VAR', idx) | ('NOT', child) |
                     ('AND'|'OR'|'XOR', left, right)
    """
    parent = {}
    cost_of = {}
    by_cost = defaultdict(list)

    # Seed: 4 variables at cost 0
    for i, tt in enumerate(VAR_TT):
        parent[tt] = ("VAR", i)
        cost_of[tt] = 0
        by_cost[0].append(tt)

    targets = set(target_tts) - {0x0000, 0xFFFF}
    found = targets & set(cost_of)

    for target_cost in range(1, 12):
        new_items = []

        # NOT of items at (target_cost - 1)
        for tt in by_cost[target_cost - 1]:
            ntt = (~tt) & MASK
            if ntt not in cost_of:
                cost_of[ntt] = target_cost
                parent[ntt] = ("NOT", tt)
                new_items.append(ntt)

        # Binary ops: cost_a + cost_b + 1 = target_cost, cost_a <= cost_b
        for cost_a in range((target_cost - 1) // 2 + 1):
            cost_b = target_cost - 1 - cost_a
            list_a = by_cost.get(cost_a, [])
            list_b = by_cost.get(cost_b, [])
            if not list_a or not list_b:
                continue

            same_cost = cost_a == cost_b
            for tt_a in list_a:
                for tt_b in list_b:
                    if same_cost and tt_b <= tt_a:
                        continue

                    for op_name, res in (
                        ("AND", tt_a & tt_b),
                        ("OR", tt_a | tt_b),
                        ("XOR", tt_a ^ tt_b),
                    ):
                        if res not in cost_of:
                            cost_of[res] = target_cost
                            parent[res] = (op_name, tt_a, tt_b)
                            new_items.append(res)

        by_cost[target_cost] = new_items
        found = targets & set(cost_of)

        print(
            f"  cost {target_cost}: +{len(new_items):>6} = "
            f"{len(cost_of):>6} total, "
            f"{len(found)}/{len(targets)} canonical found",
            file=sys.stderr,
        )

        if found == targets:
            break

    if found != targets:
        missing = targets - found
        print(
            f"WARNING: {len(missing)} canonical TTs not reached: "
            f"{[hex(t) for t in sorted(missing)[:10]]}",
            file=sys.stderr,
        )

    # Reconstruct expression trees
    def reconstruct(tt):
        entry = parent[tt]
        if entry[0] == "VAR":
            return ("VAR", entry[1])
        elif entry[0] == "NOT":
            return ("NOT", reconstruct(entry[1]))
        else:
            return (entry[0], reconstruct(entry[1]), reconstruct(entry[2]))

    expressions = {}
    for tt in target_tts:
        if tt in (0x0000, 0xFFFF):
            continue
        if tt in parent:
            expressions[tt] = reconstruct(tt)
            expressions[tt] = simplify_double_neg(expressions[tt])
        else:
            expressions[tt] = None
    return expressions


def simplify_double_neg(expr):
    """Remove NOT(NOT(x)) patterns."""
    if expr[0] == "NOT":
        child = simplify_double_neg(expr[1])
        if child[0] == "NOT":
            return child[1]
        return ("NOT", child)
    elif expr[0] == "VAR":
        return expr
    else:
        return (expr[0], simplify_double_neg(expr[1]), simplify_double_neg(expr[2]))


def eval_expr(expr, assignment):
    """Evaluate expression at a 4-variable assignment (0-15 index)."""
    if expr[0] == "VAR":
        return (assignment >> expr[1]) & 1
    elif expr[0] == "NOT":
        return 1 - eval_expr(expr[1], assignment)
    elif expr[0] == "AND":
        return eval_expr(expr[1], assignment) & eval_expr(expr[2], assignment)
    elif expr[0] == "OR":
        return eval_expr(expr[1], assignment) | eval_expr(expr[2], assignment)
    elif expr[0] == "XOR":
        return eval_expr(expr[1], assignment) ^ eval_expr(expr[2], assignment)
    raise ValueError(f"Unknown op: {expr[0]}")


def expr_to_tt(expr):
    """Evaluate expression to a 16-bit truth table."""
    result = 0
    for i in range(16):
        if eval_expr(expr, i):
            result |= 1 << i
    return result


def verify_table(canonical_list, entries, expressions):
    """Verify the NPN table is correct."""
    errors = 0

    # Verify each canonical expression matches its truth table
    for tt in canonical_list:
        if tt in (0x0000, 0xFFFF):
            continue
        expr = expressions.get(tt)
        if expr is None:
            print(f"ERROR: no expression for canonical {tt:#06x}", file=sys.stderr)
            errors += 1
            continue
        computed = expr_to_tt(expr)
        if computed != tt:
            print(
                f"ERROR: canonical {tt:#06x} expression evaluates to "
                f"{computed:#06x}",
                file=sys.stderr,
            )
            errors += 1

    # Verify inverse transform for a sample of truth tables
    for tt in range(0, NUM_TT, 37):  # sample every 37th
        cls_id, p_idx, neg_in, neg_out = entries[tt]
        canonical_tt = canonical_list[cls_id]

        # Forward: applying (p_idx, neg_in, neg_out) to tt should give canonical_tt
        transformed = apply_transform_tt(tt, p_idx, neg_in, neg_out)
        if transformed != canonical_tt:
            print(
                f"ERROR: tt={tt:#06x} forward transform gives "
                f"{transformed:#06x}, expected {canonical_tt:#06x}",
                file=sys.stderr,
            )
            errors += 1

    if errors == 0:
        print("  verification passed", file=sys.stderr)
    else:
        print(f"  {errors} verification errors!", file=sys.stderr)
    return errors == 0


def expr_to_cpp(expr):
    """Convert expression tree to C++ Expr builder code."""
    if expr[0] == "VAR":
        return f"v({expr[1]})"
    elif expr[0] == "NOT":
        return f"Expr::BitwiseNot({expr_to_cpp(expr[1])})"
    else:
        op_map = {"AND": "BitwiseAnd", "OR": "BitwiseOr", "XOR": "BitwiseXor"}
        left = expr_to_cpp(expr[1])
        right = expr_to_cpp(expr[2])
        return f"Expr::{op_map[expr[0]]}({left}, {right})"


def expr_cost(expr):
    """Compute expression cost (number of operations)."""
    if expr[0] == "VAR":
        return 0
    elif expr[0] == "NOT":
        return 1 + expr_cost(expr[1])
    else:
        return 1 + expr_cost(expr[1]) + expr_cost(expr[2])


def generate_cpp(canonical_list, entries, expressions):
    """Generate the C++ include file."""
    lines = []
    lines.append(
        "// Auto-generated by scripts/gen_npn4_table.py — DO NOT EDIT"
    )
    lines.append(
        "// NPN equivalence class lookup table for 4-variable Boolean"
    )
    lines.append(
        f"// function matching. {len(canonical_list)} NPN classes,"
    )
    lines.append("// covering all 65536 non-constant truth tables.")
    lines.append("// clang-format off")
    lines.append("")

    # Permutation table
    lines.append("static constexpr uint8_t kPerms4[24][4] = {")
    for perm in ALL_PERMS:
        lines.append(f"    {{{perm[0]},{perm[1]},{perm[2]},{perm[3]}}},")
    lines.append("};")
    lines.append("")

    # Packed NPN entry: 4 bytes each
    # byte 0: class_id (0-221)
    # byte 1: perm_idx (0-23)
    # byte 2: neg_inputs (0-15, 4-bit mask)
    # byte 3: neg_output (0 or 1)
    lines.append("struct Npn4Entry {")
    lines.append("    uint8_t class_id;")
    lines.append("    uint8_t perm_idx;")
    lines.append("    uint8_t neg_inputs;")
    lines.append("    uint8_t neg_output;")
    lines.append("};")
    lines.append("")

    lines.append("static constexpr Npn4Entry kNpn4Table[65536] = {")
    for i in range(0, NUM_TT, 4):
        row = []
        for j in range(4):
            idx = i + j
            if idx >= NUM_TT:
                break
            cls, p, n_in, n_out = entries[idx]
            row.append(f"{{{cls},{p},{n_in},{n_out}}}")
        lines.append("    " + ", ".join(row) + ",")
    lines.append("};")
    lines.append("")

    # Build canonical expression function
    lines.append("template<typename VarFn>")
    lines.append(
        "std::optional<std::unique_ptr<Expr>>"
    )
    lines.append(
        "BuildNpn4Canonical(uint8_t class_id, VarFn &&v) {"
    )
    lines.append("    switch (class_id) {")

    for i, tt in enumerate(canonical_list):
        if tt in (0x0000, 0xFFFF):
            lines.append(f"        case {i}: // {tt:#06x} (constant)")
            lines.append("            return std::nullopt;")
            continue
        expr = expressions.get(tt)
        if expr is None:
            lines.append(f"        case {i}: // {tt:#06x} (no expression)")
            lines.append("            return std::nullopt;")
            continue
        cpp = expr_to_cpp(expr)
        cost = expr_cost(expr)
        lines.append(f"        case {i}: // {tt:#06x} cost={cost}")
        lines.append(f"            return {cpp};")

    lines.append("        default:")
    lines.append("            return std::nullopt;")
    lines.append("    }")
    lines.append("}")
    lines.append("")

    # Double-NOT collapse helper
    lines.append(
        "// Collapse NOT(NOT(x)) -> x at all depths."
    )
    lines.append(
        "// NPN input/output negation can introduce double-NOTs"
    )
    lines.append(
        "// when the canonical form already contains a NOT at the"
    )
    lines.append(
        "// same position as the transform's negation."
    )
    lines.append(
        "void CollapseDoubleNot(std::unique_ptr<Expr> &expr) {"
    )
    lines.append(
        "    for (auto &child : expr->children) {"
    )
    lines.append("        CollapseDoubleNot(child);")
    lines.append("    }")
    lines.append(
        "    if (expr->kind == Expr::Kind::kNot &&"
    )
    lines.append(
        "        !expr->children.empty() &&"
    )
    lines.append(
        "        expr->children[0]->kind == Expr::Kind::kNot) {"
    )
    lines.append(
        "        expr = std::move(expr->children[0]->children[0]);"
    )
    lines.append("    }")
    lines.append("}")
    lines.append("")

    # Match function
    lines.append(
        "std::optional<std::unique_ptr<Expr>>"
    )
    lines.append("Match4varNpn(uint16_t key) {")
    lines.append("    const auto &e = kNpn4Table[key];")
    lines.append("    const auto *perm = kPerms4[e.perm_idx];")
    lines.append("")
    lines.append("    // Inverse permutation")
    lines.append("    uint32_t inv[4];")
    lines.append(
        "    for (uint32_t i = 0; i < 4; ++i) { inv[perm[i]] = i; }"
    )
    lines.append("")
    lines.append("    // Canonical var j -> actual Var(inv[j]),")
    lines.append("    // negated if neg_inputs bit inv[j] is set.")
    lines.append(
        "    const uint8_t neg = e.neg_inputs;"
    )
    lines.append(
        "    auto v = [&](uint32_t j) -> std::unique_ptr<Expr> {"
    )
    lines.append("        const uint32_t a = inv[j];")
    lines.append("        auto expr = Expr::Variable(a);")
    lines.append("        if ((neg >> a) & 1u) {")
    lines.append(
        "            return Expr::BitwiseNot(std::move(expr));"
    )
    lines.append("        }")
    lines.append("        return expr;")
    lines.append("    };")
    lines.append("")
    lines.append(
        "    auto result = BuildNpn4Canonical(e.class_id, v);"
    )
    lines.append("    if (!result) { return std::nullopt; }")
    lines.append("")
    lines.append("    if (e.neg_output != 0u) {")
    lines.append(
        "        *result = Expr::BitwiseNot(std::move(*result));"
    )
    lines.append("    }")
    lines.append("    CollapseDoubleNot(*result);")
    lines.append("    return result;")
    lines.append("}")
    lines.append("")
    lines.append("// clang-format on")
    return "\n".join(lines)


def main():
    print("NPN4 table generator", file=sys.stderr)

    print("Computing NPN canonical forms...", file=sys.stderr)
    canonical_list, entries = canonicalize_all()
    print(f"  {len(canonical_list)} NPN classes", file=sys.stderr)

    print("Finding minimal expressions via BFS...", file=sys.stderr)
    expressions = bfs_minimal_expressions(canonical_list)

    # Stats
    costs = [
        expr_cost(expressions[tt])
        for tt in canonical_list
        if tt not in (0x0000, 0xFFFF) and expressions.get(tt)
    ]
    print(
        f"  cost range: {min(costs)}-{max(costs)}, "
        f"avg={sum(costs)/len(costs):.1f}",
        file=sys.stderr,
    )

    print("Verifying...", file=sys.stderr)
    if not verify_table(canonical_list, entries, expressions):
        sys.exit(1)

    print("Generating C++...", file=sys.stderr)
    cpp = generate_cpp(canonical_list, entries, expressions)
    print(cpp)
    print("Done.", file=sys.stderr)


if __name__ == "__main__":
    main()
