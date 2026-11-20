#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <variant>

namespace bedrock::sdk {

enum class ErrorCode { Invalid, AlreadyRegistered, NotFound, WrongType,
                       Storage, Unavailable, BufferTooSmall, Internal };

struct Error {
    ErrorCode code;
    std::string message;
};

template<class T>
class Result {
public:
    static Result ok(T value) { return Result(std::move(value)); }
    static Result error(ErrorCode code, std::string message) {
        return Result(Error{code, std::move(message)});
    }
    [[nodiscard]] bool hasValue() const noexcept { return std::holds_alternative<T>(value_); }
    explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] const T& value() const& { return std::get<T>(value_); }
    [[nodiscard]] T& value() & { return std::get<T>(value_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(value_)); }
    [[nodiscard]] const Error& error() const { return std::get<Error>(value_); }

private:
    explicit Result(T value) : value_(std::move(value)) {}
    explicit Result(Error error) : value_(std::move(error)) {}
    std::variant<T, Error> value_;
};

template<>
class Result<void> {
public:
    static Result ok() { return {}; }
    static Result error(ErrorCode code, std::string message) {
        return Result(Error{code, std::move(message)});
    }
    [[nodiscard]] bool hasValue() const noexcept { return !error_; }
    explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] const Error& error() const { return *error_; }
private:
    Result() = default;
    explicit Result(Error error) : error_(std::move(error)) {}
    std::optional<Error> error_;
};

} // namespace bedrock::sdk
