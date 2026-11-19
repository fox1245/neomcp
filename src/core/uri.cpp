#include "core/uri.h"

#include <string_view>
#include <utility>

namespace neomcp::json_schema::detail {

namespace {

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

int hex_value(char c) {
    if (is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Drop the last path segment of `out` together with the '/' in front of it.
void pop_segment(std::string& out) {
    const auto slash = out.rfind('/');
    out.erase(slash == std::string::npos ? 0 : slash);
}

// RFC 3986 section 5.2.4.
std::string remove_dot_segments(std::string_view input) {
    std::string out;
    while (!input.empty()) {
        if (input.starts_with("../")) {
            input.remove_prefix(3);
        } else if (input.starts_with("./")) {
            input.remove_prefix(2);
        } else if (input.starts_with("/./")) {
            input.remove_prefix(2);
        } else if (input == "/.") {
            input = "/";
        } else if (input.starts_with("/../")) {
            input.remove_prefix(3);
            pop_segment(out);
        } else if (input == "/..") {
            input = "/";
            pop_segment(out);
        } else if (input == "." || input == "..") {
            input = {};
        } else {
            const std::size_t start = input.front() == '/' ? 1 : 0;
            auto end = input.find('/', start);
            if (end == std::string_view::npos) end = input.size();
            out.append(input.substr(0, end));
            input.remove_prefix(end);
        }
    }
    return out;
}

// RFC 3986 section 5.2.3.
std::string merge_paths(const Uri& base, std::string_view reference_path) {
    if (base.authority && base.path.empty()) return "/" + std::string(reference_path);
    const auto slash = base.path.rfind('/');
    std::string merged = slash == std::string::npos ? std::string() : base.path.substr(0, slash + 1);
    merged.append(reference_path);
    return merged;
}

}  // namespace

Uri parse_uri(std::string_view text) {
    Uri uri;
    std::string_view rest = text;
    if (const auto hash = rest.find('#'); hash != std::string_view::npos) {
        uri.fragment = std::string(rest.substr(hash + 1));
        rest = rest.substr(0, hash);
    }
    if (const auto mark = rest.find('?'); mark != std::string_view::npos) {
        uri.query = std::string(rest.substr(mark + 1));
        rest = rest.substr(0, mark);
    }
    if (!rest.empty() && is_alpha(rest.front())) {
        std::size_t end = 1;
        while (end < rest.size() &&
               (is_alpha(rest[end]) || is_digit(rest[end]) || rest[end] == '+' || rest[end] == '-' ||
                rest[end] == '.')) {
            ++end;
        }
        if (end < rest.size() && rest[end] == ':') {
            uri.scheme = std::string(rest.substr(0, end));
            rest.remove_prefix(end + 1);
        }
    }
    if (rest.starts_with("//")) {
        rest.remove_prefix(2);
        auto end = rest.find('/');
        if (end == std::string_view::npos) end = rest.size();
        uri.authority = std::string(rest.substr(0, end));
        rest.remove_prefix(end);
    }
    uri.path = std::string(rest);
    return uri;
}

Uri resolve_uri(const Uri& base, const Uri& ref) {
    Uri target;
    if (ref.scheme) {
        target.scheme = ref.scheme;
        target.authority = ref.authority;
        target.path = remove_dot_segments(ref.path);
        target.query = ref.query;
    } else {
        if (ref.authority) {
            target.authority = ref.authority;
            target.path = remove_dot_segments(ref.path);
            target.query = ref.query;
        } else {
            if (ref.path.empty()) {
                target.path = base.path;
                target.query = ref.query ? ref.query : base.query;
            } else {
                if (ref.path.front() == '/') {
                    target.path = remove_dot_segments(ref.path);
                } else {
                    target.path = remove_dot_segments(merge_paths(base, ref.path));
                }
                target.query = ref.query;
            }
            target.authority = base.authority;
        }
        target.scheme = base.scheme;
    }
    target.fragment = ref.fragment;
    return target;
}

std::string to_string(const Uri& uri) {
    std::string out;
    if (uri.scheme) out += *uri.scheme + ":";
    if (uri.authority) out += "//" + *uri.authority;
    out += uri.path;
    if (uri.query) out += "?" + *uri.query;
    if (uri.fragment) out += "#" + *uri.fragment;
    return out;
}

std::string without_fragment(const Uri& uri) {
    Uri copy = uri;
    copy.fragment.reset();
    return to_string(copy);
}

std::string percent_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hex_value(text[i + 1]) >= 0 &&
            hex_value(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2])));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

}  // namespace neomcp::json_schema::detail
