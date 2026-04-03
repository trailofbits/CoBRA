// absl must be included before MicrocodeDetector.h (which pulls hexrays.hpp):
// the IDA SDK poisons stdout/stderr/fwrite/fflush/snprintf via fpro.h macros.
#include <absl/container/flat_hash_set.h>

#include "MicrocodeDetector.h"

namespace ida_cobra {
    namespace {

        // Minimum opcode counts to classify an instruction tree as MBA.
        constexpr int kMinBoolOps  = 1;
        constexpr int kMinArithOps = 1;

        // Maximum variables CoBRA can handle.
        constexpr uint32_t kMaxVars = 16;

        struct OpcodeCounter : public minsn_visitor_t
        {
            int bool_cnt  = 0;
            int arith_cnt = 0;

            int idaapi visit_minsn() override {
                switch (curins->opcode) {
                    case m_neg:
                    case m_add:
                    case m_sub:
                    case m_mul:
                        arith_cnt++;
                        break;
                    case m_bnot:
                    case m_or:
                    case m_and:
                    case m_xor:
                        bool_cnt++;
                        break;
                    default:
                        return 0;
                }
                return (bool_cnt >= kMinBoolOps && arith_cnt >= kMinArithOps) ? 1 : 0;
            }
        };

    } // anonymous namespace

    uint64_t EvalMinsn(
        const minsn_t &insn, const std::vector< mop_t * > &var_keys,
        const std::vector< uint64_t > &var_vals, uint64_t mask
    ) {
        auto resolve_leaf = [&](const mop_t &op) -> uint64_t {
            switch (op.t) {
                case mop_n:
                    return static_cast< uint64_t >(op.nnn->value) & mask;
                case mop_r:
                case mop_l:
                case mop_S:
                case mop_v:
                    for (size_t i = 0; i < var_keys.size(); ++i) {
                        if (*var_keys[i] == op) { return var_vals[i]; }
                    }
                    return 0;
                default:
                    return 0;
            }
        };

        // Flatten the minsn tree into post-order, then evaluate
        // bottom-up with a value stack.
        auto post = MicrocodePostOrder(insn);

        std::vector< uint64_t > vals;
        for (auto it = post.rbegin(); it != post.rend(); ++it) {
            const minsn_t *n = *it;

            uint64_t r = 0;
            if (n->r.t == mop_d) {
                r = vals.back();
                vals.pop_back();
            } else {
                r = resolve_leaf(n->r);
            }

            uint64_t l = 0;
            if (n->l.t == mop_d) {
                l = vals.back();
                vals.pop_back();
            } else {
                l = resolve_leaf(n->l);
            }

            switch (n->opcode) {
                case m_add:
                    vals.push_back((l + r) & mask);
                    break;
                case m_sub:
                    vals.push_back((l - r) & mask);
                    break;
                case m_mul:
                    vals.push_back((l * r) & mask);
                    break;
                case m_and:
                    vals.push_back(l & r);
                    break;
                case m_or:
                    vals.push_back(l | r);
                    break;
                case m_xor:
                    vals.push_back(l ^ r);
                    break;
                case m_bnot:
                    vals.push_back((~l) & mask);
                    break;
                case m_neg:
                    vals.push_back((static_cast< uint64_t >(0) - l) & mask);
                    break;
                default:
                    vals.push_back(0);
                    break;
            }
        }

        return vals.back();
    }

    namespace {

        // Collect unique leaf (input) operands from a minsn tree.
        // Only walks .l and .r — never .d (the destination).
        // Uses an explicit worklist instead of recursion.
        struct LeafCollector
        {
            std::vector< mop_t * > leaves;

            void Collect(minsn_t &root) {
                std::vector< mop_t * > worklist;
                worklist.push_back(&root.r);
                worklist.push_back(&root.l);

                while (!worklist.empty()) {
                    mop_t *op = worklist.back();
                    worklist.pop_back();

                    if (op->t == mop_d) {
                        worklist.push_back(&op->d->r);
                        worklist.push_back(&op->d->l);
                        continue;
                    }
                    if (op->t == mop_n || op->t == mop_z) { continue; }

                    if (op->t == mop_r || op->t == mop_l || op->t == mop_S || op->t == mop_v) {
                        bool found = false;
                        for (const auto *existing : leaves) {
                            if (*existing == *op) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) { leaves.push_back(op); }
                    }
                }
            }
        };

        // Derive the MBA bitwidth from the operand sizes of its leaves.
        uint32_t LeafBitwidth(const std::vector< mop_t * > &leaves) {
            int max_size = 0;
            for (const auto *op : leaves) {
                if (op->size > max_size) { max_size = op->size; }
            }
            return max_size > 0 ? static_cast< uint32_t >(max_size) * 8 : 64;
        }

        // Build a human-readable name for a leaf operand.
        std::string LeafName(const mop_t &op) {
            qstring buf;
            op.print(&buf);
            return std::string(buf.c_str());
        }

    } // anonymous namespace

    bool IsMba(const minsn_t &insn) {
        if (insn.opcode >= m_jcnd) { return false; }

        if (insn.d.size > 8) { return false; }

        OpcodeCounter counter;
        return const_cast< minsn_t & >(insn).for_all_insns(counter) != 0;
    }

    namespace {

        // Return true if the opcode is one that participates in MBA
        // expressions (arithmetic or boolean).
        bool IsMbaOpcode(mcode_t op) {
            switch (op) {
                case m_neg:
                case m_bnot:
                case m_add:
                case m_sub:
                case m_mul:
                case m_or:
                case m_and:
                case m_xor:
                    return true;
                default:
                    return false;
            }
        }

        // Peel only m_mov wrappers. Accept MBA ops or xdu/xds as
        // valid roots — extensions are part of the candidate tree.
        minsn_t *MbaRoot(minsn_t *insn) {
            while (insn->opcode == m_mov && insn->l.t == mop_d) { insn = insn->l.d; }
            if (IsMbaOpcode(insn->opcode)) { return insn; }
            if (insn->opcode == m_xdu || insn->opcode == m_xds) { return insn; }
            return nullptr;
        }

    } // anonymous namespace

    std::vector< MBACandidate > DetectMbaCandidates(mba_t &mba) {
        std::vector< MBACandidate > candidates;

        struct DetectorVisitor : public minsn_visitor_t
        {
            std::vector< MBACandidate > &out;

            explicit DetectorVisitor(std::vector< MBACandidate > &o) : out(o) {}

            int idaapi visit_minsn() override {
                if (!IsMba(*curins)) { return 0; }
                minsn_t *root = MbaRoot(curins);
                if (!root) { return 0; }

                LeafCollector lc;
                lc.Collect(*root);

                if (lc.leaves.size() > kMaxVars) { return 0; }

                uint32_t bitwidth = static_cast< uint32_t >(root->d.size) * 8;
                if (bitwidth == 0 || bitwidth > 64) { return 0; }
                uint64_t mask =
                    bitwidth >= 64 ? ~uint64_t{ 0 } : (uint64_t{ 1 } << bitwidth) - 1;

                uint32_t n = static_cast< uint32_t >(lc.leaves.size());

                std::vector< uint64_t > sig;
                sig.reserve(uint64_t{ 1 } << n);

                for (uint64_t input = 0; input < (uint64_t{ 1 } << n); ++input) {
                    std::vector< uint64_t > vals(n);
                    for (uint32_t v = 0; v < n; ++v) { vals[v] = (input >> v) & 1; }

                    sig.push_back(EvalMinsn(*root, lc.leaves, vals, mask));
                }

                std::vector< std::string > names;
                names.reserve(n);
                for (auto *leaf : lc.leaves) { names.push_back(LeafName(*leaf)); }

                out.push_back(
                    MBACandidate{
                        .root      = root,
                        .leaves    = std::move(lc.leaves),
                        .var_names = std::move(names),
                        .sig       = std::move(sig),
                        .bitwidth  = bitwidth,
                    }
                );

                return 0;
            }
        };

        DetectorVisitor visitor(candidates);
        mba.for_all_topinsns(visitor);

        return candidates;
    }

    std::vector< MBACandidate > DetectMbaCandidatesCrossBlock(mba_t &mba) {
        struct CrossBlockDetector
        {
            mba_t &mba;
            absl::flat_hash_set< const minsn_t * > already_in_tree;
            std::vector< MBACandidate > candidates;

            explicit CrossBlockDetector(mba_t &m) : mba(m) {}

            void Run() {
                node_ordering_t post_order;
                mba.get_graph()->depth_first_postorder(&post_order);

                for (size_t i = 0; i < post_order.size(); ++i) {
                    int blk_idx   = post_order.node(i);
                    mblock_t *blk = mba.get_mblock(blk_idx);

                    for (minsn_t *insn = blk->tail; insn != nullptr; insn = insn->prev) {
                        if (already_in_tree.count(insn) != 0) { continue; }

                        if (!IsMba(*insn)) { continue; }
                        minsn_t *root = MbaRoot(insn);
                        if (!root) { continue; }

                        LeafCollector lc;
                        MarkTree(root, lc);

                        if (lc.leaves.size() > kMaxVars) { continue; }

                        uint32_t bitwidth = static_cast< uint32_t >(root->d.size) * 8;
                        if (bitwidth == 0 || bitwidth > 64) { continue; }
                        uint64_t mask =
                            bitwidth >= 64 ? ~uint64_t{ 0 } : (uint64_t{ 1 } << bitwidth) - 1;

                        uint32_t n = static_cast< uint32_t >(lc.leaves.size());

                        std::vector< uint64_t > sig;
                        sig.reserve(uint64_t{ 1 } << n);
                        for (uint64_t input = 0; input < (uint64_t{ 1 } << n); ++input) {
                            std::vector< uint64_t > vals(n);
                            for (uint32_t v = 0; v < n; ++v) { vals[v] = (input >> v) & 1; }
                            sig.push_back(EvalMinsn(*root, lc.leaves, vals, mask));
                        }

                        std::vector< std::string > names;
                        names.reserve(n);
                        for (auto *leaf : lc.leaves) { names.push_back(LeafName(*leaf)); }

                        candidates.push_back(
                            MBACandidate{
                                .root      = root,
                                .leaves    = std::move(lc.leaves),
                                .var_names = std::move(names),
                                .sig       = std::move(sig),
                                .bitwidth  = bitwidth,
                            }
                        );
                    }
                }
            }

            void MarkTree(minsn_t *insn, LeafCollector &lc) {
                already_in_tree.insert(insn);
                lc.Collect(*insn);
                // Cross-block extension via graph_chains_t is deferred
                // until the use-def chain API is validated via manual
                // testing in IDA. For now, this falls through to
                // intra-block behavior (same as Tier 1 but with
                // post-order + reverse walk + already_in_tree dedup).
            }
        };

        CrossBlockDetector detector(mba);
        detector.Run();
        return std::move(detector.candidates);
    }

} // namespace ida_cobra
