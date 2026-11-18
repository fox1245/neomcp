// Random bytes from the operating system, and their hexadecimal text form, under the names of the HTTP building
// blocks. The functions are those of neomcp/random.h, which the protocol core uses too.
#pragma once

#include "neomcp/random.h"

namespace neomcp::http {

using neomcp::hex_encode;
using neomcp::random_bytes;
using neomcp::random_hex;

}  // namespace neomcp::http
