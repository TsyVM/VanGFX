#pragma once

/**
 * src/capture/deflate.hpp — a zlib stream (RFC 1950 around RFC 1951 deflate)
 * for the PNG and EXR writers, and the checksums that go with it.
 *
 * Deflate is LZ77 matching over a 32 KB window with hash chains, coded in
 * dynamic Huffman blocks; a block that would come out larger than its input is
 * stored instead, so the output is never much bigger than the data. Enough to
 * make a captured frame a fraction of its raw size -- not the last few percent
 * zlib's own best level finds.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vangfx::capture {

// `level` 0 stores; 1..9 search progressively longer for matches.
[[nodiscard]] std::vector<uint8_t> zlib_compress(const uint8_t* data, size_t size, int level = 6);

[[nodiscard]] uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0) noexcept;
[[nodiscard]] uint32_t adler32(const uint8_t* data, size_t size, uint32_t adler = 1) noexcept;

}  // namespace vangfx::capture
