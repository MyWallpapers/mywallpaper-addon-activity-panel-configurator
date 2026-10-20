#include "audio_schema.hpp"
#include "audio_service.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace activity {
namespace {
const Json &Schema() {
    static const Json schema = Json::parse(audioSchema);
    return schema;
}
bool Check(const Json &s, const Json &v) {
    if (s.contains("$ref"))
        return Check(Schema().at(Json::json_pointer(s.at("$ref").get<std::string>().substr(1))), v);
    if (s.contains("const") && s.at("const") != v)
        return false;
    if (s.contains("enum") &&
        std::find(s.at("enum").begin(), s.at("enum").end(), v) == s.at("enum").end())
        return false;
    for (const auto *key : {"allOf", "anyOf", "oneOf"}) {
        if (!s.contains(key))
            continue;
        size_t count = 0;
        for (const auto &part : s.at(key))
            count += Check(part, v) ? 1 : 0;
        if ((std::string(key) == "allOf" && count != s.at(key).size()) ||
            (std::string(key) == "anyOf" && count == 0) ||
            (std::string(key) == "oneOf" && count != 1))
            return false;
    }
    if (s.contains("if")) {
        auto branch = Check(s.at("if"), v) ? "then" : "else";
        if (s.contains(branch) && !Check(s.at(branch), v))
            return false;
    }
    if (s.contains("type")) {
        const auto type = s.at("type").get<std::string>();
        if ((type == "object" && !v.is_object()) || (type == "array" && !v.is_array()) ||
            (type == "string" && !v.is_string()) || (type == "boolean" && !v.is_boolean()) ||
            (type == "null" && !v.is_null()) || (type == "number" && !v.is_number()) ||
            (type == "integer" &&
             (!v.is_number() || std::floor(v.get<double>()) != v.get<double>())))
            return false;
    }
    if (v.is_number()) {
        double n = v.get<double>();
        if (!std::isfinite(n) || (s.contains("minimum") && n < s.at("minimum").get<double>()) ||
            (s.contains("maximum") && n > s.at("maximum").get<double>()))
            return false;
        // All A18 integers cross a JS boundary, including the unbounded timeline
        // fields in the schema. The README additionally requires safe integers.
        if (s.value("type", "") == "integer" && std::abs(n) > 9007199254740991.0)
            return false;
    }
    if (v.is_string()) {
        const auto &str = v.get_ref<const std::string &>();
        // dump validates UTF-8, including overlong and surrogate encodings.
        (void)v.dump();
        size_t chars = 0;
        for (unsigned char c : str)
            if ((c & 0xc0) != 0x80)
                ++chars;
        if ((s.contains("minLength") && chars < s.at("minLength").get<size_t>()) ||
            (s.contains("maxLength") && chars > s.at("maxLength").get<size_t>()) ||
            (s.contains("x-maxUtf8Bytes") && str.size() > s.at("x-maxUtf8Bytes").get<size_t>()))
            return false;
        if (s.contains("pattern")) {
            // Patterns are trusted, pinned schema data, never provider input.
            static std::map<std::string, std::regex> patterns;
            const auto pattern = s.at("pattern").get<std::string>();
            auto it = patterns.find(pattern);
            if (it == patterns.end())
                it = patterns.emplace(pattern, std::regex(pattern)).first;
            // The base64 pattern can recurse deeply in std::regex on MSVC.
            // Its exact grammar is checked iteratively by DecodeBase64 instead.
            if (s.value("maxLength", 0) == 43692) {
                if (str.size() > 43692)
                    return false;
                (void)DecodeBase64(str);
            } else if (!std::regex_search(str, it->second))
                return false;
        }
    }
    if (v.is_object()) {
        if (s.contains("required"))
            for (const auto &key : s.at("required"))
                if (!v.contains(key.get<std::string>()))
                    return false;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (s.contains("properties") && s.at("properties").contains(it.key())) {
                if (!Check(s.at("properties").at(it.key()), it.value()))
                    return false;
            } else if (s.contains("additionalProperties") && s.at("additionalProperties") == false)
                return false;
        }
    }
    if (v.is_array() && s.contains("items"))
        for (const auto &item : v)
            if (!Check(s.at("items"), item))
                return false;
    return true;
}
} // namespace
bool AudioContract(const char *definition, const Json &value) {
    try {
        return Check(Schema().at("$defs").at(definition), value);
    } catch (const std::exception &) {
        return false;
    }
}
std::string DecodeBase64(const std::string &text) {
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (text.empty() || text.size() % 4 || text.size() > 43692)
        throw std::runtime_error("Page base64 invalide");
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (size_t p = 0; p < text.size(); p += 4) {
        unsigned n = 0;
        int padding = 0;
        for (size_t i = 0; i < 4; ++i) {
            char c = text[p + i];
            if (c == '=') {
                if (p + 4 != text.size() || i < 2)
                    throw std::runtime_error("Padding base64 invalide");
                ++padding;
                n <<= 6;
            } else {
                auto a = alphabet.find(c);
                if (a == std::string::npos || padding)
                    throw std::runtime_error("Alphabet base64 invalide");
                n = (n << 6) | static_cast<unsigned>(a);
            }
        }
        if (padding > 2 || (padding == 2 && (n & 0xffff)) || (padding == 1 && (n & 0xff)))
            throw std::runtime_error("Bits base64 non canoniques");
        out += static_cast<char>(n >> 16);
        if (padding < 2)
            out += static_cast<char>(n >> 8);
        if (padding < 1)
            out += static_cast<char>(n);
    }
    return out;
}
std::string Sha256(const std::string &bytes) {
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::array<uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint64_t bitLength = static_cast<uint64_t>(bytes.size()) * 8;
    const size_t padded = (bytes.size() + 9 + 63) / 64 * 64;
    for (size_t offset = 0; offset < padded; offset += 64) {
        uint32_t w[64]{};
        for (size_t i = 0; i < 64; ++i) {
            size_t p = offset + i;
            unsigned char b = p < bytes.size()    ? static_cast<unsigned char>(bytes[p])
                              : p == bytes.size() ? 0x80
                                                  : 0;
            if (p >= padded - 8)
                b = static_cast<unsigned char>(bitLength >> ((padded - 1 - p) * 8));
            w[i / 4] |= static_cast<uint32_t>(b) << ((3 - i % 4) * 8);
        }
        for (int i = 16; i < 64; ++i) {
            auto a = w[i - 15], b = w[i - 2];
            w[i] = w[i - 16] + (std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3)) + w[i - 7] +
                   (std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10));
        }
        auto [a, b, c, d, e, f, g, z] = h;
        for (int i = 0; i < 64; ++i) {
            auto t1 = z + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                      ((e & f) ^ (~e & g)) + k[i] + w[i];
            auto t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) +
                      ((a & b) ^ (a & c) ^ (b & c));
            z = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += z;
    }
    std::ostringstream out;
    out << "sha256:" << std::hex << std::setfill('0');
    for (auto n : h)
        out << std::setw(8) << n;
    return out.str();
}
} // namespace activity
