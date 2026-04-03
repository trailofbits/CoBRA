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
        const minsn_t &insn, const absl::flat_hash_map< const mop_t *, uint64_t > &var_values,
        uint64_t mask
    ) {
        auto eval_operand = [&](const mop_t &op) -> uint64_t {
            switch (op.t) {
                case mop_d:
                    return EvalMinsn(*op.d, var_values, mask);
                case mop_n:
                    return static_cast< uint64_t >(op.nnn->value) & mask;
                case mop_r:
                case mop_l:
                case mop_S:
                case mop_v: {
                    auto it = var_values.find(&op);
                    if (it != var_values.end()) { return it->second; }
                    return 0;
                }
                default:
                    return 0;
            }
        };

        uint64_t l = eval_operand(insn.l);
        uint64_t r = eval_operand(insn.r);

        switch (insn.opcode) {
            case m_add:
                return (l + r) & mask;
            case m_sub:
                return (l - r) & mask;
            case m_mul:
                return (l * r) & mask;
            case m_and:
                return l & r;
            case m_or:
                return l | r;
            case m_xor:
                return l ^ r;
            case m_bnot:
                return (~l) & mask;
            case m_neg:
                return (static_cast< uint64_t >(0) - l) & mask;
            default:
                return 0;
        }
    }

    namespace {

        // Collect leaf operands from a minsn tree by walking .l and .r recursively.
        // Non-mop_d, non-mop_n operands become leaves.
        struct LeafCollector : public mop_visitor_t
        {
            std::vector< mop_t * > leaves;
            absl::flat_hash_set< const mop_t * > seen;

            int idaapi visit_mop(mop_t *op, const tinfo_t *, bool) override {
                if (op->t == mop_d || op->t == mop_n || op->t == mop_z) {
                    return 0; // recurse into nested insns, skip constants/empty
                }

                // Variable-like operand: register, local, stack, global
                if (op->t == mop_r || op->t == mop_l || op->t == mop_S || op->t == mop_v) {
                    if (seen.insert(op).second) { leaves.push_back(op); }
                }
                prune = true; // don't descend further into this operand
                return 0;
            }
        };

        // Build a human-readable name for a leaf operand.
        std::string LeafName(const mop_t &op) {
            qstring buf;
            op.print(&buf);
            return std::string(buf.c_str());
        }

    } // anonymous namespace

    bool IsMba(const minsn_t &insn) {
        if (is_mcode_xdsu(insn.opcode)) { return false; }

        if (insn.opcode >= m_jcnd) { return false; }

        if (insn.d.size > 8) { return false; }

        OpcodeCounter counter;
        return const_cast< minsn_t & >(insn).for_all_insns(counter) != 0;
    }

    std::vector< MBACandidate > DetectMbaCandidates(mba_t &mba) {
        std::vector< MBACandidate > candidates;

        struct DetectorVisitor : public minsn_visitor_t
        {
            std::vector< MBACandidate > &out;

            explicit DetectorVisitor(std::vector< MBACandidate > &o) : out(o) {}

            int idaapi visit_minsn() override {
                if (!IsMba(*curins)) { return 0; }

                // Collect leaves
                LeafCollector lc;
                curins->for_all_ops(lc);

                if (lc.leaves.size() > kMaxVars) { return 0; }

                uint32_t bitwidth =
                    curins->d.size > 0 ? static_cast< uint32_t >(curins->d.size) * 8 : 64;
                uint64_t mask =
                    bitwidth >= 64 ? ~uint64_t{ 0 } : (uint64_t{ 1 } << bitwidth) - 1;

                uint32_t n = static_cast< uint32_t >(lc.leaves.size());

                // Compute boolean signature: evaluate on all 2^n inputs
                // from {0, 1}^n
                std::vector< uint64_t > sig;
                sig.reserve(uint64_t{ 1 } << n);

                for (uint64_t input = 0; input < (uint64_t{ 1 } << n); ++input) {
                    absl::flat_hash_map< const mop_t *, uint64_t > vals;
                    for (uint32_t v = 0; v < n; ++v) { vals[lc.leaves[v]] = (input >> v) & 1; }

                    sig.push_back(EvalMinsn(*curins, vals, mask));
                }

                // Build var names
                std::vector< std::string > names;
                names.reserve(n);
                for (auto *leaf : lc.leaves) { names.push_back(LeafName(*leaf)); }

                out.push_back(
                    MBACandidate{
                        .root      = curins,
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
                mba.get_graph()->depth_first_postorder_for_all_entries(&post_order);

                for (size_t i = 0; i < post_order.size(); ++i) {
                    int blk_idx   = post_order.node(i);
                    mblock_t *blk = mba.get_mblock(blk_idx);

                    for (minsn_t *insn = blk->tail; insn != nullptr; insn = insn->prev) {
                        if (already_in_tree.count(insn) != 0) { continue; }

                        if (!IsMba(*insn)) { continue; }

                        LeafCollector lc;
                        MarkTree(insn, lc);

                        if (lc.leaves.size() > kMaxVars) { continue; }

                        uint32_t bitwidth =
                            insn->d.size > 0 ? static_cast< uint32_t >(insn->d.size) * 8 : 64;
                        uint64_t mask =
                            bitwidth >= 64 ? ~uint64_t{ 0 } : (uint64_t{ 1 } << bitwidth) - 1;

                        uint32_t n = static_cast< uint32_t >(lc.leaves.size());

                        std::vector< uint64_t > sig;
                        sig.reserve(uint64_t{ 1 } << n);
                        for (uint64_t input = 0; input < (uint64_t{ 1 } << n); ++input) {
                            absl::flat_hash_map< const mop_t *, uint64_t > vals;
                            for (uint32_t v = 0; v < n; ++v) {
                                vals[lc.leaves[v]] = (input >> v) & 1;
                            }
                            sig.push_back(EvalMinsn(*insn, vals, mask));
                        }

                        std::vector< std::string > names;
                        names.reserve(n);
                        for (auto *leaf : lc.leaves) { names.push_back(LeafName(*leaf)); }

                        candidates.push_back(
                            MBACandidate{
                                .root      = insn,
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
                insn->for_all_ops(lc);
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
