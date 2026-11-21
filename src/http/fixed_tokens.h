// What a FixedTokenTable holds, and the scan of its digests. Internal to neomcp::http; the tests reach it.
#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "crypto/sha256.h"
#include "neomcp/authorization.h"
#include "neomcp/http/fixed_tokens.h"

namespace neomcp::http {

struct FixedTokenTable::Entries {
    std::vector<neomcp::detail::Sha256Digest> digests;     // the digest of each entry's token
    std::vector<std::shared_ptr<const Authorization>> grants;  // what each entry grants, in the same order
};

namespace detail {

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
