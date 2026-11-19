// The header collection of the HTTP layer, and the comparison of header field names.
//
// Nothing here depends on an HTTP library, on a socket or on the request and response model, so a client of HTTP
// uses it as a server does. neomcp/http/message.h includes this header, so a program that includes that one has these
// names as before.
#pragma once

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neomcp::http {

// True when the two strings are equal after folding ASCII letters to one case. Header field names compare this way.
bool equal_ignoring_case(std::string_view left, std::string_view right) noexcept;

// Header fields in the order they were added. Names compare without regard to ASCII case; values are kept as given.
class Headers {
public:
    struct Field {
        std::string name;
        std::string value;
    };

    Headers() = default;
    Headers(std::initializer_list<Field> fields);

    // Appends a field. A name may occur more than once.
    Headers& add(std::string name, std::string value);

    // Replaces every field of that name by one with the new value, at the position of the first; appends when there
    // is none.
    Headers& set(std::string name, std::string value);

    // Removes every field of that name and returns how many there were.
    std::size_t remove(std::string_view name);

    // The value of the first field of that name. The view stays valid until the headers are changed.
    std::optional<std::string_view> get(std::string_view name) const noexcept;

    // The values of every field of that name, in order. Views as for get().
    std::vector<std::string_view> get_all(std::string_view name) const;

    std::size_t count(std::string_view name) const noexcept;
    bool contains(std::string_view name) const noexcept { return count(name) > 0; }

    const std::vector<Field>& fields() const noexcept { return fields_; }
    std::size_t size() const noexcept { return fields_.size(); }
    bool empty() const noexcept { return fields_.empty(); }

private:
    std::vector<Field> fields_;
};

}  // namespace neomcp::http
