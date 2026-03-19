#include "ExprParser.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Result.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <set>
#include <stack>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        enum class TokenType {
            kNumber,
            kVariable,
            kOp,
            kLParen,
            kRParen,
        };

        struct Token
        {
            TokenType type;
            std::string value;
            int precedence;
            bool right_assoc;
            bool is_unary;
        };

        Result< std::vector< Token > > Tokenize(const std::string &expr) {
            std::vector< Token > tokens;
            size_t i = 0;
            while (i < expr.size()) {
                if (std::isspace(expr[i]) != 0) {
                    ++i;
                    continue;
                }

                if (i + 1 < expr.size() && expr[i] == '0'
                    && (expr[i + 1] == 'x' || expr[i + 1] == 'X'))
                {
                    i                  += 2; // skip "0x"
                    const size_t start  = i;
                    while (i < expr.size()
                           && (std::isxdigit(static_cast< unsigned char >(expr[i])) != 0))
                    {
                        ++i;
                    }
                    if (i == start) {
                        return Err< std::vector< Token > >(
                            CobraError::kParseError,
                            "empty hex literal at position " + std::to_string(i - 2)
                        );
                    }
                    uint64_t val = 0;
                    auto [ptr, ec] =
                        std::from_chars(expr.data() + start, expr.data() + i, val, 16);
                    if (ec == std::errc::result_out_of_range) {
                        return Err< std::vector< Token > >(
                            CobraError::kParseError,
                            "hex literal out of 64-bit range at position "
                                + std::to_string(start - 2)
                        );
                    }
                    tokens.push_back(
                        { .type        = TokenType::kNumber,
                          .value       = std::to_string(val),
                          .precedence  = 0,
                          .right_assoc = false,
                          .is_unary    = false }
                    );
                    continue;
                }

                if (std::isdigit(expr[i]) != 0) {
                    const size_t start = i;
                    while (i < expr.size() && (std::isdigit(expr[i]) != 0)) { ++i; }
                    tokens.push_back(
                        { .type        = TokenType::kNumber,
                          .value       = expr.substr(start, i - start),
                          .precedence  = 0,
                          .right_assoc = false,
                          .is_unary    = false }
                    );
                    continue;
                }

                if ((std::isalpha(expr[i]) != 0) || expr[i] == '_') {
                    const size_t start = i;
                    while (i < expr.size() && ((std::isalnum(expr[i]) != 0) || expr[i] == '_'))
                    {
                        ++i;
                    }
                    tokens.push_back(
                        { .type        = TokenType::kVariable,
                          .value       = expr.substr(start, i - start),
                          .precedence  = 0,
                          .right_assoc = false,
                          .is_unary    = false }
                    );
                    continue;
                }

                if (expr[i] == '(') {
                    tokens.push_back(
                        { .type        = TokenType::kLParen,
                          .value       = "(",
                          .precedence  = 0,
                          .right_assoc = false,
                          .is_unary    = false }
                    );
                    ++i;
                    continue;
                }
                if (expr[i] == ')') {
                    tokens.push_back(
                        { .type        = TokenType::kRParen,
                          .value       = ")",
                          .precedence  = 0,
                          .right_assoc = false,
                          .is_unary    = false }
                    );
                    ++i;
                    continue;
                }

                // Two-character shift operators
                if (expr[i] == '<' && i + 1 < expr.size() && expr[i + 1] == '<') {
                    tokens.push_back(
                        { .type        = TokenType::kOp,
                          .value       = "<<",
                          .precedence  = 4,
                          .right_assoc = false,
                          .is_unary    = false }
                    );
                    i += 2;
                    continue;
                }
                if (expr[i] == '>' && i + 1 < expr.size() && expr[i + 1] == '>') {
                    tokens.push_back(
                        { .type        = TokenType::kOp,
                          .value       = ">>",
                          .precedence  = 4,
                          .right_assoc = false,
                          .is_unary    = false }
                    );
                    i += 2;
                    continue;
                }

                // Operators
                const char c              = expr[i];
                const bool could_be_unary = tokens.empty()
                    || tokens.back().type == TokenType::kOp
                    || tokens.back().type == TokenType::kLParen;

                if (c == '~') {
                    tokens.push_back(
                        { .type        = TokenType::kOp,
                          .value       = "~",
                          .precedence  = 1,
                          .right_assoc = true,
                          .is_unary    = true }
                    );
                    ++i;
                    continue;
                }
                if (c == '-' && could_be_unary) {
                    tokens.push_back(
                        { .type        = TokenType::kOp,
                          .value       = "neg",
                          .precedence  = 1,
                          .right_assoc = true,
                          .is_unary    = true }
                    );
                    ++i;
                    continue;
                }

                int prec = 0;
                switch (c) {
                    case '*':
                        prec = 2;
                        break;
                    case '+':
                        prec = 3;
                        break;
                    case '-':
                        prec = 3;
                        break;
                    case '&':
                        prec = 5;
                        break;
                    case '^':
                        prec = 6;
                        break;
                    case '|':
                        prec = 7;
                        break;
                    default:
                        return Err< std::vector< Token > >(
                            CobraError::kParseError,
                            std::string("unexpected character '") + c + "' at position "
                                + std::to_string(i)
                        );
                }
                tokens.push_back(
                    { .type        = TokenType::kOp,
                      .value       = std::string(1, c),
                      .precedence  = prec,
                      .right_assoc = false,
                      .is_unary    = false }
                );
                ++i;
            }
            return tokens;
        }

        // Shunting-yard to postfix
        Result< std::vector< Token > > ToPostfix(const std::vector< Token > &tokens) {
            std::vector< Token > output;
            std::stack< Token > ops;

            for (const auto &tok : tokens) {
                switch (tok.type) {
                    case TokenType::kNumber:
                    case TokenType::kVariable:
                        output.push_back(tok);
                        break;
                    case TokenType::kOp:
                        while (!ops.empty() && ops.top().type == TokenType::kOp
                               && ((tok.right_assoc && ops.top().precedence < tok.precedence)
                                   || (!tok.right_assoc
                                       && ops.top().precedence <= tok.precedence)))
                        {
                            output.push_back(ops.top());
                            ops.pop();
                        }
                        ops.push(tok);
                        break;
                    case TokenType::kLParen:
                        ops.push(tok);
                        break;
                    case TokenType::kRParen:
                        while (!ops.empty() && ops.top().type != TokenType::kLParen) {
                            output.push_back(ops.top());
                            ops.pop();
                        }
                        if (ops.empty()) {
                            return Err< std::vector< Token > >(
                                CobraError::kParseError, "mismatched parentheses"
                            );
                        }
                        ops.pop(); // remove LParen
                        break;
                }
            }
            while (!ops.empty()) {
                if (ops.top().type == TokenType::kLParen) {
                    return Err< std::vector< Token > >(
                        CobraError::kParseError, "mismatched parentheses"
                    );
                }
                output.push_back(ops.top());
                ops.pop();
            }
            return output;
        }

        std::vector< std::string > CollectSortedVars(const std::vector< Token > &tokens) {
            std::set< std::string > var_set;
            for (const auto &tok : tokens) {
                if (tok.type == TokenType::kVariable) { var_set.insert(tok.value); }
            }
            return { var_set.begin(), var_set.end() };
        }

        Result< std::unique_ptr< Expr > > BuildAstFromPostfix(
            const std::vector< Token > &postfix, const std::vector< std::string > &var_names,
            uint32_t bitwidth
        ) {
            const uint64_t mask = Bitmask(bitwidth);
            std::stack< std::unique_ptr< Expr > > stack;

            for (const auto &tok : postfix) {
                if (tok.type == TokenType::kNumber) {
                    uint64_t num_val = 0;
                    std::from_chars(
                        tok.value.data(), tok.value.data() + tok.value.size(), num_val
                    );
                    stack.push(Expr::Constant(num_val & mask));
                } else if (tok.type == TokenType::kVariable) {
                    auto it  = std::find(var_names.begin(), var_names.end(), tok.value);
                    auto idx = static_cast< uint32_t >(std::distance(var_names.begin(), it));
                    stack.push(Expr::Variable(idx));
                } else if (tok.type == TokenType::kOp) {
                    if (tok.is_unary) {
                        if (stack.empty()) {
                            return Err< std::unique_ptr< Expr > >(
                                CobraError::kParseError, "malformed expression"
                            );
                        }
                        auto operand = std::move(stack.top());
                        stack.pop();
                        if (tok.value == "~") {
                            stack.push(Expr::BitwiseNot(std::move(operand)));
                        } else if (tok.value == "neg") {
                            stack.push(Expr::Negate(std::move(operand)));
                        } else {
                            return Err< std::unique_ptr< Expr > >(
                                CobraError::kParseError, "unknown unary op: " + tok.value
                            );
                        }
                    } else {
                        if (stack.size() < 2) {
                            return Err< std::unique_ptr< Expr > >(
                                CobraError::kParseError, "malformed expression"
                            );
                        }
                        auto rhs = std::move(stack.top());
                        stack.pop();
                        auto lhs = std::move(stack.top());
                        stack.pop();
                        if (tok.value == "<<" || tok.value == ">>") {
                            if (rhs->kind != Expr::Kind::kConstant) {
                                return Err< std::unique_ptr< Expr > >(
                                    CobraError::kParseError,
                                    "unsupported: shift amount must be "
                                    "an integer literal"
                                );
                            }
                            const uint64_t k = rhs->constant_val;
                            if (k >= bitwidth) {
                                return Err< std::unique_ptr< Expr > >(
                                    CobraError::kParseError,
                                    "shift amount " + std::to_string(k) + " out of range for "
                                        + std::to_string(bitwidth) + "-bit mode"
                                );
                            }
                            if (tok.value == "<<") {
                                const uint64_t multiplier = (1ULL << k) & mask;
                                stack.push(
                                    Expr::Mul(std::move(lhs), Expr::Constant(multiplier))
                                );
                            } else {
                                stack.push(Expr::LogicalShr(std::move(lhs), k));
                            }
                        } else if (tok.value == "+") {
                            stack.push(Expr::Add(std::move(lhs), std::move(rhs)));
                        } else if (tok.value == "-") {
                            stack.push(Expr::Add(std::move(lhs), Expr::Negate(std::move(rhs))));
                        } else if (tok.value == "*") {
                            stack.push(Expr::Mul(std::move(lhs), std::move(rhs)));
                        } else if (tok.value == "&") {
                            stack.push(Expr::BitwiseAnd(std::move(lhs), std::move(rhs)));
                        } else if (tok.value == "|") {
                            stack.push(Expr::BitwiseOr(std::move(lhs), std::move(rhs)));
                        } else if (tok.value == "^") {
                            stack.push(Expr::BitwiseXor(std::move(lhs), std::move(rhs)));
                        } else {
                            return Err< std::unique_ptr< Expr > >(
                                CobraError::kParseError, "unknown binary op: " + tok.value
                            );
                        }
                    }
                }
            }
            if (stack.size() != 1) {
                return Err< std::unique_ptr< Expr > >(
                    CobraError::kParseError, "malformed expression"
                );
            }
            return std::move(stack.top());
        }

        Result< void > ValidateShifts(const std::vector< Token > &postfix, uint32_t bitwidth) {
            for (size_t i = 0; i < postfix.size(); ++i) {
                if (postfix[i].type != TokenType::kOp) { continue; }
                if (postfix[i].value != "<<" && postfix[i].value != ">>") { continue; }
                if (i == 0 || postfix[i - 1].type != TokenType::kNumber) {
                    return Err< void >(
                        CobraError::kParseError,
                        "unsupported: shift amount must be "
                        "an integer literal"
                    );
                }
                uint64_t k = 0;
                std::from_chars(
                    postfix[i - 1].value.data(),
                    postfix[i - 1].value.data() + postfix[i - 1].value.size(), k
                );
                if (k >= bitwidth) {
                    return Err< void >(
                        CobraError::kParseError,
                        "shift amount " + std::to_string(k) + " out of range for "
                            + std::to_string(bitwidth) + "-bit mode"
                    );
                }
            }
            return {};
        }

    } // namespace

    Result< ParseResult > ParseAndEvaluate(const std::string &expr, uint32_t bitwidth) {
        if (expr.empty()) {
            return Err< ParseResult >(CobraError::kParseError, "empty expression");
        }

        auto tokens = Tokenize(expr);
        if (!tokens) { return std::unexpected(std::move(tokens.error())); }

        if (tokens->empty()) {
            return Err< ParseResult >(CobraError::kParseError, "empty expression");
        }

        std::vector< std::string > vars = CollectSortedVars(*tokens);

        // Guard against OOM: reject expressions with too many variables
        if (vars.size() > 20) {
            return Err< ParseResult >(
                CobraError::kTooManyVariables,
                "Expression has " + std::to_string(vars.size())
                    + " variables (max 20 before elimination)"
            );
        }

        // Convert to postfix
        auto postfix = ToPostfix(*tokens);
        if (!postfix) { return std::unexpected(std::move(postfix.error())); }

        auto shifts_ok = ValidateShifts(*postfix, bitwidth);
        if (!shifts_ok) { return std::unexpected(std::move(shifts_ok.error())); }

        // Compile postfix into a compact form: resolve variable
        // indices and parse constants once, not 2^n times.
        enum class CompiledOp : uint8_t {
            kPushConst,
            kPushVar,
            kAdd,
            kSub,
            kMul,
            kAnd,
            kOr,
            kXor,
            kShl,
            kShr,
            kNot,
            kNeg
        };

        struct CompiledToken
        {
            CompiledOp op;
            uint64_t operand;
        };

        const uint64_t kMask = Bitmask(bitwidth);
        std::vector< CompiledToken > compiled;
        compiled.reserve(postfix->size());

        // Compile and validate stack discipline in one pass.
        // depth tracks the simulated stack height.
        int depth = 0; // NOLINT(misc-const-correctness)
        for (const auto &tok : *postfix) {
            if (tok.type == TokenType::kNumber) {
                uint64_t num_val = 0;
                std::from_chars(tok.value.data(), tok.value.data() + tok.value.size(), num_val);
                compiled.push_back(
                    { .op = CompiledOp::kPushConst, .operand = num_val & kMask }
                );
                ++depth;
            } else if (tok.type == TokenType::kVariable) {
                auto it = std::find(vars.begin(), vars.end(), tok.value);
                compiled.push_back(
                    { .op      = CompiledOp::kPushVar,
                      .operand = static_cast< uint64_t >(std::distance(vars.begin(), it)) }
                );
                ++depth;
            } else if (tok.type == TokenType::kOp) {
                if (tok.is_unary) {
                    if (depth < 1) {
                        return Err< ParseResult >(
                            CobraError::kParseError, "malformed expression"
                        );
                    }
                    if (tok.value == "~") {
                        compiled.push_back({ .op = CompiledOp::kNot, .operand = 0 });
                    } else {
                        compiled.push_back({ .op = CompiledOp::kNeg, .operand = 0 });
                    }
                } else {
                    if (depth < 2) {
                        return Err< ParseResult >(
                            CobraError::kParseError, "malformed expression"
                        );
                    }
                    --depth;
                    CompiledOp op = CompiledOp::kAdd;
                    if (tok.value == "+") {
                        op = CompiledOp::kAdd;
                    } else if (tok.value == "-") {
                        op = CompiledOp::kSub;
                    } else if (tok.value == "*") {
                        op = CompiledOp::kMul;
                    } else if (tok.value == "&") {
                        op = CompiledOp::kAnd;
                    } else if (tok.value == "|") {
                        op = CompiledOp::kOr;
                    } else if (tok.value == "^") {
                        op = CompiledOp::kXor;
                    } else if (tok.value == "<<") {
                        op = CompiledOp::kShl;
                    } else if (tok.value == ">>") {
                        op = CompiledOp::kShr;
                    }
                    compiled.push_back({ .op = op, .operand = 0 });
                }
            }
        }
        if (depth != 1) {
            return Err< ParseResult >(CobraError::kParseError, "malformed expression");
        }

        // Evaluate over all {0,1} combinations
        const auto kNumVars = static_cast< uint32_t >(vars.size());
        const size_t kLen   = 1ULL << kNumVars;
        std::vector< uint64_t > sig(kLen);
        std::vector< uint64_t > stack;
        stack.reserve(compiled.size());
        std::vector< uint64_t > inputs(kNumVars);

        for (size_t i = 0; i < kLen; ++i) {
            for (uint32_t v = 0; v < kNumVars; ++v) { inputs[v] = (i >> v) & 1; }
            stack.clear();
            for (const auto &ct : compiled) {
                switch (ct.op) {
                    case CompiledOp::kPushConst:
                        stack.push_back(ct.operand);
                        break;
                    case CompiledOp::kPushVar:
                        stack.push_back(inputs[ct.operand] & kMask);
                        break;
                    case CompiledOp::kNot:
                        stack.back() = (~stack.back()) & kMask;
                        break;
                    case CompiledOp::kNeg:
                        stack.back() = (-stack.back()) & kMask;
                        break;
                    default: {
                        const uint64_t b = stack.back();
                        stack.pop_back();
                        const uint64_t a = stack.back();
                        uint64_t r       = 0;
                        switch (ct.op) {
                            case CompiledOp::kAdd:
                                r = (a + b) & kMask;
                                break;
                            case CompiledOp::kSub:
                                r = (a - b) & kMask;
                                break;
                            case CompiledOp::kMul:
                                r = (a * b) & kMask;
                                break;
                            case CompiledOp::kAnd:
                                r = a & b;
                                break;
                            case CompiledOp::kOr:
                                r = (a | b) & kMask;
                                break;
                            case CompiledOp::kXor:
                                r = (a ^ b) & kMask;
                                break;
                            case CompiledOp::kShl:
                                r = (a << b) & kMask;
                                break;
                            case CompiledOp::kShr:
                                r = (a >> b) & kMask;
                                break;
                            default:
                                break;
                        }
                        stack.back() = r;
                        break;
                    }
                }
            }
            sig[i] = stack.back();
        }

        return Ok(ParseResult{ .sig = std::move(sig), .vars = std::move(vars) });
    }

    bool IsLinearMba(const std::string &expr) {
        if (expr.empty()) { return true; }

        auto tokens = Tokenize(expr);
        if (!tokens) { return true; }

        auto postfix = ToPostfix(*tokens);
        if (!postfix) { return true; }

        // Track whether each stack element depends on variables.
        std::stack< bool > has_var;
        for (const auto &tok : *postfix) {
            if (tok.type == TokenType::kNumber) {
                has_var.push(false);
            } else if (tok.type == TokenType::kVariable) {
                has_var.push(true);
            } else if (tok.type == TokenType::kOp) {
                if (tok.is_unary) {
                    // Unary ops preserve variable-dependence
                } else {
                    if (has_var.size() < 2) { return true; }
                    const bool rhs = has_var.top();
                    has_var.pop();
                    const bool lhs = has_var.top();
                    has_var.pop();
                    if (tok.value == "*" && lhs && rhs) { return false; }
                    has_var.push(lhs || rhs);
                }
            }
        }
        return true;
    }

    Result< AstResult > ParseToAst(const std::string &expr, uint32_t bitwidth) {
        if (expr.empty()) {
            return Err< AstResult >(CobraError::kParseError, "empty expression");
        }

        auto tokens = Tokenize(expr);
        if (!tokens) { return std::unexpected(std::move(tokens.error())); }

        if (tokens->empty()) {
            return Err< AstResult >(CobraError::kParseError, "empty expression");
        }

        std::vector< std::string > vars = CollectSortedVars(*tokens);

        if (vars.size() > 20) {
            return Err< AstResult >(
                CobraError::kTooManyVariables,
                "Expression has " + std::to_string(vars.size())
                    + " variables (max 20 before elimination)"
            );
        }

        auto postfix = ToPostfix(*tokens);
        if (!postfix) { return std::unexpected(std::move(postfix.error())); }

        auto shifts_ok = ValidateShifts(*postfix, bitwidth);
        if (!shifts_ok) { return std::unexpected(std::move(shifts_ok.error())); }

        auto tree = BuildAstFromPostfix(*postfix, vars, bitwidth);
        if (!tree) { return std::unexpected(std::move(tree.error())); }

        return Ok(AstResult{ .expr = std::move(*tree), .vars = std::move(vars) });
    }

} // namespace cobra
