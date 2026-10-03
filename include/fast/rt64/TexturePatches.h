#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Fast::Rt64TexturePatches {

// Mask and replacement buffers follow the legacy blended-texture API: mask is
// one byte per texel (nonzero selects a texel to clear/replace), while
// replacement is packed in the same native format as the source texture.
void Register(const char* path, const uint8_t* mask, const uint8_t* replacement);
void Unregister(const char* path);

// Returns a patched copy in the same N64 format and dimensions, or nullopt if
// the path is unregistered, the mask is absent, or an alpha-less format has
// no replacement. `canonical` is row-major asset data before RT64's RDRAM
// word swap.
std::optional<std::vector<uint8_t>> Apply(const char* path, const uint8_t* canonical, size_t size,
                                          uint8_t format, uint8_t sizeCode, uint32_t width,
                                          uint32_t height);

} // namespace Fast::Rt64TexturePatches
