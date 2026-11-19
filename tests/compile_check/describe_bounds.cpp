// A number that is not finite in the option of a member. NEOMCP_CASE selects the use that is checked:
//   0  options made from finite numbers (also the largest ones), used for a schema: this must compile (it shows that
//      the cases below fail for their numbers and not for the file)
//   1  minimum of an infinity     2  maximum of a NaN     3  one_of with an infinity     4  default_value of a NaN
// Cases 1 to 4 must not compile: the number would otherwise be written as null into the schema the tool publishes.
#include <limits>

#include "neomcp/describe.h"

namespace cc {

struct Range {
    double low;
    float high;
};

}  // namespace cc

template <>
struct neomcp::Described<cc::Range> {
#if NEOMCP_CASE == 0
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&cc::Range::low, "low", "", neomcp::minimum(std::numeric_limits<double>::lowest()),
                      neomcp::maximum(std::numeric_limits<double>::max())),
        neomcp::field(&cc::Range::high, "high", "", neomcp::one_of(1.0f, 2.0f), neomcp::default_value(1.0f)));
#elif NEOMCP_CASE == 1
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&cc::Range::low, "low", "", neomcp::minimum(std::numeric_limits<double>::infinity())));
#elif NEOMCP_CASE == 2
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&cc::Range::high, "high", "", neomcp::maximum(std::numeric_limits<float>::quiet_NaN())));
#elif NEOMCP_CASE == 3
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&cc::Range::low, "low", "", neomcp::one_of(1.0, std::numeric_limits<double>::infinity())));
#elif NEOMCP_CASE == 4
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&cc::Range::low, "low", "", neomcp::default_value(std::numeric_limits<double>::quiet_NaN())));
#else
#error "NEOMCP_CASE must be 0 to 4"
#endif
};

int use() { return static_cast<int>(neomcp::schema_of<cc::Range>().size()); }
