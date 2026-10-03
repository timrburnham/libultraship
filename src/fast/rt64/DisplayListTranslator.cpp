#include "fast/rt64/DisplayListTranslator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>

#if defined(__linux__)
#include <link.h>
#endif
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "fast/lus_gbi.h"
#undef GIMMCMD
#include "fast/resource/type/DisplayList.h"
#include "fast/resource/type/Matrix.h"
#include "fast/resource/type/Texture.h"
#include "fast/resource/type/Vertex.h"
#include "fast/rt64/TexturePatches.h"
#include "libultraship/libultra/gbi.h"
#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"

namespace Fast {
namespace {
constexpr uint32_t FramebufferSlotSize = 0x28000;
constexpr uint32_t FramebufferSlotBase = 0x1000;
std::mutex gFramebufferMutex;
std::unordered_map<int, DisplayListTranslator::NativeFramebuffer> gFramebuffers;
#if defined(__linux__)
std::mutex gReadableRangesMutex;
std::vector<std::pair<uintptr_t, uintptr_t>> gReadableRanges;
bool gReadableRangesLoaded = false;
#endif

struct RtCommand {
    uint32_t w0;
    uint32_t w1;
};

#pragma pack(push, 1)
struct RtS2dexBg {
    uint16_t imageW, imageX, frameW;
    int16_t frameX;
    uint16_t imageH, imageY, frameH;
    int16_t frameY;
    uint32_t imageAddress;
    uint8_t imageSiz, imageFmt;
    uint16_t imageLoad, imageFlip, imagePal;
    uint16_t tmemH, tmemW, tmemLoadTH, tmemLoadSH, tmemSize, tmemSizeW;
};

struct RtS2dexScaleBg {
    uint16_t imageW, imageX, frameW;
    int16_t frameX;
    uint16_t imageH, imageY, frameH;
    int16_t frameY;
    uint32_t imageAddress;
    uint8_t imageSiz, imageFmt;
    uint16_t imageLoad, imageFlip, imagePal;
    uint16_t scaleH, scaleW;
    int32_t imageYorig;
    uint8_t padding[4];
};

union RtS2dexBgObject {
    RtS2dexBg bg;
    RtS2dexScaleBg scaleBg;
};
#pragma pack(pop)

static_assert(sizeof(RtS2dexBg) == 40);
static_assert(sizeof(RtS2dexScaleBg) == 40);

constexpr uint32_t Opcode(uint32_t w0) { return w0 >> 24; }
constexpr uint32_t Field(uint32_t w, uint32_t shift, uint32_t bits) {
    return (w >> shift) & ((1u << bits) - 1u);
}

uint64_t ReadHash(const Gfx& payload) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(payload.words.w0)) << 32) |
           static_cast<uint32_t>(payload.words.w1);
}

int32_t SignExtend24(uint32_t value) {
    return static_cast<int32_t>(value << 8) >> 8;
}

bool IsReadableNativePointer(uintptr_t address, size_t size) {
    if (address > std::numeric_limits<uint32_t>::max()) {
        return true;
    }
    if (size > std::numeric_limits<uintptr_t>::max() - address) {
        return false;
    }
#if defined(__linux__)
    auto rangeContainsAddress = [&]() {
        const auto it = std::upper_bound(gReadableRanges.begin(), gReadableRanges.end(), address,
                                         [](uintptr_t value, const auto& range) { return value < range.first; });
        if (it == gReadableRanges.begin()) {
            return false;
        }
        const auto& range = *std::prev(it);
        return address >= range.first && address + size <= range.second;
    };
    {
        std::lock_guard lock(gReadableRangesMutex);
        if (!gReadableRangesLoaded || !rangeContainsAddress()) {
            std::ifstream maps("/proc/self/maps");
            std::vector<std::pair<uintptr_t, uintptr_t>> refreshed;
            std::string line;
            while (std::getline(maps, line)) {
                std::istringstream fields(line);
                std::string bounds;
                std::string permissions;
                if (!(fields >> bounds >> permissions) || permissions.empty() || permissions[0] != 'r') {
                    continue;
                }
                const size_t dash = bounds.find('-');
                if (dash == std::string::npos) {
                    continue;
                }
                try {
                    const uintptr_t start = std::stoull(bounds.substr(0, dash), nullptr, 16);
                    const uintptr_t end = std::stoull(bounds.substr(dash + 1), nullptr, 16);
                    refreshed.emplace_back(start, end);
                } catch (...) {
                    continue;
                }
            }
            if (!refreshed.empty()) {
                gReadableRanges = std::move(refreshed);
            }
            gReadableRangesLoaded = true;
        }
        if (rangeContainsAddress()) {
            return true;
        }
    }
    struct Search {
        uintptr_t address;
        uintptr_t end;
        bool found = false;
    } search{address, address + size, false};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* opaque) {
            auto& range = *static_cast<Search*>(opaque);
            for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr)& header = info->dlpi_phdr[i];
                if (header.p_type != PT_LOAD || (header.p_flags & PF_R) == 0) {
                    continue;
                }
                const uintptr_t segmentStart = static_cast<uintptr_t>(info->dlpi_addr) + header.p_vaddr;
                const uintptr_t segmentEnd = segmentStart + header.p_memsz;
                if (range.address >= segmentStart && range.end <= segmentEnd) {
                    range.found = true;
                    return 1;
                }
            }
            return 0;
        },
        &search);
    return search.found;
#else
    (void)size;
    return false;
#endif
}

MtxS ToFixedMatrix(const MtxF& matrix) {
    MtxS result{};
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            const double scaled = static_cast<double>(matrix.mf[row][column]) * 65536.0;
            // Match guMtxF2L: native and interpolated draws must quantize identically.
            const int64_t fixed = static_cast<int64_t>(scaled);
            result.intPart[row][column ^ 1] = static_cast<uint16_t>(static_cast<int16_t>(fixed >> 16));
            result.fracPart[row][column ^ 1] = static_cast<uint16_t>(fixed & 0xffff);
        }
    }
    return result;
}
} // namespace

DisplayListTranslator::DisplayListTranslator() : mRdram(RdramSize, 0) {}

void DisplayListTranslator::DefineFramebuffer(int id, uint32_t width, uint32_t height) {
    if (id == 0) {
        if (width == 0 || height == 0) {
            throw std::invalid_argument("Framebuffer dimensions must be nonzero");
        }
        std::lock_guard lock(gFramebufferMutex);
        gFramebuffers[id] = {id, width, height, ColorAddress};
        return;
    }
    if (id < 1 || id > 5 || width == 0 || height == 0 ||
        static_cast<uint64_t>(width) * height * 2 > FramebufferSlotSize) {
        throw std::invalid_argument("Native framebuffer ID or RGBA16 dimensions exceed the synthetic RDRAM slots");
    }
    const uint32_t address = FramebufferSlotBase + static_cast<uint32_t>(id - 1) * FramebufferSlotSize;
    if (address + FramebufferSlotSize > ColorAddress) {
        throw std::logic_error("Native framebuffer slots overlap the main color buffer");
    }
    std::lock_guard lock(gFramebufferMutex);
    gFramebuffers[id] = {id, width, height, address};
}

std::optional<DisplayListTranslator::NativeFramebuffer> DisplayListTranslator::GetFramebuffer(int id) {
    if (id == 0) {
        std::lock_guard lock(gFramebufferMutex);
        const auto it = gFramebuffers.find(0);
        return it == gFramebuffers.end() ? std::optional<NativeFramebuffer>{{0, 320, 240, ColorAddress}}
                                         : std::optional<NativeFramebuffer>{it->second};
    }
    std::lock_guard lock(gFramebufferMutex);
    const auto it = gFramebuffers.find(id);
    if (it == gFramebuffers.end()) {
        return std::nullopt;
    }
    return it->second;
}

void DisplayListTranslator::Reset() {
    mNextAddress = 0x200000;
    mSegments.fill(0);
    mPointerAddresses.clear();
    mTextureCopies.clear();
    mTranslatedLists.clear();
    mListsInProgress.clear();
    mRetainedResources.clear();
    mOwnedNativeTextureData.clear();
    mCurrentUcode = 4; // native UcodeHandlers::ucode_f3dex2
    mLatestNativeDepthImage = 0;
    mActiveFramebufferId = 0;
    mCurrentTextureFramebufferId = -1;
    mInterpolationIndex = 0;
    mInterpolationIndexTarget = 0;
    mFramebufferOperations.clear();
    mNativeWideRectangles.clear();
    mNativeTextures.clear();
}

uint32_t DisplayListTranslator::Allocate(size_t size, size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        throw std::invalid_argument("Display-list allocation alignment must be a power of two");
    }
    const uint64_t aligned = (static_cast<uint64_t>(mNextAddress) + alignment - 1) & ~(alignment - 1);
    const uint64_t end = aligned + size;
    if (end > RdramSize || end < aligned) {
        std::ostringstream message;
        message << "Translated RDRAM allocation of " << size << " bytes at 0x" << std::hex << aligned
                << " exceeds 8 MiB (arena cursor 0x" << mNextAddress << ')';
        throw std::runtime_error(message.str());
    }
    mNextAddress = static_cast<uint32_t>(end);
    return static_cast<uint32_t>(aligned);
}

uint32_t DisplayListTranslator::CopyBytes(const void* source, size_t size, size_t alignment) {
    if (source == nullptr && size != 0) {
        throw std::runtime_error("Cannot copy a null display-list data pointer");
    }
    const uint32_t address = Allocate(size, alignment);
    if (size != 0) {
        std::memcpy(mRdram.data() + address, source, size);
    }
    return address;
}

uint32_t DisplayListTranslator::CopyTexture(const uint8_t* source, size_t size) {
    if (source == nullptr && size != 0) {
        throw std::runtime_error("Cannot copy a null texture image");
    }
    if (const auto it = mTextureCopies.find(source); it != mTextureCopies.end() && it->second.second >= size) {
        return it->second.first;
    }
    const uint32_t address = Allocate((size + 3) & ~size_t(3), 8);
    // RT64 models the N64's word-swapped RDRAM and XORs byte addresses with 3
    // when loading texels. Store canonical asset bytes in that representation.
    for (size_t i = 0; i < size; ++i) {
        mRdram[address + (i ^ 3u)] = source[i];
    }
    mTextureCopies[source] = {address, size};
    return address;
}

uint32_t DisplayListTranslator::CopyTextureResource(const std::shared_ptr<Texture>& texture, size_t nativeSize,
                                                     uint32_t nativeWidth, uint32_t bitsPerTexel, uint8_t format,
                                                     uint8_t sizeCode, const char* path) {
    if (!texture || texture->ImageData == nullptr) {
        throw std::runtime_error("Texture resource has no image data");
    }
    const size_t imageBytes = texture->ImageDataSize ? texture->ImageDataSize : nativeSize;
    const uint8_t* image = texture->ImageData;
    const bool canonicalRgba32 = texture->Type == TextureType::RGBA32bpp && format == G_IM_FMT_RGBA &&
                                 sizeCode == G_IM_SIZ_32b && nativeWidth > 1 && imageBytes > 4096;
    const bool nativeRgbaPayload =
        ((texture->Flags & (TEX_FLAG_LOAD_AS_RAW | TEX_FLAG_LOAD_AS_IMG)) != 0 || canonicalRgba32) &&
        texture->Width != 0 && texture->Height != 0 &&
        imageBytes >= static_cast<size_t>(texture->Width) * texture->Height * 4;
    if (nativeRgbaPayload && nativeSize != 0 && nativeWidth != 0 && bitsPerTexel != 0) {
        if (const auto it = mTextureCopies.find(image); it != mTextureCopies.end() && it->second.second >= nativeSize) {
            const bool alreadyNative = std::any_of(mNativeTextures.begin(), mNativeTextures.end(),
                [&](const NativeTexture& registered) { return registered.address == it->second.first; });
            if (alreadyNative) return it->second.first;
        }
        const bool raw = (texture->Flags & TEX_FLAG_LOAD_AS_RAW) != 0;
        const double scaledWidth = raw
                                       ? static_cast<double>(texture->Width) * 32.0 /
                                             (static_cast<double>(bitsPerTexel) * std::max(1.0f, texture->HByteScale))
                                       : static_cast<double>(nativeWidth);
        const uint32_t logicalWidth = static_cast<uint32_t>(std::max(1.0, std::round(scaledWidth)));
        const uint32_t logicalHeight = raw
                                           ? static_cast<uint32_t>(std::max(
                                                 1.0, std::round(static_cast<double>(texture->Height) /
                                                                 std::max(1.0f, texture->VPixelScale))))
                                           : static_cast<uint32_t>(std::max(
                                                 1.0, std::round(static_cast<double>(logicalWidth) * texture->Height /
                                                                 std::max<uint16_t>(1, texture->Width))));
        const uint32_t actualNativeWidth = logicalWidth == 0 ? nativeWidth : logicalWidth;
        const size_t logicalSize = std::max<size_t>(
            nativeSize, (static_cast<uint64_t>(actualNativeWidth) * logicalHeight * bitsPerTexel + 7) / 8);
        const size_t placeholderSize = std::max<size_t>(8, (logicalSize + 3) & ~size_t(3));
        const uint32_t address = Allocate(placeholderSize, 8);
        std::memset(mRdram.data() + address, 0, placeholderSize);
        const uint8_t* rgbaImage = image;
        if (path != nullptr && actualNativeWidth == texture->Width && logicalHeight == texture->Height) {
            if (auto patched = Rt64TexturePatches::Apply(path, image, imageBytes, G_IM_FMT_RGBA, G_IM_SIZ_32b,
                                                        texture->Width, texture->Height)) {
                auto owned = std::make_shared<std::vector<uint8_t>>(image, image + imageBytes);
                std::memcpy(owned->data(), patched->data(), patched->size());
                rgbaImage = owned->data();
                mOwnedNativeTextureData.emplace_back(std::move(owned));
            }
        }
        mNativeTextures.push_back({address, rgbaImage, imageBytes, texture->Width, texture->Height,
                                   actualNativeWidth, logicalHeight, format, sizeCode});
        mTextureCopies[image] = {address, logicalSize};
        return address;
    }
    if (path != nullptr) {
        uint8_t patchFormat = 0;
        uint8_t patchSizeCode = 0;
        bool hasPatchFormat = true;
        switch (texture->Type) {
            case TextureType::RGBA32bpp:
                patchFormat = G_IM_FMT_RGBA;
                patchSizeCode = G_IM_SIZ_32b;
                break;
            case TextureType::RGBA16bpp:
                patchFormat = G_IM_FMT_RGBA;
                patchSizeCode = G_IM_SIZ_16b;
                break;
            case TextureType::Palette4bpp:
                patchFormat = G_IM_FMT_CI;
                patchSizeCode = G_IM_SIZ_4b;
                break;
            case TextureType::Palette8bpp:
                patchFormat = G_IM_FMT_CI;
                patchSizeCode = G_IM_SIZ_8b;
                break;
            case TextureType::Grayscale4bpp:
                patchFormat = G_IM_FMT_I;
                patchSizeCode = G_IM_SIZ_4b;
                break;
            case TextureType::Grayscale8bpp:
                patchFormat = G_IM_FMT_I;
                patchSizeCode = G_IM_SIZ_8b;
                break;
            case TextureType::GrayscaleAlpha4bpp:
                patchFormat = G_IM_FMT_IA;
                patchSizeCode = G_IM_SIZ_4b;
                break;
            case TextureType::GrayscaleAlpha8bpp:
                patchFormat = G_IM_FMT_IA;
                patchSizeCode = G_IM_SIZ_8b;
                break;
            case TextureType::GrayscaleAlpha16bpp:
                patchFormat = G_IM_FMT_IA;
                patchSizeCode = G_IM_SIZ_16b;
                break;
            default:
                hasPatchFormat = false;
                break;
        }
        if (hasPatchFormat) {
            if (auto patched = Rt64TexturePatches::Apply(path, image, imageBytes, patchFormat, patchSizeCode,
                                                        texture->Width, texture->Height)) {
                std::vector<uint8_t> fullPatch(image, image + imageBytes);
                std::memcpy(fullPatch.data(), patched->data(), patched->size());
                const uint8_t* patchedData = fullPatch.data();
                const uint32_t address = CopyTexture(patchedData, fullPatch.size());
                mTextureCopies.erase(patchedData);
                return address;
            }
        }
    }
    return CopyTexture(image, imageBytes);
}

uint32_t DisplayListTranslator::CopyVertices(const void* source, size_t count) {
    if (source == nullptr && count != 0) {
        throw std::runtime_error("Cannot copy a null vertex array");
    }
    const auto* input = static_cast<const uint8_t*>(source);
    const uint32_t address = Allocate(count * sizeof(F3DVtx), 8);
    auto* output = mRdram.data() + address;
    for (size_t vertex = 0; vertex < count; ++vertex) {
        const uint8_t* src = input + vertex * sizeof(F3DVtx);
        uint8_t* dst = output + vertex * sizeof(F3DVtx);
        // RT64 reads word-swapped N64 Vtx data as y,x / flag,z / t,s, then
        // a,b,g,r. LUS stores the same fields natively as x,y / z,flag / s,t
        // and r,g,b,a on little-endian hosts.
        for (size_t group : { size_t(0), size_t(4), size_t(8) }) {
            std::memcpy(dst + group, src + group + 2, 2);
            std::memcpy(dst + group + 2, src + group, 2);
        }
        dst[12] = src[15];
        dst[13] = src[14];
        dst[14] = src[13];
        dst[15] = src[12];
    }
    return address;
}

uint32_t DisplayListTranslator::CopyMatrix(const MtxS& matrix) {
    // Native MtxS and RT64 FixedMatrix both use paired columns in each u32;
    // RT64's accessors apply the column XOR when reading these words.
    return CopyBytes(&matrix, sizeof(matrix), 8);
}

uint32_t DisplayListTranslator::CopyMatrix(const void* source) {
    if (source == nullptr) {
        throw std::runtime_error("Cannot copy a null matrix");
    }
    return CopyMatrix(*static_cast<const MtxS*>(source));
}

uint32_t DisplayListTranslator::CopyMatrixWithReplacement(const void* source) {
    if (source == nullptr) {
        throw std::runtime_error("Cannot copy a null matrix");
    }
    auto* nativeMatrix = const_cast<Mtx*>(static_cast<const Mtx*>(source));
    if (const auto it = mMtxReplacements.find(nativeMatrix); it != mMtxReplacements.end()) {
        return CopyMatrix(ToFixedMatrix(it->second));
    }
    return CopyMatrix(source);
}

uint32_t DisplayListTranslator::ResolveTextureAddress(uintptr_t address, size_t size, uint32_t nativeWidth,
                                                      uint32_t bitsPerTexel, uint8_t format, uint8_t sizeCode) {
    uintptr_t source = address;
    const uint32_t addressSegment = static_cast<uint32_t>((address >> 24) & 0x0f);
    const bool hasSegmentBase = address <= std::numeric_limits<uint32_t>::max() && (address & 1u) != 0 &&
                                mSegments[addressSegment] != 0;
    if (hasSegmentBase) {
        const uint32_t segment = static_cast<uint32_t>((address >> 24) & 0x0f);
        const uint32_t offset = static_cast<uint32_t>(address & 0x00fffffeu);
        const uintptr_t segmentBase = mSegments[segment];
        if (segmentBase > RdramSize) {
            source = segmentBase + offset;
        } else {
            const uint32_t resolved = static_cast<uint32_t>(segmentBase + offset);
            if (static_cast<uint64_t>(resolved) + size > RdramSize) {
                throw std::runtime_error("Segmented texture address is outside RT64 RDRAM");
            }
            return resolved;
        }
    }
    if (source <= RdramSize && static_cast<uint64_t>(source) + size <= RdramSize) {
        return static_cast<uint32_t>(source);
    }
    if (source > std::numeric_limits<uint32_t>::max() ||
        (source > RdramSize && IsReadableNativePointer(source, size))) {
        const char* path = reinterpret_cast<const char*>(source);
        auto context = Ship::Context::GetInstance();
        auto manager = context ? context->GetResourceManager() : nullptr;
        if (manager && std::memcmp(path, "__OTR__", 6) == 0 && manager->OtrSignatureCheck(path)) {
            auto resource = manager->LoadResource(path);
            auto texture = std::dynamic_pointer_cast<Texture>(resource);
            if (!texture || texture->ImageData == nullptr) {
                throw std::runtime_error(std::string("Native G_SETTIMG OTR path does not resolve to texture data: ") + path);
            }
            mRetainedResources.emplace_back(resource);
            return CopyTextureResource(texture, size, nativeWidth, bitsPerTexel, format, sizeCode, path);
        }
        return CopyTexture(reinterpret_cast<const uint8_t*>(source), size);
    }
    throw std::runtime_error("Texture image address is neither translated RDRAM nor a native pointer");
}

uint32_t DisplayListTranslator::CopyMovemem(const void* source, size_t size, uint8_t index) {
    if (source == nullptr && size != 0) {
        throw std::runtime_error("Cannot copy null move-memory data");
    }
    const auto* input = static_cast<const uint8_t*>(source);
    const uint32_t address = Allocate(size, 8);
    auto* output = mRdram.data() + address;
    if (index == F3DEX2_G_MV_VIEWPORT) {
        for (size_t i = 0; i + 3 < size; i += 4) {
            output[i] = input[i + 2];
            output[i + 1] = input[i + 3];
            output[i + 2] = input[i];
            output[i + 3] = input[i + 1];
        }
        if (size % 4 != 0) {
            std::memcpy(output + size - size % 4, input + size - size % 4, size % 4);
        }
    } else {
        // Preserve the directional view even when its unused padding is nonzero.
        // Native positional lights are decoded separately by the RSP at vertex
        // load time, where the geometry mode selects the correct light layout.
        for (size_t i = 0; i < size; ++i) {
            output[i] = input[(i & ~size_t(3)) + (3 - (i & 3))];
        }
    }
    return address;
}

uint32_t DisplayListTranslator::LowerS2dexBackground(const void* source, bool scaled) {
    if (source == nullptr) {
        throw std::runtime_error("S2DEX background command has a null object pointer");
    }
    const auto* native = static_cast<const F3DuObjBg*>(source);
    const F3DuObjBg_t& bg = native->b;
    const uint32_t bitsPerTexel = bg.imageSiz == 0 ? 4 : bg.imageSiz == 1 ? 8 : bg.imageSiz == 2 ? 16 : 32;
    size_t imageBytes = (static_cast<size_t>(bg.imageW) * bg.imageH * bitsPerTexel + 127) / 128;
    imageBytes = std::clamp<size_t>(imageBytes, 8, 1u << 20);
    const uint32_t imageAddress = ResolveTextureAddress(reinterpret_cast<uintptr_t>(bg.imagePtr), imageBytes,
                                                        std::max<uint32_t>(1, bg.imageW >> 2), bitsPerTexel,
                                                        static_cast<uint8_t>(bg.imageFmt),
                                                        static_cast<uint8_t>(bg.imageSiz));

    RtS2dexBgObject packed{};
    if (scaled) {
        const F3DuObjScaleBg_t& scale = native->s;
        packed.scaleBg = { scale.imageW,
                           scale.imageX,
                           scale.frameW,
                           scale.frameX,
                           scale.imageH,
                           scale.imageY,
                           scale.frameH,
                           scale.frameY,
                           imageAddress,
                           scale.imageSiz,
                           scale.imageFmt,
                           scale.imageLoad,
                           scale.imageFlip,
                           scale.imagePal,
                           scale.scaleH,
                           scale.scaleW,
                           scale.imageYorig,
                           {} };
        return CopyBytes(&packed, sizeof(packed), 8);
    }

    packed.bg = { bg.imageW,
                  bg.imageX,
                  bg.frameW,
                  bg.frameX,
                  bg.imageH,
                  bg.imageY,
                  bg.frameH,
                  bg.frameY,
                  imageAddress,
                  bg.imageSiz,
                  bg.imageFmt,
                  bg.imageLoad,
                  bg.imageFlip,
                  bg.imagePal,
                  bg.tmemH,
                  bg.tmemW,
                  bg.tmemLoadTH,
                  bg.tmemLoadSH,
                  bg.tmemSize,
                  bg.tmemSizeW };
    return CopyBytes(&packed, sizeof(packed), 8);
}

uint32_t DisplayListTranslator::ResolveResourcePointer(const void* pointer, size_t size, size_t alignment) {
    if (pointer == nullptr) {
        throw std::runtime_error("Display list refers to a missing resource");
    }
    if (const auto it = mPointerAddresses.find(pointer); it != mPointerAddresses.end()) {
        return it->second;
    }
    const auto address = CopyBytes(pointer, size, alignment);
    mPointerAddresses.emplace(pointer, address);
    return address;
}

uint32_t DisplayListTranslator::ResolveAddress(uintptr_t address, size_t size, size_t alignment) {
    const uint32_t addressSegment = static_cast<uint32_t>((address >> 24) & 0x0f);
    const bool hasSegmentBase = address <= std::numeric_limits<uint32_t>::max() && (address & 1u) != 0 &&
                                mSegments[addressSegment] != 0;
    if (!hasSegmentBase && address > RdramSize && IsReadableNativePointer(address, size)) {
        return ResolveResourcePointer(reinterpret_cast<const void*>(address), size, alignment);
    }
    if (address <= std::numeric_limits<uint32_t>::max() && (address & 1u) != 0) {
        const uint32_t segment = static_cast<uint32_t>((address >> 24) & 0x0f);
        const uint32_t offset = static_cast<uint32_t>(address & 0x00fffffeu);
        const uintptr_t segmentBase = mSegments[segment];
        if (segmentBase > RdramSize) {
            return ResolveResourcePointer(reinterpret_cast<const void*>(segmentBase + offset), size, alignment);
        }
        const uint32_t resolved = static_cast<uint32_t>(segmentBase + offset);
        if (static_cast<uint64_t>(resolved) + size > RdramSize) {
            throw std::runtime_error("Segmented display-list address is outside RT64 RDRAM");
        }
        return resolved;
    }
    if (address <= RdramSize && static_cast<uint64_t>(address) + size <= RdramSize) {
        return static_cast<uint32_t>(address);
    }
    if (address <= std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Unmarked 32-bit display-list address cannot be resolved");
    }
    return ResolveResourcePointer(reinterpret_cast<const void*>(address), size, alignment);
}

uint32_t DisplayListTranslator::LoadHashResource(uint64_t hash, size_t size, size_t alignment) {
    auto manager = Ship::Context::GetInstance()->GetResourceManager();
    auto resource = manager->LoadResource(hash);
    if (!resource) {
        throw std::runtime_error("Unable to load OTR resource hash " + std::to_string(hash));
    }
    mRetainedResources.emplace_back(resource);
    if (auto texture = std::dynamic_pointer_cast<Texture>(resource)) {
        const size_t bytes = texture->ImageDataSize ? texture->ImageDataSize : size;
        const uint32_t address = CopyTexture(texture->ImageData, bytes);
        mPointerAddresses.emplace(texture->ImageData, address);
        return address;
    }
    const void* pointer = nullptr;
    if (auto vertices = std::dynamic_pointer_cast<Vertex>(resource)) {
        pointer = vertices->GetPointer();
    } else if (auto matrix = std::dynamic_pointer_cast<Matrix>(resource)) {
        pointer = matrix->GetPointer();
    } else if (auto displayList = std::dynamic_pointer_cast<DisplayList>(resource)) {
        pointer = displayList->GetPointer();
    }
    if (pointer == nullptr) {
        throw std::runtime_error("OTR resource type cannot be copied into RT64 RDRAM");
    }
    return ResolveResourcePointer(pointer, size ? size : resource->GetPointerSize(), alignment);
}

uint32_t DisplayListTranslator::LoadPathResource(const char* path, size_t size, size_t alignment) {
    if (path == nullptr) {
        throw std::runtime_error("OTR display-list command contains a null resource path");
    }
    auto resource = Ship::Context::GetInstance()->GetResourceManager()->LoadResource(path);
    if (!resource) {
        throw std::runtime_error(std::string("Unable to load OTR resource: ") + path);
    }
    mRetainedResources.emplace_back(resource);
    if (auto texture = std::dynamic_pointer_cast<Texture>(resource)) {
        const size_t bytes = texture->ImageDataSize ? texture->ImageDataSize : size;
        const uint32_t address = CopyTexture(texture->ImageData, bytes);
        mPointerAddresses.emplace(texture->ImageData, address);
        return address;
    }
    const void* pointer = nullptr;
    if (auto vertices = std::dynamic_pointer_cast<Vertex>(resource)) {
        pointer = vertices->GetPointer();
    } else if (auto matrix = std::dynamic_pointer_cast<Matrix>(resource)) {
        pointer = matrix->GetPointer();
    } else if (auto displayList = std::dynamic_pointer_cast<DisplayList>(resource)) {
        pointer = displayList->GetPointer();
    }
    if (pointer == nullptr) {
        throw std::runtime_error("OTR resource type cannot be copied into RT64 RDRAM");
    }
    return ResolveResourcePointer(pointer, size ? size : resource->GetPointerSize(), alignment);
}

DisplayListTranslator::TranslationState DisplayListTranslator::CaptureState() const {
    return {mSegments, mCurrentUcode, mLatestNativeDepthImage, mActiveFramebufferId,
            mCurrentTextureFramebufferId, mInterpolationIndexTarget};
}

void DisplayListTranslator::ApplyState(const TranslationState& state) {
    mSegments = state.segments;
    mCurrentUcode = state.ucode;
    mLatestNativeDepthImage = state.latestDepthImage;
    mActiveFramebufferId = state.activeFramebuffer;
    mCurrentTextureFramebufferId = state.textureFramebuffer;
    mInterpolationIndexTarget = state.interpolationTarget;
}

uint32_t DisplayListTranslator::Translate(Gfx* commands, const std::unordered_map<Mtx*, MtxF>& mtxReplacements,
                                          uint32_t interpolationIndex) {
    if (commands == nullptr) {
        throw std::invalid_argument("Cannot translate a null display list");
    }
    Reset();
    mMtxReplacements = mtxReplacements;
    mInterpolationIndex = interpolationIndex;
    return LowerDisplayList(commands, 0);
}

uint32_t DisplayListTranslator::LowerDisplayList(Gfx* commands, size_t depth) {
    constexpr size_t MaxDepth = 64;
    constexpr size_t MaxCommands = 4096;
    constexpr size_t MaxOutputCommands = 16384;
    constexpr size_t ReservedCommands = MaxOutputCommands + 1;
    if (depth >= MaxDepth) {
        throw std::runtime_error("Display-list nesting exceeds the 64-level translation limit");
    }
    if (!mListsInProgress.insert(commands).second) {
        throw std::runtime_error("Recursive display-list cycle cannot be lowered into RT64 RDRAM");
    }
    const TranslationState inputState = CaptureState();
    if (const auto it = mTranslatedLists.find(commands); it != mTranslatedLists.end()) {
        for (const CachedList& entry : it->second) {
            if (entry.input == inputState) {
                ApplyState(entry.output);
                mListsInProgress.erase(commands);
                return entry.address;
            }
        }
    }

    std::vector<RtCommand> output;
    output.reserve(64);
    bool lastWasFullSync = false;
    bool ended = false;

    auto emit = [&](uint32_t w0, uint32_t w1) {
        if (output.size() >= ReservedCommands) {
            throw std::runtime_error("Translated display list exceeds the 4096-command limit");
        }
        output.push_back({ w0, w1 });
        lastWasFullSync = Opcode(w0) == static_cast<uint8_t>(RDP_G_RDPFULLSYNC);
    };
    auto addFullSync = [&]() {
        if (!lastWasFullSync) {
            emit(static_cast<uint32_t>(RDP_G_RDPFULLSYNC) << 24, 0);
        }
    };

    for (size_t commandIndex = 0; commandIndex < MaxCommands; ++commandIndex) {
        Gfx& source = commands[commandIndex];
        const uint32_t w0 = static_cast<uint32_t>(source.words.w0);
        const uint32_t w1 = static_cast<uint32_t>(source.words.w1);
        const uint32_t opcode = Opcode(w0);
        auto hasSegmentBase = [&](uintptr_t address) {
            return address <= std::numeric_limits<uint32_t>::max() && (address & 1u) != 0 &&
                   mSegments[(address >> 24) & 0x0fu] != 0;
        };
        uint32_t lowerW0 = w0;
        uint32_t lowerW1 = w1;
        bool consumePayload = false;

        if (opcode == static_cast<uint8_t>(F3DEX2_G_ENDDL)) {
            if (depth == 0) {
                addFullSync();
            }
            emit(w0, w1);
            ended = true;
            break;
        }
        if (opcode == static_cast<uint8_t>(F3DEX2_G_LOAD_UCODE)) {
            mCurrentUcode = w0 & 0x00ffffffu;
            // The RT64 runtime intercepts this native command and maps the
            // Shipwright ucode index to its configured F3DEX2/S2DEX2 GBI.
            emit(w0, w1);
            continue;
        }
        if (opcode == OTR_G_DL_OTR_HASH || opcode == OTR_G_DL_OTR_FILEPATH) {
            Gfx* child = nullptr;
            bool byHash = opcode == OTR_G_DL_OTR_HASH;
            if (byHash) {
                if (commandIndex + 1 >= MaxCommands) {
                    throw std::runtime_error("Truncated OTR display-list hash command");
                }
                const uint64_t hash = ReadHash(commands[++commandIndex]);
                auto resource = Ship::Context::GetInstance()->GetResourceManager()->LoadResource(hash);
                auto dl = std::dynamic_pointer_cast<DisplayList>(resource);
                if (!dl) {
                    throw std::runtime_error("OTR display-list hash does not resolve to a display list: " +
                                             std::to_string(hash));
                }
                mRetainedResources.emplace_back(resource);
                child = dl->GetPointer();
                if (child == nullptr) {
                    throw std::runtime_error("OTR display-list resource has no instruction data");
                }
            } else {
                auto resource = Ship::Context::GetInstance()->GetResourceManager()->LoadResource(
                    reinterpret_cast<const char*>(source.words.w1), true);
                auto dl = std::dynamic_pointer_cast<DisplayList>(resource);
                if (!dl) {
                    throw std::runtime_error("OTR display-list path does not resolve to a display list");
                }
                mRetainedResources.emplace_back(resource);
                child = dl->GetPointer();
            }
            const uint32_t childAddress = LowerDisplayList(child, depth + 1);
            lowerW0 = (w0 & 0xfffeffffu) | (Field(w0, 16, 1) << 16);
            lowerW0 = (lowerW0 & 0x00ffffffu) | (static_cast<uint32_t>(F3DEX2_G_DL) << 24);
            lowerW1 = childAddress;
            emit(lowerW0, lowerW1);
            continue;
        }
        if (opcode == static_cast<uint8_t>(F3DEX2_G_DL)) {
            // A segmented or native pointer may directly address a display list.
            // For native pointers, recursively lower it so its embedded pointers
            // and OTR commands are translated as well.
            if (source.words.w1 > RdramSize && !hasSegmentBase(source.words.w1) &&
                IsReadableNativePointer(source.words.w1, sizeof(Gfx))) {
                lowerW1 = LowerDisplayList(reinterpret_cast<Gfx*>(source.words.w1), depth + 1);
            } else if (source.words.w1 <= std::numeric_limits<uint32_t>::max() && (source.words.w1 & 1u) != 0 &&
                       mSegments[(source.words.w1 >> 24) & 0x0f] > RdramSize) {
                const uintptr_t child = mSegments[(source.words.w1 >> 24) & 0x0f] +
                                        (source.words.w1 & 0x00fffffeu);
                lowerW1 = LowerDisplayList(reinterpret_cast<Gfx*>(child), depth + 1);
            } else {
                lowerW1 = ResolveAddress(source.words.w1, sizeof(Gfx), 8);
            }
            const bool noPush = Field(w0, 16, 1) == G_DL_NOPUSH;
            uint32_t dlW0 = (w0 & 0x00ffffffu) | (static_cast<uint32_t>(F3DEX2_G_DL) << 24);
            if (noPush && depth == 0) {
                // A top-level branch never returns to its caller, so turn it
                // into a call and place the synthetic sync after the child.
                dlW0 &= ~(1u << 16);
                emit(dlW0, lowerW1);
                addFullSync();
                emit(static_cast<uint32_t>(static_cast<uint8_t>(F3DEX2_G_ENDDL)) << 24, 0);
                ended = true;
                break;
            }
            emit(dlW0, lowerW1);
            if (noPush) {
                ended = true;
                break;
            }
            continue;
        }
        if (opcode == OTR_G_VTX_OTR_HASH || opcode == OTR_G_MTX_OTR) {
            if (commandIndex + 1 >= MaxCommands) {
                throw std::runtime_error("Truncated expanded OTR display-list command");
            }
            const uint64_t hash = ReadHash(commands[++commandIndex]);
            if (opcode == OTR_G_VTX_OTR_HASH) {
                const uintptr_t offset = source.words.w1;
                const size_t count = Field(w0, 12, 8);
                const void* vertices = nullptr;
                if (offset > 0x000fffff) {
                    // The interpreter emits this form when the first argument
                    // is already a live native vertex pointer.
                    vertices = reinterpret_cast<const void*>(offset);
                } else {
                    auto resource = Ship::Context::GetInstance()->GetResourceManager()->LoadResource(hash);
                    if (!resource || resource->GetRawPointer() == nullptr) {
                        throw std::runtime_error("OTR vertex hash does not resolve: " + std::to_string(hash));
                    }
                    const size_t resourceBytes = resource->GetPointerSize();
                    const size_t vertexBytes = count * sizeof(F3DVtx);
                    if (offset > resourceBytes || vertexBytes > resourceBytes - offset) {
                        throw std::runtime_error("OTR vertex hash range is outside its resource: " +
                                                 std::to_string(hash));
                    }
                    mRetainedResources.emplace_back(resource);
                    vertices = reinterpret_cast<const uint8_t*>(resource->GetRawPointer()) + offset;
                }
                lowerW0 = (static_cast<uint32_t>(F3DEX2_G_VTX) << 24) | (w0 & 0x00ffffffu);
                lowerW1 = CopyVertices(vertices, count);
            } else {
                auto resource = Ship::Context::GetInstance()->GetResourceManager()->LoadResource(hash);
                if (!resource) {
                    throw std::runtime_error("OTR matrix hash does not resolve: " + std::to_string(hash));
                }
                mRetainedResources.emplace_back(resource);
                auto matrixResource = std::dynamic_pointer_cast<Matrix>(resource);
                if (!matrixResource) {
                    throw std::runtime_error("OTR matrix hash does not resolve to a matrix resource");
                }
                const void* matrix = matrixResource->GetPointer();
                lowerW0 = (static_cast<uint32_t>(F3DEX2_G_MTX) << 24) | (w0 & 0x00ffffffu);
                lowerW1 = CopyMatrixWithReplacement(matrix);
            }
            emit(lowerW0, lowerW1);
            continue;
        }
        if (opcode == OTR_G_VTX_OTR_FILEPATH || opcode == OTR_G_MTX_OTR_FILEPATH) {
            const char* path = reinterpret_cast<const char*>(source.words.w1);
            if (opcode == OTR_G_VTX_OTR_FILEPATH) {
                if (commandIndex + 1 >= MaxCommands) {
                    throw std::runtime_error("Truncated OTR vertex filepath command");
                }
                Gfx& payload = commands[++commandIndex];
                const size_t count = static_cast<uint32_t>(payload.words.w0);
                const size_t destination = static_cast<uint32_t>(payload.words.w1) >> 16;
                const size_t sourceOffset = static_cast<uint32_t>(payload.words.w1) & 0xffff;
                auto resource = Ship::Context::GetInstance()->GetResourceManager()->LoadResource(path);
                if (!resource || resource->GetRawPointer() == nullptr) {
                    throw std::runtime_error(std::string("OTR vertex path does not resolve to vertex data: ") + path);
                }
                const size_t vertexBytes = count * sizeof(F3DVtx);
                const size_t sourceByteOffset = sourceOffset * sizeof(F3DVtx);
                const size_t resourceBytes = resource->GetPointerSize();
                if (sourceByteOffset > resourceBytes || vertexBytes > resourceBytes - sourceByteOffset) {
                    throw std::runtime_error(std::string("OTR vertex path range is outside its resource: ") + path);
                }
                mRetainedResources.emplace_back(resource);
                const auto* sourceVertices = reinterpret_cast<const uint8_t*>(resource->GetRawPointer()) +
                                             sourceByteOffset;
                lowerW0 = (static_cast<uint32_t>(F3DEX2_G_VTX) << 24) | ((count & 0xff) << 12) |
                          ((destination + count) & 0x7f) << 1;
                lowerW1 = CopyVertices(sourceVertices, count);
            } else {
                lowerW0 = (static_cast<uint32_t>(F3DEX2_G_MTX) << 24) | (w0 & 0x00ffffffu);
                auto resource = Ship::Context::GetInstance()->GetResourceManager()->LoadResource(path);
                if (!resource || resource->GetRawPointer() == nullptr || resource->GetPointerSize() < sizeof(MtxS)) {
                    throw std::runtime_error(std::string("OTR matrix path does not resolve to matrix data: ") + path);
                }
                mRetainedResources.emplace_back(resource);
                lowerW1 = CopyMatrixWithReplacement(resource->GetRawPointer());
            }
            emit(lowerW0, lowerW1);
            continue;
        }
        if (opcode == OTR_G_SETTIMG_OTR_HASH || opcode == OTR_G_SETTIMG_OTR_FILEPATH) {
            std::shared_ptr<Texture> texture;
            const char* texturePath = nullptr;
            if (opcode == OTR_G_SETTIMG_OTR_HASH) {
                if (commandIndex + 1 >= MaxCommands) {
                    throw std::runtime_error("Truncated OTR texture hash command");
                }
                const uint64_t hash = ReadHash(commands[++commandIndex]);
                auto manager = Ship::Context::GetInstance()->GetResourceManager();
                texturePath = manager->GetArchiveManager()->HashToCString(hash);
                auto resource = manager->LoadResource(hash);
                texture = std::dynamic_pointer_cast<Texture>(resource);
                if (!texture) {
                    throw std::runtime_error("OTR image hash does not resolve to a texture: " + std::to_string(hash));
                }
                mRetainedResources.emplace_back(resource);
            } else {
                texturePath = reinterpret_cast<const char*>(source.words.w1);
                auto resource = Ship::Context::GetInstance()->GetResourceManager()->LoadResource(
                    texturePath, true);
                texture = std::dynamic_pointer_cast<Texture>(resource);
                if (!texture) {
                    throw std::runtime_error("OTR image path does not resolve to a texture");
                }
                mRetainedResources.emplace_back(resource);
            }
            lowerW0 = (w0 & 0x00ffffffu) | (static_cast<uint32_t>(RDP_G_SETTIMG) << 24);
            const uint32_t nativeWidth = Field(w0, 0, 12) + 1;
            const uint32_t sizeCode = Field(w0, 19, 2);
            const uint32_t bitsPerTexel = sizeCode == 0 ? 4 : sizeCode == 1 ? 8 : sizeCode == 2 ? 16 : 32;
            const uint32_t nativeHeight = texture->Width == 0
                                              ? 1
                                              : std::max<uint32_t>(1, texture->Height * nativeWidth / texture->Width);
            const size_t nativeSize = (static_cast<uint64_t>(nativeWidth) * nativeHeight * bitsPerTexel + 7) / 8;
            lowerW1 = CopyTextureResource(texture, nativeSize, nativeWidth, bitsPerTexel,
                                          static_cast<uint8_t>(Field(w0, 21, 3)),
                                          static_cast<uint8_t>(sizeCode), texturePath);
            emit(lowerW0, lowerW1);
            continue;
        }
        if (opcode == OTR_G_TRI1_OTR) {
            if (w1 != 0) {
                // XML-exported OTR triangles use a compact custom layout:
                // v0 is in w0's low byte and v1/v2 are in w1's high/low byte.
                // Expand those raw indices into F3DEX2's three 7-bit slots.
                const uint32_t v0 = w0 & 0x7fu;
                const uint32_t v1 = (w1 >> 16) & 0x7fu;
                const uint32_t v2 = w1 & 0x7fu;
                lowerW0 = (static_cast<uint32_t>(F3DEX2_G_TRI1) << 24) | (v0 << 17) | (v1 << 9) | (v2 << 1);
                emit(lowerW0, 0);
            } else {
                // The native gsSP1TriangleOTR helper already packs the
                // F3DEX2 index slots into w0's low 24 bits.
                lowerW0 = (static_cast<uint32_t>(F3DEX2_G_TRI1) << 24) | (w0 & 0x00ffffffu);
                emit(lowerW0, 0);
            }
            continue;
        }
        if (opcode == OTR_G_BRANCH_Z_OTR) {
            if (commandIndex + 1 >= MaxCommands) {
                throw std::runtime_error("Truncated OTR branch-Z resource hash");
            }
            const uint64_t hash = ReadHash(commands[++commandIndex]);
            auto resource = Ship::Context::GetInstance()->GetResourceManager()->LoadResource(hash);
            auto displayList = std::dynamic_pointer_cast<DisplayList>(resource);
            if (!displayList || displayList->GetPointer() == nullptr) {
                throw std::runtime_error("OTR branch-Z hash does not resolve to a display list");
            }
            mRetainedResources.emplace_back(resource);
            const uint32_t branchAddress = LowerDisplayList(displayList->GetPointer(), depth + 1);
            emit(static_cast<uint32_t>(static_cast<uint8_t>(F3DEX2_G_RDPHALF_1)) << 24, branchAddress);
            emit((static_cast<uint32_t>(static_cast<uint8_t>(F3DEX2_G_BRANCH_Z)) << 24) | (w0 & 0x00000fffu), w1);
            continue;
        }
        if (opcode == OTR_G_PUSHCD) {
            // The legacy directory stack is debug metadata; resources in
            // these display lists carry complete paths or archive hashes.
            emit(static_cast<uint32_t>(static_cast<uint8_t>(F3DEX2_G_SPNOOP)) << 24, 0);
            continue;
        }
        if (opcode == OTR_G_MOVEMEM_HASH) {
            if (commandIndex + 1 >= MaxCommands) {
                throw std::runtime_error("Truncated OTR move-memory resource hash");
            }
            const uint8_t index = static_cast<uint8_t>(w1 >> 24);
            const uint8_t byteOffset = static_cast<uint8_t>(w1 >> 16);
            const uint8_t hasOffset = static_cast<uint8_t>(w1 >> 8);
            (void)hasOffset; // Only the legacy non-F3DEX2 handler uses this bit.
            const uint64_t hash = ReadHash(commands[++commandIndex]);
            auto resource = Ship::Context::GetInstance()->GetResourceManager()->LoadResource(hash);
            if (!resource || resource->GetRawPointer() == nullptr) {
                throw std::runtime_error("OTR move-memory hash does not resolve to data: " + std::to_string(hash));
            }
            mRetainedResources.emplace_back(resource);

            // Legacy F3DEX2 ignores hasOffset and feeds the resource bytes to
            // GfxSpMovememF3dex2. Its supported blocks are a 16-byte viewport
            // or 24-byte LightEntry. Index zero and unknown indices are ignored
            // by that handler, so consume their hash packet as an SP no-op.
            if (index == 0 || (index != static_cast<uint8_t>(F3DEX2_G_MV_VIEWPORT) &&
                               index != static_cast<uint8_t>(F3DEX2_G_MV_LIGHT))) {
                emit(static_cast<uint32_t>(static_cast<uint8_t>(F3DEX2_G_SPNOOP)) << 24, 0);
                continue;
            }
            const size_t moveSize = resource->GetPointerSize();
            if (moveSize == 0 || moveSize > 256 || (moveSize & 7u) != 0) {
                throw std::runtime_error("OTR move-memory resource has an unsupported byte size: " +
                                         std::to_string(moveSize));
            }
            if ((byteOffset & 7u) != 0) {
                throw std::runtime_error("OTR move-memory offset is not aligned to 8 bytes");
            }
            const uint32_t dataAddress = CopyMovemem(resource->GetRawPointer(), moveSize, index);
            lowerW0 = (static_cast<uint32_t>(static_cast<uint8_t>(F3DEX2_G_MOVEMEM)) << 24) |
                      (static_cast<uint32_t>((moveSize - 1) / 8) << 19) |
                      (static_cast<uint32_t>(byteOffset / 8) << 8) | index;
            lowerW1 = dataAddress;
            emit(lowerW0, lowerW1);
            continue;
        }
        if (opcode == OTR_G_MARKER) {
            // OTR display-list files encode the marker as a 128-bit packet;
            // its second Gfx contains metadata consumed by the legacy marker
            // handler and has no meaning to RT64.
            if (commandIndex + 1 >= MaxCommands) {
                throw std::runtime_error("Truncated OTR marker payload");
            }
            ++commandIndex;
            emit(static_cast<uint32_t>(static_cast<uint8_t>(F3DEX2_G_SPNOOP)) << 24, 0);
            continue;
        }
        if (opcode == OTR_G_TEXRECT_WIDE) {
            if (commandIndex + 2 >= MaxCommands) {
                throw std::runtime_error("Truncated wide texture-rectangle command");
            }
            const Gfx& endpoints = commands[++commandIndex];
            const Gfx& textureStep = commands[++commandIndex];
            const uint32_t endpointW0 = static_cast<uint32_t>(endpoints.words.w0);
            const uint32_t endpointW1 = static_cast<uint32_t>(endpoints.words.w1);
            const uint32_t stepW0 = static_cast<uint32_t>(textureStep.words.w0);
            const uint32_t stepW1 = static_cast<uint32_t>(textureStep.words.w1);
            NativeWideRectangle rectangle{};
            rectangle.ulx = SignExtend24(endpointW0);
            rectangle.uly = SignExtend24(endpointW1);
            rectangle.lrx = SignExtend24(w0);
            rectangle.lry = SignExtend24(w1);
            rectangle.uls = static_cast<int16_t>(stepW0 >> 16);
            rectangle.ult = static_cast<int16_t>(stepW0);
            rectangle.dsdx = static_cast<int16_t>(stepW1 >> 16);
            rectangle.dtdy = static_cast<int16_t>(stepW1);
            rectangle.tile = static_cast<uint8_t>(Field(endpointW0, 24, 3));
            const uint32_t rectangleIndex = static_cast<uint32_t>(mNativeWideRectangles.size());
            mNativeWideRectangles.emplace_back(rectangle);
            emit(static_cast<uint32_t>(NativeWideRectangleOpcode) << 24, rectangleIndex);
            continue;
        }
        if (opcode == OTR_G_FILLWIDERECT) {
            if (commandIndex + 1 >= MaxCommands) {
                throw std::runtime_error("Truncated wide fill-rectangle command");
            }
            const Gfx& upperLeft = commands[++commandIndex];
            NativeWideRectangle rectangle{};
            rectangle.ulx = SignExtend24(static_cast<uint32_t>(upperLeft.words.w0));
            rectangle.uly = SignExtend24(static_cast<uint32_t>(upperLeft.words.w1));
            rectangle.lrx = SignExtend24(w0);
            rectangle.lry = SignExtend24(w1);
            rectangle.fill = true;
            const uint32_t rectangleIndex = static_cast<uint32_t>(mNativeWideRectangles.size());
            mNativeWideRectangles.emplace_back(rectangle);
            emit(static_cast<uint32_t>(NativeWideRectangleOpcode) << 24, rectangleIndex);
            continue;
        }
        if (opcode == OTR_G_SETTIMG_FB) {
            const int framebufferId = static_cast<int>(source.words.w1 & 0xffu);
            const auto framebuffer = GetFramebuffer(framebufferId);
            if (!framebuffer) {
                throw std::runtime_error("G_SETTIMG_FB references an unregistered native framebuffer");
            }
            FramebufferOperation operation{};
            const bool followedByImageRectangle = commandIndex + 1 < MaxCommands &&
                Opcode(static_cast<uint32_t>(commands[commandIndex + 1].words.w0)) == OTR_G_IMAGERECT;
            operation.type = followedByImageRectangle ? FramebufferOperation::Type::Flush
                                                      : FramebufferOperation::Type::BindTexture;
            operation.sourceId = framebufferId;
            if (!followedByImageRectangle) {
                operation.width = Field(w0, 0, 12) + 1;
                operation.format = static_cast<uint8_t>(Field(w0, 21, 3));
                operation.sizeCode = static_cast<uint8_t>(Field(w0, 19, 2));
                operation.tile = G_TX_RENDERTILE;
            }
            const uint32_t operationIndex = static_cast<uint32_t>(mFramebufferOperations.size());
            mFramebufferOperations.emplace_back(operation);
            emit(static_cast<uint32_t>(NativeFramebufferOperationOpcode) << 24, operationIndex);
            mCurrentTextureFramebufferId = framebufferId;
            continue;
        }
        if (opcode == OTR_G_SETFB) {
            const int framebufferId = static_cast<int>(source.words.w1 & 0xffu);
            const auto framebuffer = GetFramebuffer(framebufferId);
            if (!framebuffer) {
                throw std::runtime_error("G_SETFB references an unregistered native framebuffer");
            }
            FramebufferOperation operation{};
            operation.type = FramebufferOperation::Type::Invalidate;
            operation.sourceId = framebufferId;
            const uint32_t operationIndex = static_cast<uint32_t>(mFramebufferOperations.size());
            mFramebufferOperations.emplace_back(operation);
            emit(static_cast<uint32_t>(NativeFramebufferOperationOpcode) << 24, operationIndex);
            mActiveFramebufferId = framebufferId;
            const uint32_t cimgW0 = (static_cast<uint32_t>(static_cast<uint8_t>(RDP_G_SETCIMG)) << 24) |
                                     (2u << 19) | ((framebuffer->width - 1) & 0xfffu);
            emit(cimgW0, framebuffer->address);
            emit(static_cast<uint32_t>(static_cast<uint8_t>(RDP_G_SETZIMG)) << 24, DepthAddress);
            continue;
        }
        if (opcode == OTR_G_RESETFB) {
            if (mActiveFramebufferId != 0) {
                FramebufferOperation operation{};
                operation.type = FramebufferOperation::Type::Flush;
                operation.sourceId = mActiveFramebufferId;
                const uint32_t operationIndex = static_cast<uint32_t>(mFramebufferOperations.size());
                mFramebufferOperations.emplace_back(operation);
                emit(static_cast<uint32_t>(NativeFramebufferOperationOpcode) << 24, operationIndex);
            }
            mActiveFramebufferId = 0;
            const auto mainFramebuffer = GetFramebuffer(0).value();
            const uint32_t cimgW0 = (static_cast<uint32_t>(static_cast<uint8_t>(RDP_G_SETCIMG)) << 24) |
                                     (2u << 19) | ((mainFramebuffer.width - 1) & 0xfffu);
            emit(cimgW0, mainFramebuffer.address);
            emit(static_cast<uint32_t>(static_cast<uint8_t>(RDP_G_SETZIMG)) << 24, DepthAddress);
            continue;
        }
        if (opcode == OTR_G_COPYFB) {
            FramebufferOperation operation{};
            operation.type = FramebufferOperation::Type::Copy;
            operation.destinationId = static_cast<int>((w0 >> 11) & 0x7ffu);
            operation.sourceId = static_cast<int>(w0 & 0x7ffu);
            operation.oncePerFrame = ((w0 >> 22) & 1u) != 0;
            operation.copiedFlag = static_cast<uintptr_t>(source.words.w1);
            if (!GetFramebuffer(operation.sourceId) || !GetFramebuffer(operation.destinationId)) {
                throw std::runtime_error("G_COPYFB references an unregistered native framebuffer");
            }
            mFramebufferOperations.push_back(operation);
            emit(static_cast<uint32_t>(NativeFramebufferOperationOpcode) << 24,
                 static_cast<uint32_t>(mFramebufferOperations.size() - 1));
            continue;
        }
        if (opcode == OTR_G_READFB) {
            if (commandIndex + 1 >= MaxCommands) {
                throw std::runtime_error("Truncated G_READFB command");
            }
            const Gfx& extent = commands[++commandIndex];
            FramebufferOperation operation{};
            operation.type = FramebufferOperation::Type::Read;
            operation.sourceId = static_cast<int>(w0 & 0xffu);
            operation.destination = static_cast<uintptr_t>(source.words.w1);
            operation.x = static_cast<uint16_t>(extent.words.w0);
            operation.y = static_cast<uint16_t>(static_cast<uint32_t>(extent.words.w0) >> 16);
            operation.width = static_cast<uint16_t>(extent.words.w1);
            operation.height = static_cast<uint16_t>(static_cast<uint32_t>(extent.words.w1) >> 16);
            operation.byteSwap = ((w0 >> 8) & 1u) != 0;
            if (!GetFramebuffer(operation.sourceId)) {
                throw std::runtime_error("G_READFB references an unregistered native framebuffer");
            }
            mFramebufferOperations.push_back(operation);
            emit(static_cast<uint32_t>(NativeFramebufferOperationOpcode) << 24,
                 static_cast<uint32_t>(mFramebufferOperations.size() - 1));
            continue;
        }
        if (opcode == OTR_G_IMAGERECT) {
            if (commandIndex + 2 >= MaxCommands) {
                throw std::runtime_error("Truncated G_IMAGERECT command");
            }
            const Gfx& first = commands[++commandIndex];
            const Gfx& second = commands[++commandIndex];
            const uint32_t tile = w0 & 7u;
            const uint32_t width = static_cast<uint16_t>(static_cast<uint32_t>(source.words.w1) >> 16);
            const uint32_t height = static_cast<uint16_t>(source.words.w1);
            const int32_t ulx = static_cast<int16_t>(static_cast<uint32_t>(first.words.w0) >> 16);
            const int32_t uly = static_cast<int16_t>(first.words.w0);
            const int32_t lrx = static_cast<int16_t>(static_cast<uint32_t>(second.words.w0) >> 16);
            const int32_t lry = static_cast<int16_t>(second.words.w0);
            const int32_t s0 = static_cast<int16_t>(static_cast<uint32_t>(first.words.w1) >> 16);
            const int32_t t0 = static_cast<int16_t>(first.words.w1);
            const int32_t s1 = static_cast<int16_t>(static_cast<uint32_t>(second.words.w1) >> 16);
            const int32_t t1 = static_cast<int16_t>(second.words.w1);
            const int32_t dx = lrx - ulx;
            const int32_t dy = lry - uly;
            if (width == 0 || height == 0 || dx <= 0 || dy <= 0) {
                throw std::runtime_error("G_IMAGERECT has empty image or rectangle dimensions");
            }
            if (mCurrentTextureFramebufferId < 0) {
                throw std::runtime_error("G_IMAGERECT has no preceding G_SETTIMG_FB framebuffer source");
            }
            const auto framebuffer = GetFramebuffer(mCurrentTextureFramebufferId);
            if (!framebuffer) {
                throw std::runtime_error("G_IMAGERECT framebuffer source was removed during translation");
            }
            const uint32_t sourceWidth = framebuffer->width;
            const uint32_t sourceHeight = framebuffer->height;
            const uint32_t tileWidthLimit = std::min<uint32_t>(1024, sourceWidth);
            const uint32_t lineBytes = tileWidthLimit * 2;
            const uint32_t rowsPerLoad = std::max<uint32_t>(1, 4096 / lineBytes);
            const uint32_t loadTile = 7;
            const int32_t deltaT = t1 - t0;
            const int32_t deltaS = s1 - s0;
            if (deltaT <= 0 || deltaS <= 0 || s0 < 0 || s1 > static_cast<int32_t>(sourceWidth) || t0 < 0 ||
                t1 > static_cast<int32_t>(sourceHeight)) {
                throw std::runtime_error("G_IMAGERECT framebuffer source coordinates are outside positive image bounds");
            }
            const int32_t dsdx = static_cast<int32_t>((static_cast<int64_t>(s1 - s0) * 4096) / dx);
            const int32_t dtdy = static_cast<int32_t>((static_cast<int64_t>(deltaT) * 4096) / dy);
            // Native framebuffer images are not limited by TMEM. One image
            // preserves the full-resolution copy and avoids seams between strips.
            if (sourceWidth <= 1024 && sourceHeight <= 1024) {
                const uint32_t tileW0 = (static_cast<uint32_t>(static_cast<uint8_t>(RDP_G_SETTILE)) << 24) |
                                       (G_IM_SIZ_16b << 19) | (((sourceWidth * 2 + 7) / 8) << 9);
                emit(tileW0, (tile << 24) | (G_TX_CLAMP << 18) | (G_TX_CLAMP << 8));
                emit(static_cast<uint32_t>(static_cast<uint8_t>(RDP_G_SETTILESIZE)) << 24,
                     (tile << 24) | ((sourceWidth - 1) * 4 << 12) | ((sourceHeight - 1) * 4));
                FramebufferOperation operation{};
                operation.type = FramebufferOperation::Type::BindTexture;
                operation.sourceId = mCurrentTextureFramebufferId;
                operation.width = sourceWidth;
                operation.height = sourceHeight;
                operation.format = G_IM_FMT_RGBA;
                operation.sizeCode = G_IM_SIZ_16b;
                operation.tile = tile;
                mFramebufferOperations.emplace_back(operation);
                emit(static_cast<uint32_t>(NativeFramebufferOperationOpcode) << 24,
                     static_cast<uint32_t>(mFramebufferOperations.size() - 1));
                NativeWideRectangle rectangle{};
                rectangle.ulx = ulx; rectangle.uly = uly;
                rectangle.lrx = lrx; rectangle.lry = lry;
                rectangle.uls = static_cast<int16_t>(s0 * 32);
                rectangle.ult = static_cast<int16_t>(t0 * 32);
                rectangle.dsdx = static_cast<int16_t>(dsdx);
                rectangle.dtdy = static_cast<int16_t>(dtdy);
                rectangle.tile = tile;
                mNativeWideRectangles.emplace_back(rectangle);
                emit(static_cast<uint32_t>(NativeWideRectangleOpcode) << 24,
                     static_cast<uint32_t>(mNativeWideRectangles.size() - 1));
                continue;
            }
            const uint32_t imageW0 = (static_cast<uint32_t>(static_cast<uint8_t>(RDP_G_SETTIMG)) << 24) |
                                     (2u << 19) | ((sourceWidth - 1) & 0xfffu);
            for (uint32_t row = 0; row < sourceHeight; row += rowsPerLoad) {
                const uint32_t rows = std::min(rowsPerLoad, sourceHeight - row);
                const int32_t firstT = std::max<int32_t>(t0, static_cast<int32_t>(row));
                const int32_t lastT = std::min<int32_t>(t1, static_cast<int32_t>(row + rows));
                if (firstT >= lastT) {
                    continue;
                }
                const int32_t y0 = std::clamp<int32_t>(
                    uly + static_cast<int32_t>((static_cast<int64_t>(firstT - t0) * dy) / deltaT), uly, lry);
                const int32_t y1 = std::clamp<int32_t>(
                    uly + static_cast<int32_t>((static_cast<int64_t>(lastT - t0) * dy) / deltaT), uly, lry);
                if (y0 >= y1) {
                    continue;
                }
                for (uint32_t column = 0; column < sourceWidth; column += tileWidthLimit) {
                    const uint32_t columns = std::min(tileWidthLimit, sourceWidth - column);
                    const int32_t firstS = std::max<int32_t>(s0, static_cast<int32_t>(column));
                    const int32_t lastS = std::min<int32_t>(s1, static_cast<int32_t>(column + columns));
                    if (firstS >= lastS) {
                        continue;
                    }
                    const int32_t x0 = std::clamp<int32_t>(
                        ulx + static_cast<int32_t>((static_cast<int64_t>(firstS - s0) * dx) / deltaS), ulx, lrx);
                    const int32_t x1 = std::clamp<int32_t>(
                        ulx + static_cast<int32_t>((static_cast<int64_t>(lastS - s0) * dx) / deltaS), ulx, lrx);
                    if (x0 >= x1) {
                        continue;
                    }
                    const uint32_t line = (columns * 2 + 7) / 8;
                    const uint32_t loadTileW0 = (static_cast<uint32_t>(static_cast<uint8_t>(RDP_G_SETTILE)) << 24) |
                                                (2u << 19) | ((line & 0x1ffu) << 9);
                    const uint32_t imageOffset = row * sourceWidth * 2 + column * 2;
                    const uint32_t imageAddress = framebuffer->address + imageOffset;
                    const uint32_t tileExtentW = ((columns - 1) * 4) << 12;
                    const uint32_t tileExtentH = (rows - 1) * 4;
                    emit(imageW0, imageAddress);
                    emit(loadTileW0, loadTile << 24);
                    emit(static_cast<uint32_t>(static_cast<uint8_t>(RDP_G_LOADTILE)) << 24,
                         (loadTile << 24) | tileExtentW | tileExtentH);
                    emit(loadTileW0, tile << 24);
                    emit(static_cast<uint32_t>(static_cast<uint8_t>(RDP_G_SETTILESIZE)) << 24,
                         (tile << 24) | tileExtentW | tileExtentH);
                    const int32_t uls = (firstS - static_cast<int32_t>(column)) << 5;
                    const int32_t ult = (firstT - static_cast<int32_t>(row)) << 5;
                    if (x0 < 0 || y0 < 0 || x1 > 0xfff || y1 > 0xfff) {
                        NativeWideRectangle rectangle{};
                        rectangle.ulx = x0;
                        rectangle.uly = y0;
                        rectangle.lrx = x1;
                        rectangle.lry = y1;
                        rectangle.uls = static_cast<int16_t>(uls);
                        rectangle.ult = static_cast<int16_t>(ult);
                        rectangle.dsdx = static_cast<int16_t>(dsdx);
                        rectangle.dtdy = static_cast<int16_t>(dtdy);
                        rectangle.tile = static_cast<uint8_t>(tile);
                        const uint32_t rectangleIndex = static_cast<uint32_t>(mNativeWideRectangles.size());
                        mNativeWideRectangles.emplace_back(rectangle);
                        emit(static_cast<uint32_t>(NativeWideRectangleOpcode) << 24, rectangleIndex);
                    } else {
                        emit((static_cast<uint32_t>(static_cast<uint8_t>(RDP_G_TEXRECT)) << 24) |
                                 (static_cast<uint32_t>(x1) << 12) | static_cast<uint32_t>(y1),
                             (tile << 24) | (static_cast<uint32_t>(x0) << 12) |
                                 static_cast<uint32_t>(y0));
                        emit(static_cast<uint32_t>(static_cast<uint8_t>(F3DEX2_G_RDPHALF_1)) << 24,
                             (static_cast<uint32_t>(uls) << 16) | static_cast<uint16_t>(ult));
                        emit(static_cast<uint32_t>(static_cast<uint8_t>(F3DEX2_G_RDPHALF_2)) << 24,
                             (static_cast<uint32_t>(dsdx) << 16) | static_cast<uint16_t>(dtdy));
                    }
                }
            }
            continue;
        }
        if (opcode == OTR_G_SETGRAYSCALE) {
            emit(w0, w1);
            continue;
        }
        if (opcode == OTR_G_SETINTENSITY) {
            emit(w0, w1);
            continue;
        }
        if (opcode == static_cast<uint8_t>(RDP_G_SETTARGETINTERPINDEX)) {
            mInterpolationIndexTarget = static_cast<uint32_t>(source.words.w1);
            emit(static_cast<uint32_t>(static_cast<uint8_t>(F3DEX2_G_SPNOOP)) << 24, 0);
            continue;
        }
        if (opcode == static_cast<uint8_t>(RDP_G_SETTILESIZE_INTERP)) {
            if (commandIndex + 2 >= MaxCommands) {
                throw std::runtime_error("Truncated interpolated tile-size command");
            }
            const Gfx& first = commands[++commandIndex];
            const Gfx& second = commands[++commandIndex];
            if (mInterpolationIndex == mInterpolationIndexTarget) {
                auto floatWord = [](uintptr_t bits) {
                    float value = 0.0f;
                    const uint32_t word = static_cast<uint32_t>(bits);
                    std::memcpy(&value, &word, sizeof(value));
                    if (!std::isfinite(value)) {
                        throw std::runtime_error("Interpolated tile size contains a non-finite coordinate");
                    }
                    double quantized = std::fmod(std::round(static_cast<double>(value) * 4.0), 4096.0);
                    if (quantized < 0.0) {
                        quantized += 4096.0;
                    }
                    return static_cast<uint32_t>(quantized) & 0x0fffu;
                };
                const uint32_t uls = floatWord(first.words.w0);
                const uint32_t ult = floatWord(first.words.w1);
                const uint32_t lrs = floatWord(second.words.w0);
                const uint32_t lrt = floatWord(second.words.w1);
                const uint32_t tile = Field(w1, 24, 3);
                emit((static_cast<uint32_t>(static_cast<uint8_t>(RDP_G_SETTILESIZE)) << 24) | (uls << 12) | ult,
                     (tile << 24) | (lrs << 12) | lrt);
            }
            continue;
        }
        if (opcode == OTR_G_EXTRAGEOMETRYMODE) {
            emit(w0, w1);
            continue;
        }
        if (opcode == OTR_G_INVALTEXCACHE) {
            // Translated textures receive fresh RDRAM addresses and are uploaded
            // by RT64's normal tile-load path, so the native cache key is not
            // meaningful after lowering.
            emit(static_cast<uint32_t>(static_cast<uint8_t>(F3DEX2_G_SPNOOP)) << 24, 0);
            continue;
        }
        if (mCurrentUcode == ucode_s2dex) {
            if (opcode == static_cast<uint8_t>(F3DEX2_G_BG_1CYC) || opcode == static_cast<uint8_t>(F3DEX2_G_BG_COPY)) {
                const uint32_t objectAddress = LowerS2dexBackground(reinterpret_cast<const void*>(source.words.w1),
                                                                    opcode == static_cast<uint8_t>(F3DEX2_G_BG_1CYC));
                emit(w0, objectAddress);
                continue;
            }
            if (opcode == static_cast<uint8_t>(F3DEX2_G_OBJ_RENDERMODE)) {
                emit(w0, w1);
                continue;
            }
            if (opcode < 0x0c) {
                throw std::runtime_error("Unsupported S2DEX object opcode 0x" + std::to_string(opcode));
            }
        }
        switch (opcode) {
        case OTR_G_SETFB: {
            case OTR_G_RESETFB:
            case OTR_G_PUSHCD:
            case OTR_G_MARKER:
            case OTR_G_INVALTEXCACHE:
            case OTR_G_SETGRAYSCALE:
            case OTR_G_EXTRAGEOMETRYMODE:
            case OTR_G_COPYFB:
            case OTR_G_IMAGERECT:
            case OTR_G_DL_INDEX:
            case OTR_G_READFB:
            case OTR_G_REGBLENDEDTEX:
            case OTR_G_MOVEMEM_HASH:
            case OTR_G_LOAD_SHADER:
                std::ostringstream message;
                message << "Unsupported Shipwright OTR display-list opcode 0x" << std::hex << opcode << " (w0=0x"
                        << w0 << ')';
                throw std::runtime_error(message.str());
            }
            default:
                break;
        }

        if (opcode == static_cast<uint8_t>(F3DEX2_G_VTX)) {
            const size_t count = Field(w0, 12, 8);
            if (source.words.w1 > RdramSize && !hasSegmentBase(source.words.w1) &&
                IsReadableNativePointer(source.words.w1, count * sizeof(F3DVtx))) {
                lowerW1 = CopyVertices(reinterpret_cast<const void*>(source.words.w1), count);
            } else if ((source.words.w1 & 1u) != 0 &&
                       mSegments[(source.words.w1 >> 24) & 0x0f] > RdramSize) {
                const uintptr_t vertices = mSegments[(source.words.w1 >> 24) & 0x0f] +
                                           (source.words.w1 & 0x00fffffeu);
                lowerW1 = CopyVertices(reinterpret_cast<const void*>(vertices), count);
            } else {
                lowerW1 = ResolveAddress(source.words.w1, count * sizeof(F3DVtx), 8);
            }
        } else if (opcode == static_cast<uint8_t>(F3DEX2_G_MTX)) {
            auto* nativeMatrix = reinterpret_cast<Mtx*>(source.words.w1);
            if (const auto it = mMtxReplacements.find(nativeMatrix); it != mMtxReplacements.end()) {
                const MtxS replacement = ToFixedMatrix(it->second);
                lowerW1 = CopyMatrix(replacement);
            } else {
                if (source.words.w1 > RdramSize && !hasSegmentBase(source.words.w1) &&
                    IsReadableNativePointer(source.words.w1, sizeof(MtxS))) {
                    lowerW1 = CopyMatrix(reinterpret_cast<const void*>(source.words.w1));
                } else if ((source.words.w1 & 1u) != 0 &&
                           mSegments[(source.words.w1 >> 24) & 0x0f] > RdramSize) {
                    const uintptr_t matrix = mSegments[(source.words.w1 >> 24) & 0x0f] +
                                             (source.words.w1 & 0x00fffffeu);
                    lowerW1 = CopyMatrix(reinterpret_cast<const void*>(matrix));
                } else {
                    lowerW1 = ResolveAddress(source.words.w1, sizeof(MtxS), 8);
                }
            }
        } else if (opcode == static_cast<uint8_t>(F3DEX2_G_MOVEMEM)) {
            // Viewport and light data are 16-byte aligned N64 structures. The
            // descriptor encodes their exact byte count for F3DEX2.
            const size_t length = (Field(w0, 19, 5) + 1) * 8;
            const uint8_t index = Field(w0, 0, 8);
            if (index != F3DEX2_G_MV_VIEWPORT && index != F3DEX2_G_MV_LIGHT) {
                // Ship's interpreter ignores other move-memory indices,
                // including the empty index-zero packet in stock assets.
                emit(static_cast<uint32_t>(static_cast<uint8_t>(F3DEX2_G_SPNOOP)) << 24, 0);
                continue;
            }
            uintptr_t nativeData = source.words.w1;
            if (source.words.w1 <= std::numeric_limits<uint32_t>::max() && (source.words.w1 & 1u) != 0 &&
                mSegments[(source.words.w1 >> 24) & 0x0f] > RdramSize) {
                nativeData = mSegments[(source.words.w1 >> 24) & 0x0f] + (source.words.w1 & 0x00fffffeu);
            }
            if (nativeData > RdramSize && !hasSegmentBase(nativeData) && IsReadableNativePointer(nativeData, length) &&
                (index == F3DEX2_G_MV_VIEWPORT || index == F3DEX2_G_MV_LIGHT)) {
                lowerW1 = CopyMovemem(reinterpret_cast<const void*>(nativeData), length, index);
            } else {
                lowerW1 = ResolveAddress(source.words.w1, length, 8);
            }
        } else if (opcode == static_cast<uint8_t>(F3DEX2_G_MOVEWORD) &&
                   Field(w0, 16, 8) == G_MW_SEGMENT_INTERP) {
            const uint32_t offset = Field(w0, 0, 16);
            const uint32_t segment = offset % 16;
            const uint32_t interpolationTarget = offset / 16;
            if (interpolationTarget == mInterpolationIndex) {
                mSegments[segment] = source.words.w1;
            }
            // RT64's GBI has no handler for this OTR interpolation command;
            // resolve the selected base here for later tagged addresses.
            lowerW0 = static_cast<uint32_t>(static_cast<uint8_t>(F3DEX2_G_SPNOOP)) << 24;
            lowerW1 = 0;
        } else if (opcode == static_cast<uint8_t>(F3DEX2_G_MOVEWORD) && Field(w0, 16, 8) == G_MW_SEGMENT) {
            const uint32_t segment = (Field(w0, 0, 16) >> 2) & 0x0f;
            const uintptr_t base = source.words.w1;
            mSegments[segment] = base;
            lowerW0 = static_cast<uint32_t>(F3DEX2_G_SPNOOP) << 24;
            lowerW1 = 0;
        } else if (opcode == static_cast<uint8_t>(RDP_G_LOADBLOCK)) {
            // CALC_DXT in the legacy macros floors width to 64-bit words.
            // For widths such as 46 IA8 texels it produces a five-word DXT,
            // while the render tile line is rounded up to six words. RT64
            // uses DXT to advance/swap TMEM rows, so normalize only the
            // recognizable DPLoadTextureBlock sequence where its render-tile
            // line and size are available immediately afterward.
            bool pipeSyncSeen = false;
            for (size_t lookahead = commandIndex + 1;
                 lookahead < std::min(commandIndex + 7, MaxCommands); ++lookahead) {
                const uint32_t nextW0 = static_cast<uint32_t>(commands[lookahead].words.w0);
                const uint32_t nextW1 = static_cast<uint32_t>(commands[lookahead].words.w1);
                const uint32_t nextOpcode = Opcode(nextW0);
                if (nextOpcode == static_cast<uint8_t>(F3DEX2_G_ENDDL) ||
                    nextOpcode == static_cast<uint8_t>(F3DEX2_G_DL)) {
                    break;
                }
                if (nextOpcode == static_cast<uint8_t>(RDP_G_RDPPIPESYNC)) {
                    pipeSyncSeen = true;
                    continue;
                }
                if (!pipeSyncSeen) {
                    continue;
                }
                if (nextOpcode == static_cast<uint8_t>(RDP_G_SETTILE)) {
                    const uint32_t renderTile = Field(nextW1, 24, 3);
                    const uint32_t lineWords = Field(nextW0, 9, 9);
                    const uint32_t renderSize = Field(nextW0, 19, 2);
                    if (lineWords == 0) {
                        continue;
                    }
                    for (size_t tileSizeIndex = lookahead + 1;
                         tileSizeIndex < std::min(lookahead + 4, MaxCommands); ++tileSizeIndex) {
                        const uint32_t tileSizeW0 = static_cast<uint32_t>(commands[tileSizeIndex].words.w0);
                        const uint32_t tileSizeW1 = static_cast<uint32_t>(commands[tileSizeIndex].words.w1);
                        if (Opcode(tileSizeW0) == static_cast<uint8_t>(F3DEX2_G_ENDDL) ||
                            Opcode(tileSizeW0) == static_cast<uint8_t>(F3DEX2_G_DL)) {
                            break;
                        }
                        if (Opcode(tileSizeW0) == static_cast<uint8_t>(RDP_G_SETTILESIZE) &&
                            Field(tileSizeW1, 24, 3) == renderTile) {
                            // RGBA32 occupies two TMEM banks, so one image
                            // row spans twice as many source words as its
                            // render-tile line field. Other render sizes use
                            // the line word count directly.
                            const uint32_t dxtWords = lineWords * (renderSize == G_IM_SIZ_32b ? 2 : 1);
                            const uint32_t dxt = ((1u << G_TX_DXT_FRAC) + dxtWords - 1) / dxtWords;
                            const uint32_t originalDxt = Field(static_cast<uint32_t>(source.words.w1), 0, 12);
                            if (dxt != originalDxt) {
                                lowerW1 = (static_cast<uint32_t>(source.words.w1) & ~0xfffu) | (dxt & 0xfffu);
                            }
                            lookahead = MaxCommands;
                            break;
                        }
                    }
                    break;
                }
                if (nextOpcode == static_cast<uint8_t>(RDP_G_SETTIMG) ||
                    nextOpcode == static_cast<uint8_t>(RDP_G_LOADBLOCK) ||
                    nextOpcode == static_cast<uint8_t>(RDP_G_LOADTILE) ||
                    nextOpcode == static_cast<uint8_t>(F3DEX2_G_ENDDL)) {
                    break;
                }
            }
        } else if (opcode == static_cast<uint8_t>(RDP_G_SETTIMG)) {
            mCurrentTextureFramebufferId = -1;
            // The exact byte count is finalized by the following load command.
            // Reserve enough space for the largest command-addressable texture;
            // the source itself remains owned by its originating resource or game.
            const uintptr_t sourceAddress = source.words.w1;
            const uint32_t width = Field(w0, 0, 12) + 1;
            const uint32_t sizeCode = Field(w0, 19, 2);
            const uint32_t bitsPerTexel = sizeCode == 0 ? 4 : sizeCode == 1 ? 8 : sizeCode == 2 ? 16 : 32;
            size_t estimate = std::max<size_t>(8, (static_cast<size_t>(width) * width * bitsPerTexel + 7) / 8);
            for (size_t lookahead = commandIndex + 1;
                 lookahead < commandIndex + 33 && Opcode(static_cast<uint32_t>(commands[lookahead].words.w0)) !=
                                                      static_cast<uint8_t>(RDP_G_SETTIMG) &&
                 Opcode(static_cast<uint32_t>(commands[lookahead].words.w0)) != static_cast<uint8_t>(F3DEX2_G_ENDDL);
                 ++lookahead) {
                const uint32_t followingOpcode = Opcode(static_cast<uint32_t>(commands[lookahead].words.w0));
                if (followingOpcode == static_cast<uint8_t>(RDP_G_LOADBLOCK)) {
                    const uint32_t texels = Field(static_cast<uint32_t>(commands[lookahead].words.w1), 12, 12) + 1;
                    estimate = (static_cast<size_t>(texels) * bitsPerTexel + 7) / 8;
                    break;
                }
                if (followingOpcode == static_cast<uint8_t>(RDP_G_LOADTILE)) {
                    const uint32_t loadW = ((Field(static_cast<uint32_t>(commands[lookahead].words.w1), 12, 12) -
                                             Field(static_cast<uint32_t>(commands[lookahead].words.w0), 12, 12)) >> 2) + 1;
                    const uint32_t loadH = ((Field(static_cast<uint32_t>(commands[lookahead].words.w1), 0, 12) -
                                             Field(static_cast<uint32_t>(commands[lookahead].words.w0), 0, 12)) >> 2) + 1;
                    estimate = (static_cast<size_t>(loadW) * loadH * bitsPerTexel + 7) / 8;
                    break;
                }
            }
            estimate = std::clamp<size_t>(estimate, 8, 1u << 20);
            const uint8_t format = static_cast<uint8_t>(Field(w0, 21, 3));
            lowerW1 = ResolveTextureAddress(sourceAddress, estimate, width, bitsPerTexel, format,
                                            static_cast<uint8_t>(sizeCode));
        } else if (opcode == static_cast<uint8_t>(RDP_G_SETCIMG)) {
            uintptr_t futureDepthImage = 0;
            if (mActiveFramebufferId != 0) {
                // Pause previews set their scratch depth address as CIMG in a
                // child list, clear it, then bind the same address as ZIMG.
                // Scan from this CIMG in its own list so a SETFB in a parent
                // list does not need to see through nested calls.
                for (size_t lookahead = commandIndex + 1;
                     lookahead < std::min(commandIndex + 65, MaxCommands); ++lookahead) {
                    const uint32_t upcomingOpcode = Opcode(static_cast<uint32_t>(commands[lookahead].words.w0));
                    if (upcomingOpcode == static_cast<uint8_t>(RDP_G_SETZIMG)) {
                        futureDepthImage = commands[lookahead].words.w1;
                        break;
                    }
                    if (upcomingOpcode == static_cast<uint8_t>(F3DEX2_G_ENDDL) ||
                        upcomingOpcode == OTR_G_SETFB || upcomingOpcode == OTR_G_RESETFB) {
                        break;
                    }
                }
            }
            if (futureDepthImage != 0 && source.words.w1 == futureDepthImage) {
                lowerW1 = DepthAddress;
            } else if (mLatestNativeDepthImage != 0 && source.words.w1 == mLatestNativeDepthImage) {
                lowerW1 = DepthAddress;
            } else if (mActiveFramebufferId != 0) {
                const auto framebuffer = GetFramebuffer(mActiveFramebufferId);
                if (!framebuffer) {
                    throw std::runtime_error("Active framebuffer was removed during display-list translation");
                }
                lowerW1 = framebuffer->address;
            } else {
                lowerW1 = ColorAddress;
            }
        } else if (opcode == static_cast<uint8_t>(RDP_G_SETZIMG)) {
            mLatestNativeDepthImage = source.words.w1;
            lowerW1 = DepthAddress;
        }

        if (opcode == static_cast<uint8_t>(F3DEX2_G_SETOTHERMODE_H)) {
            const uint32_t size = Field(w0, 0, 8) + 1;
            const uint32_t shift = Field(w0, 8, 8);
            const uint32_t offset = shift + size <= 32 ? 32 - shift - size : 0;
            if (offset == G_MDSFT_TEXTLUT && size == 2 && (lowerW1 == 2 || lowerW1 == 3)) {
                // RT64's partial SETOTHERMODE_H handler ORs the payload into
                // the complete H word. Ship emits the LUT enum unshifted for
                // this specific 2-bit field, so normalize it to native bit
                // positions while leaving already-shifted values untouched.
                lowerW1 <<= G_MDSFT_TEXTLUT;
            }
        }

        emit(lowerW0, lowerW1);
        (void)consumePayload;
    }

    if (!ended) {
        throw std::runtime_error("Display list did not terminate within 4096 commands");
    }
    const uint32_t listAddress = Allocate(output.size() * sizeof(RtCommand), 8);
    std::memcpy(mRdram.data() + listAddress, output.data(), output.size() * sizeof(RtCommand));
    mListsInProgress.erase(commands);
    mTranslatedLists[commands].push_back({inputState, CaptureState(), listAddress});
    return listAddress;
}

} // namespace Fast
