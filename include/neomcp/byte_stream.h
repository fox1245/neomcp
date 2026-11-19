// The two interfaces a transport over a pair of byte streams reads and writes through: where input bytes come from
// and where output bytes go. They know nothing of the protocol or of the server, so the implementations over file
// descriptors (the internal target neomcp::io, which owns this header) can be built without the server, and a
// transport other than the server's can use them too. neomcp/stdio.h includes this header, so a program that includes
// that one has both names as before.
#pragma once

#include <cstddef>
#include <string_view>

namespace neomcp {

// Where input bytes come from. Called by one thread at a time (the thread that runs the transport), except
// interrupt().
class ByteReader {
public:
    virtual ~ByteReader() = default;

    // Blocks until at least one byte is available and stores up to `capacity` of them in `buffer`. Returns how many,
    // or 0 at the end of the input, when the input failed beyond retrying, or after interrupt().
    virtual std::size_t read(char* buffer, std::size_t capacity) = 0;

    // May be called from any thread while read() blocks, and makes it return 0 soon (and every later call too). The
    // default does nothing: a reader that cannot be woken keeps a failed write unnoticed until its next read returns.
    virtual void interrupt() noexcept {}
};

// Where output bytes go. Called by one thread at a time.
class ByteWriter {
public:
    virtual ~ByteWriter() = default;

    // Writes all of `bytes`; false when it could not (the reader of the output is gone, or the stream failed).
    virtual bool write(std::string_view bytes) = 0;
};

}  // namespace neomcp
