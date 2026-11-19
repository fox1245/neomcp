// Conversion of a description whose names are not unique. NEOMCP_CASE selects the use that is checked:
//   0  a description with unique names, converted both ways: this must compile (it shows that the cases below fail
//      for their names and not for the file)
//   1  from_json of a struct with two members of one name       2  to_json of the same
//   3  from_json of an enum with two enumerators of one name    4  to_json of the same
// Cases 1 to 4 must not compile: a document would otherwise lose a member, or decode to the wrong one, silently.
#include <string>

#include "neomcp/describe.h"

namespace cc {

struct Pair {
    int a;
    int b;
};

enum class Flag { on, off };

}  // namespace cc

#if NEOMCP_CASE == 0 || NEOMCP_CASE == 3 || NEOMCP_CASE == 4
template <>
struct neomcp::Described<cc::Pair> {
    static constexpr auto fields = neomcp::fields(neomcp::field(&cc::Pair::a, "a", ""), neomcp::field(&cc::Pair::b, "b", ""));
};
#else
template <>
struct neomcp::Described<cc::Pair> {
    static constexpr auto fields = neomcp::fields(neomcp::field(&cc::Pair::a, "same", ""), neomcp::field(&cc::Pair::b, "same", ""));
};
#endif

#if NEOMCP_CASE == 0 || NEOMCP_CASE == 1 || NEOMCP_CASE == 2
template <>
struct neomcp::DescribedEnum<cc::Flag> {
    static constexpr neomcp::EnumEntry<cc::Flag> values[] = {{cc::Flag::on, "on"}, {cc::Flag::off, "off"}};
};
#else
template <>
struct neomcp::DescribedEnum<cc::Flag> {
    static constexpr neomcp::EnumEntry<cc::Flag> values[] = {{cc::Flag::on, "same"}, {cc::Flag::off, "same"}};
};
#endif

int use() {
#if NEOMCP_CASE == 0
    const cc::Pair pair = neomcp::from_json<cc::Pair>(neomcp::to_json(cc::Pair{1, 2}));
    const cc::Flag flag = neomcp::from_json<cc::Flag>(neomcp::to_json(cc::Flag::on));
    return pair.a + static_cast<int>(flag);
#elif NEOMCP_CASE == 1
    return neomcp::from_json<cc::Pair>(neomcp::json::object()).a;
#elif NEOMCP_CASE == 2
    return static_cast<int>(neomcp::to_json(cc::Pair{1, 2}).size());
#elif NEOMCP_CASE == 3
    return static_cast<int>(neomcp::from_json<cc::Flag>(neomcp::json("same")));
#elif NEOMCP_CASE == 4
    return static_cast<int>(neomcp::to_json(cc::Flag::on).size());
#else
#error "NEOMCP_CASE must be 0 to 4"
#endif
}
