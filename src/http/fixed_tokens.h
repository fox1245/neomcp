// What a FixedTokenTable holds, and the scan of its digests. Internal to neomcp::http; the tests reach it.
#pragma once

#include <cstddef>
#include <memory>
#include <set>
#include <span>
#include <vector>

#include "crypto/sha256.h"
#include "neomcp/authorization.h"
#include "neomcp/http/fixed_tokens.h"

namespace neomcp::http {

struct FixedTokenTable::Entries {
    std::vector<neomcp::detail::Sha256Digest> digests;     // the digest of each entry's token
    std::vector<std::shared_ptr<const Authorization>> grants;  // what each entry grants, in the same order
    std::set<const Authorization*> objects;                    // the objects of `grants`
};

namespace detail {

// What the endpoint does with tables besides looking tokens up.
struct FixedTokenAccess {
    // `next`, where each entry whose digest `previous` holds with an equal grant takes the grant object of `previous`.
    // A stream holds the grant object of the token that opened it, so its token is still in a table, with the same
    // grant, exactly when that table holds the object: the object of an unchanged entry passes from table to table.
    static FixedTokenTable carried(const FixedTokenTable& next, const FixedTokenTable& previous);
    // Whether `grant` is one of the grant objects of `table`.
    static bool holds(const FixedTokenTable& table, const Authorization* grant);
};

// The place of the digest in `digests` that `equal` finds equal to `digest`, or digests.size() when none is. Every
// digest is compared, once each and in order, whichever matches: the time of a scan depends on the size of the table,
// never on where (or whether) the digest is in it. `equal` is the comparison in constant time of the library; a test
// passes one that counts.
template <class Equal>
std::size_t scan_digests(std::span<const neomcp::detail::Sha256Digest> digests,
                         const neomcp::detail::Sha256Digest& digest, Equal&& equal) {
    std::size_t found = digests.size();
    for (std::size_t at = 0; at < digests.size(); ++at) {
        const bool same = equal(digests[at], digest);
        // No branch leaves the loop: a match only records its place (two entries never share a digest).
        found = same ? at : found;
    }
    return found;
}

}  // namespace detail

}  // namespace neomcp::http
