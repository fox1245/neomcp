#include "schema/value.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>
#include <string>

namespace neomcp::json_schema::detail {

namespace {

using Type = json::value_t;

constexpr double two_pow_63 = 9223372036854775808.0;
constexpr double two_pow_64 = 18446744073709551616.0;

// An integer as sign and magnitude, so that the whole range of int64 and uint64 is held exactly. Zero is never
// negative.
struct Integer {
    bool negative = false;
    std::uint64_t magnitude = 0;
};

bool as_integer(const json& value, Integer& out) {
    switch (value.type()) {
        case Type::number_unsigned:
            out = {false, *value.get_ptr<const json::number_unsigned_t*>()};
            return true;
        case Type::number_integer: {
            const auto signed_value = *value.get_ptr<const json::number_integer_t*>();
            if (signed_value < 0) {
                out = {true, static_cast<std::uint64_t>(-(signed_value + 1)) + 1U};
            } else {
                out = {false, static_cast<std::uint64_t>(signed_value)};
            }
            return true;
        }
        default:
            return false;
    }
}

bool as_double(const json& value, double& out) {
    if (value.type() != Type::number_float) return false;
    out = *value.get_ptr<const json::number_float_t*>();
    return true;
}

Order flip(Order order) {
    switch (order) {
        case Order::less: return Order::greater;
        case Order::greater: return Order::less;
        default: return order;
    }
}

Order compare_integers(const Integer& a, const Integer& b) {
    if (a.negative != b.negative) return a.negative ? Order::less : Order::greater;
    if (a.magnitude == b.magnitude) return Order::equal;
    const bool smaller_magnitude = a.magnitude < b.magnitude;
    return smaller_magnitude != a.negative ? Order::less : Order::greater;
}

Order compare_integer_double(const Integer& integer, double number) {
    if (std::isnan(number)) return Order::unordered;
    if (number >= two_pow_64) return Order::less;
    if (number < -two_pow_63) return Order::greater;
    // Now `number` is finite and its integer part fits in a sign and a 64-bit magnitude.
    const double whole = std::trunc(number);
    Integer whole_integer;
    if (whole >= 0) {
        whole_integer = {false, static_cast<std::uint64_t>(whole)};
    } else {
        whole_integer = {true, static_cast<std::uint64_t>(-whole)};
    }
    const Order by_integer_part = compare_integers(integer, whole_integer);
    if (by_integer_part != Order::equal) return by_integer_part;
    const double fraction = number - whole;
    if (fraction > 0) return Order::less;
    if (fraction < 0) return Order::greater;
    return Order::equal;
}

// A decimal number: digits * 10^exponent, with no trailing zero in `digits`. Zero has no digits.
struct Decimal {
    std::uint64_t digits = 0;
    int exponent = 0;
};

Decimal strip_zeros(std::uint64_t digits, int exponent) {
    if (digits == 0) return {};
    while (digits % 10 == 0) {
        digits /= 10;
        ++exponent;
    }
    return {digits, exponent};
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// The decimal a number prints as. A double goes through its shortest printed form because that is the number the
// schema author wrote, not the binary fraction nearest to it.
std::optional<Decimal> to_decimal(const json& value) {
    Integer integer;
    if (as_integer(value, integer)) return strip_zeros(integer.magnitude, 0);
    double number = 0;
    if (!as_double(value, number) || !std::isfinite(number)) return std::nullopt;

    const std::string text = value.dump();
    std::size_t at = 0;
    if (at < text.size() && text[at] == '-') ++at;
    std::string digits;
    int fraction_digits = 0;
    bool after_point = false;
    for (; at < text.size() && (is_digit(text[at]) || text[at] == '.'); ++at) {
        if (text[at] == '.') {
            after_point = true;
        } else {
            digits.push_back(text[at]);
            if (after_point) ++fraction_digits;
        }
    }
    int exponent = 0;
    if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
        ++at;
        bool negative = false;
        if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
            negative = text[at] == '-';
            ++at;
        }
        for (; at < text.size() && is_digit(text[at]); ++at) {
            if (exponent < 100000) exponent = exponent * 10 + (text[at] - '0');
        }
        if (negative) exponent = -exponent;
    }
    if (at != text.size() || digits.empty()) return std::nullopt;

    const auto first = digits.find_first_not_of('0');
    if (first == std::string::npos) return Decimal{};
    digits.erase(0, first);
    int scale = exponent - fraction_digits;
    while (digits.back() == '0') {
        digits.pop_back();
        ++scale;
    }
    // A double prints with at most 17 significant digits, which always fit; 19 digits are below 2^64.
    if (digits.size() > 19) return std::nullopt;
    std::uint64_t magnitude = 0;
    for (const char c : digits) magnitude = magnitude * 10 + static_cast<std::uint64_t>(c - '0');
    return Decimal{magnitude, scale};
}

std::uint64_t mix(std::uint64_t seed, std::uint64_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

std::uint64_t hash_text(std::string_view text) {
    return static_cast<std::uint64_t>(std::hash<std::string_view>{}(text));
}

std::uint64_t hash_integer(const Integer& integer) {
    return mix(integer.negative ? 1 : 2, integer.magnitude);
}

std::uint64_t hash_number(const json& value) {
    Integer integer;
    if (as_integer(value, integer)) return hash_integer(integer);
    double number = 0;
    as_double(value, number);
    // A double that stands for an integer must hash like that integer.
    if (std::isfinite(number) && std::floor(number) == number && number >= -two_pow_63 &&
        number < two_pow_64) {
        if (number >= 0) return hash_integer({false, static_cast<std::uint64_t>(number)});
        return hash_integer({true, static_cast<std::uint64_t>(-number)});
    }
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(number));
    std::memcpy(&bits, &number, sizeof bits);
    return mix(3, bits);
}

Equality equal_at(const json& a, const json& b, std::size_t depth, std::size_t max_depth) {
    if (a.is_number() || b.is_number()) {
        if (!a.is_number() || !b.is_number()) return Equality::different;
        return compare_numbers(a, b) == Order::equal ? Equality::equal : Equality::different;
    }
    if (a.type() != b.type()) return Equality::different;
    switch (a.type()) {
        case Type::null:
            return Equality::equal;
        case Type::boolean:
            return *a.get_ptr<const json::boolean_t*>() == *b.get_ptr<const json::boolean_t*>()
                       ? Equality::equal
                       : Equality::different;
        case Type::string:
            return *a.get_ptr<const json::string_t*>() == *b.get_ptr<const json::string_t*>()
                       ? Equality::equal
                       : Equality::different;
        case Type::array: {
            const auto& left = *a.get_ptr<const json::array_t*>();
            const auto& right = *b.get_ptr<const json::array_t*>();
            if (left.size() != right.size()) return Equality::different;
            if (depth >= max_depth) return Equality::too_deep;
            for (std::size_t i = 0; i < left.size(); ++i) {
                const Equality item = equal_at(left[i], right[i], depth + 1, max_depth);
                if (item != Equality::equal) return item;
            }
            return Equality::equal;
        }
        case Type::object: {
            const auto& left = *a.get_ptr<const json::object_t*>();
            const auto& right = *b.get_ptr<const json::object_t*>();
            if (left.size() != right.size()) return Equality::different;
            if (depth >= max_depth) return Equality::too_deep;
            for (const auto& member : left) {
                const auto other = right.find(std::string_view(member.first));
                if (other == right.end()) return Equality::different;
                const Equality item = equal_at(member.second, other->second, depth + 1, max_depth);
                if (item != Equality::equal) return item;
            }
            return Equality::equal;
        }
        default:
            return Equality::different;
    }
}

std::optional<std::uint64_t> hash_at(const json& value, std::size_t depth, std::size_t max_depth) {
    switch (value.type()) {
        case Type::null:
            return mix(11, 0);
        case Type::boolean:
            return mix(12, *value.get_ptr<const json::boolean_t*>() ? 1 : 0);
        case Type::number_integer:
        case Type::number_unsigned:
        case Type::number_float:
            return hash_number(value);
        case Type::string:
            return mix(13, hash_text(*value.get_ptr<const json::string_t*>()));
        case Type::array: {
            if (depth >= max_depth) return std::nullopt;
            std::uint64_t hash = mix(14, 0);
            for (const auto& item : *value.get_ptr<const json::array_t*>()) {
                const auto item_hash = hash_at(item, depth + 1, max_depth);
                if (!item_hash) return std::nullopt;
                hash = mix(hash, *item_hash);
            }
            return hash;
        }
        case Type::object: {
            if (depth >= max_depth) return std::nullopt;
            // Members have no order, so their hashes are added.
            std::uint64_t sum = 0;
            for (const auto& member : *value.get_ptr<const json::object_t*>()) {
                const auto member_hash = hash_at(member.second, depth + 1, max_depth);
                if (!member_hash) return std::nullopt;
                sum += mix(hash_text(member.first), *member_hash);
            }
            return mix(15, sum);
        }
        default:
            return mix(16, 0);
    }
}

}  // namespace

bool is_integral(const json& value) {
    double number = 0;
    if (as_double(value, number)) return std::isfinite(number) && std::floor(number) == number;
    return value.is_number();
}

Order compare_numbers(const json& a, const json& b) {
    Integer left_integer;
    Integer right_integer;
    const bool left_is_integer = as_integer(a, left_integer);
    const bool right_is_integer = as_integer(b, right_integer);
    if (left_is_integer && right_is_integer) return compare_integers(left_integer, right_integer);
    double left_double = 0;
    double right_double = 0;
    if (left_is_integer && as_double(b, right_double)) {
        return compare_integer_double(left_integer, right_double);
    }
    if (right_is_integer && as_double(a, left_double)) {
        return flip(compare_integer_double(right_integer, left_double));
    }
    if (as_double(a, left_double) && as_double(b, right_double)) {
        if (std::isnan(left_double) || std::isnan(right_double)) return Order::unordered;
        if (left_double < right_double) return Order::less;
        if (left_double > right_double) return Order::greater;
        return Order::equal;
    }
    return Order::unordered;
}

bool is_multiple_of(const json& value, const json& divisor) {
    const auto dividend = to_decimal(value);
    const auto quotient_base = to_decimal(divisor);
    if (!dividend || !quotient_base || quotient_base->digits == 0) return false;
    if (dividend->digits == 0) return true;
    // dividend / divisor = (A / B) * 10^(ea - eb) with A and B free of the factor 10, so the quotient is an integer
    // exactly when B divides A * 10^(ea - eb): never for a negative exponent, and otherwise when what is left of B
    // after the factors it shares with A is 2^x * 5^y with x and y not above the exponent.
    const int scale = dividend->exponent - quotient_base->exponent;
    if (scale < 0) return false;
    std::uint64_t rest = quotient_base->digits / std::gcd(dividend->digits, quotient_base->digits);
    int twos = 0;
    int fives = 0;
    while (rest % 2 == 0) {
        rest /= 2;
        ++twos;
    }
    while (rest % 5 == 0) {
        rest /= 5;
        ++fives;
    }
    return rest == 1 && twos <= scale && fives <= scale;
}

std::optional<std::size_t> to_count(const json& value) {
    constexpr auto largest = (std::numeric_limits<std::size_t>::max)();
    Integer integer;
    if (as_integer(value, integer)) {
        if (integer.negative) return std::nullopt;
        return integer.magnitude > largest ? largest : static_cast<std::size_t>(integer.magnitude);
    }
    double number = 0;
    if (!as_double(value, number) || !std::isfinite(number) || std::floor(number) != number || number < 0) {
        return std::nullopt;
    }
    if (number >= two_pow_64) return largest;
    const auto count = static_cast<std::uint64_t>(number);
    return count > largest ? largest : static_cast<std::size_t>(count);
}

Equality equal(const json& a, const json& b, std::size_t max_depth) { return equal_at(a, b, 0, max_depth); }

std::optional<std::uint64_t> hash_value(const json& value, std::size_t max_depth) {
    return hash_at(value, 0, max_depth);
}

std::string_view type_name(const json& value) {
    switch (value.type()) {
        case Type::null: return "null";
        case Type::boolean: return "boolean";
        case Type::object: return "object";
        case Type::array: return "array";
        case Type::string: return "string";
        case Type::number_integer:
        case Type::number_unsigned: return "integer";
        case Type::number_float: return "number";
        default: return "unknown";
    }
}

}  // namespace neomcp::json_schema::detail
