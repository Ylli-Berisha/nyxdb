#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace nyx {

struct Error {
    std::string message;

    explicit Error(std::string msg) : message(std::move(msg)) {}
};

template <typename T>
class Result {
public:
    static Result ok(T val)          { return Result(std::in_place_index<0>, std::move(val)); }
    static Result err(Error e)       { return Result(std::in_place_index<1>, std::move(e)); }
    static Result err(std::string m) { return err(Error{std::move(m)}); }

    bool is_ok()  const { return data_.index() == 0; }
    bool is_err() const { return data_.index() == 1; }

    T&         value()       { return std::get<0>(data_); }
    const T&   value() const { return std::get<0>(data_); }
    const Error& error() const { return std::get<1>(data_); }

private:
    std::variant<T, Error> data_;

    template <std::size_t I, typename... Args>
    explicit Result(std::in_place_index_t<I> tag, Args&&... args)
        : data_(tag, std::forward<Args>(args)...) {}
};

template <>
class Result<void> {
public:
    static Result ok()               { return Result(true); }
    static Result err(Error e)       { return Result(std::move(e)); }
    static Result err(std::string m) { return err(Error{std::move(m)}); }

    bool is_ok()  const { return ok_; }
    bool is_err() const { return !ok_; }
    const Error& error() const { return *error_; }

private:
    bool                 ok_;
    std::optional<Error> error_;

    explicit Result(bool ok) : ok_(ok) {}
    explicit Result(Error e) : ok_(false), error_(std::move(e)) {}
};

} // namespace nyx
