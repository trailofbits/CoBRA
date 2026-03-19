#pragma once

#include <expected>
#include <string>
#include <utility>

namespace cobra {

    enum class CobraError {
        kParseError,
        kNonLinearInput,
        kTooManyVariables,
        kNoReduction,
        kVerificationFailed,
    };

    struct ErrorInfo
    {
        CobraError code;
        std::string message;
    };

    template< typename T >
    using Result = std::expected< T, ErrorInfo >;

    template< typename T >
    Result< T > Ok(T value) {
        return Result< T >(std::move(value));
    }

    template< typename T >
    Result< T > Err(CobraError code, std::string message) {
        return std::unexpected(ErrorInfo{ .code = code, .message = std::move(message) });
    }

} // namespace cobra
