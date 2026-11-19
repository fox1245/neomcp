// URI templates (RFC 6570): a parser for the whole syntax and a matcher for the forms a resource template needs.
//
//   auto parsed = neomcp::UriTemplate::parse("file:///{+path}{?encoding}");
//   const auto& tpl = std::get<neomcp::UriTemplate>(parsed);
//   auto bindings = tpl.match("file:///docs/a%20b.txt?encoding=utf-8");   // path = "docs/a b.txt", encoding = "utf-8"
//
// Parsing. Every expression form of the RFC is parsed: the operators + # . / ; ? & and the modifiers (prefix `:n`
// and explode `*`). The operators the RFC reserves for future use (= , ! @ |), a brace that does not belong to an
// expression, an empty or malformed variable name and a character no literal may hold are errors.
//
// Matching. Only some forms can be matched, and a template that uses another one parses but reports it through
// unsupported() and matches nothing:
//
//   {var}  {a,b}   simple expansion: the raw text of a value is a run of unreserved characters and percent-encoded
//                  octets (RFC 3986), so it holds no `/`, `?`, `#`, `:` or any other reserved character
//   {+var} {+a,b}  reserved expansion: the raw text of a value may also hold reserved characters (`/` included)
//   {?a,b}         form-style query: `?a=1&b=2`; allowed once, as the last element of the template
//
// The rules, stated so that one URI never matches one template in two ways:
//  - Literal text of the template must appear in the URI exactly (case-sensitive, no normalisation).
//  - The variables of an expression are separated by a literal comma, and two expressions are never adjacent
//    (a literal lies between them) unless the second is the query expression that ends the template; unsupported()
//    reports any other adjacency. Each variable that a literal follows,
//    and that is not the last variable of the template, takes the shortest value that lets that literal match next;
//    the last variable takes everything up to the template's final literal. The match is decided in one pass over
//    the URI and does not backtrack.
//  - A byte of a value must be allowed by the expression (see above) or be a byte above 0x7F; a `%` must begin a
//    percent-encoded octet (two hexadecimal digits). Values are percent-decoded; a malformed escape makes the URI
//    not match. The decoded bytes are not checked to be UTF-8.
//  - What is said above about reserved characters is about the raw text. The DECODED value of every expression form
//    may hold any byte except NUL: `{name}` against `a%2Fb` yields `a/b`, and against `%2E%2E` yields `..`. A URI whose
//    decoded value would hold a NUL byte (`%00`) does not match, because no resource name holds one and a NUL ends a
//    string in C interfaces. A user of a decoded value must not take it for a safe file name or path.
//  - A template that ends in `{?a,b}`: the URI is split at its first `?`. The part before it is matched by the rest
//    of the template, which must hold no literal `?`. What follows it is a list of `name=value` pairs joined by `&`,
//    in any order, each name one of the template's, at most once; a pair that does not fit, an empty list after the
//    `?` and a name the template lacks make the URI not match. A variable with no pair is unbound.
//  - A variable appears once in a template.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace neomcp {

// Why a template text is not a template: the byte offset of the offending character, and what is wrong there.
struct UriTemplateError {
    std::size_t position = 0;
    std::string message;
};

class UriTemplate {
public:
    enum class Operator {
        simple,              // {var}
        reserved,            // {+var}
        fragment,            // {#var}
        label,               // {.var}
        path,                // {/var}
        path_parameter,      // {;var}
        query,               // {?var}
        query_continuation   // {&var}
    };

    struct Variable {
        std::string name;
        std::optional<std::size_t> prefix_length;  // `:n`
        bool explode = false;                      // `*`
    };

    struct Expression {
        Operator op = Operator::simple;
        std::vector<Variable> variables;  // at least one
    };

    // A piece of the template: literal text, or an expression.
    using Part = std::variant<std::string, Expression>;

    // The template that a text spells, or the first error in it. Never throws for bad input.
    static std::variant<UriTemplate, UriTemplateError> parse(std::string_view text);

    const std::string& text() const noexcept { return text_; }
    std::span<const Part> parts() const noexcept { return parts_; }
    // The names of the variables, in the order they appear.
    std::vector<std::string> variables() const;

    // Empty when match() can serve the template; else the first form it cannot serve, written so that it names the
    // form (for example "the operator '#' of {#var}").
    const std::optional<std::string>& unsupported() const noexcept { return unsupported_; }

    // The variables a URI binds, with their percent-decoded values, in the order of the template; empty when the URI
    // does not match. A template for which unsupported() is not empty matches nothing.
    using Bindings = std::vector<std::pair<std::string, std::string>>;
    std::optional<Bindings> match(std::string_view uri) const;

    // The character of an operator (`+` for reserved, ...); empty for the simple operator.
    static std::string_view operator_character(Operator op) noexcept;

private:
    UriTemplate() = default;
    std::optional<Bindings> match_without_nul_check(std::string_view uri) const;
    friend class UriTemplateParser;

    std::string text_;
    std::vector<Part> parts_;
    std::optional<std::string> unsupported_;
};

}  // namespace neomcp
