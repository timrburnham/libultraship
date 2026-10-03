#include "fast/rt64/TexturePatches.h"

#include <cstring>
#include <map>
#include <mutex>

namespace Fast::Rt64TexturePatches {
namespace {
struct Patch {
    const uint8_t* mask;
    const uint8_t* replacement;
};

std::map<std::string, Patch> gPatches;
std::mutex gPatchesMutex;

std::string NormalizePath(const char* path) {
    if (path == nullptr) {
        return {};
    }
    std::string result(path);
    if (result.starts_with("__OTR__")) {
        result.erase(0, 7);
    }
    return result;
}

uint8_t GetNibble(const uint8_t* bytes, size_t index) {
    const uint8_t byte = bytes[index / 2];
    return (index & 1u) == 0 ? byte >> 4 : byte & 0x0f;
}

void SetNibble(uint8_t* bytes, size_t index, uint8_t nibble) {
    uint8_t& byte = bytes[index / 2];
    if ((index & 1u) == 0) {
        byte = static_cast<uint8_t>((byte & 0x0f) | (nibble << 4));
    } else {
        byte = static_cast<uint8_t>((byte & 0xf0) | nibble);
    }
}
} // namespace

void Register(const char* path, const uint8_t* mask, const uint8_t* replacement) {
    const std::string key = NormalizePath(path);
    if (key.empty()) {
        return;
    }
    std::lock_guard lock(gPatchesMutex);
    gPatches[key] = {mask, replacement};
}

void Unregister(const char* path) {
    const std::string key = NormalizePath(path);
    if (key.empty()) {
        return;
    }
    std::lock_guard lock(gPatchesMutex);
    gPatches.erase(key);
}

std::optional<std::vector<uint8_t>> Apply(const char* path, const uint8_t* canonical, size_t size,
                                          uint8_t format, uint8_t sizeCode, uint32_t width,
                                          uint32_t height) {
    if (canonical == nullptr || width == 0 || height == 0) {
        return std::nullopt;
    }

    Patch patch{};
    {
        const std::string key = NormalizePath(path);
        std::lock_guard lock(gPatchesMutex);
        const auto found = gPatches.find(key);
        if (found == gPatches.end()) {
            return std::nullopt;
        }
        patch = found->second;
    }
    if (patch.mask == nullptr) {
        return std::nullopt;
    }

    const uint64_t pixels64 = static_cast<uint64_t>(width) * height;
    if (pixels64 > SIZE_MAX) {
        return std::nullopt;
    }
    const size_t pixels = static_cast<size_t>(pixels64);
    size_t bytesPerPixel = 0;
    if (sizeCode == 0) {
        bytesPerPixel = 0;
    } else if (sizeCode == 1) {
        bytesPerPixel = 1;
    } else if (sizeCode == 2) {
        bytesPerPixel = 2;
    } else if (sizeCode == 3) {
        bytesPerPixel = 4;
    } else {
        return std::nullopt;
    }
    if (bytesPerPixel != 0 && pixels > SIZE_MAX / bytesPerPixel) {
        return std::nullopt;
    }
    const size_t expectedSize = bytesPerPixel == 0 ? pixels / 2 + pixels % 2 : pixels * bytesPerPixel;
    if (expectedSize > size) {
        return std::nullopt;
    }

    // Native replacement pixels can be copied in any supported N64 format.
    // Without a replacement, only formats with an alpha channel can represent
    // transparent masked pixels without changing the display-list state.
    if (format > 4 || (patch.replacement == nullptr && format != 0 && format != 3)) {
        return std::nullopt;
    }
    if ((format == 0 && sizeCode != 2 && sizeCode != 3) ||
        (format == 3 && sizeCode != 0 && sizeCode != 1 && sizeCode != 2) ||
        (format == 1 && sizeCode != 2) || (format == 2 && sizeCode != 0 && sizeCode != 1) ||
        (format == 4 && sizeCode != 0 && sizeCode != 1)) {
        return std::nullopt;
    }

    std::vector<uint8_t> result(canonical, canonical + expectedSize);
    for (size_t i = 0; i < pixels; ++i) {
        const bool masked = patch.mask[i] != 0;
        if (patch.replacement != nullptr) {
            if (masked) {
                if (sizeCode == 0) {
                    SetNibble(result.data(), i, GetNibble(patch.replacement, i));
                } else {
                    std::memcpy(result.data() + i * bytesPerPixel,
                                patch.replacement + i * bytesPerPixel, bytesPerPixel);
                }
            }
            continue;
        }

        if (masked) {
            if (sizeCode == 0) {
                SetNibble(result.data(), i, 0);
            } else {
                std::memset(result.data() + i * bytesPerPixel, 0, bytesPerPixel);
            }
        }
    }
    return result;
}

} // namespace Fast::Rt64TexturePatches
