#include "cobra/core/PatternMatcher.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Profile.h"
#include "cobra/core/Trace.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        bool AllEqual(const std::vector< uint64_t > &sig) {
            return std::all_of(sig.begin(), sig.end(), [&](uint64_t v) { return v == sig[0]; });
        }

        std::optional< std::unique_ptr< Expr > >
        Match1var(const std::vector< uint64_t > &sig, uint32_t bitwidth) {
            if (sig[0] == 0 && sig[1] == 1) { return Expr::Variable(0); }
            if (sig[0] == 0 && sig[1] != 0) {
                return Expr::Mul(Expr::Constant(sig[1]), Expr::Variable(0));
            }
            const uint64_t a = ModSub(sig[1], sig[0], bitwidth);
            if (a != 0) {
                auto term = Expr::Mul(Expr::Constant(a), Expr::Variable(0));
                return Expr::Add(Expr::Constant(sig[0]), std::move(term));
            }
            return std::nullopt;
        }

        bool IsBooleanSig(const std::vector< uint64_t > &sig) {
            return std::all_of(sig.begin(), sig.end(), [](uint64_t v) { return v <= 1; });
        }

        uint32_t PackBoolSig(const std::vector< uint64_t > &sig) {
            uint32_t key = 0;
            for (size_t i = 0; i < sig.size(); ++i) {
                if (sig[i] != 0u) { key |= (uint32_t{ 1 } << i); }
            }
            return key;
        }

        // All 16 Boolean functions of 2 variables, keyed by packed sig.
        // sig index i encodes x=bit0, y=bit1 of i.
        // Key bit i is set when f(x,y)=1 for that (x,y) assignment.
        // Constants (0x0, 0xF) are handled earlier by all_equal.
        std::optional< std::unique_ptr< Expr > > Match2varBoolean(uint8_t key) {
            auto x = []() { return Expr::Variable(0); };
            auto y = []() { return Expr::Variable(1); };

            // key bits: [f(0,0), f(1,0), f(0,1), f(1,1)]
            switch (key) {
                case 0x1:
                    return Expr::BitwiseNot(Expr::BitwiseOr(x(), y()));
                case 0x2:
                    return Expr::BitwiseAnd(x(), Expr::BitwiseNot(y()));
                case 0x3:
                    return Expr::BitwiseNot(y());
                case 0x4:
                    return Expr::BitwiseAnd(Expr::BitwiseNot(x()), y());
                case 0x5:
                    return Expr::BitwiseNot(x());
                case 0x6:
                    return Expr::BitwiseXor(x(), y());
                case 0x7:
                    return Expr::BitwiseNot(Expr::BitwiseAnd(x(), y()));
                case 0x8:
                    return Expr::BitwiseAnd(x(), y());
                case 0x9:
                    return Expr::BitwiseNot(Expr::BitwiseXor(x(), y()));
                case 0xA:
                    return x();
                case 0xB:
                    return Expr::BitwiseOr(x(), Expr::BitwiseNot(y()));
                case 0xC:
                    return y();
                case 0xD:
                    return Expr::BitwiseOr(Expr::BitwiseNot(x()), y());
                case 0xE:
                    return Expr::BitwiseOr(x(), y());
                default:
                    return std::nullopt;
            }
        }

        // Complete table of all 254 non-constant Boolean functions of 3
        // variables, keyed by packed sig. Each entry is the minimal-cost
        // expression found by BFS over {AND, OR, XOR, NOT} from variables.
        // sig index i encodes x=bit0, y=bit1, z=bit2.
        // Generated programmatically and verified against truth tables.
        std::optional< std::unique_ptr< Expr > > Match3varBoolean(uint8_t key) {
            auto x = []() { return Expr::Variable(0); };
            auto y = []() { return Expr::Variable(1); };
            auto z = []() { return Expr::Variable(2); };

            switch (key) {
                case 0x01: // ~(x | y | z)
                    return Expr::BitwiseNot(Expr::BitwiseOr(x(), Expr::BitwiseOr(y(), z())));
                case 0x02: // x & ~(y | z)
                    return Expr::BitwiseAnd(x(), Expr::BitwiseNot(Expr::BitwiseOr(y(), z())));
                case 0x03: // ~(y | z)
                    return Expr::BitwiseNot(Expr::BitwiseOr(y(), z()));
                case 0x04: // y & ~(x | z)
                    return Expr::BitwiseAnd(y(), Expr::BitwiseNot(Expr::BitwiseOr(x(), z())));
                case 0x05: // ~(x | z)
                    return Expr::BitwiseNot(Expr::BitwiseOr(x(), z()));
                case 0x06: // ~z & (x ^ y)
                    return Expr::BitwiseAnd(Expr::BitwiseNot(z()), Expr::BitwiseXor(x(), y()));
                case 0x07: // ~(z | x & y)
                    return Expr::BitwiseNot(Expr::BitwiseOr(z(), Expr::BitwiseAnd(x(), y())));
                case 0x08: // x & y & ~z
                    return Expr::BitwiseAnd(x(), Expr::BitwiseAnd(y(), Expr::BitwiseNot(z())));
                case 0x09: // ~(z | (x ^ y))
                    return Expr::BitwiseNot(Expr::BitwiseOr(z(), Expr::BitwiseXor(x(), y())));
                case 0x0A: // x & ~z
                    return Expr::BitwiseAnd(x(), Expr::BitwiseNot(z()));
                case 0x0B: // ~z & (x | ~y)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseNot(z()), Expr::BitwiseOr(x(), Expr::BitwiseNot(y()))
                    );
                case 0x0C: // y & ~z
                    return Expr::BitwiseAnd(y(), Expr::BitwiseNot(z()));
                case 0x0D: // ~z & (y | ~x)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseNot(z()), Expr::BitwiseOr(y(), Expr::BitwiseNot(x()))
                    );
                case 0x0E: // ~z & (x | y)
                    return Expr::BitwiseAnd(Expr::BitwiseNot(z()), Expr::BitwiseOr(x(), y()));
                case 0x0F: // ~z
                    return Expr::BitwiseNot(z());
                case 0x10: // z & ~(x | y)
                    return Expr::BitwiseAnd(z(), Expr::BitwiseNot(Expr::BitwiseOr(x(), y())));
                case 0x11: // ~(x | y)
                    return Expr::BitwiseNot(Expr::BitwiseOr(x(), y()));
                case 0x12: // ~y & (x ^ z)
                    return Expr::BitwiseAnd(Expr::BitwiseNot(y()), Expr::BitwiseXor(x(), z()));
                case 0x13: // ~(y | x & z)
                    return Expr::BitwiseNot(Expr::BitwiseOr(y(), Expr::BitwiseAnd(x(), z())));
                case 0x14: // ~x & (y ^ z)
                    return Expr::BitwiseAnd(Expr::BitwiseNot(x()), Expr::BitwiseXor(y(), z()));
                case 0x15: // ~(x | y & z)
                    return Expr::BitwiseNot(Expr::BitwiseOr(x(), Expr::BitwiseAnd(y(), z())));
                case 0x16: // x ^ ((x & y) | (y ^ z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseOr(Expr::BitwiseAnd(x(), y()), Expr::BitwiseXor(y(), z()))
                    );
                case 0x17: // x ^ ((x ^ ~y) | (y ^ z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseOr(
                            Expr::BitwiseXor(x(), Expr::BitwiseNot(y())),
                            Expr::BitwiseXor(y(), z())
                        )
                    );
                case 0x18: // (x ^ z) & (y ^ z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseXor(x(), z()), Expr::BitwiseXor(y(), z())
                    );
                case 0x19: // ~((x ^ y) | x & z)
                    return Expr::BitwiseNot(
                        Expr::BitwiseOr(Expr::BitwiseXor(x(), y()), Expr::BitwiseAnd(x(), z()))
                    );
                case 0x1A: // z ^ (x | y & z)
                    return Expr::BitwiseXor(
                        z(), Expr::BitwiseOr(x(), Expr::BitwiseAnd(y(), z()))
                    );
                case 0x1B: // (x | y) ^ (z | ~x)
                    return Expr::BitwiseXor(
                        Expr::BitwiseOr(x(), y()), Expr::BitwiseOr(z(), Expr::BitwiseNot(x()))
                    );
                case 0x1C: // z ^ (y | x & z)
                    return Expr::BitwiseXor(
                        z(), Expr::BitwiseOr(y(), Expr::BitwiseAnd(x(), z()))
                    );
                case 0x1D: // (x | y) ^ (z | ~y)
                    return Expr::BitwiseXor(
                        Expr::BitwiseOr(x(), y()), Expr::BitwiseOr(z(), Expr::BitwiseNot(y()))
                    );
                case 0x1E: // z ^ (x | y)
                    return Expr::BitwiseXor(z(), Expr::BitwiseOr(x(), y()));
                case 0x1F: // ~(z & (x | y))
                    return Expr::BitwiseNot(Expr::BitwiseAnd(z(), Expr::BitwiseOr(x(), y())));
                case 0x20: // x & z & ~y
                    return Expr::BitwiseAnd(x(), Expr::BitwiseAnd(z(), Expr::BitwiseNot(y())));
                case 0x21: // ~(y | (x ^ z))
                    return Expr::BitwiseNot(Expr::BitwiseOr(y(), Expr::BitwiseXor(x(), z())));
                case 0x22: // x & ~y
                    return Expr::BitwiseAnd(x(), Expr::BitwiseNot(y()));
                case 0x23: // ~y & (x | ~z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseNot(y()), Expr::BitwiseOr(x(), Expr::BitwiseNot(z()))
                    );
                case 0x24: // (x ^ y) & (y ^ z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseXor(x(), y()), Expr::BitwiseXor(y(), z())
                    );
                case 0x25: // ~(x & y | (x ^ z))
                    return Expr::BitwiseNot(
                        Expr::BitwiseOr(Expr::BitwiseAnd(x(), y()), Expr::BitwiseXor(x(), z()))
                    );
                case 0x26: // y ^ (x | y & z)
                    return Expr::BitwiseXor(
                        y(), Expr::BitwiseOr(x(), Expr::BitwiseAnd(y(), z()))
                    );
                case 0x27: // (x & y) ^ (x | ~z)
                    return Expr::BitwiseXor(
                        Expr::BitwiseAnd(x(), y()), Expr::BitwiseOr(x(), Expr::BitwiseNot(z()))
                    );
                case 0x28: // x & (y ^ z)
                    return Expr::BitwiseAnd(x(), Expr::BitwiseXor(y(), z()));
                case 0x29: // y ^ ((x ^ ~z) | y & z)
                    return Expr::BitwiseXor(
                        y(),
                        Expr::BitwiseOr(
                            Expr::BitwiseXor(x(), Expr::BitwiseNot(z())),
                            Expr::BitwiseAnd(y(), z())
                        )
                    );
                case 0x2A: // x & ~(y & z)
                    return Expr::BitwiseAnd(x(), Expr::BitwiseNot(Expr::BitwiseAnd(y(), z())));
                case 0x2B: // x ^ ~((x ^ y) | (x ^ z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseNot(
                            Expr::BitwiseOr(
                                Expr::BitwiseXor(x(), y()), Expr::BitwiseXor(x(), z())
                            )
                        )
                    );
                case 0x2C: // (x | y) & (y ^ z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseOr(x(), y()), Expr::BitwiseXor(y(), z())
                    );
                case 0x2D: // z ^ (y | ~x)
                    return Expr::BitwiseXor(z(), Expr::BitwiseOr(y(), Expr::BitwiseNot(x())));
                case 0x2E: // (x | y) ^ (y & z)
                    return Expr::BitwiseXor(
                        Expr::BitwiseOr(x(), y()), Expr::BitwiseAnd(y(), z())
                    );
                case 0x2F: // ~z | (x & ~y)
                    return Expr::BitwiseOr(
                        Expr::BitwiseNot(z()), Expr::BitwiseAnd(x(), Expr::BitwiseNot(y()))
                    );
                case 0x30: // z & ~y
                    return Expr::BitwiseAnd(z(), Expr::BitwiseNot(y()));
                case 0x31: // ~y & (z | ~x)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseNot(y()), Expr::BitwiseOr(z(), Expr::BitwiseNot(x()))
                    );
                case 0x32: // ~y & (x | z)
                    return Expr::BitwiseAnd(Expr::BitwiseNot(y()), Expr::BitwiseOr(x(), z()));
                case 0x33: // ~y
                    return Expr::BitwiseNot(y());
                case 0x34: // y ^ (z | x & y)
                    return Expr::BitwiseXor(
                        y(), Expr::BitwiseOr(z(), Expr::BitwiseAnd(x(), y()))
                    );
                case 0x35: // (x | z) ^ (y | ~z)
                    return Expr::BitwiseXor(
                        Expr::BitwiseOr(x(), z()), Expr::BitwiseOr(y(), Expr::BitwiseNot(z()))
                    );
                case 0x36: // y ^ (x | z)
                    return Expr::BitwiseXor(y(), Expr::BitwiseOr(x(), z()));
                case 0x37: // ~(y & (x | z))
                    return Expr::BitwiseNot(Expr::BitwiseAnd(y(), Expr::BitwiseOr(x(), z())));
                case 0x38: // (x | z) & (y ^ z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseOr(x(), z()), Expr::BitwiseXor(y(), z())
                    );
                case 0x39: // y ^ (z | ~x)
                    return Expr::BitwiseXor(y(), Expr::BitwiseOr(z(), Expr::BitwiseNot(x())));
                case 0x3A: // (x | z) ^ (y & z)
                    return Expr::BitwiseXor(
                        Expr::BitwiseOr(x(), z()), Expr::BitwiseAnd(y(), z())
                    );
                case 0x3B: // ~y | (x & ~z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseNot(y()), Expr::BitwiseAnd(x(), Expr::BitwiseNot(z()))
                    );
                case 0x3C: // y ^ z
                    return Expr::BitwiseXor(y(), z());
                case 0x3D: // (y ^ z) | ~(x | y)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(y(), z()), Expr::BitwiseNot(Expr::BitwiseOr(x(), y()))
                    );
                case 0x3E: // (x & ~y) | (y ^ z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseAnd(x(), Expr::BitwiseNot(y())), Expr::BitwiseXor(y(), z())
                    );
                case 0x3F: // ~(y & z)
                    return Expr::BitwiseNot(Expr::BitwiseAnd(y(), z()));
                case 0x40: // y & z & ~x
                    return Expr::BitwiseAnd(y(), Expr::BitwiseAnd(z(), Expr::BitwiseNot(x())));
                case 0x41: // ~(x | (y ^ z))
                    return Expr::BitwiseNot(Expr::BitwiseOr(x(), Expr::BitwiseXor(y(), z())));
                case 0x42: // (x ^ y) & (x ^ z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseXor(x(), y()), Expr::BitwiseXor(x(), z())
                    );
                case 0x43: // ~(x & y | (y ^ z))
                    return Expr::BitwiseNot(
                        Expr::BitwiseOr(Expr::BitwiseAnd(x(), y()), Expr::BitwiseXor(y(), z()))
                    );
                case 0x44: // y & ~x
                    return Expr::BitwiseAnd(y(), Expr::BitwiseNot(x()));
                case 0x45: // ~x & (y | ~z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseNot(x()), Expr::BitwiseOr(y(), Expr::BitwiseNot(z()))
                    );
                case 0x46: // x ^ (y | x & z)
                    return Expr::BitwiseXor(
                        x(), Expr::BitwiseOr(y(), Expr::BitwiseAnd(x(), z()))
                    );
                case 0x47: // (x & y) ^ (y | ~z)
                    return Expr::BitwiseXor(
                        Expr::BitwiseAnd(x(), y()), Expr::BitwiseOr(y(), Expr::BitwiseNot(z()))
                    );
                case 0x48: // y & (x ^ z)
                    return Expr::BitwiseAnd(y(), Expr::BitwiseXor(x(), z()));
                case 0x49: // x ^ ((x & z) | (y ^ ~z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseOr(
                            Expr::BitwiseAnd(x(), z()),
                            Expr::BitwiseXor(y(), Expr::BitwiseNot(z()))
                        )
                    );
                case 0x4A: // (x | y) & (x ^ z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseOr(x(), y()), Expr::BitwiseXor(x(), z())
                    );
                case 0x4B: // z ^ (x | ~y)
                    return Expr::BitwiseXor(z(), Expr::BitwiseOr(x(), Expr::BitwiseNot(y())));
                case 0x4C: // y & ~(x & z)
                    return Expr::BitwiseAnd(y(), Expr::BitwiseNot(Expr::BitwiseAnd(x(), z())));
                case 0x4D: // x ^ ((x ^ y) | (x ^ ~z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseOr(
                            Expr::BitwiseXor(x(), y()),
                            Expr::BitwiseXor(x(), Expr::BitwiseNot(z()))
                        )
                    );
                case 0x4E: // (x | y) ^ (x & z)
                    return Expr::BitwiseXor(
                        Expr::BitwiseOr(x(), y()), Expr::BitwiseAnd(x(), z())
                    );
                case 0x4F: // ~z | (y & ~x)
                    return Expr::BitwiseOr(
                        Expr::BitwiseNot(z()), Expr::BitwiseAnd(y(), Expr::BitwiseNot(x()))
                    );
                case 0x50: // z & ~x
                    return Expr::BitwiseAnd(z(), Expr::BitwiseNot(x()));
                case 0x51: // ~x & (z | ~y)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseNot(x()), Expr::BitwiseOr(z(), Expr::BitwiseNot(y()))
                    );
                case 0x52: // x ^ (z | x & y)
                    return Expr::BitwiseXor(
                        x(), Expr::BitwiseOr(z(), Expr::BitwiseAnd(x(), y()))
                    );
                case 0x53: // (x & z) ^ (z | ~y)
                    return Expr::BitwiseXor(
                        Expr::BitwiseAnd(x(), z()), Expr::BitwiseOr(z(), Expr::BitwiseNot(y()))
                    );
                case 0x54: // ~x & (y | z)
                    return Expr::BitwiseAnd(Expr::BitwiseNot(x()), Expr::BitwiseOr(y(), z()));
                case 0x55: // ~x
                    return Expr::BitwiseNot(x());
                case 0x56: // x ^ (y | z)
                    return Expr::BitwiseXor(x(), Expr::BitwiseOr(y(), z()));
                case 0x57: // ~(x & (y | z))
                    return Expr::BitwiseNot(Expr::BitwiseAnd(x(), Expr::BitwiseOr(y(), z())));
                case 0x58: // (x ^ z) & (y | z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseXor(x(), z()), Expr::BitwiseOr(y(), z())
                    );
                case 0x59: // x ^ (z | ~y)
                    return Expr::BitwiseXor(x(), Expr::BitwiseOr(z(), Expr::BitwiseNot(y())));
                case 0x5A: // x ^ z
                    return Expr::BitwiseXor(x(), z());
                case 0x5B: // (x ^ z) | ~(x | y)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(x(), z()), Expr::BitwiseNot(Expr::BitwiseOr(x(), y()))
                    );
                case 0x5C: // (x & z) ^ (y | z)
                    return Expr::BitwiseXor(
                        Expr::BitwiseAnd(x(), z()), Expr::BitwiseOr(y(), z())
                    );
                case 0x5D: // ~x | (y & ~z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseNot(x()), Expr::BitwiseAnd(y(), Expr::BitwiseNot(z()))
                    );
                case 0x5E: // (x ^ z) | (y & ~x)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(x(), z()), Expr::BitwiseAnd(y(), Expr::BitwiseNot(x()))
                    );
                case 0x5F: // ~(x & z)
                    return Expr::BitwiseNot(Expr::BitwiseAnd(x(), z()));
                case 0x60: // z & (x ^ y)
                    return Expr::BitwiseAnd(z(), Expr::BitwiseXor(x(), y()));
                case 0x61: // x ^ ((x & y) | (y ^ ~z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseOr(
                            Expr::BitwiseAnd(x(), y()),
                            Expr::BitwiseXor(y(), Expr::BitwiseNot(z()))
                        )
                    );
                case 0x62: // (x ^ y) & (x | z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseXor(x(), y()), Expr::BitwiseOr(x(), z())
                    );
                case 0x63: // y ^ (x | ~z)
                    return Expr::BitwiseXor(y(), Expr::BitwiseOr(x(), Expr::BitwiseNot(z())));
                case 0x64: // (x ^ y) & (y | z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseXor(x(), y()), Expr::BitwiseOr(y(), z())
                    );
                case 0x65: // x ^ (y | ~z)
                    return Expr::BitwiseXor(x(), Expr::BitwiseOr(y(), Expr::BitwiseNot(z())));
                case 0x66: // x ^ y
                    return Expr::BitwiseXor(x(), y());
                case 0x67: // (x ^ y) | ~(x | z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(x(), y()), Expr::BitwiseNot(Expr::BitwiseOr(x(), z()))
                    );
                case 0x68: // (x & y) ^ (z & (x | y))
                    return Expr::BitwiseXor(
                        Expr::BitwiseAnd(x(), y()),
                        Expr::BitwiseAnd(z(), Expr::BitwiseOr(x(), y()))
                    );
                case 0x69: // x ^ y ^ ~z
                    return Expr::BitwiseXor(x(), Expr::BitwiseXor(y(), Expr::BitwiseNot(z())));
                case 0x6A: // x ^ (y & z)
                    return Expr::BitwiseXor(x(), Expr::BitwiseAnd(y(), z()));
                case 0x6B: // y ^ ~((x ^ z) & (y | z))
                    return Expr::BitwiseXor(
                        y(),
                        Expr::BitwiseNot(
                            Expr::BitwiseAnd(
                                Expr::BitwiseXor(x(), z()), Expr::BitwiseOr(y(), z())
                            )
                        )
                    );
                case 0x6C: // y ^ (x & z)
                    return Expr::BitwiseXor(y(), Expr::BitwiseAnd(x(), z()));
                case 0x6D: // x ^ ~((x | z) & (y ^ z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseNot(
                            Expr::BitwiseAnd(
                                Expr::BitwiseOr(x(), z()), Expr::BitwiseXor(y(), z())
                            )
                        )
                    );
                case 0x6E: // (x ^ y) | (x & ~z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(x(), y()), Expr::BitwiseAnd(x(), Expr::BitwiseNot(z()))
                    );
                case 0x6F: // ~z | (x ^ y)
                    return Expr::BitwiseOr(Expr::BitwiseNot(z()), Expr::BitwiseXor(x(), y()));
                case 0x70: // z & ~(x & y)
                    return Expr::BitwiseAnd(z(), Expr::BitwiseNot(Expr::BitwiseAnd(x(), y())));
                case 0x71: // x ^ ((x ^ z) | (x ^ ~y))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseOr(
                            Expr::BitwiseXor(x(), z()),
                            Expr::BitwiseXor(x(), Expr::BitwiseNot(y()))
                        )
                    );
                case 0x72: // (x & y) ^ (x | z)
                    return Expr::BitwiseXor(
                        Expr::BitwiseAnd(x(), y()), Expr::BitwiseOr(x(), z())
                    );
                case 0x73: // ~y | (z & ~x)
                    return Expr::BitwiseOr(
                        Expr::BitwiseNot(y()), Expr::BitwiseAnd(z(), Expr::BitwiseNot(x()))
                    );
                case 0x74: // (x & y) ^ (y | z)
                    return Expr::BitwiseXor(
                        Expr::BitwiseAnd(x(), y()), Expr::BitwiseOr(y(), z())
                    );
                case 0x75: // ~x | (z & ~y)
                    return Expr::BitwiseOr(
                        Expr::BitwiseNot(x()), Expr::BitwiseAnd(z(), Expr::BitwiseNot(y()))
                    );
                case 0x76: // (x ^ y) | (z & ~x)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(x(), y()), Expr::BitwiseAnd(z(), Expr::BitwiseNot(x()))
                    );
                case 0x77: // ~(x & y)
                    return Expr::BitwiseNot(Expr::BitwiseAnd(x(), y()));
                case 0x78: // z ^ (x & y)
                    return Expr::BitwiseXor(z(), Expr::BitwiseAnd(x(), y()));
                case 0x79: // x ^ ~((x | y) & (y ^ z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseNot(
                            Expr::BitwiseAnd(
                                Expr::BitwiseOr(x(), y()), Expr::BitwiseXor(y(), z())
                            )
                        )
                    );
                case 0x7A: // (x ^ z) | (x & ~y)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(x(), z()), Expr::BitwiseAnd(x(), Expr::BitwiseNot(y()))
                    );
                case 0x7B: // ~y | (x ^ z)
                    return Expr::BitwiseOr(Expr::BitwiseNot(y()), Expr::BitwiseXor(x(), z()));
                case 0x7C: // (y ^ z) | (y & ~x)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(y(), z()), Expr::BitwiseAnd(y(), Expr::BitwiseNot(x()))
                    );
                case 0x7D: // ~x | (y ^ z)
                    return Expr::BitwiseOr(Expr::BitwiseNot(x()), Expr::BitwiseXor(y(), z()));
                case 0x7E: // (x ^ y) | (x ^ z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(x(), y()), Expr::BitwiseXor(x(), z())
                    );
                case 0x7F: // ~(x & y & z)
                    return Expr::BitwiseNot(Expr::BitwiseAnd(x(), Expr::BitwiseAnd(y(), z())));
                case 0x80: // x & y & z
                    return Expr::BitwiseAnd(x(), Expr::BitwiseAnd(y(), z()));
                case 0x81: // ~((x ^ y) | (x ^ z))
                    return Expr::BitwiseNot(
                        Expr::BitwiseOr(Expr::BitwiseXor(x(), y()), Expr::BitwiseXor(x(), z()))
                    );
                case 0x82: // x & (y ^ ~z)
                    return Expr::BitwiseAnd(x(), Expr::BitwiseXor(y(), Expr::BitwiseNot(z())));
                case 0x83: // (x | ~y) & (y ^ ~z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseOr(x(), Expr::BitwiseNot(y())),
                        Expr::BitwiseXor(y(), Expr::BitwiseNot(z()))
                    );
                case 0x84: // y & (x ^ ~z)
                    return Expr::BitwiseAnd(y(), Expr::BitwiseXor(x(), Expr::BitwiseNot(z())));
                case 0x85: // (x ^ ~z) & (y | ~x)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseXor(x(), Expr::BitwiseNot(z())),
                        Expr::BitwiseOr(y(), Expr::BitwiseNot(x()))
                    );
                case 0x86: // x ^ ((x | y) & (y ^ z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseAnd(Expr::BitwiseOr(x(), y()), Expr::BitwiseXor(y(), z()))
                    );
                case 0x87: // z ^ ~(x & y)
                    return Expr::BitwiseXor(z(), Expr::BitwiseNot(Expr::BitwiseAnd(x(), y())));
                case 0x88: // x & y
                    return Expr::BitwiseAnd(x(), y());
                case 0x89: // (x ^ ~y) & (x | ~z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseXor(x(), Expr::BitwiseNot(y())),
                        Expr::BitwiseOr(x(), Expr::BitwiseNot(z()))
                    );
                case 0x8A: // x & (y | ~z)
                    return Expr::BitwiseAnd(x(), Expr::BitwiseOr(y(), Expr::BitwiseNot(z())));
                case 0x8B: // (x & y) | ~(y | z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseAnd(x(), y()), Expr::BitwiseNot(Expr::BitwiseOr(y(), z()))
                    );
                case 0x8C: // y & (x | ~z)
                    return Expr::BitwiseAnd(y(), Expr::BitwiseOr(x(), Expr::BitwiseNot(z())));
                case 0x8D: // (x & y) | ~(x | z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseAnd(x(), y()), Expr::BitwiseNot(Expr::BitwiseOr(x(), z()))
                    );
                case 0x8E: // x ^ ((x ^ y) & (y ^ z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseAnd(Expr::BitwiseXor(x(), y()), Expr::BitwiseXor(y(), z()))
                    );
                case 0x8F: // ~z | (x & y)
                    return Expr::BitwiseOr(Expr::BitwiseNot(z()), Expr::BitwiseAnd(x(), y()));
                case 0x90: // z & (x ^ ~y)
                    return Expr::BitwiseAnd(z(), Expr::BitwiseXor(x(), Expr::BitwiseNot(y())));
                case 0x91: // (x ^ ~y) & (z | ~x)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseXor(x(), Expr::BitwiseNot(y())),
                        Expr::BitwiseOr(z(), Expr::BitwiseNot(x()))
                    );
                case 0x92: // x ^ ((x | z) & (y ^ z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseAnd(Expr::BitwiseOr(x(), z()), Expr::BitwiseXor(y(), z()))
                    );
                case 0x93: // y ^ ~(x & z)
                    return Expr::BitwiseXor(y(), Expr::BitwiseNot(Expr::BitwiseAnd(x(), z())));
                case 0x94: // y ^ ((x ^ z) & (y | z))
                    return Expr::BitwiseXor(
                        y(),
                        Expr::BitwiseAnd(Expr::BitwiseXor(x(), z()), Expr::BitwiseOr(y(), z()))
                    );
                case 0x95: // x ^ ~(y & z)
                    return Expr::BitwiseXor(x(), Expr::BitwiseNot(Expr::BitwiseAnd(y(), z())));
                case 0x96: // x ^ y ^ z
                    return Expr::BitwiseXor(x(), Expr::BitwiseXor(y(), z()));
                case 0x97: // x ^ ((y ^ z) | ~(x | y))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseOr(
                            Expr::BitwiseXor(y(), z()),
                            Expr::BitwiseNot(Expr::BitwiseOr(x(), y()))
                        )
                    );
                case 0x98: // (x | z) & (x ^ ~y)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseOr(x(), z()), Expr::BitwiseXor(x(), Expr::BitwiseNot(y()))
                    );
                case 0x99: // x ^ ~y
                    return Expr::BitwiseXor(x(), Expr::BitwiseNot(y()));
                case 0x9A: // x ^ (z & ~y)
                    return Expr::BitwiseXor(x(), Expr::BitwiseAnd(z(), Expr::BitwiseNot(y())));
                case 0x9B: // ~((x ^ y) & (y | z))
                    return Expr::BitwiseNot(
                        Expr::BitwiseAnd(Expr::BitwiseXor(x(), y()), Expr::BitwiseOr(y(), z()))
                    );
                case 0x9C: // y ^ (z & ~x)
                    return Expr::BitwiseXor(y(), Expr::BitwiseAnd(z(), Expr::BitwiseNot(x())));
                case 0x9D: // ~((x ^ y) & (x | z))
                    return Expr::BitwiseNot(
                        Expr::BitwiseAnd(Expr::BitwiseXor(x(), y()), Expr::BitwiseOr(x(), z()))
                    );
                case 0x9E: // y ^ x ^ (z | x & y)
                    return Expr::BitwiseXor(
                        y(),
                        Expr::BitwiseXor(x(), Expr::BitwiseOr(z(), Expr::BitwiseAnd(x(), y())))
                    );
                case 0x9F: // ~(z & (x ^ y))
                    return Expr::BitwiseNot(Expr::BitwiseAnd(z(), Expr::BitwiseXor(x(), y())));
                case 0xA0: // x & z
                    return Expr::BitwiseAnd(x(), z());
                case 0xA1: // (x | ~y) & (x ^ ~z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseOr(x(), Expr::BitwiseNot(y())),
                        Expr::BitwiseXor(x(), Expr::BitwiseNot(z()))
                    );
                case 0xA2: // x & (z | ~y)
                    return Expr::BitwiseAnd(x(), Expr::BitwiseOr(z(), Expr::BitwiseNot(y())));
                case 0xA3: // (x & z) | ~(y | z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseAnd(x(), z()), Expr::BitwiseNot(Expr::BitwiseOr(y(), z()))
                    );
                case 0xA4: // (x | y) & (x ^ ~z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseOr(x(), y()), Expr::BitwiseXor(x(), Expr::BitwiseNot(z()))
                    );
                case 0xA5: // x ^ ~z
                    return Expr::BitwiseXor(x(), Expr::BitwiseNot(z()));
                case 0xA6: // x ^ (y & ~z)
                    return Expr::BitwiseXor(x(), Expr::BitwiseAnd(y(), Expr::BitwiseNot(z())));
                case 0xA7: // ~((x ^ z) & (y | z))
                    return Expr::BitwiseNot(
                        Expr::BitwiseAnd(Expr::BitwiseXor(x(), z()), Expr::BitwiseOr(y(), z()))
                    );
                case 0xA8: // x & (y | z)
                    return Expr::BitwiseAnd(x(), Expr::BitwiseOr(y(), z()));
                case 0xA9: // x ^ ~(y | z)
                    return Expr::BitwiseXor(x(), Expr::BitwiseNot(Expr::BitwiseOr(y(), z())));
                case 0xAA: // x
                    return x();
                case 0xAB: // x | ~(y | z)
                    return Expr::BitwiseOr(x(), Expr::BitwiseNot(Expr::BitwiseOr(y(), z())));
                case 0xAC: // y ^ (z & (x ^ y))
                    return Expr::BitwiseXor(
                        y(), Expr::BitwiseAnd(z(), Expr::BitwiseXor(x(), y()))
                    );
                case 0xAD: // (x & y) | (x ^ ~z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseAnd(x(), y()), Expr::BitwiseXor(x(), Expr::BitwiseNot(z()))
                    );
                case 0xAE: // x | (y & ~z)
                    return Expr::BitwiseOr(x(), Expr::BitwiseAnd(y(), Expr::BitwiseNot(z())));
                case 0xAF: // x | ~z
                    return Expr::BitwiseOr(x(), Expr::BitwiseNot(z()));
                case 0xB0: // z & (x | ~y)
                    return Expr::BitwiseAnd(z(), Expr::BitwiseOr(x(), Expr::BitwiseNot(y())));
                case 0xB1: // (x | y) ^ ~(x & z)
                    return Expr::BitwiseXor(
                        Expr::BitwiseOr(x(), y()), Expr::BitwiseNot(Expr::BitwiseAnd(x(), z()))
                    );
                case 0xB2: // x ^ ((x ^ z) & (y ^ z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseAnd(Expr::BitwiseXor(x(), z()), Expr::BitwiseXor(y(), z()))
                    );
                case 0xB3: // ~y | (x & z)
                    return Expr::BitwiseOr(Expr::BitwiseNot(y()), Expr::BitwiseAnd(x(), z()));
                case 0xB4: // z ^ (y & ~x)
                    return Expr::BitwiseXor(z(), Expr::BitwiseAnd(y(), Expr::BitwiseNot(x())));
                case 0xB5: // ~((x | y) & (x ^ z))
                    return Expr::BitwiseNot(
                        Expr::BitwiseAnd(Expr::BitwiseOr(x(), y()), Expr::BitwiseXor(x(), z()))
                    );
                case 0xB6: // z ^ x ^ (y | x & z)
                    return Expr::BitwiseXor(
                        z(),
                        Expr::BitwiseXor(x(), Expr::BitwiseOr(y(), Expr::BitwiseAnd(x(), z())))
                    );
                case 0xB7: // ~(y & (x ^ z))
                    return Expr::BitwiseNot(Expr::BitwiseAnd(y(), Expr::BitwiseXor(x(), z())));
                case 0xB8: // z ^ (y & (x ^ z))
                    return Expr::BitwiseXor(
                        z(), Expr::BitwiseAnd(y(), Expr::BitwiseXor(x(), z()))
                    );
                case 0xB9: // (x & z) | (x ^ ~y)
                    return Expr::BitwiseOr(
                        Expr::BitwiseAnd(x(), z()), Expr::BitwiseXor(x(), Expr::BitwiseNot(y()))
                    );
                case 0xBA: // x | (z & ~y)
                    return Expr::BitwiseOr(x(), Expr::BitwiseAnd(z(), Expr::BitwiseNot(y())));
                case 0xBB: // x | ~y
                    return Expr::BitwiseOr(x(), Expr::BitwiseNot(y()));
                case 0xBC: // (x & y) | (y ^ z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseAnd(x(), y()), Expr::BitwiseXor(y(), z())
                    );
                case 0xBD: // (x ^ ~y) | (y ^ z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(x(), Expr::BitwiseNot(y())), Expr::BitwiseXor(y(), z())
                    );
                case 0xBE: // x | (y ^ z)
                    return Expr::BitwiseOr(x(), Expr::BitwiseXor(y(), z()));
                case 0xBF: // x | ~(y & z)
                    return Expr::BitwiseOr(x(), Expr::BitwiseNot(Expr::BitwiseAnd(y(), z())));
                case 0xC0: // y & z
                    return Expr::BitwiseAnd(y(), z());
                case 0xC1: // (y | ~x) & (y ^ ~z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseOr(y(), Expr::BitwiseNot(x())),
                        Expr::BitwiseXor(y(), Expr::BitwiseNot(z()))
                    );
                case 0xC2: // (x | y) & (y ^ ~z)
                    return Expr::BitwiseAnd(
                        Expr::BitwiseOr(x(), y()), Expr::BitwiseXor(y(), Expr::BitwiseNot(z()))
                    );
                case 0xC3: // y ^ ~z
                    return Expr::BitwiseXor(y(), Expr::BitwiseNot(z()));
                case 0xC4: // y & (z | ~x)
                    return Expr::BitwiseAnd(y(), Expr::BitwiseOr(z(), Expr::BitwiseNot(x())));
                case 0xC5: // (x | z) ^ ~(y & z)
                    return Expr::BitwiseXor(
                        Expr::BitwiseOr(x(), z()), Expr::BitwiseNot(Expr::BitwiseAnd(y(), z()))
                    );
                case 0xC6: // y ^ (x & ~z)
                    return Expr::BitwiseXor(y(), Expr::BitwiseAnd(x(), Expr::BitwiseNot(z())));
                case 0xC7: // ~((x | z) & (y ^ z))
                    return Expr::BitwiseNot(
                        Expr::BitwiseAnd(Expr::BitwiseOr(x(), z()), Expr::BitwiseXor(y(), z()))
                    );
                case 0xC8: // y & (x | z)
                    return Expr::BitwiseAnd(y(), Expr::BitwiseOr(x(), z()));
                case 0xC9: // y ^ ~(x | z)
                    return Expr::BitwiseXor(y(), Expr::BitwiseNot(Expr::BitwiseOr(x(), z())));
                case 0xCA: // x ^ (z & (x ^ y))
                    return Expr::BitwiseXor(
                        x(), Expr::BitwiseAnd(z(), Expr::BitwiseXor(x(), y()))
                    );
                case 0xCB: // (x & y) | (y ^ ~z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseAnd(x(), y()), Expr::BitwiseXor(y(), Expr::BitwiseNot(z()))
                    );
                case 0xCC: // y
                    return y();
                case 0xCD: // y | ~(x | z)
                    return Expr::BitwiseOr(y(), Expr::BitwiseNot(Expr::BitwiseOr(x(), z())));
                case 0xCE: // y | (x & ~z)
                    return Expr::BitwiseOr(y(), Expr::BitwiseAnd(x(), Expr::BitwiseNot(z())));
                case 0xCF: // y | ~z
                    return Expr::BitwiseOr(y(), Expr::BitwiseNot(z()));
                case 0xD0: // z & (y | ~x)
                    return Expr::BitwiseAnd(z(), Expr::BitwiseOr(y(), Expr::BitwiseNot(x())));
                case 0xD1: // (x | y) ^ ~(y & z)
                    return Expr::BitwiseXor(
                        Expr::BitwiseOr(x(), y()), Expr::BitwiseNot(Expr::BitwiseAnd(y(), z()))
                    );
                case 0xD2: // z ^ (x & ~y)
                    return Expr::BitwiseXor(z(), Expr::BitwiseAnd(x(), Expr::BitwiseNot(y())));
                case 0xD3: // ~((x | y) & (y ^ z))
                    return Expr::BitwiseNot(
                        Expr::BitwiseAnd(Expr::BitwiseOr(x(), y()), Expr::BitwiseXor(y(), z()))
                    );
                case 0xD4: // x ^ ((x ^ y) | (x ^ z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseOr(Expr::BitwiseXor(x(), y()), Expr::BitwiseXor(x(), z()))
                    );
                case 0xD5: // ~x | (y & z)
                    return Expr::BitwiseOr(Expr::BitwiseNot(x()), Expr::BitwiseAnd(y(), z()));
                case 0xD6: // z ^ y ^ (x | y & z)
                    return Expr::BitwiseXor(
                        z(),
                        Expr::BitwiseXor(y(), Expr::BitwiseOr(x(), Expr::BitwiseAnd(y(), z())))
                    );
                case 0xD7: // ~(x & (y ^ z))
                    return Expr::BitwiseNot(Expr::BitwiseAnd(x(), Expr::BitwiseXor(y(), z())));
                case 0xD8: // z ^ (x & (y ^ z))
                    return Expr::BitwiseXor(
                        z(), Expr::BitwiseAnd(x(), Expr::BitwiseXor(y(), z()))
                    );
                case 0xD9: // (x ^ ~y) | (y & z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(x(), Expr::BitwiseNot(y())), Expr::BitwiseAnd(y(), z())
                    );
                case 0xDA: // (x & y) | (x ^ z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseAnd(x(), y()), Expr::BitwiseXor(x(), z())
                    );
                case 0xDB: // (x ^ z) | (x ^ ~y)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(x(), z()), Expr::BitwiseXor(x(), Expr::BitwiseNot(y()))
                    );
                case 0xDC: // y | (z & ~x)
                    return Expr::BitwiseOr(y(), Expr::BitwiseAnd(z(), Expr::BitwiseNot(x())));
                case 0xDD: // y | ~x
                    return Expr::BitwiseOr(y(), Expr::BitwiseNot(x()));
                case 0xDE: // y | (x ^ z)
                    return Expr::BitwiseOr(y(), Expr::BitwiseXor(x(), z()));
                case 0xDF: // y | ~(x & z)
                    return Expr::BitwiseOr(y(), Expr::BitwiseNot(Expr::BitwiseAnd(x(), z())));
                case 0xE0: // z & (x | y)
                    return Expr::BitwiseAnd(z(), Expr::BitwiseOr(x(), y()));
                case 0xE1: // z ^ ~(x | y)
                    return Expr::BitwiseXor(z(), Expr::BitwiseNot(Expr::BitwiseOr(x(), y())));
                case 0xE2: // x ^ (y & (x ^ z))
                    return Expr::BitwiseXor(
                        x(), Expr::BitwiseAnd(y(), Expr::BitwiseXor(x(), z()))
                    );
                case 0xE3: // (x & z) | (y ^ ~z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseAnd(x(), z()), Expr::BitwiseXor(y(), Expr::BitwiseNot(z()))
                    );
                case 0xE4: // y ^ (x & (y ^ z))
                    return Expr::BitwiseXor(
                        y(), Expr::BitwiseAnd(x(), Expr::BitwiseXor(y(), z()))
                    );
                case 0xE5: // (x ^ ~z) | (y & z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(x(), Expr::BitwiseNot(z())), Expr::BitwiseAnd(y(), z())
                    );
                case 0xE6: // (x ^ y) | (x & z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(x(), y()), Expr::BitwiseAnd(x(), z())
                    );
                case 0xE7: // (x ^ y) | (x ^ ~z)
                    return Expr::BitwiseOr(
                        Expr::BitwiseXor(x(), y()), Expr::BitwiseXor(x(), Expr::BitwiseNot(z()))
                    );
                case 0xE8: // x ^ ((x ^ y) & (x ^ z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseAnd(Expr::BitwiseXor(x(), y()), Expr::BitwiseXor(x(), z()))
                    );
                case 0xE9: // x ^ ~((x & y) | (y ^ z))
                    return Expr::BitwiseXor(
                        x(),
                        Expr::BitwiseNot(
                            Expr::BitwiseOr(
                                Expr::BitwiseAnd(x(), y()), Expr::BitwiseXor(y(), z())
                            )
                        )
                    );
                case 0xEA: // x | (y & z)
                    return Expr::BitwiseOr(x(), Expr::BitwiseAnd(y(), z()));
                case 0xEB: // x | (y ^ ~z)
                    return Expr::BitwiseOr(x(), Expr::BitwiseXor(y(), Expr::BitwiseNot(z())));
                case 0xEC: // y | (x & z)
                    return Expr::BitwiseOr(y(), Expr::BitwiseAnd(x(), z()));
                case 0xED: // y | (x ^ ~z)
                    return Expr::BitwiseOr(y(), Expr::BitwiseXor(x(), Expr::BitwiseNot(z())));
                case 0xEE: // x | y
                    return Expr::BitwiseOr(x(), y());
                case 0xEF: // x | y | ~z
                    return Expr::BitwiseOr(x(), Expr::BitwiseOr(y(), Expr::BitwiseNot(z())));
                case 0xF0: // z
                    return z();
                case 0xF1: // z | ~(x | y)
                    return Expr::BitwiseOr(z(), Expr::BitwiseNot(Expr::BitwiseOr(x(), y())));
                case 0xF2: // z | (x & ~y)
                    return Expr::BitwiseOr(z(), Expr::BitwiseAnd(x(), Expr::BitwiseNot(y())));
                case 0xF3: // z | ~y
                    return Expr::BitwiseOr(z(), Expr::BitwiseNot(y()));
                case 0xF4: // z | (y & ~x)
                    return Expr::BitwiseOr(z(), Expr::BitwiseAnd(y(), Expr::BitwiseNot(x())));
                case 0xF5: // z | ~x
                    return Expr::BitwiseOr(z(), Expr::BitwiseNot(x()));
                case 0xF6: // z | (x ^ y)
                    return Expr::BitwiseOr(z(), Expr::BitwiseXor(x(), y()));
                case 0xF7: // z | ~(x & y)
                    return Expr::BitwiseOr(z(), Expr::BitwiseNot(Expr::BitwiseAnd(x(), y())));
                case 0xF8: // z | (x & y)
                    return Expr::BitwiseOr(z(), Expr::BitwiseAnd(x(), y()));
                case 0xF9: // z | (x ^ ~y)
                    return Expr::BitwiseOr(z(), Expr::BitwiseXor(x(), Expr::BitwiseNot(y())));
                case 0xFA: // x | z
                    return Expr::BitwiseOr(x(), z());
                case 0xFB: // x | z | ~y
                    return Expr::BitwiseOr(x(), Expr::BitwiseOr(z(), Expr::BitwiseNot(y())));
                case 0xFC: // y | z
                    return Expr::BitwiseOr(y(), z());
                case 0xFD: // y | z | ~x
                    return Expr::BitwiseOr(y(), Expr::BitwiseOr(z(), Expr::BitwiseNot(x())));
                case 0xFE: // x | y | z
                    return Expr::BitwiseOr(x(), Expr::BitwiseOr(y(), z()));
                default:
                    return std::nullopt;
            }
        }

        // Shannon decomposition of 4-variable Boolean functions.
        // Splits on variable 3 (w): f(x,y,z,w) decomposes into cofactors
        // f0(x,y,z) = f(x,y,z,0) and f1(x,y,z) = f(x,y,z,1), each matched
        // by the 3-var table. Covers all 65536 non-constant functions.
        std::optional< std::unique_ptr< Expr > > Match4varBoolean(uint32_t key) {
            auto w = []() { return Expr::Variable(3); };

            const auto f0 = static_cast< uint8_t >(key & 0xFF);
            const auto f1 = static_cast< uint8_t >((key >> 8) & 0xFF);

            // Pure w or ~w
            if (f0 == 0x00 && f1 == 0xFF) { return w(); }
            if (f0 == 0xFF && f1 == 0x00) { return Expr::BitwiseNot(w()); }

            // One cofactor is constant 0: f = w & f1 or ~w & f0
            if (f0 == 0x00) {
                auto e1 = Match3varBoolean(f1);
                if (!e1) { return std::nullopt; }
                return Expr::BitwiseAnd(w(), std::move(*e1));
            }
            if (f1 == 0x00) {
                auto e0 = Match3varBoolean(f0);
                if (!e0) { return std::nullopt; }
                return Expr::BitwiseAnd(Expr::BitwiseNot(w()), std::move(*e0));
            }

            // One cofactor is constant 1: f = ~w | f1 or w | f0
            if (f0 == 0xFF) {
                auto e1 = Match3varBoolean(f1);
                if (!e1) { return std::nullopt; }
                return Expr::BitwiseOr(Expr::BitwiseNot(w()), std::move(*e1));
            }
            if (f1 == 0xFF) {
                auto e0 = Match3varBoolean(f0);
                if (!e0) { return std::nullopt; }
                return Expr::BitwiseOr(w(), std::move(*e0));
            }

            // Cofactors equal: function doesn't depend on w
            if (f0 == f1) { return Match3varBoolean(f0); }

            // Cofactors complementary: f = f0 ^ w
            if (f0 == static_cast< uint8_t >(~f1)) {
                auto e0 = Match3varBoolean(f0);
                if (!e0) { return std::nullopt; }
                return Expr::BitwiseXor(std::move(*e0), w());
            }

            // General mux: f = f0 ^ (w & (f0 ^ f1))
            auto e0     = Match3varBoolean(f0);
            auto e_diff = Match3varBoolean(static_cast< uint8_t >(f0 ^ f1));
            if (!e0 || !e_diff) { return std::nullopt; }
            return Expr::BitwiseXor(std::move(*e0), Expr::BitwiseAnd(w(), std::move(*e_diff)));
        }

        // Shannon decomposition of 5-variable Boolean functions.
        // Splits on variable 4 (v): f(x,y,z,w,v) decomposes into cofactors
        // f0(x,y,z,w) = f(...,0) and f1(x,y,z,w) = f(...,1), each matched
        // by the 4-var decomposer. Covers all 2^32 non-constant functions.
        std::optional< std::unique_ptr< Expr > > Match5varBoolean(uint32_t key) {
            auto v = []() { return Expr::Variable(4); };

            const auto f0 = static_cast< uint16_t >(key & 0xFFFF);
            const auto f1 = static_cast< uint16_t >((key >> 16) & 0xFFFF);

            // Pure v or ~v
            if (f0 == 0x0000 && f1 == 0xFFFF) { return v(); }
            if (f0 == 0xFFFF && f1 == 0x0000) { return Expr::BitwiseNot(v()); }

            // One cofactor is constant 0
            if (f0 == 0x0000) {
                auto e1 = Match4varBoolean(f1);
                if (!e1) { return std::nullopt; }
                return Expr::BitwiseAnd(v(), std::move(*e1));
            }
            if (f1 == 0x0000) {
                auto e0 = Match4varBoolean(f0);
                if (!e0) { return std::nullopt; }
                return Expr::BitwiseAnd(Expr::BitwiseNot(v()), std::move(*e0));
            }

            // One cofactor is constant 1
            if (f0 == 0xFFFF) {
                auto e1 = Match4varBoolean(f1);
                if (!e1) { return std::nullopt; }
                return Expr::BitwiseOr(Expr::BitwiseNot(v()), std::move(*e1));
            }
            if (f1 == 0xFFFF) {
                auto e0 = Match4varBoolean(f0);
                if (!e0) { return std::nullopt; }
                return Expr::BitwiseOr(v(), std::move(*e0));
            }

            // Cofactors equal: function doesn't depend on v
            if (f0 == f1) { return Match4varBoolean(f0); }

            // Cofactors complementary: f = f0 ^ v
            if (f0 == static_cast< uint16_t >(~f1)) {
                auto e0 = Match4varBoolean(f0);
                if (!e0) { return std::nullopt; }
                return Expr::BitwiseXor(std::move(*e0), v());
            }

            // General mux: f = f0 ^ (v & (f0 ^ f1))
            auto e0     = Match4varBoolean(f0);
            auto e_diff = Match4varBoolean(static_cast< uint16_t >(f0 ^ f1));
            if (!e0 || !e_diff) { return std::nullopt; }
            return Expr::BitwiseXor(std::move(*e0), Expr::BitwiseAnd(v(), std::move(*e_diff)));
        }

        // Try to decompose a non-boolean signature as c + k * boolean_pattern
        // where c is the constant term, k is a scalar, and boolean_pattern
        // is a pure bitwise function with entries in {0, 1}.
        // Returns nullopt if the signature has more than two distinct values
        // or the boolean quotient doesn't match any known pattern.
        std::optional< std::unique_ptr< Expr > > MatchScaledBoolean(
            const std::vector< uint64_t > &sig, uint32_t num_vars, uint32_t bitwidth
        ) {
            const uint64_t c = sig[0];
            uint64_t k       = 0;
            for (size_t i = 1; i < sig.size(); ++i) {
                const uint64_t r = ModSub(sig[i], c, bitwidth);
                if (r == 0) { continue; }
                if (k == 0) {
                    k = r;
                } else if (r != k) {
                    return std::nullopt;
                }
            }
            if (k == 0) { return std::nullopt; }

            std::vector< uint64_t > bool_sig(sig.size());
            for (size_t i = 0; i < sig.size(); ++i) {
                bool_sig[i] = ModSub(sig[i], c, bitwidth) == 0 ? 0 : 1;
            }

            // Forward declaration in enclosing scope handles recursion.
            auto inner = MatchPattern(bool_sig, num_vars, bitwidth);
            if (!inner) { return std::nullopt; }

            auto result = std::move(*inner);
            if (k != 1) { result = Expr::Mul(Expr::Constant(k), std::move(result)); }
            if (c != 0) { result = Expr::Add(Expr::Constant(c), std::move(result)); }
            return result;
        }

    } // namespace

    std::optional< std::unique_ptr< Expr > >
    MatchPattern(const std::vector< uint64_t > &sig, uint32_t num_vars, uint32_t bitwidth) {
        COBRA_ZONE_N("MatchPattern");
        COBRA_TRACE("PatternMatcher", "MatchPattern: vars={} bitwidth={}", num_vars, bitwidth);

        // Constant: all entries equal (works for any variable count)
        if (AllEqual(sig)) {
            COBRA_TRACE("PatternMatcher", "MatchPattern: {}", "HIT");
            return Expr::Constant(sig[0]);
        }

        std::optional< std::unique_ptr< Expr > > result;

        if (num_vars == 1) {
            result = Match1var(sig, bitwidth);
        } else if (num_vars == 2 && IsBooleanSig(sig)) {
            // 2-var Boolean: table lookup for all 16 functions
            result = Match2varBoolean(static_cast< uint8_t >(PackBoolSig(sig)));
        } else if (num_vars == 3 && IsBooleanSig(sig)) {
            // 3-var Boolean: table lookup for all 256 functions
            result = Match3varBoolean(static_cast< uint8_t >(PackBoolSig(sig)));
        } else if (num_vars == 4 && IsBooleanSig(sig)) {
            // 4-var Boolean: Shannon decomposition into two 3-var lookups
            result = Match4varBoolean(static_cast< uint16_t >(PackBoolSig(sig)));
        } else if (num_vars == 5 && IsBooleanSig(sig)) {
            // 5-var Boolean: Shannon decomposition into two 4-var lookups
            result = Match5varBoolean(PackBoolSig(sig));
        } else if (!IsBooleanSig(sig)) {
            // Scaled boolean: c + k * boolean_pattern
            result = MatchScaledBoolean(sig, num_vars, bitwidth);
        }

        COBRA_TRACE("PatternMatcher", "MatchPattern: {}", result ? "HIT" : "MISS");
        return result;
    }

} // namespace cobra
