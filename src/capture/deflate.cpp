/**
 * src/capture/deflate.cpp — see deflate.hpp.
 */

#include "deflate.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <queue>
#include <vector>

namespace vangfx::capture {

namespace {

constexpr int      kWindow    = 32768;
constexpr int      kMinMatch  = 3;
constexpr int      kMaxMatch  = 258;
constexpr int      kHashBits  = 15;
constexpr size_t   kBlockTokens = 16384;

constexpr uint16_t kLenBase[29]  = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                     67, 83, 99, 115, 131, 163, 195, 227, 258 };
constexpr uint8_t  kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
constexpr uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
constexpr uint8_t  kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
constexpr uint8_t  kClOrder[19]   = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

struct Token {
    uint16_t lit_or_len;   // a literal byte, or a match length when dist != 0
    uint16_t dist;
};

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
    void put(uint32_t bits, int n) {
        buf_ |= static_cast<uint64_t>(bits) << count_;
        count_ += n;
        while (count_ >= 8) { out_.push_back(static_cast<uint8_t>(buf_)); buf_ >>= 8; count_ -= 8; }
    }
    void align() { if (count_ > 0) put(0, 8 - count_); }
private:
    std::vector<uint8_t>& out_;
    uint64_t              buf_ = 0;
    int                   count_ = 0;
};

// Length (3..258) and distance (1..32768) to their codes, by table: these run
// for every token of every image.
struct CodeTables {
    uint8_t len[259]{};
    uint8_t dist[32769]{};
    CodeTables() {
        for (int l = 3; l <= 258; ++l) {
            int c = 0;
            while (c < 28 && kLenBase[c + 1] <= l) ++c;
            len[l] = static_cast<uint8_t>(c);
        }
        for (int d = 1; d <= 32768; ++d) {
            int c = 0;
            while (c < 29 && kDistBase[c + 1] <= d) ++c;
            dist[d] = static_cast<uint8_t>(c);
        }
    }
};
const CodeTables& tables() { static const CodeTables t; return t; }

int len_code(int len) noexcept   { return tables().len[len]; }
int dist_code(int dist) noexcept { return tables().dist[dist]; }

// Huffman code lengths no longer than `max_bits`. Frequencies are halved
// until the tree fits, which costs a little optimality on skewed data and
// keeps the builder simple.
void build_lengths(std::vector<uint32_t> freq, int max_bits, std::vector<uint8_t>& lens) {
    const int n = static_cast<int>(freq.size());
    lens.assign(n, 0);
    for (;;) {
        struct Node { uint64_t f; int left, right, sym; };
        std::vector<Node> nodes;
        using Item = std::pair<uint64_t, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
        for (int s = 0; s < n; ++s) {
            if (!freq[s]) continue;
            nodes.push_back({ freq[s], -1, -1, s });
            heap.push({ freq[s], static_cast<int>(nodes.size()) - 1 });
        }
        if (nodes.empty()) return;
        if (nodes.size() == 1) { lens[nodes[0].sym] = 1; return; }
        while (heap.size() > 1) {
            const auto a = heap.top(); heap.pop();
            const auto b = heap.top(); heap.pop();
            nodes.push_back({ a.first + b.first, a.second, b.second, -1 });
            heap.push({ a.first + b.first, static_cast<int>(nodes.size()) - 1 });
        }
        int max_depth = 0;
        std::vector<std::pair<int, int>> stack{ { heap.top().second, 0 } };
        while (!stack.empty()) {
            const auto [i, d] = stack.back();
            stack.pop_back();
            if (nodes[i].sym >= 0) { lens[nodes[i].sym] = static_cast<uint8_t>(d); max_depth = std::max(max_depth, d); continue; }
            stack.push_back({ nodes[i].left, d + 1 });
            stack.push_back({ nodes[i].right, d + 1 });
        }
        if (max_depth <= max_bits) return;
        for (auto& f : freq) if (f) f = std::max<uint32_t>(1, f >> 1);
    }
}

// Canonical codes, bit-reversed for an LSB-first stream.
void make_codes(const std::vector<uint8_t>& lens, std::vector<uint16_t>& codes) {
    const int n = static_cast<int>(lens.size());
    codes.assign(n, 0);
    int bl_count[16] = {};
    for (uint8_t l : lens) if (l) ++bl_count[l];
    int next[16] = {};
    int code = 0;
    for (int bits = 1; bits < 16; ++bits) { code = (code + bl_count[bits - 1]) << 1; next[bits] = code; }
    for (int s = 0; s < n; ++s) {
        const int l = lens[s];
        if (!l) continue;
        int c = next[l]++, r = 0;
        for (int i = 0; i < l; ++i) { r = (r << 1) | (c & 1); c >>= 1; }
        codes[s] = static_cast<uint16_t>(r);
    }
}

// A tree needs two codes at least to be complete; give unused symbols a count
// where the data used fewer.
void ensure_two(std::vector<uint32_t>& freq) {
    int used = 0;
    for (auto f : freq) used += f != 0;
    for (size_t s = 0; used < 2 && s < freq.size(); ++s) if (!freq[s]) { freq[s] = 1; ++used; }
}

struct ClSymbol { uint8_t sym, extra_bits; uint8_t extra; };

// The run-length form of the concatenated literal/length and distance code lengths.
std::vector<ClSymbol> rle_lengths(const std::vector<uint8_t>& all) {
    std::vector<ClSymbol> out;
    size_t i = 0;
    while (i < all.size()) {
        const uint8_t l = all[i];
        size_t run = 1;
        while (i + run < all.size() && all[i + run] == l) ++run;
        if (l == 0 && run >= 3) {
            size_t left = run;
            while (left >= 11) { const size_t r = std::min<size_t>(left, 138); out.push_back({ 18, 7, uint8_t(r - 11) }); left -= r; }
            if (left >= 3) { out.push_back({ 17, 3, uint8_t(left - 3) }); left = 0; }
            while (left--) out.push_back({ 0, 0, 0 });
        } else if (l != 0 && run >= 4) {
            out.push_back({ l, 0, 0 });
            size_t left = run - 1;
            while (left >= 3) { const size_t r = std::min<size_t>(left, 6); out.push_back({ 16, 2, uint8_t(r - 3) }); left -= r; }
            while (left--) out.push_back({ l, 0, 0 });
        } else {
            for (size_t k = 0; k < run; ++k) out.push_back({ l, 0, 0 });
        }
        i += run;
    }
    return out;
}

void stored_blocks(BitWriter& bw, const uint8_t* data, size_t size, bool final_block) {
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(size - pos, 65535);
        const bool last = final_block && pos + n == size;
        bw.put(last ? 1 : 0, 1);
        bw.put(0, 2);
        bw.align();
        bw.put(static_cast<uint32_t>(n), 16);
        bw.put(static_cast<uint32_t>(~n) & 0xFFFF, 16);
        for (size_t k = 0; k < n; ++k) bw.put(data[pos + k], 8);
        pos += n;
    } while (pos < size);
}

// One block: dynamic Huffman, or stored when that is smaller.
void emit_block(BitWriter& bw, const std::vector<Token>& tokens, const uint8_t* raw, size_t raw_size, bool final_block) {
    std::vector<uint32_t> lit_freq(286, 0), dist_freq(30, 0);
    for (const Token& t : tokens) {
        if (!t.dist) { ++lit_freq[t.lit_or_len]; continue; }
        ++lit_freq[257 + len_code(t.lit_or_len)];
        ++dist_freq[dist_code(t.dist)];
    }
    lit_freq[256] = 1;
    ensure_two(lit_freq);
    ensure_two(dist_freq);
    std::vector<uint8_t> lit_len, dist_len;
    build_lengths(lit_freq, 15, lit_len);
    build_lengths(dist_freq, 15, dist_len);

    int hlit = 286; while (hlit > 257 && !lit_len[hlit - 1]) --hlit;
    int hdist = 30; while (hdist > 1 && !dist_len[hdist - 1]) --hdist;
    std::vector<uint8_t> all(lit_len.begin(), lit_len.begin() + hlit);
    all.insert(all.end(), dist_len.begin(), dist_len.begin() + hdist);
    const auto cl = rle_lengths(all);
    std::vector<uint32_t> cl_freq(19, 0);
    for (const auto& s : cl) ++cl_freq[s.sym];
    ensure_two(cl_freq);
    std::vector<uint8_t> cl_len;
    build_lengths(cl_freq, 7, cl_len);
    int hclen = 19; while (hclen > 4 && !cl_len[kClOrder[hclen - 1]]) --hclen;

    // Cost of the dynamic block against a stored copy of its input.
    uint64_t bits = 3 + 5 + 5 + 4 + 3ull * hclen;
    for (const auto& s : cl) bits += cl_len[s.sym] + s.extra_bits;
    for (const Token& t : tokens) {
        if (!t.dist) { bits += lit_len[t.lit_or_len]; continue; }
        const int lc = len_code(t.lit_or_len), dc = dist_code(t.dist);
        bits += lit_len[257 + lc] + kLenExtra[lc] + dist_len[dc] + kDistExtra[dc];
    }
    bits += lit_len[256];
    const uint64_t stored_bits = (raw_size / 65535 + 1) * 40 + 8ull * raw_size;
    if (bits >= stored_bits) { stored_blocks(bw, raw, raw_size, final_block); return; }

    std::vector<uint16_t> lit_code, dist_code_v, cl_code;
    make_codes(lit_len, lit_code);
    make_codes(dist_len, dist_code_v);
    make_codes(cl_len, cl_code);

    bw.put(final_block ? 1 : 0, 1);
    bw.put(2, 2);
    bw.put(static_cast<uint32_t>(hlit - 257), 5);
    bw.put(static_cast<uint32_t>(hdist - 1), 5);
    bw.put(static_cast<uint32_t>(hclen - 4), 4);
    for (int i = 0; i < hclen; ++i) bw.put(cl_len[kClOrder[i]], 3);
    for (const auto& s : cl) {
        bw.put(cl_code[s.sym], cl_len[s.sym]);
        if (s.extra_bits) bw.put(s.extra, s.extra_bits);
    }
    for (const Token& t : tokens) {
        if (!t.dist) { bw.put(lit_code[t.lit_or_len], lit_len[t.lit_or_len]); continue; }
        const int lc = len_code(t.lit_or_len), dc = dist_code(t.dist);
        bw.put(lit_code[257 + lc], lit_len[257 + lc]);
        if (kLenExtra[lc]) bw.put(static_cast<uint32_t>(t.lit_or_len - kLenBase[lc]), kLenExtra[lc]);
        bw.put(dist_code_v[dc], dist_len[dc]);
        if (kDistExtra[dc]) bw.put(static_cast<uint32_t>(t.dist - kDistBase[dc]), kDistExtra[dc]);
    }
    bw.put(lit_code[256], lit_len[256]);
}

uint32_t hash3(const uint8_t* p) noexcept {
    const uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
    return (v * 2654435761u) >> (32 - kHashBits);
}

void deflate(BitWriter& bw, const uint8_t* data, size_t size, int level) {
    if (level <= 0 || size == 0) { stored_blocks(bw, data, size, true); return; }
    const int max_chain = level >= 8 ? 256 : level >= 6 ? 64 : level >= 4 ? 16 : 4;
    const bool lazy = level >= 4;

    std::vector<int32_t> head(size_t(1) << kHashBits, -1);
    std::vector<int32_t> prev(kWindow, -1);
    auto insert = [&](size_t pos) {
        if (pos + kMinMatch > size) return;
        const uint32_t h = hash3(data + pos);
        prev[pos & (kWindow - 1)] = head[h];
        head[h] = static_cast<int32_t>(pos);
    };
    auto longest = [&](size_t pos, int& best_dist) -> int {
        best_dist = 0;
        if (pos + kMinMatch > size) return 0;
        const int limit = static_cast<int>(std::min<size_t>(kMaxMatch, size - pos));
        int best = kMinMatch - 1;
        int cand = head[hash3(data + pos)];
        for (int chain = 0; cand >= 0 && chain < max_chain; ++chain) {
            const size_t c = static_cast<size_t>(cand);
            if (pos - c > static_cast<size_t>(kWindow - 1) || c >= pos) break;
            if (data[c + best] == data[pos + best]) {
                int l = 0;
                while (l < limit && data[c + l] == data[pos + l]) ++l;
                if (l > best) { best = l; best_dist = static_cast<int>(pos - c); if (l == limit) break; }
            }
            const int next = prev[c & (kWindow - 1)];
            if (next >= cand) break;
            cand = next;
        }
        return best >= kMinMatch ? best : 0;
    };

    std::vector<Token> tokens;
    tokens.reserve(kBlockTokens + 2);
    size_t block_start = 0, pos = 0;
    while (pos < size) {
        int dist = 0;
        int len = longest(pos, dist);
        if (len && lazy && pos + 1 < size) {
            insert(pos);
            int dist2 = 0;
            const int len2 = longest(pos + 1, dist2);
            if (len2 > len) {
                tokens.push_back({ data[pos], 0 });
                ++pos;
                len = len2; dist = dist2;
            } else {
                for (int k = 1; k < len; ++k) insert(pos + k);
                tokens.push_back({ static_cast<uint16_t>(len), static_cast<uint16_t>(dist) });
                pos += len;
                goto next;
            }
        }
        if (len) {
            for (int k = 0; k < len; ++k) insert(pos + k);
            tokens.push_back({ static_cast<uint16_t>(len), static_cast<uint16_t>(dist) });
            pos += len;
        } else {
            insert(pos);
            tokens.push_back({ data[pos], 0 });
            ++pos;
        }
    next:
        if (tokens.size() >= kBlockTokens) {
            emit_block(bw, tokens, data + block_start, pos - block_start, pos >= size);
            tokens.clear();
            block_start = pos;
        }
    }
    if (!tokens.empty() || block_start == 0) emit_block(bw, tokens, data + block_start, pos - block_start, true);
}

} // namespace

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc) noexcept {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t adler32(const uint8_t* data, size_t size, uint32_t adler) noexcept {
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
    while (size) {
        const size_t n = std::min<size_t>(size, 5552);
        for (size_t i = 0; i < n; ++i) { a += data[i]; b += a; }
        a %= 65521; b %= 65521;
        data += n; size -= n;
    }
    return (b << 16) | a;
}

std::vector<uint8_t> zlib_compress(const uint8_t* data, size_t size, int level) {
    std::vector<uint8_t> out;
    out.reserve(size / 2 + 64);
    out.push_back(0x78);
    out.push_back(0x9C);
    {
        BitWriter bw(out);
        deflate(bw, data, size, level);
        bw.align();
    }
    const uint32_t a = adler32(data, size);
    out.push_back(static_cast<uint8_t>(a >> 24));
    out.push_back(static_cast<uint8_t>(a >> 16));
    out.push_back(static_cast<uint8_t>(a >> 8));
    out.push_back(static_cast<uint8_t>(a));
    return out;
}

}  // namespace vangfx::capture
