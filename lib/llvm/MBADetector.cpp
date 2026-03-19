#include "MBADetector.h"
#include "cobra/core/BitWidth.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Casting.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <queue>
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
                case llvm::Instruction::ZExt:
                case llvm::Instruction::SExt:
                    return true;
                default:
                    return false;
            }
        }

        void CollectTree(
            llvm::Instruction *root, llvm::SmallVector< llvm::Instruction *, 16 > &tree_insts,
            std::vector< llvm::Value * > &leaves
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
                    tree_insts.push_back(inst);
                    for (auto &op : inst->operands()) { work.push(op.get()); }
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
            const llvm::DenseMap< llvm::Value *, uint64_t > &assignments, uint32_t bitwidth
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
            // Check if any Mul instruction has both operands depending on variables.
            // Uses a DFS that tracks whether each value is variable-dependent.
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
                    depends_on_var[v] = true; // leaf = variable
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
                    return true; // signal polynomial mul found
                }
                return dep;
            };

            // We need to detect polynomial mul, not just return dependency.
            // Rewrite as a separate traversal.
            llvm::DenseSet< llvm::Value * > visited;
            std::queue< llvm::Value * > work;
            work.push(root);

            // First pass: compute variable-dependence for all nodes
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

            // Second pass: check all Mul instructions
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

    } // namespace

    std::vector< MBACandidate >
    DetectMbaCandidates(llvm::BasicBlock &bb, uint32_t min_ast_size, uint32_t /*max_vars*/) {
        std::vector< MBACandidate > candidates;
        llvm::DenseSet< llvm::Instruction * > already_in_tree;

        for (auto &inst : bb) {
            if (!IsMbaOpcode(inst.getOpcode())) { continue; }
            if (already_in_tree.contains(&inst) != 0u) { continue; }

            if (!inst.getType()->isIntegerTy()) { continue; }
            const uint32_t bw = inst.getType()->getIntegerBitWidth();
            if (bw > 64) { continue; }

            llvm::SmallVector< llvm::Instruction *, 16 > tree_insts;
            std::vector< llvm::Value * > leaves;
            CollectTree(&inst, tree_insts, leaves);

            if (tree_insts.size() < min_ast_size) { continue; }

            constexpr uint32_t kPreElimCap = 20;
            if (leaves.size() > kPreElimCap) { continue; }

            // Reject polynomial MBA (variable*variable multiplication)
            const llvm::DenseSet< llvm::Value * > tree_set(
                tree_insts.begin(), tree_insts.end()
            );
            if (HasPolynomialMul(&inst, tree_set)) { continue; }

            for (auto *ti : tree_insts) { already_in_tree.insert(ti); }

            const auto num_vars  = static_cast< uint32_t >(leaves.size());
            const size_t sig_len = 1ULL << num_vars;
            std::vector< uint64_t > sig(sig_len);

            for (size_t i = 0; i < sig_len; ++i) {
                llvm::DenseMap< llvm::Value *, uint64_t > assignments;
                for (uint32_t v = 0; v < num_vars; ++v) {
                    assignments[leaves[v]] = (i >> v) & 1;
                }
                sig[i] = EvaluateTree(&inst, assignments, bw);
            }

            std::vector< std::string > var_names;
            for (uint32_t v = 0; v < num_vars; ++v) {
                if (leaves[v]->hasName()) {
                    var_names.push_back(leaves[v]->getName().str());
                } else {
                    var_names.push_back("v" + std::to_string(v));
                }
            }

            candidates.push_back(
                MBACandidate{ .root        = &inst,
                              .leaf_values = std::move(leaves),
                              .var_names   = std::move(var_names),
                              .sig         = std::move(sig),
                              .bitwidth    = bw }
            );
        }

        return candidates;
    }

} // namespace cobra
