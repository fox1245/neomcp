// Calls the tool of add.h on an open client and says whether it answered as it should.
#pragma once

#include <iostream>
#include <utility>

#include <neomcp/client.h>

#include "add.h"

inline int call_add(const char* program, neomcp::Result<neomcp::Client> opened) {
    if (!opened) {
        std::cerr << program << ": the client did not open: " << opened.error().message << "\n";
        return 1;
    }
    neomcp::Client client = std::move(opened).value();
    const auto sum = client.call<int>("add", AddArgs{2, 3});
    if (!sum) {
        std::cerr << program << ": the call failed: " << sum.error().message << "\n";
        return 1;
    }
    if (sum->is_error || !sum->value || *sum->value != 5) {
        std::cerr << program << ": the tool did not answer 5\n";
        return 1;
    }
    std::cout << program << ": 2 + 3 = " << *sum->value << "\n";
    return 0;
}
