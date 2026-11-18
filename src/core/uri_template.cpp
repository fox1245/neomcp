#include "neomcp/uri_template.h"

#include <algorithm>
#include <set>
#include <utility>

namespace neomcp {

namespace {

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

int hex_value(char c) {
    if (is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

constexpr unsigned char first_non_ascii = 0x80;

// RFC 3986 unreserved characters.
bool is_unreserved(char c) { return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~'; }

// RFC 3986 reserved characters: gen-delims and sub-delims.
bool is_reserved(char c) {
    constexpr std::string_view reserved = ":/?#[]@!$&'()*+,;=";
    return reserved.find(c) != std::string_view::npos;
}

bool is_non_ascii(char c) { return static_cast<unsigned char>(c) >= first_non_ascii; }

// What RFC 6570 section 2.1 lets a literal hold, besides percent-encoded octets and characters above 0x7F.
bool is_literal_character(char c) {
    constexpr std::string_view forbidden = " \"'%<>\\^`{|}";
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) return false;
    return forbidden.find(c) == std::string_view::npos;
}

struct OperatorEntry {
    char character;
    UriTemplate::Operator op;
};

constexpr OperatorEntry operators[] = {
    {'+', UriTemplate::Operator::reserved},        {'#', UriTemplate::Operator::fragment},
    {'.', UriTemplate::Operator::label},           {'/', UriTemplate::Operator::path},
    {';', UriTemplate::Operator::path_parameter},  {'?', UriTemplate::Operator::query},
    {'&', UriTemplate::Operator::query_continuation},
};

// RFC 6570 section 2.2: operators set aside for future extensions, which a processor must not guess at.
constexpr std::string_view reserved_for_future = "=,!@|";

constexpr std::size_t longest_prefix_digits = 4;  // max-length is below 10000

}  // namespace

std::string_view UriTemplate::operator_character(Operator op) noexcept {
    for (const auto& entry : operators) {
        if (entry.op == op) return std::string_view(&entry.character, 1);
    }
    return {};
}

class UriTemplateParser {
public:
    explicit UriTemplateParser(std::string_view text) : text_(text) {}

    std::variant<UriTemplate, UriTemplateError> run() {
        UriTemplate result;
        result.text_ = std::string(text_);
        std::string literal;
        std::size_t i = 0;
        while (i < text_.size()) {
            const char c = text_[i];
            if (c == '{') {
                if (!literal.empty()) {
                    result.parts_.emplace_back(std::move(literal));
                    literal.clear();
                }
                const std::size_t close = text_.find_first_of("{}", i + 1);
                if (close == std::string_view::npos || text_[close] == '{') {
                    return error(i, close == std::string_view::npos ? "an expression is not closed by '}'"
                                                                    : "an expression holds a '{'");
                }
                auto expression = expression_in(i + 1, close);
                if (auto* problem = std::get_if<UriTemplateError>(&expression)) return std::move(*problem);
                result.parts_.emplace_back(std::move(std::get<UriTemplate::Expression>(expression)));
                i = close + 1;
                continue;
            }
            if (c == '}') return error(i, "a '}' does not close an expression");
            if (c == '%') {
                if (i + 2 >= text_.size() || !is_hex(text_[i + 1]) || !is_hex(text_[i + 2])) {
                    return error(i, "a '%' in a literal must begin a percent-encoded octet");
                }
                literal.append(text_.substr(i, 3));
                i += 3;
                continue;
            }
            if (!is_literal_character(c)) {
                return error(i, "this character may not appear in a literal");
            }
            literal.push_back(c);
            ++i;
        }
        if (!literal.empty()) result.parts_.emplace_back(std::move(literal));
        result.unsupported_ = find_unsupported(result.parts_);
        return result;
    }

private:
    static std::variant<UriTemplate, UriTemplateError> error(std::size_t position, std::string message) {
        return UriTemplateError{position, std::move(message)};
    }

    // The expression between the braces, which hold text_[begin, end).
    std::variant<UriTemplate::Expression, UriTemplateError> expression_in(std::size_t begin, std::size_t end) const {
        if (begin == end) return UriTemplateError{begin - 1, "an expression is empty"};
        UriTemplate::Expression expression;
        std::size_t i = begin;
        const char first = text_[i];
        for (const auto& entry : operators) {
            if (entry.character == first) {
                expression.op = entry.op;
                ++i;
                break;
            }
        }
        if (i == begin && reserved_for_future.find(first) != std::string_view::npos) {
            return UriTemplateError{i, std::string("the operator '") + first + "' is reserved for future extensions"};
        }
        for (;;) {
            auto variable = variable_in(i, end);
            if (auto* problem = std::get_if<UriTemplateError>(&variable)) return std::move(*problem);
            expression.variables.push_back(std::move(std::get<UriTemplate::Variable>(variable)));
            if (i >= end) break;
            if (text_[i] != ',') return UriTemplateError{i, "a variable is followed by a character that is not ','"};
            ++i;
        }
        return expression;
    }

    // Reads one variable specification from `i`, which it advances past it.
    std::variant<UriTemplate::Variable, UriTemplateError> variable_in(std::size_t& i, std::size_t end) const {
        UriTemplate::Variable variable;
        const std::size_t name_begin = i;
        bool expect_character = true;  // a name begins with, and after each '.' continues with, a character
        while (i < end) {
            const char c = text_[i];
            if (is_alpha(c) || is_digit(c) || c == '_') {
                variable.name.push_back(c);
                ++i;
                expect_character = false;
            } else if (c == '%') {
                if (i + 2 >= end || !is_hex(text_[i + 1]) || !is_hex(text_[i + 2])) {
                    return UriTemplateError{i, "a '%' in a name must begin a percent-encoded octet"};
                }
                variable.name.append(text_.substr(i, 3));
                i += 3;
                expect_character = false;
            } else if (c == '.') {
                if (expect_character) return UriTemplateError{i, "a variable name may not begin with, or hold two, '.'"};
                variable.name.push_back(c);
                ++i;
                expect_character = true;
            } else {
                break;
            }
        }
        if (variable.name.empty()) return UriTemplateError{name_begin, "a variable name is missing or holds a character it may not"};
        if (expect_character) return UriTemplateError{i - 1, "a variable name may not end with '.'"};
        if (i < end && text_[i] == '*') {
            variable.explode = true;
            ++i;
        } else if (i < end && text_[i] == ':') {
            ++i;
            const std::size_t digits_begin = i;
            std::size_t value = 0;
            while (i < end && is_digit(text_[i])) {
                value = value * 10 + static_cast<std::size_t>(text_[i] - '0');
                ++i;
            }
            const std::size_t digits = i - digits_begin;
            if (digits == 0 || digits > longest_prefix_digits || text_[digits_begin] == '0') {
                return UriTemplateError{digits_begin, "a prefix modifier needs a length from 1 to 9999"};
            }
            variable.prefix_length = value;
        }
        return variable;
    }

    static std::string describe(const UriTemplate::Expression& expression) {
        std::string out = "{";
        out += UriTemplate::operator_character(expression.op);
        bool first = true;
        for (const auto& variable : expression.variables) {
            if (!first) out += ',';
            first = false;
            out += variable.name;
            if (variable.explode) out += '*';
            if (variable.prefix_length) out += ':' + std::to_string(*variable.prefix_length);
        }
        out += '}';
        return out;
    }

    // The first thing about the template that match() cannot serve.
    static std::optional<std::string> find_unsupported(const std::vector<UriTemplate::Part>& parts) {
        std::set<std::string> seen;
        bool literal_has_question_mark = false;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (const auto* literal = std::get_if<std::string>(&parts[i])) {
                if (literal->find('?') != std::string::npos) literal_has_question_mark = true;
                continue;
            }
            const auto& expression = std::get<UriTemplate::Expression>(parts[i]);
            const std::string shown = describe(expression);
            using Op = UriTemplate::Operator;
            if (expression.op != Op::simple && expression.op != Op::reserved && expression.op != Op::query) {
                return "the operator '" + std::string(UriTemplate::operator_character(expression.op)) + "' of " + shown;
            }
            for (const auto& variable : expression.variables) {
                if (variable.explode) return "the explode modifier '*' of " + shown;
                if (variable.prefix_length) return "the prefix modifier ':" + std::to_string(*variable.prefix_length) + "' of " + shown;
                if (!seen.insert(variable.name).second) return "the variable '" + variable.name + "' appears more than once";
            }
            if (expression.op == Op::query) {
                if (i + 1 != parts.size()) return "the query expression " + shown + " is not the end of the template";
                if (literal_has_question_mark) return "a literal '?' before the query expression " + shown;
                continue;
            }
            const bool next_is_expression =
                i + 1 < parts.size() && std::holds_alternative<UriTemplate::Expression>(parts[i + 1]);
            if (next_is_expression) {
                const auto& next = std::get<UriTemplate::Expression>(parts[i + 1]);
                if (next.op != Op::query) return "the expressions " + shown + " and " + describe(next) + " are adjacent";
            }
        }
        return std::nullopt;
    }

    std::string_view text_;
};

std::variant<UriTemplate, UriTemplateError> UriTemplate::parse(std::string_view text) {
    return UriTemplateParser(text).run();
}

std::vector<std::string> UriTemplate::variables() const {
    std::vector<std::string> names;
    for (const auto& part : parts_) {
        if (const auto* expression = std::get_if<Expression>(&part)) {
            for (const auto& variable : expression->variables) names.push_back(variable.name);
        }
    }
    return names;
}

namespace {

// How far a value extends from `i`: the length of the one unit (an allowed character or a percent-encoded octet) that
// starts there; zero when none does.
std::size_t value_unit(std::string_view text, std::size_t i, bool allow_reserved) {
    const char c = text[i];
    if (c == '%') return i + 2 < text.size() && is_hex(text[i + 1]) && is_hex(text[i + 2]) ? 3 : 0;
    if (is_unreserved(c) || is_non_ascii(c)) return 1;
    return allow_reserved && is_reserved(c) ? 1 : 0;
}

// Whether the whole text is a run of value units.
bool is_value(std::string_view text, bool allow_reserved) {
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t length = value_unit(text, i, allow_reserved);
        if (length == 0) return false;
        i += length;
    }
    return true;
}

std::string decode_percent(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%') {
            out.push_back(static_cast<char>((hex_value(text[i + 1]) << 4) | hex_value(text[i + 2])));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

// One element of the part of a template before its query: a literal, or a variable and what it may hold.
struct Item {
    bool is_variable = false;
    std::string literal;
    std::string name;
    bool allow_reserved = false;
};

bool starts_with_at(std::string_view text, std::size_t at, std::string_view prefix) {
    return at <= text.size() && text.size() - at >= prefix.size() && text.compare(at, prefix.size(), prefix) == 0;
}

std::optional<UriTemplate::Bindings> match_items(std::string_view text, const std::vector<Item>& items) {
    UriTemplate::Bindings bindings;
    std::size_t pos = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& item = items[i];
        if (!item.is_variable) {
            if (!starts_with_at(text, pos, item.literal)) return std::nullopt;
            pos += item.literal.size();
            continue;
        }
        std::size_t end = 0;
        if (i + 1 == items.size()) {
            end = text.size();
        } else if (i + 2 == items.size()) {
            // The last variable, then the template's final literal: the URI must end with it.
            const std::string& tail = items[i + 1].literal;
            if (text.size() - pos < tail.size() || !starts_with_at(text, text.size() - tail.size(), tail)) {
                return std::nullopt;
            }
            end = text.size() - tail.size();
        } else {
            // The shortest value after which the next literal follows.
            const std::string& next = items[i + 1].literal;
            std::size_t k = pos;
            while (!starts_with_at(text, k, next)) {
                if (k >= text.size()) return std::nullopt;
                const std::size_t length = value_unit(text, k, item.allow_reserved);
                if (length == 0) return std::nullopt;
                k += length;
            }
            end = k;
        }
        if (end < pos) return std::nullopt;
        const std::string_view value = text.substr(pos, end - pos);
        if (!is_value(value, item.allow_reserved)) return std::nullopt;
        bindings.emplace_back(item.name, decode_percent(value));
        pos = end;
    }
    return pos == text.size() ? std::optional<UriTemplate::Bindings>(std::move(bindings)) : std::nullopt;
}

}  // namespace

std::optional<UriTemplate::Bindings> UriTemplate::match(std::string_view uri) const {
    auto bindings = match_without_nul_check(uri);
    if (!bindings) return bindings;
    // A NUL byte is refused in the decoded value of every expression form (see the header).
    for (const auto& binding : *bindings) {
        if (binding.second.find('\0') != std::string::npos) return std::nullopt;
    }
    return bindings;
}

std::optional<UriTemplate::Bindings> UriTemplate::match_without_nul_check(std::string_view uri) const {
    if (unsupported_) return std::nullopt;
    std::vector<Item> items;
    const Expression* query = nullptr;
    for (const auto& part : parts_) {
        if (const auto* literal = std::get_if<std::string>(&part)) {
            Item item;
            item.literal = *literal;
            items.push_back(std::move(item));
            continue;
        }
        const auto& expression = std::get<Expression>(part);
        if (expression.op == Operator::query) {
            query = &expression;
            continue;
        }
        bool first = true;
        for (const auto& variable : expression.variables) {
            if (!first) {
                Item comma;
                comma.literal = ",";
                items.push_back(std::move(comma));
            }
            first = false;
            Item item;
            item.is_variable = true;
            item.name = variable.name;
            item.allow_reserved = expression.op == Operator::reserved;
            items.push_back(std::move(item));
        }
    }

    std::string_view path = uri;
    std::optional<std::string_view> query_text;
    if (query != nullptr) {
        const std::size_t mark = uri.find('?');
        if (mark != std::string_view::npos) {
            path = uri.substr(0, mark);
            query_text = uri.substr(mark + 1);
        }
    }
    auto bindings = match_items(path, items);
    if (!bindings || query == nullptr || !query_text) return bindings;

    // `name=value` pairs joined by '&'.
    if (query_text->empty()) return std::nullopt;
    std::set<std::string_view> used;
    std::vector<std::pair<std::size_t, std::pair<std::string, std::string>>> found;  // by the variable's place
    std::size_t begin = 0;
    for (;;) {
        const std::size_t amp = query_text->find('&', begin);
        const std::string_view pair = query_text->substr(begin, amp == std::string_view::npos ? amp : amp - begin);
        const std::size_t equals = pair.find('=');
        if (equals == std::string_view::npos) return std::nullopt;
        const std::string_view name = pair.substr(0, equals);
        const std::string_view value = pair.substr(equals + 1);
        const auto& variables = query->variables;
        const auto place = std::find_if(variables.begin(), variables.end(),
                                        [&](const Variable& variable) { return variable.name == name; });
        if (place == variables.end() || !used.insert(name).second || !is_value(value, false)) return std::nullopt;
        found.push_back({static_cast<std::size_t>(place - variables.begin()),
                         {std::string(name), decode_percent(value)}});
        if (amp == std::string_view::npos) break;
        begin = amp + 1;
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& entry : found) bindings->push_back(std::move(entry.second));
    return bindings;
}

}  // namespace neomcp
