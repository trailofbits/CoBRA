#include "MBADetector.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Simplifier.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Casting.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        bool IsMbaOpcode(unsigned opcode) {
            switch (opcode) { // NOLINT(hicpp-multiway-paths-covered)
                case llvm::Instruction::Add:
                case llvm::Instruction::Sub:
                case llvm::Instruction::Mul:
                case llvm::Instruction::And:
                case llvm::Instruction::Or:
                case llvm::Instruction::Xor:
                case llvm::Instruction::LShr:
                case llvm::Instruction::ZExt:
                case llvm::Instruction::SExt:
                    return true;
                default:
                    return false;
            }
        }

        // BFS from root following operands.  MBA-opcode instructions
        // are added to tree_insts; everything else becomes a leaf.
        // PHI nodes are treated as transparent when all incoming
        // values are MBA opcodes — the first arm is followed and a
        // redirect entry is recorded for evaluation / expr building.
        void CollectTree(
            llvm::Instruction *root, llvm::SmallVector< llvm::Instruction *, 16 > &tree_insts,
            std::vector< llvm::Value * > &leaves,
            llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            bool try_phi_transparency = true
        ) {
            llvm::DenseSet< llvm::Value * > visited;
            std::queue< llvm::Value * > work;
            work.push(root);

            while (!work.empty()) {
                auto *v = work.front();
                work.pop();
                if (!visited.insert(v).second) { continue; }

                auto *inst = llvm::dyn_cast< llvm::Instruction >(v);
                if ((inst != nullptr) && IsMbaOpcode(inst->getOpcode())) {
                    // LShr with variable shift amount is unsupported —
                    // treat the whole instruction as a leaf.
                    if (inst->getOpcode() == llvm::Instruction::LShr
                        && !llvm::isa< llvm::ConstantInt >(inst->getOperand(1)))
                    {
                        if (std::find(leaves.begin(), leaves.end(), v) == leaves.end()) {
                            leaves.push_back(v);
                        }
                        continue;
                    }

                    tree_insts.push_back(inst);
                    for (auto &op : inst->operands()) { work.push(op.get()); }
                } else if (auto *phi = llvm::dyn_cast< llvm::PHINode >(v)) {
                    // Check if every incoming value is an MBA opcode
                    // instruction (or a constant).  If so, treat the
                    // phi as transparent and follow the first arm.
                    bool all_mba = phi->getNumIncomingValues() > 0;
                    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
                        auto *inc = phi->getIncomingValue(i);
                        if (llvm::isa< llvm::ConstantInt >(inc)) { continue; }
                        auto *inc_inst = llvm::dyn_cast< llvm::Instruction >(inc);
                        if ((inc_inst == nullptr) || !IsMbaOpcode(inc_inst->getOpcode())) {
                            all_mba = false;
                            break;
                        }
                    }

                    if (all_mba && try_phi_transparency) {
                        auto *chosen       = phi->getIncomingValue(0);
                        phi_redirects[phi] = chosen;
                        work.push(chosen);
                    } else {
                        if (std::find(leaves.begin(), leaves.end(), v) == leaves.end()) {
                            leaves.push_back(v);
                        }
                    }
                } else {
                    if (!llvm::isa< llvm::ConstantInt >(v)) {
                        if (std::find(leaves.begin(), leaves.end(), v) == leaves.end()) {
                            leaves.push_back(v);
                        }
                    }
                }
            }
        }

        uint64_t EvaluateTree(
            llvm::Instruction *root,
            const llvm::DenseMap< llvm::Value *, uint64_t > &assignments, uint32_t bitwidth,
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects
        ) {
            llvm::DenseMap< llvm::Value *, uint64_t > cache;
            uint64_t mask = Bitmask(bitwidth);

            std::function< uint64_t(llvm::Value *) > eval = [&](llvm::Value *v) -> uint64_t {
                auto it = cache.find(v);
                if (it != cache.end()) { return it->second; }

                auto ait = assignments.find(v);
                if (ait != assignments.end()) {
                    cache[v] = ait->second;
                    return ait->second;
                }

                if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(v)) {
                    const uint64_t val = ci->getZExtValue() & mask;
                    cache[v]           = val;
                    return val;
                }

                // Phi redirect: follow the chosen arm.
                auto pit = phi_redirects.find(v);
                if (pit != phi_redirects.end()) {
                    const uint64_t val = eval(pit->second);
                    cache[v]           = val;
                    return val;
                }

                auto *inst = llvm::cast< llvm::Instruction >(v);

                if (inst->getOpcode() == llvm::Instruction::ZExt
                    || inst->getOpcode() == llvm::Instruction::SExt)
                {
                    const uint64_t operand = eval(inst->getOperand(0));
                    cache[v]               = operand & mask;
                    return cache[v];
                }

                const uint64_t lhs = eval(inst->getOperand(0));
                const uint64_t rhs = eval(inst->getOperand(1));
                uint64_t result    = 0;

                switch (inst->getOpcode()) {
                    case llvm::Instruction::Add:
                        result = ModAdd(lhs, rhs, bitwidth);
                        break;
                    case llvm::Instruction::Sub:
                        result = ModSub(lhs, rhs, bitwidth);
                        break;
                    case llvm::Instruction::Mul:
                        result = ModMul(lhs, rhs, bitwidth);
                        break;
                    case llvm::Instruction::And:
                        result = (lhs & rhs) & mask;
                        break;
                    case llvm::Instruction::Or:
                        result = (lhs | rhs) & mask;
                        break;
                    case llvm::Instruction::Xor:
                        result = (lhs ^ rhs) & mask;
                        break;
                    case llvm::Instruction::LShr:
                        result = (lhs >> rhs) & mask;
                        break;
                    default:
                        result = 0;
                        break;
                }

                cache[v] = result;
                return result;
            };

            return eval(root);
        }

        bool HasPolynomialMul(
            llvm::Instruction *root, const llvm::DenseSet< llvm::Value * > &visited_tree
        ) {
            llvm::DenseMap< llvm::Value *, bool > depends_on_var;

            std::function< bool(llvm::Value *) > check = [&](llvm::Value *v) -> bool {
                auto it = depends_on_var.find(v);
                if (it != depends_on_var.end()) { return it->second; }

                if (llvm::isa< llvm::ConstantInt >(v)) {
                    depends_on_var[v] = false;
                    return false;
                }

                auto *inst = llvm::dyn_cast< llvm::Instruction >(v);
                if (!inst || !visited_tree.contains(inst)) {
                    depends_on_var[v] = true;
                    return true;
                }

                if (inst->getOpcode() == llvm::Instruction::ZExt
                    || inst->getOpcode() == llvm::Instruction::SExt)
                {
                    const bool dep    = check(inst->getOperand(0));
                    depends_on_var[v] = dep;
                    return dep;
                }

                const bool lhs_dep = check(inst->getOperand(0));
                const bool rhs_dep =
                    inst->getNumOperands() > 1 ? check(inst->getOperand(1)) : false;
                const bool dep    = lhs_dep || rhs_dep;
                depends_on_var[v] = dep;

                if (inst->getOpcode() == llvm::Instruction::Mul && lhs_dep && rhs_dep) {
                    return true;
                }
                return dep;
            };

            llvm::DenseSet< llvm::Value * > visited;
            std::queue< llvm::Value * > work;
            work.push(root);

            while (!work.empty()) {
                auto *v = work.front();
                work.pop();
                if (!visited.insert(v).second) { continue; }
                check(v);
                if (auto *inst = llvm::dyn_cast< llvm::Instruction >(v)) {
                    if (visited_tree.contains(inst) != 0u) {
                        for (auto &op : inst->operands()) { work.push(op.get()); }
                    }
                }
            }

            for (auto *v : visited) {
                auto *inst = llvm::dyn_cast< llvm::Instruction >(v);
                if (inst == nullptr) { continue; }
                if (inst->getOpcode() != llvm::Instruction::Mul) { continue; }
                if (visited_tree.contains(inst) == 0u) { continue; }
                const bool lhs_dep = depends_on_var.lookup(inst->getOperand(0));
                const bool rhs_dep = depends_on_var.lookup(inst->getOperand(1));
                if (lhs_dep && rhs_dep) { return true; }
            }
            return false;
        }

        std::unique_ptr< Expr > BuildExprFromIR(
            llvm::Value *v, const std::vector< llvm::Value * > &leaves,
            const llvm::DenseSet< llvm::Value * > &tree_set, uint64_t mask,
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects
        ) {
            // Phi redirect: build from the chosen arm.
            auto pit = phi_redirects.find(v);
            if (pit != phi_redirects.end()) {
                return BuildExprFromIR(pit->second, leaves, tree_set, mask, phi_redirects);
            }

            // Constant
            if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(v)) {
                return Expr::Constant(ci->getZExtValue() & mask);
            }

            // Leaf (variable)
            auto leaf_it = std::find(leaves.begin(), leaves.end(), v);
            if (leaf_it != leaves.end()) {
                auto idx = static_cast< uint32_t >(leaf_it - leaves.begin());
                return Expr::Variable(idx);
            }

            auto *inst = llvm::dyn_cast< llvm::Instruction >(v);
            if (inst == nullptr || !tree_set.contains(inst)) { return nullptr; }

            // ZExt/SExt — pass through to inner operand
            if (inst->getOpcode() == llvm::Instruction::ZExt
                || inst->getOpcode() == llvm::Instruction::SExt)
            {
                return BuildExprFromIR(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects
                );
            }

            // LShr with constant shift amount
            if (inst->getOpcode() == llvm::Instruction::LShr) {
                auto *shift_amt = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1));
                if (shift_amt == nullptr) { return nullptr; }
                auto child =
                    BuildExprFromIR(inst->getOperand(0), leaves, tree_set, mask, phi_redirects);
                if (child == nullptr) { return nullptr; }
                return Expr::LogicalShr(std::move(child), shift_amt->getZExtValue());
            }

            // Binary operations
            auto lhs =
                BuildExprFromIR(inst->getOperand(0), leaves, tree_set, mask, phi_redirects);
            auto rhs =
                BuildExprFromIR(inst->getOperand(1), leaves, tree_set, mask, phi_redirects);
            if (lhs == nullptr || rhs == nullptr) { return nullptr; }

            switch (inst->getOpcode()) {
                case llvm::Instruction::Add:
                    return Expr::Add(std::move(lhs), std::move(rhs));
                case llvm::Instruction::Sub:
                    return Expr::Add(std::move(lhs), Expr::Negate(std::move(rhs)));
                case llvm::Instruction::Mul:
                    return Expr::Mul(std::move(lhs), std::move(rhs));
                case llvm::Instruction::And:
                    return Expr::BitwiseAnd(std::move(lhs), std::move(rhs));
                case llvm::Instruction::Or:
                    return Expr::BitwiseOr(std::move(lhs), std::move(rhs));
                case llvm::Instruction::Xor: {
                    // Detect NOT: xor %x, -1
                    if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1))) {
                        if (ci->isAllOnesValue()) { return Expr::BitwiseNot(std::move(lhs)); }
                    }
                    if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(0))) {
                        if (ci->isAllOnesValue()) { return Expr::BitwiseNot(std::move(rhs)); }
                    }
                    return Expr::BitwiseXor(std::move(lhs), std::move(rhs));
                }
                default:
                    return nullptr;
            }
        }

        // Check that every leaf dependency of an alternative phi arm
        // is either a constant or present in `leaf_set`.  This guards
        // against evaluating arms whose subtrees reference variables
        // outside the candidate's variable set.
        bool ArmDepsInLeafSet(
            llvm::Value *arm_root, const llvm::DenseSet< llvm::Value * > &leaf_set
        ) {
            llvm::DenseSet< llvm::Value * > visited;
            std::queue< llvm::Value * > work;
            work.push(arm_root);

            while (!work.empty()) {
                auto *v = work.front();
                work.pop();
                if (!visited.insert(v).second) { continue; }

                if (llvm::isa< llvm::ConstantInt >(v)) { continue; }
                if (leaf_set.contains(v)) { continue; }

                auto *inst = llvm::dyn_cast< llvm::Instruction >(v);
                if ((inst == nullptr) || !IsMbaOpcode(inst->getOpcode())) { return false; }

                // LShr with variable shift — can't evaluate
                if (inst->getOpcode() == llvm::Instruction::LShr
                    && !llvm::isa< llvm::ConstantInt >(inst->getOperand(1)))
                {
                    if (!leaf_set.contains(inst)) { return false; }
                    continue;
                }

                for (auto &op : inst->operands()) { work.push(op.get()); }
            }
            return true;
        }

        // Verify that all incoming arms of each transparent phi
        // evaluate identically for random inputs.  Returns false if
        // any arm diverges, indicating the phi should be treated as
        // a leaf instead.
        bool VerifyPhiArms(
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            const std::vector< llvm::Value * > &leaves, uint32_t bitwidth
        ) {
            if (phi_redirects.empty()) { return true; }

            const uint64_t mask           = Bitmask(bitwidth);
            constexpr uint32_t kNumProbes = 8;

            llvm::DenseSet< llvm::Value * > leaf_set;
            for (auto *lv : leaves) { leaf_set.insert(lv); }

            // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp)
            std::mt19937_64 rng(0xC0B7A);

            for (const auto &[phi_val, chosen] : phi_redirects) {
                auto *phi = llvm::cast< llvm::PHINode >(phi_val);

                for (uint32_t probe = 0; probe < kNumProbes; ++probe) {
                    llvm::DenseMap< llvm::Value *, uint64_t > assignments;
                    for (auto *leaf : leaves) { assignments[leaf] = rng() & mask; }

                    // Evaluate chosen arm.
                    uint64_t chosen_val = 0;
                    if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(chosen)) {
                        chosen_val = ci->getZExtValue() & mask;
                    } else {
                        chosen_val = EvaluateTree(
                            llvm::cast< llvm::Instruction >(chosen), assignments, bitwidth,
                            phi_redirects
                        );
                    }

                    // Check every other arm.
                    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
                        auto *inc = phi->getIncomingValue(i);
                        if (inc == chosen) { continue; }

                        // Pre-check: arm's dependencies must be in our leaf set.
                        if (!ArmDepsInLeafSet(inc, leaf_set)) { return false; }

                        uint64_t inc_val = 0;
                        if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(inc)) {
                            inc_val = ci->getZExtValue() & mask;
                        } else {
                            inc_val = EvaluateTree(
                                llvm::cast< llvm::Instruction >(inc), assignments, bitwidth,
                                phi_redirects
                            );
                        }

                        if (inc_val != chosen_val) { return false; }
                    }
                }
            }
            return true;
        }

    } // namespace

    std::vector< MBACandidate >
    DetectMbaCandidates(llvm::Function &f, uint32_t min_ast_size, uint32_t /*max_vars*/) {
        std::vector< MBACandidate > candidates;
        llvm::DenseSet< llvm::Instruction * > already_in_tree;

        // Post-order: process uses before defs across blocks.
        // Within each block, reverse iteration hits outermost roots
        // first, so the largest MBA tree claims inner nodes before
        // they can be emitted as standalone candidates.
        for (auto *bb : post_order(&f)) {
            for (auto &inst : llvm::reverse(*bb)) {
                if (!IsMbaOpcode(inst.getOpcode())) { continue; }
                if (already_in_tree.contains(&inst) != 0u) { continue; }

                if (!inst.getType()->isIntegerTy()) { continue; }
                const uint32_t bw = inst.getType()->getIntegerBitWidth();
                if (bw > 64) { continue; }

                llvm::SmallVector< llvm::Instruction *, 16 > tree_insts;
                std::vector< llvm::Value * > leaves;
                llvm::DenseMap< llvm::Value *, llvm::Value * > phi_redirects;
                CollectTree(&inst, tree_insts, leaves, phi_redirects);

                if (tree_insts.size() < min_ast_size) { continue; }

                constexpr uint32_t kPreElimCap = 20;
                if (leaves.size() > kPreElimCap) { continue; }

                const llvm::DenseSet< llvm::Value * > tree_set(
                    tree_insts.begin(), tree_insts.end()
                );
                if (HasPolynomialMul(&inst, tree_set)) { continue; }

                // Verify transparent phis — if any arm diverges,
                // re-collect without phi transparency.
                if (!phi_redirects.empty() && !VerifyPhiArms(phi_redirects, leaves, bw)) {
                    tree_insts.clear();
                    leaves.clear();
                    phi_redirects.clear();
                    CollectTree(
                        &inst, tree_insts, leaves, phi_redirects,
                        /*try_phi_transparency=*/false
                    );

                    if (tree_insts.size() < min_ast_size) { continue; }
                    if (leaves.size() > kPreElimCap) { continue; }
                }

                for (auto *ti : tree_insts) { already_in_tree.insert(ti); }

                const auto num_vars  = static_cast< uint32_t >(leaves.size());
                const size_t sig_len = 1ULL << num_vars;
                std::vector< uint64_t > sig(sig_len);

                for (size_t i = 0; i < sig_len; ++i) {
                    llvm::DenseMap< llvm::Value *, uint64_t > assignments;
                    for (uint32_t v = 0; v < num_vars; ++v) {
                        assignments[leaves[v]] = (i >> v) & 1;
                    }
                    sig[i] = EvaluateTree(&inst, assignments, bw, phi_redirects);
                }

                std::vector< std::string > var_names;
                for (uint32_t v = 0; v < num_vars; ++v) {
                    if (leaves[v]->hasName()) {
                        var_names.push_back(leaves[v]->getName().str());
                    } else {
                        var_names.push_back("v" + std::to_string(v));
                    }
                }

                const uint64_t mask = Bitmask(bw);
                auto expr = BuildExprFromIR(&inst, leaves, tree_set, mask, phi_redirects);

                // Build evaluator lambda for full-width verification.
                // Captures raw pointers to LLVM Values which remain valid
                // for the lifetime of the function being processed.
                Evaluator evaluator;
                if (expr != nullptr) {
                    evaluator = [root_inst = &inst, leaf_vals = leaves, bitwidth = bw,
                                 redirects = phi_redirects](
                                    const std::vector< uint64_t > &vals
                                ) -> uint64_t {
                        llvm::DenseMap< llvm::Value *, uint64_t > assignments;
                        for (size_t i = 0; i < leaf_vals.size(); ++i) {
                            assignments[leaf_vals[i]] = vals[i];
                        }
                        return EvaluateTree(root_inst, assignments, bitwidth, redirects);
                    };
                }

                candidates.push_back(
                    MBACandidate{ .root        = &inst,
                                  .leaf_values = std::move(leaves),
                                  .var_names   = std::move(var_names),
                                  .sig         = std::move(sig),
                                  .bitwidth    = bw,
                                  .expr        = std::move(expr),
                                  .evaluator   = std::move(evaluator) }
                );
            }
        }

        return candidates;
    }

} // namespace cobra
