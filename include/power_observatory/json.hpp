// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "power_observatory/result.hpp"

namespace po {

// A small, strict JSON document model.
//
// Objects preserve insertion order internally but are emitted with their keys
// sorted, so two documents that carry the same data always serialize to
// byte-identical text. Numbers are carried as 64-bit integers; this runtime has
// no need for binary floating point anywhere on its public surface, and leaving
// it out removes an entire class of cross-platform formatting differences.
class JsonValue {
 public:
  using Array = std::vector<JsonValue>;
  using Member = std::pair<std::string, JsonValue>;
  using Object = std::vector<Member>;

  enum class Type : std::uint8_t { Null = 0, Boolean = 1, Integer = 2, Unsigned = 3, String = 4, Array = 5, Object = 6 };

  JsonValue() = default;
  JsonValue(std::nullptr_t) {}
  JsonValue(bool value) : type_(Type::Boolean), boolean_(value) {}
  JsonValue(int value) : type_(Type::Integer), integer_(value) {}
  JsonValue(std::int64_t value) : type_(Type::Integer), integer_(value) {}
  JsonValue(std::uint64_t value) : type_(Type::Unsigned), unsigned_(value) {}
  JsonValue(std::string value) : type_(Type::String), string_(std::move(value)) {}
  JsonValue(std::string_view value) : type_(Type::String), string_(value) {}
  JsonValue(const char* value) : type_(Type::String), string_(value == nullptr ? "" : value) {}

  [[nodiscard]] static JsonValue array() {
    JsonValue value;
    value.type_ = Type::Array;
    return value;
  }

  [[nodiscard]] static JsonValue object() {
    JsonValue value;
    value.type_ = Type::Object;
    return value;
  }

  [[nodiscard]] Type type() const noexcept { return type_; }
  [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }

  [[nodiscard]] bool as_boolean() const noexcept { return boolean_; }
  [[nodiscard]] std::int64_t as_integer() const noexcept { return integer_; }
  [[nodiscard]] std::uint64_t as_unsigned() const noexcept { return unsigned_; }

  // Reads a numeric field whichever representation it carries. A value that came
  // from the parser is Integer when it fits in a signed 64-bit value and
  // Unsigned otherwise, so a consumer must not depend on which one it got.
  [[nodiscard]] std::uint64_t as_u64() const noexcept {
    return type_ == Type::Unsigned ? unsigned_
                                   : (integer_ < 0 ? 0ull : static_cast<std::uint64_t>(integer_));
  }

  [[nodiscard]] std::int64_t as_i64() const noexcept {
    return type_ == Type::Integer ? integer_ : static_cast<std::int64_t>(unsigned_);
  }
  [[nodiscard]] const std::string& as_string() const noexcept { return string_; }
  [[nodiscard]] const Array& as_array() const noexcept { return array_; }
  [[nodiscard]] const Object& as_object() const noexcept { return object_; }

  void push(JsonValue value);
  void set(std::string key, JsonValue value);
  [[nodiscard]] const JsonValue* find(std::string_view key) const;
  [[nodiscard]] bool contains(std::string_view key) const { return find(key) != nullptr; }
  [[nodiscard]] std::size_t size() const noexcept;

  [[nodiscard]] std::string dump(bool pretty = false) const;
  [[nodiscard]] static Result<JsonValue> parse(std::string_view text);

 private:
  void dump_into(std::string& out, bool pretty, int depth) const;

  Type type_{Type::Null};
  bool boolean_{false};
  std::int64_t integer_{0};
  std::uint64_t unsigned_{0};
  std::string string_;
  Array array_;
  Object object_;
};

// Escapes a string as a JSON string literal, including the surrounding quotes.
[[nodiscard]] std::string json_escape(std::string_view text);

}  // namespace po
