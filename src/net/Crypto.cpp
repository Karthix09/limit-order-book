#include "net/Crypto.hpp"

namespace net {

namespace {
constexpr std::uint32_t rotl(std::uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); }
}  // namespace

std::array<std::uint8_t, 20> sha1(std::string_view data) {
    std::uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu, h3 = 0x10325476u, h4 = 0xC3D2E1F0u;

    std::string msg(data);
    const std::uint64_t bitLen = static_cast<std::uint64_t>(data.size()) * 8u;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xFFu));

    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(msg.data() + off + static_cast<std::size_t>(i) * 4);
            w[i] = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
                   std::uint32_t{p[3]};
        }
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<std::uint8_t, 20> digest{};
    const std::uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        digest[static_cast<std::size_t>(i) * 4 + 0] = static_cast<std::uint8_t>(hs[i] >> 24);
        digest[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(hs[i] >> 16);
        digest[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>(hs[i] >> 8);
        digest[static_cast<std::size_t>(i) * 4 + 3] = static_cast<std::uint8_t>(hs[i]);
    }
    return digest;
}

std::string base64Encode(const std::uint8_t* data, std::size_t len) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const std::uint32_t n = (std::uint32_t{data[i]} << 16) | (std::uint32_t{data[i + 1]} << 8) | data[i + 2];
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(kAlphabet[(n >> 6) & 63]);
        out.push_back(kAlphabet[n & 63]);
    }
    if (i + 1 == len) {
        const std::uint32_t n = std::uint32_t{data[i]} << 16;
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out += "==";
    } else if (i + 2 == len) {
        const std::uint32_t n = (std::uint32_t{data[i]} << 16) | (std::uint32_t{data[i + 1]} << 8);
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(kAlphabet[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::string webSocketAcceptKey(std::string_view clientKey) {
    std::string material(clientKey);
    material += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const auto digest = sha1(material);
    return base64Encode(digest.data(), digest.size());
}

}  // namespace net
