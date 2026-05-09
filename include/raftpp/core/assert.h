#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "raftpp/fmt.h"
#include "raftpp/logging.h"

namespace raftpp::detail {

template <typename T>
inline constexpr bool kIsStringLike = std::is_convertible_v<T, std::string_view>;

template <typename T, typename = void>
struct HasToString : std::false_type {};

template <typename T>
struct HasToString<T, std::void_t<decltype(std::declval<const T&>().ToString())>> : std::true_type {
};

template <typename T, typename = void>
struct IsFmtFormattable : std::false_type {};

template <typename T>
struct IsFmtFormattable<T, std::void_t<decltype(fmt::format("{}", std::declval<const T&>()))>>
    : std::true_type {};

template <typename T>
std::string DiagnosticToString(const T& value) {
    if constexpr (std::is_same_v<std::decay_t<T>, std::nullopt_t>) {
        return "nullopt";
    } else if constexpr (HasToString<T>::value) {
        return value.ToString();
    } else if constexpr (kIsStringLike<T>) {
        return std::string(std::string_view(value));
    } else if constexpr (IsFmtFormattable<T>::value) {
        return fmt::format("{}", value);
    } else {
        return "<unformattable diagnostic>";
    }
}

template <typename T>
void AppendDiagnostic(std::string& message, const T& value) {
    message.append(" | ");
    message.append(DiagnosticToString(value));
}

template <typename... Diagnostics>
void AppendDiagnostics(std::string& message, const Diagnostics&... diagnostics) {
    (AppendDiagnostic(message, diagnostics), ...);
}

template <typename... Diagnostics>
std::string BuildMessage(std::string_view message, const Diagnostics&... diagnostics) {
    std::string result(message);
    AppendDiagnostics(result, diagnostics...);
    return result;
}

template <typename... Diagnostics>
std::string BuildAssertMessage(
    std::string_view prefix, const char* expression, std::optional<std::string_view> message,
    const Diagnostics&... diagnostics
) {
    std::string result(prefix);
    result.append(expression);
    if (message.has_value() && !message->empty()) {
        result.append(": ");
        result.append(*message);
    }
    AppendDiagnostics(result, diagnostics...);
    return result;
}

[[noreturn]] inline void Panic(const char* file, int line) {
    logging::LogWithLocation("raftpp", logging::LogLevel::kCritical, file, line, __func__, "panic");
    std::abort();
}

[[noreturn]] inline void Panic(const char* file, int line, std::string_view message) {
    logging::Log("raftpp", logging::LogLevel::kCritical, file, line, __func__, message);
    std::abort();
}

template <typename First, typename... Rest>
[[noreturn]] inline void Panic(
    const char* file, int line, const First& first, const Rest&... rest
) {
    if constexpr (kIsStringLike<First>) {
        Panic(file, line, BuildMessage(std::string_view(first), rest...));
    } else {
        Panic(file, line, BuildMessage("panic", first, rest...));
    }
}

inline void Assert(const char* file, int line, const char* expression, bool condition) {
    if (condition) {
        return;
    }
    Panic(file, line, BuildAssertMessage("Assertion failed: ", expression, std::nullopt));
}

template <typename First, typename... Rest>
inline void Assert(
    const char* file, int line, const char* expression, bool condition, const First& first,
    const Rest&... rest
) {
    if (condition) {
        return;
    }
    if constexpr (kIsStringLike<First>) {
        Panic(
            file, line,
            BuildAssertMessage("Assertion failed: ", expression, std::string_view(first), rest...)
        );
    } else {
        Panic(
            file, line,
            BuildAssertMessage("Assertion failed: ", expression, std::nullopt, first, rest...)
        );
    }
}

inline void DebugAssert(const char* file, int line, const char* expression, bool condition) {
    if (condition) {
        return;
    }
    Panic(file, line, BuildAssertMessage("Debug assertion failed: ", expression, std::nullopt));
}

template <typename First, typename... Rest>
inline void DebugAssert(
    const char* file, int line, const char* expression, bool condition, const First& first,
    const Rest&... rest
) {
    if (condition) {
        return;
    }
    if constexpr (kIsStringLike<First>) {
        Panic(
            file, line,
            BuildAssertMessage(
                "Debug assertion failed: ", expression, std::string_view(first), rest...
            )
        );
    } else {
        Panic(
            file, line,
            BuildAssertMessage("Debug assertion failed: ", expression, std::nullopt, first, rest...)
        );
    }
}

}  // namespace raftpp::detail

#define ASSERT(condition, ...)                                                                     \
    do {                                                                                           \
        ::raftpp::detail::Assert(                                                                  \
            __FILE__, __LINE__, #condition, static_cast<bool>(condition) __VA_OPT__(, __VA_ARGS__) \
        );                                                                                         \
    } while (0)

#ifndef NDEBUG
#define DEBUG_ASSERT(condition, ...)                                                               \
    do {                                                                                           \
        ::raftpp::detail::DebugAssert(                                                             \
            __FILE__, __LINE__, #condition, static_cast<bool>(condition) __VA_OPT__(, __VA_ARGS__) \
        );                                                                                         \
    } while (0)
#else
#define DEBUG_ASSERT(...) \
    do {                  \
    } while (0)
#endif

#define PANIC(...)                                                             \
    do {                                                                       \
        ::raftpp::detail::Panic(__FILE__, __LINE__ __VA_OPT__(, __VA_ARGS__)); \
    } while (0)
