#include "wire.hpp"
#include <algorithm>
#include <stdexcept>
namespace activity::wire {
std::optional<Json> Read(std::istream &in) {
    std::string record;
    bool started = false;
    for (;;) {
        unsigned char header[4];
        in.read(reinterpret_cast<char *>(header), 4);
        if (in.gcount() == 0 && !started)
            return std::nullopt;
        if (in.gcount() != 4)
            throw std::runtime_error("En-tête process-v2 tronqué");
        uint32_t raw = uint32_t(header[0]) | (uint32_t(header[1]) << 8) |
                       (uint32_t(header[2]) << 16) | (uint32_t(header[3]) << 24);
        auto kind = raw >> 30, length = raw & 0x3fffffff;
        if (!length || length > maxChunk || (!started && kind > 1) || (started && kind < 2))
            throw std::runtime_error("Fragment process-v2 invalide");
        auto before = record.size();
        record.resize(before + length);
        in.read(record.data() + before, length);
        if (static_cast<uint32_t>(in.gcount()) != length)
            throw std::runtime_error("Corps process-v2 tronqué");
        if (kind == 0 || kind == 3)
            return Json::parse(record);
        started = true;
    }
}
void Write(std::ostream &out, const Json &frame) {
    auto bytes = frame.dump();
    for (size_t offset = 0; offset < bytes.size(); offset += maxChunk) {
        auto length = static_cast<uint32_t>(std::min<size_t>(maxChunk, bytes.size() - offset));
        uint32_t kind = bytes.size() <= maxChunk          ? 0
                        : offset == 0                     ? 1
                        : offset + length == bytes.size() ? 3
                                                          : 2;
        uint32_t raw = (kind << 30) | length;
        unsigned char h[] = {static_cast<unsigned char>(raw), static_cast<unsigned char>(raw >> 8),
                             static_cast<unsigned char>(raw >> 16),
                             static_cast<unsigned char>(raw >> 24)};
        out.write(reinterpret_cast<char *>(h), 4);
        out.write(bytes.data() + offset, length);
    }
    out.flush();
    if (!out)
        throw std::runtime_error("Sortie process-v2 fermée");
}
} // namespace activity::wire
