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
        const minsn_t &insn,
        const std::vector< mop_t * > &var_keys,
        const std::vector< uint64_t > &var_vals,
        uint64_t mask
    ) {
        auto eval_operand = [&](const mop_t &op) -> uint64_t {
            switch (op.t) {
                case mop_d:
                    return EvalMinsn(*op.d, var_keys, var_vals, mask);
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

        // Collect unique leaf (input) operands from a minsn tree.
        // Only walks .l and .r — never .d (the destination).
        // Deduplicates by value equality (mop_t::operator==).
        struct LeafCollector
        {
            std::vector< mop_t * > leaves;

            void Collect(minsn_t &insn) {
                CollectOp(insn.l);
                CollectOp(insn.r);
            }

        private:
            void CollectOp(mop_t &op) {
                if (op.t == mop_d) {
                    Collect(*op.d);
                    return;
                }
                if (op.t == mop_n || op.t == mop_z) { return; }

                if (op.t == mop_r || op.t == mop_l || op.t == mop_S || op.t == mop_v) {
                    for (const auto *existing : leaves) {
                        if (*existing == op) { return; }
                    }
                    leaves.push_back(&op);
                }
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

                LeafCollector lc;
                lc.Collect(*curins);

                if (lc.leaves.size() > kMaxVars) { return 0; }

                uint32_t bitwidth =
                    curins->d.size > 0 ? static_cast< uint32_t >(curins->d.size) * 8 : 64;
                uint64_t mask =
                    bitwidth >= 64 ? ~uint64_t{ 0 } : (uint64_t{ 1 } << bitwidth) - 1;

                uint32_t n = static_cast< uint32_t >(lc.leaves.size());

                std::vector< uint64_t > sig;
                sig.reserve(uint64_t{ 1 } << n);

                for (uint64_t input = 0; input < (uint64_t{ 1 } << n); ++input) {
                    std::vector< uint64_t > vals(n);
                    for (uint32_t v = 0; v < n; ++v) { vals[v] = (input >> v) & 1; }

                    sig.push_back(EvalMinsn(*curins, lc.leaves, vals, mask));
                }

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
                mba.get_graph()->depth_first_postorder(&post_order);

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
                            std::vector< uint64_t > vals(n);
                            for (uint32_t v = 0; v < n; ++v) {
                                vals[v] = (input >> v) & 1;
                            }
                            sig.push_back(EvalMinsn(*insn, lc.leaves, vals, mask));
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
