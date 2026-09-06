#pragma once

#include "switchrecomp/common/error.hpp"

#include <cassert>
#include <utility>
#include <variant>

namespace switchrecomp
{

template <typename T>
class Result
{
public:
    static Result success(T value)
    {
        return Result(std::move(value));
    }

    static Result failure(Error error)
    {
        return Result(std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept
    {
        return std::holds_alternative<T>(storage_);
    }

    explicit operator bool() const noexcept
    {
        return has_value();
    }

    [[nodiscard]] const T& value() const &
    {
        assert(has_value());
        return std::get<T>(storage_);
    }

    [[nodiscard]] T& value() &
    {
        assert(has_value());
        return std::get<T>(storage_);
    }

    [[nodiscard]] T&& value() &&
    {
        assert(has_value());
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] const Error& error() const
    {
        assert(!has_value());
        return std::get<Error>(storage_);
    }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(Error error) : storage_(std::move(error)) {}

    std::variant<T, Error> storage_;
};

template <>
class Result<void>
{
public:
    static Result success()
    {
        return Result(true, {});
    }

    static Result failure(Error error)
    {
        return Result(false, std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept
    {
        return success_;
    }

    explicit operator bool() const noexcept
    {
        return has_value();
    }

    [[nodiscard]] const Error& error() const
    {
        assert(!success_);
        return error_;
    }

private:
    Result(bool success, Error error) : success_(success), error_(std::move(error)) {}

    bool success_;
    Error error_;
};

} // namespace switchrecomp
