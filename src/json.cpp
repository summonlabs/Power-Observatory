// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/json.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string>

namespace po {
namespace {

constexpr int kMaxDepth = 64;
constexpr std::size_t kMaxInputBytes = 64u * 1024u * 1024u;

void append_code_point(std::string& out, std::uint32_t code_point) {
  if (code_point <= 0x7Fu) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else if (code_point <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else if (code_point <= 0x10FFFFu) {
    out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  }
}

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  [[nodiscard]] Result<JsonValue> run() {
    skip_whitespace();
    Result<JsonValue> value = parse_value(0);
    if (!value) {
      return value.error();
    }
    skip_whitespace();
    if (cursor_ != text_.size()) {
      return Error(ReasonCode::ParseError, "trailing characters after the JSON document at offset " +
                                               std::to_string(cursor_));
    }
    return value;
  }

 private:
  void skip_whitespace() noexcept {
    while (cursor_ < text_.size()) {
      const char character = text_[cursor_];
      if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
        ++cursor_;
      } else {
        break;
      }
    }
  }

  [[nodiscard]] bool consume(char expected) noexcept {
    if (cursor_ < text_.size() && text_[cursor_] == expected) {
      ++cursor_;
      return true;
    }
    return false;
  }

  [[nodiscard]] bool consume_literal(std::string_view literal) noexcept {
    if (text_.size() - cursor_ < literal.size()) {
      return false;
    }
    if (text_.compare(cursor_, literal.size(), literal) != 0) {
      return false;
    }
    cursor_ += literal.size();
    return true;
  }

  [[nodiscard]] Result<JsonValue> parse_value(int depth) {
    if (depth > kMaxDepth) {
      return Error(ReasonCode::ParseError, "JSON document nests deeper than the supported depth of " +
                                               std::to_string(kMaxDepth));
    }
    if (cursor_ >= text_.size()) {
      return Error(ReasonCode::UnexpectedEndOfInput, "JSON document ended where a value was expected");
    }
    const char character = text_[cursor_];
    switch (character) {
      case '{':
        return parse_object(depth);
      case '[':
        return parse_array(depth);
      case '"':
        return parse_string();
      case 't':
        if (consume_literal("true")) {
          return JsonValue(true);
        }
        break;
      case 'f':
        if (consume_literal("false")) {
          return JsonValue(false);
        }
        break;
      case 'n':
        if (consume_literal("null")) {
          return JsonValue(nullptr);
        }
        break;
      default:
        if (character == '-' || (character >= '0' && character <= '9')) {
          return parse_number();
        }
        break;
    }
    return Error(ReasonCode::ParseError,
                 "unexpected character at offset " + std::to_string(cursor_) + " in the JSON document");
  }

  [[nodiscard]] Result<JsonValue> parse_object(int depth) {
    ++cursor_;  // consume '{'
    JsonValue object = JsonValue::object();
    skip_whitespace();
    if (consume('}')) {
      return object;
    }
    for (;;) {
      skip_whitespace();
      if (cursor_ >= text_.size() || text_[cursor_] != '"') {
        return Error(ReasonCode::ParseError, "object key expected at offset " + std::to_string(cursor_));
      }
      Result<JsonValue> key = parse_string();
      if (!key) {
        return key.error();
      }
      skip_whitespace();
      if (!consume(':')) {
        return Error(ReasonCode::ParseError, "':' expected after object key at offset " + std::to_string(cursor_));
      }
      skip_whitespace();
      Result<JsonValue> value = parse_value(depth + 1);
      if (!value) {
        return value.error();
      }
      object.set(key.value().as_string(), std::move(value).value());
      skip_whitespace();
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        return object;
      }
      if (cursor_ >= text_.size()) {
        return Error(ReasonCode::UnexpectedEndOfInput, "object is not terminated");
      }
      return Error(ReasonCode::ParseError, "',' or '}' expected at offset " + std::to_string(cursor_));
    }
  }

  [[nodiscard]] Result<JsonValue> parse_array(int depth) {
    ++cursor_;  // consume '['
    JsonValue array = JsonValue::array();
    skip_whitespace();
    if (consume(']')) {
      return array;
    }
    for (;;) {
      skip_whitespace();
      Result<JsonValue> value = parse_value(depth + 1);
      if (!value) {
        return value.error();
      }
      array.push(std::move(value).value());
      skip_whitespace();
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        return array;
      }
      if (cursor_ >= text_.size()) {
        return Error(ReasonCode::UnexpectedEndOfInput, "array is not terminated");
      }
      return Error(ReasonCode::ParseError, "',' or ']' expected at offset " + std::to_string(cursor_));
    }
  }

  [[nodiscard]] Result<JsonValue> parse_string() {
    ++cursor_;  // consume opening quote
    std::string out;
    for (;;) {
      if (cursor_ >= text_.size()) {
        return Error(ReasonCode::UnexpectedEndOfInput, "string literal is not terminated");
      }
      const char character = text_[cursor_++];
      if (character == '"') {
        return JsonValue(std::move(out));
      }
      if (static_cast<unsigned char>(character) < 0x20u) {
        return Error(ReasonCode::ParseError, "unescaped control character inside a string literal");
      }
      if (character != '\\') {
        out.push_back(character);
        continue;
      }
      if (cursor_ >= text_.size()) {
        return Error(ReasonCode::UnexpectedEndOfInput, "escape sequence is truncated");
      }
      const char escape = text_[cursor_++];
      switch (escape) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (text_.size() - cursor_ < 4) {
            return Error(ReasonCode::UnexpectedEndOfInput, "\\u escape is truncated");
          }
          std::uint32_t code_point = 0;
          for (int index = 0; index < 4; ++index) {
            const char digit = text_[cursor_ + static_cast<std::size_t>(index)];
            code_point <<= 4;
            if (digit >= '0' && digit <= '9') {
              code_point |= static_cast<std::uint32_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
              code_point |= static_cast<std::uint32_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
              code_point |= static_cast<std::uint32_t>(digit - 'A' + 10);
            } else {
              return Error(ReasonCode::ParseError, "\\u escape contains a non-hexadecimal digit");
            }
          }
          cursor_ += 4;
          if (code_point >= 0xD800u && code_point <= 0xDBFFu) {
            if (text_.size() - cursor_ < 6 || text_[cursor_] != '\\' || text_[cursor_ + 1] != 'u') {
              return Error(ReasonCode::ParseError, "high surrogate is not followed by a low surrogate");
            }
            cursor_ += 2;
            std::uint32_t low = 0;
            for (int index = 0; index < 4; ++index) {
              const char digit = text_[cursor_ + static_cast<std::size_t>(index)];
              low <<= 4;
              if (digit >= '0' && digit <= '9') {
                low |= static_cast<std::uint32_t>(digit - '0');
              } else if (digit >= 'a' && digit <= 'f') {
                low |= static_cast<std::uint32_t>(digit - 'a' + 10);
              } else if (digit >= 'A' && digit <= 'F') {
                low |= static_cast<std::uint32_t>(digit - 'A' + 10);
              } else {
                return Error(ReasonCode::ParseError, "\\u escape contains a non-hexadecimal digit");
              }
            }
            cursor_ += 4;
            if (low < 0xDC00u || low > 0xDFFFu) {
              return Error(ReasonCode::ParseError, "high surrogate is not followed by a low surrogate");
            }
            code_point = 0x10000u + ((code_point - 0xD800u) << 10) + (low - 0xDC00u);
          } else if (code_point >= 0xDC00u && code_point <= 0xDFFFu) {
            return Error(ReasonCode::ParseError, "unpaired low surrogate in a string literal");
          }
          append_code_point(out, code_point);
          break;
        }
        default:
          return Error(ReasonCode::ParseError, "unrecognized escape sequence");
      }
    }
  }

  [[nodiscard]] Result<JsonValue> parse_number() {
    const std::size_t start = cursor_;
    bool negative = false;
    if (consume('-')) {
      negative = true;
    }
    if (cursor_ >= text_.size() || text_[cursor_] < '0' || text_[cursor_] > '9') {
      return Error(ReasonCode::ParseError, "number has no digits at offset " + std::to_string(start));
    }
    std::uint64_t magnitude = 0;
    constexpr std::uint64_t kSignedLimit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    // A negative literal may reach one further than the signed maximum; a
    // positive literal may use the whole unsigned range.
    const std::uint64_t limit = negative ? kSignedLimit + 1ull : std::numeric_limits<std::uint64_t>::max();
    while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9') {
      const std::uint64_t digit = static_cast<std::uint64_t>(text_[cursor_] - '0');
      if (magnitude > (limit - digit) / 10u) {
        return Error(ReasonCode::ValueOutOfRange,
                     "integer literal at offset " + std::to_string(start) + " exceeds 64-bit range");
      }
      magnitude = magnitude * 10u + digit;
      ++cursor_;
    }
    if (cursor_ < text_.size() && (text_[cursor_] == '.' || text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
      return Error(ReasonCode::UnsupportedToken,
                   "fractional or exponent notation at offset " + std::to_string(start) +
                       " is not supported: this runtime represents every number as a 64-bit integer");
    }
    if (negative) {
      if (magnitude == kSignedLimit + 1ull) {
        return JsonValue(std::numeric_limits<std::int64_t>::min());
      }
      return JsonValue(-static_cast<std::int64_t>(magnitude));
    }
    if (magnitude <= kSignedLimit) {
      return JsonValue(static_cast<std::int64_t>(magnitude));
    }
    return JsonValue(magnitude);
  }

  std::string_view text_;
  std::size_t cursor_{0};
};

}  // namespace

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  out.push_back('"');
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (byte < 0x20u) {
          static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
          out.append("\\u00");
          out.push_back(kHex[(byte >> 4) & 0x0Fu]);
          out.push_back(kHex[byte & 0x0Fu]);
        } else {
          out.push_back(character);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

void JsonValue::push(JsonValue value) {
  if (type_ != Type::Array) {
    type_ = Type::Array;
    array_.clear();
  }
  array_.push_back(std::move(value));
}

void JsonValue::set(std::string key, JsonValue value) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    object_.clear();
  }
  for (Member& member : object_) {
    if (member.first == key) {
      member.second = std::move(value);
      return;
    }
  }
  object_.emplace_back(std::move(key), std::move(value));
}

const JsonValue* JsonValue::find(std::string_view key) const {
  if (type_ != Type::Object) {
    return nullptr;
  }
  for (const Member& member : object_) {
    if (member.first == key) {
      return &member.second;
    }
  }
  return nullptr;
}

std::size_t JsonValue::size() const noexcept {
  switch (type_) {
    case Type::Array:
      return array_.size();
    case Type::Object:
      return object_.size();
    case Type::String:
      return string_.size();
    default:
      return 0;
  }
}

void JsonValue::dump_into(std::string& out, bool pretty, int depth) const {
  const auto indent = [&out, pretty](int level) {
    if (pretty) {
      out.push_back('\n');
      out.append(static_cast<std::size_t>(level) * 2, ' ');
    }
  };

  switch (type_) {
    case Type::Null:
      out.append("null");
      return;
    case Type::Boolean:
      out.append(boolean_ ? "true" : "false");
      return;
    case Type::Integer:
      out.append(std::to_string(integer_));
      return;
    case Type::Unsigned:
      out.append(std::to_string(unsigned_));
      return;
    case Type::String:
      out.append(json_escape(string_));
      return;
    case Type::Array: {
      if (array_.empty()) {
        out.append("[]");
        return;
      }
      out.push_back('[');
      bool first = true;
      for (const JsonValue& element : array_) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        indent(depth + 1);
        element.dump_into(out, pretty, depth + 1);
      }
      indent(depth);
      out.push_back(']');
      return;
    }
    case Type::Object: {
      if (object_.empty()) {
        out.append("{}");
        return;
      }
      std::vector<const Member*> ordered;
      ordered.reserve(object_.size());
      for (const Member& member : object_) {
        ordered.push_back(&member);
      }
      std::sort(ordered.begin(), ordered.end(),
                [](const Member* left, const Member* right) { return left->first < right->first; });
      out.push_back('{');
      bool first = true;
      for (const Member* member : ordered) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        indent(depth + 1);
        out.append(json_escape(member->first));
        out.push_back(':');
        if (pretty) {
          out.push_back(' ');
        }
        member->second.dump_into(out, pretty, depth + 1);
      }
      indent(depth);
      out.push_back('}');
      return;
    }
  }
}

std::string JsonValue::dump(bool pretty) const {
  std::string out;
  dump_into(out, pretty, 0);
  if (pretty) {
    out.push_back('\n');
  }
  return out;
}

Result<JsonValue> JsonValue::parse(std::string_view text) {
  if (text.size() > kMaxInputBytes) {
    return Error(ReasonCode::StorageExhausted, "JSON input exceeds the 64 MiB bound this runtime will parse");
  }
  Parser parser(text);
  return parser.run();
}

}  // namespace po
