// SPDX-License-Identifier: Apache-2.0
#include "groomlab/png.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "groomlab/parallel.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace groomlab {

namespace {

// ---- Deflate (RFC 1951): one block, fixed Huffman codes ------------------------

constexpr int kWindow = 32768;
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr int kHashBits = 15;
constexpr int kMaxChain = 48;  // candidates tried per position: speed over the last few percent

constexpr std::uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                           31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// A Huffman code goes out most significant bit first: reversed, it is put like any other bits.
std::uint32_t reversed(std::uint32_t code, int length) {
    std::uint32_t r = 0;
    for (int i = 0; i < length; ++i) r = (r << 1) | ((code >> i) & 1u);
    return r;
}

// The fixed codes (RFC 1951 3.2.6), reversed, and which length and distance symbol each
// length and distance takes, worked out once.
struct FixedCodes {
    std::uint16_t literal[288];
    std::uint8_t literalBits[288];
    std::uint8_t distance[30];
    std::uint8_t lengthSymbol[kMaxMatch + 1];
    std::uint8_t distanceSymbol[kWindow + 1];

    FixedCodes() {
        for (int s = 0; s < 288; ++s) {
            std::uint32_t code = 0;
            int bits = 0;
            if (s <= 143) code = 0x30u + static_cast<std::uint32_t>(s), bits = 8;
            else if (s <= 255) code = 0x190u + static_cast<std::uint32_t>(s - 144), bits = 9;
            else if (s <= 279) code = static_cast<std::uint32_t>(s - 256), bits = 7;
            else code = 0xC0u + static_cast<std::uint32_t>(s - 280), bits = 8;
            literal[s] = static_cast<std::uint16_t>(reversed(code, bits));
            literalBits[s] = static_cast<std::uint8_t>(bits);
        }
        for (int d = 0; d < 30; ++d) distance[d] = static_cast<std::uint8_t>(reversed(static_cast<std::uint32_t>(d), 5));
        for (int length = 0; length <= kMaxMatch; ++length) {
            int l = 28;
            while (l > 0 && kLengthBase[l] > length) --l;
            lengthSymbol[length] = static_cast<std::uint8_t>(l);
        }
        for (int dist = 0; dist <= kWindow; ++dist) {
            int d = 29;
            while (d > 0 && kDistBase[d] > dist) --d;
            distanceSymbol[dist] = static_cast<std::uint8_t>(d);
        }
    }
};

const FixedCodes& fixedCodes() {
    static const FixedCodes codes;
    return codes;
}

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    // `n` (up to 32) bits of `bits`, least significant first (deflate's order).
    void put(std::uint32_t bits, int n) {
        buffer_ |= static_cast<std::uint64_t>(bits) << count_;
        count_ += n;
        bits_ += static_cast<std::uint64_t>(n);
        if (count_ >= 32) {
            const std::uint32_t word = static_cast<std::uint32_t>(buffer_);
            const std::uint8_t bytes[4] = {static_cast<std::uint8_t>(word), static_cast<std::uint8_t>(word >> 8),
                                           static_cast<std::uint8_t>(word >> 16), static_cast<std::uint8_t>(word >> 24)};
            out_.insert(out_.end(), bytes, bytes + 4);
            buffer_ >>= 32;
            count_ -= 32;
        }
    }
    // Bits [begin, end) of a stream another writer wrote into `bytes` (padded by 8 bytes).
    void append(const std::vector<std::uint8_t>& bytes, std::uint64_t begin, std::uint64_t end) {
        while (begin < end) {
            const int take = static_cast<int>(std::min<std::uint64_t>(32, end - begin));
            std::uint64_t word = 0;
            std::memcpy(&word, bytes.data() + (begin >> 3), sizeof(word));  // little endian
            word >>= (begin & 7u);
            put(static_cast<std::uint32_t>(word & ((std::uint64_t{1} << take) - 1u)), take);
            begin += static_cast<std::uint64_t>(take);
        }
    }
    void flush() {
        while (count_ > 0) {
            out_.push_back(static_cast<std::uint8_t>(buffer_ & 0xFFu));
            buffer_ >>= 8;
            count_ = std::max(0, count_ - 8);
        }
        buffer_ = 0;
    }
    std::uint64_t bits() const { return bits_; }

private:
    std::vector<std::uint8_t>& out_;
    std::uint64_t buffer_ = 0;
    int count_ = 0;
    std::uint64_t bits_ = 0;
};

void literal(BitWriter& w, int symbol) {
    const FixedCodes& c = fixedCodes();
    w.put(c.literal[symbol], c.literalBits[symbol]);
}

void match(BitWriter& w, int length, int distance) {
    const FixedCodes& c = fixedCodes();
    const int l = c.lengthSymbol[length];
    const int s = 257 + l;
    // The length's code and its extra bits, then the distance's: put together, the same bits.
    w.put(c.literal[s] | (static_cast<std::uint32_t>(length - kLengthBase[l]) << c.literalBits[s]),
          c.literalBits[s] + kLengthExtra[l]);
    const int d = c.distanceSymbol[distance];
    w.put(c.distance[d] | (static_cast<std::uint32_t>(distance - kDistBase[d]) << 5), 5 + kDistExtra[d]);
}

std::uint32_t hash3(const std::uint8_t* p) {
    const std::uint32_t v = (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
    return (v * 2654435761u) >> (32 - kHashBits);
}

int firstDifferentByte(std::uint64_t x) {
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanForward64(&index, x);
    return static_cast<int>(index) / 8;
#else
    return __builtin_ctzll(x) / 8;
#endif
}

// The LZ77 match finder: hash chains over the window. Every position is inserted, whatever the
// parse does there, so the match found at a position depends only on the data (the chain holds
// the positions before it within the window): a finder started anywhere, after inserting the
// window before its start, finds what one started at 0 does.
class Matcher {
public:
    Matcher(const std::uint8_t* data, std::size_t n) : data_(data), n_(n), head_(std::size_t(1) << kHashBits, -1), prev_(kWindow, -1) {}

    void insert(std::size_t at) {
        if (at + kMinMatch > n_) return;
        const std::uint32_t h = hash3(data_ + at);
        prev_[at & (kWindow - 1)] = head_[h];
        head_[h] = static_cast<std::int32_t>(at);
    }

    // The longest match at `i` (the first found of that length); length 0 when none.
    void find(std::size_t i, int* bestLength, int* bestDistance) const {
        *bestLength = 0;
        *bestDistance = 0;
        if (i + kMinMatch > n_) return;
        const std::uint8_t* data = data_;
        const int limit = static_cast<int>(std::min<std::size_t>(kMaxMatch, n_ - i));
        std::int64_t candidate = head_[hash3(data + i)];
        int best = 0;
        for (int chain = 0; chain < kMaxChain && candidate >= 0; ++chain) {
            const std::size_t c = static_cast<std::size_t>(candidate);
            const std::size_t distance = i - c;
            if (distance == 0 || distance > static_cast<std::size_t>(kWindow)) break;
            if (data[c + static_cast<std::size_t>(best)] == data[i + static_cast<std::size_t>(best)]) {
                int length = 0;
                while (length + 8 <= limit) {
                    std::uint64_t a = 0, b = 0;
                    std::memcpy(&a, data + c + static_cast<std::size_t>(length), 8);
                    std::memcpy(&b, data + i + static_cast<std::size_t>(length), 8);
                    if (a != b) {
                        length += firstDifferentByte(a ^ b);
                        goto measured;
                    }
                    length += 8;
                }
                while (length < limit && data[c + static_cast<std::size_t>(length)] == data[i + static_cast<std::size_t>(length)]) ++length;
            measured:
                if (length > best) {
                    best = length;
                    *bestDistance = static_cast<int>(distance);
                    if (length >= limit) break;
                }
            }
            const std::int64_t next = prev_[c & (kWindow - 1)];
            if (next >= candidate) break;  // the slot was reused by a newer position: the chain ends
            candidate = next;
        }
        *bestLength = best;
    }

private:
    const std::uint8_t* data_;
    std::size_t n_;
    std::vector<std::int32_t> head_;
    std::vector<std::int32_t> prev_;
};

// One greedy step at `i`: a match (its positions inserted) or a literal. Returns the next position.
std::size_t step(Matcher& m, const std::uint8_t* data, std::size_t i, BitWriter& w) {
    int length = 0, distance = 0;
    m.find(i, &length, &distance);
    if (length >= kMinMatch) {
        match(w, length, distance);
        for (int k = 0; k < length; ++k) m.insert(i + static_cast<std::size_t>(k));
        return i + static_cast<std::size_t>(length);
    }
    literal(w, data[i]);
    m.insert(i);
    return i + 1;
}

// A stretch of the data parsed greedily from its start, as if the parse arrived there.
struct Stretch {
    std::size_t begin = 0, end = 0;  // [begin, end): where its parse starts; it runs until it passes end
    std::vector<std::uint8_t> bytes;  // its bits (padded by 8 bytes)
    std::uint64_t bits = 0;
    std::size_t reached = 0;  // the position after its last token
    // Where its first tokens start and the bit each starts at: the real parse joins it at one.
    std::vector<std::pair<std::size_t, std::uint64_t>> starts;
};

constexpr std::size_t kStretch = std::size_t(1) << 20;  // bytes per stretch parsed in parallel (default)
constexpr std::size_t kJoinSpan = 4096;  // the real parse must join a stretch's within this

void parseStretch(const std::uint8_t* data, std::size_t n, Stretch& s) {
    Matcher m(data, n);
    for (std::size_t p = s.begin > static_cast<std::size_t>(kWindow) ? s.begin - kWindow : 0; p < s.begin; ++p) m.insert(p);
    BitWriter w(s.bytes);
    std::size_t i = s.begin;
    while (i < s.end) {
        if (i < s.begin + kJoinSpan) s.starts.emplace_back(i, w.bits());
        i = step(m, data, i, w);
    }
    s.bits = w.bits();
    w.flush();
    s.bytes.resize(s.bytes.size() + 8, 0);
    s.reached = i;
}

// Deflate with greedy LZ77 parsing. Large data is parsed in stretches side by side, each from
// its own start; the real parse (from 0) runs serially only where a stretch's start differs
// (a match crossing into it), until it lands on a position the stretch's parse also took, from
// where the two are the same: the bytes are those of one serial parse.
void deflate(const std::vector<std::uint8_t>& data, std::vector<std::uint8_t>& out, std::size_t stretch) {
    BitWriter w(out);
    w.put(1, 1);  // BFINAL
    w.put(1, 2);  // BTYPE 01: fixed Huffman codes
    const std::size_t n = data.size();
    const std::uint8_t* bytes = data.data();
    stretch = stretch > 0 ? stretch : kStretch;
    const std::size_t count = std::max<std::size_t>(1, (n + stretch - 1) / stretch);
    std::vector<Stretch> stretches(count);
    for (std::size_t k = 0; k < count; ++k) {
        stretches[k].begin = k * stretch;
        stretches[k].end = std::min(n, (k + 1) * stretch);
    }
    parallelFor(0, count, [&](std::size_t k) { parseStretch(bytes, n, stretches[k]); }, 1);

    std::size_t i = 0;
    for (const Stretch& s : stretches) {
        if (i >= s.end) continue;  // a match ran over all of it
        if (i == s.begin) {        // where it started (the first one)
            w.append(s.bytes, 0, s.bits);
            i = s.reached;
            continue;
        }
        // Serially from where the real parse is, until it joins the stretch's parse.
        Matcher m(bytes, n);
        for (std::size_t p = i > static_cast<std::size_t>(kWindow) ? i - kWindow : 0; p < i; ++p) m.insert(p);
        auto at = s.starts.begin();
        while (i < s.end) {
            at = std::lower_bound(at, s.starts.end(), std::make_pair(i, std::uint64_t{0}),
                                  [](const auto& a, const auto& b) { return a.first < b.first; });
            if (at != s.starts.end() && at->first == i) {
                w.append(s.bytes, at->second, s.bits);
                i = s.reached;
                break;
            }
            i = step(m, bytes, i, w);  // past the recorded starts this simply runs to the end
        }
    }
    literal(w, 256);  // end of block
    w.flush();
}

// Adler-32 (RFC 1950), the sums reduced once per 5552 bytes (the most that cannot overflow).
std::uint32_t adler32(const std::uint8_t* data, std::size_t n) {
    std::uint32_t a = 1, b = 0;
    std::size_t i = 0;
    while (i < n) {
        const std::size_t end = std::min(n, i + 5552);
        for (; i < end; ++i) {
            a += data[i];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

// ---- PNG ----------------------------------------------------------------------

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) {
    static const auto table = [] {
        std::vector<std::uint32_t> t(256);
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

// Row filter `f` of byte x (row and the row above, `up` null on the first row).
std::uint8_t filtered(int f, const std::uint8_t* row, const std::uint8_t* up, std::size_t x, std::size_t bpp) {
    const int a = x >= bpp ? row[x - bpp] : 0;
    const int b = up ? up[x] : 0;
    const int c = up && x >= bpp ? up[x - bpp] : 0;
    int predicted = 0;
    switch (f) {
        case 1: predicted = a; break;
        case 2: predicted = b; break;
        case 3: predicted = (a + b) / 2; break;
        case 4: predicted = paeth(a, b, c); break;
        default: break;
    }
    return static_cast<std::uint8_t>(row[x] - predicted);
}

void checkImage(int width, int height, int channels, const std::vector<std::uint8_t>& pixels) {
    if (width <= 0 || height <= 0 || channels < 1 || channels > 4) throw std::invalid_argument("encodePng: bad size or channels");
    const std::size_t stride = static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
    if (pixels.size() != stride * static_cast<std::size_t>(height)) {
        throw std::invalid_argument("encodePng: pixel count does not match the size");
    }
}

}  // namespace

std::vector<std::uint8_t> zlibCompress(const std::vector<std::uint8_t>& data, std::size_t stretchBytes) {
    std::vector<std::uint8_t> out = {0x78, 0x01};
    out.reserve(data.size() / 4 + 64);
    deflate(data, out, stretchBytes);
    put32(out, adler32(data.data(), data.size()));
    return out;
}

namespace {

// The PNG of rows of `bytesPerSample` (1: 8 bits, 2: 16 bits big-endian) samples, `channels` a
// pixel; `pixels` holds height rows of width * channels * bytesPerSample bytes (checked).
std::vector<std::uint8_t> encodeRows(int width, int height, int channels, int bytesPerSample,
                                     const std::vector<std::uint8_t>& pixels) {
    const std::size_t stride = static_cast<std::size_t>(width) * static_cast<std::size_t>(channels * bytesPerSample);
    // Each row with the filter leaving the smallest sum of |value| (as signed bytes); rows apart.
    std::vector<std::uint8_t> raw((stride + 1) * static_cast<std::size_t>(height));
    const std::size_t bpp = static_cast<std::size_t>(channels * bytesPerSample);
    parallelFor(0, static_cast<std::size_t>(height), [&](std::size_t y) {
        const std::uint8_t* row = pixels.data() + y * stride;
        const std::uint8_t* up = y > 0 ? row - stride : nullptr;
        long best = -1;
        int bestFilter = 0;
        for (int f = 0; f < 5; ++f) {
            long sum = 0;
            for (std::size_t x = 0; x < stride; ++x) {
                const std::uint8_t value = filtered(f, row, up, x, bpp);
                sum += value < 128 ? value : 256 - value;
            }
            if (best < 0 || sum < best) {
                best = sum;
                bestFilter = f;
            }
        }
        std::uint8_t* dst = raw.data() + y * (stride + 1);
        dst[0] = static_cast<std::uint8_t>(bestFilter);
        for (std::size_t x = 0; x < stride; ++x) dst[1 + x] = filtered(bestFilter, row, up, x, bpp);
    }, 16);

    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, static_cast<std::uint32_t>(width));
    put32(ihdr, static_cast<std::uint32_t>(height));
    ihdr.push_back(static_cast<std::uint8_t>(8 * bytesPerSample));  // bit depth
    const std::uint8_t colorType[5] = {0, 0, 4, 2, 6};  // gray, gray+alpha, rgb, rgba
    ihdr.push_back(colorType[channels]);
    ihdr.push_back(0);  // deflate
    ihdr.push_back(0);  // adaptive filtering
    ihdr.push_back(0);  // no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", zlibCompress(raw));
    chunk(out, "IEND", {});
    return out;
}

}  // namespace

std::vector<std::uint8_t> encodePng(int width, int height, int channels, const std::vector<std::uint8_t>& pixels) {
    checkImage(width, height, channels, pixels);
    return encodeRows(width, height, channels, 1, pixels);
}

std::vector<std::uint8_t> encodePng16(int width, int height, int channels, const std::vector<std::uint16_t>& pixels) {
    if (width <= 0 || height <= 0 || channels < 1 || channels > 4) throw std::invalid_argument("encodePng16: bad size or channels");
    if (pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(channels)) {
        throw std::invalid_argument("encodePng16: pixel count does not match the size");
    }
    std::vector<std::uint8_t> bytes(pixels.size() * 2);
    for (std::size_t i = 0; i < pixels.size(); ++i) {  // big-endian, as PNG stores 16-bit samples
        bytes[2 * i] = static_cast<std::uint8_t>(pixels[i] >> 8);
        bytes[2 * i + 1] = static_cast<std::uint8_t>(pixels[i] & 0xFF);
    }
    return encodeRows(width, height, channels, 2, bytes);
}

std::vector<std::vector<std::uint8_t>> encodePngs(const std::vector<PngImage>& images) {
    for (const PngImage& image : images) {
        if (!image.pixels) throw std::invalid_argument("encodePngs: an image has no pixels");
        checkImage(image.width, image.height, image.channels, *image.pixels);
    }
    std::vector<std::vector<std::uint8_t>> out(images.size());
    parallelFor(0, images.size(), [&](std::size_t i) {
        const PngImage& image = images[i];
        out[i] = encodePng(image.width, image.height, image.channels, *image.pixels);
    }, 1);
    return out;
}

namespace {

void writeBytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("can't write " + path);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("can't write " + path);
}

}  // namespace

void writeCompressedPng(const std::string& path, int width, int height, int channels,
                        const std::vector<std::uint8_t>& pixels) {
    writeBytes(path, encodePng(width, height, channels, pixels));
}

void writeCompressedPng16(const std::string& path, int width, int height, int channels,
                          const std::vector<std::uint16_t>& pixels) {
    writeBytes(path, encodePng16(width, height, channels, pixels));
}

void writeCompressedPngs(const std::vector<std::string>& paths, const std::vector<PngImage>& images) {
    if (paths.size() != images.size()) throw std::invalid_argument("writeCompressedPngs: a path per image");
    const std::vector<std::vector<std::uint8_t>> encoded = encodePngs(images);
    for (std::size_t i = 0; i < paths.size(); ++i) writeBytes(paths[i], encoded[i]);
}

}  // namespace groomlab
