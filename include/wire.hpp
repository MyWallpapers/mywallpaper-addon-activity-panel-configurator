#pragma once
#include "model.hpp"
#include <istream>
#include <ostream>
namespace activity::wire {
inline constexpr uint32_t maxChunk = 1024 * 1024;
std::optional<Json> Read(std::istream &in);
void Write(std::ostream &out, const Json &frame);
} // namespace activity::wire
