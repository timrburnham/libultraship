#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fast/types.h"
#include "ship/resource/Resource.h"

union Gfx;

namespace Fast {
class Texture;

// Lowers Shipwright's pointer-based Gfx commands into a contiguous RDRAM image
// and rewrites every data/display-list address into an RT64 RDRAM offset.
class DisplayListTranslator {
    friend struct DisplayListTranslatorTestAccess;

  public:
    static constexpr uint32_t RdramSize = 0x800000;
    static constexpr uint32_t ColorAddress = 0x100000;
    static constexpr uint32_t DepthAddress = 0x180000;
    static constexpr uint8_t NativeFramebufferOperationOpcode = 0x43;

    struct NativeFramebuffer {
        int id = 0;
        uint32_t width = 320;
        uint32_t height = 240;
        uint32_t address = ColorAddress;
    };

    struct FramebufferOperation {
        enum class Type { Copy, Read, Flush, BindTexture, Invalidate };
        Type type = Type::Copy;
        int sourceId = 0;
        int destinationId = 0;
        uintptr_t destination = 0;
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool byteSwap = false;
        bool oncePerFrame = false;
        uintptr_t copiedFlag = 0;
        uint8_t format = 0;
        uint8_t sizeCode = 0;
        uint8_t tile = 0;
    };

    struct NativeTexture {
        uint32_t address = 0;
        const uint8_t* rgba = nullptr;
        size_t byteCount = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t nativeWidth = 0;
        uint32_t nativeHeight = 0;
        uint8_t format = 0;
        uint8_t sizeCode = 0;
    };

    static void DefineFramebuffer(int id, uint32_t width, uint32_t height);
    static std::optional<NativeFramebuffer> GetFramebuffer(int id);

    DisplayListTranslator();

    // The returned value is the byte address of the lowered root display list.
    // Resource references are retained until Reset() so RDRAM copies stay valid
    // throughout RT64's asynchronous workload processing.
    uint32_t Translate(Gfx* commands, const std::unordered_map<Mtx*, MtxF>& mtxReplacements = {},
                       uint32_t interpolationIndex = 0);

    // Reset transient arena allocations while preserving the fixed framebuffer
    // region and its contents.
    void Reset();

    std::vector<uint8_t>& GetRdram() { return mRdram; }
    const std::vector<uint8_t>& GetRdram() const { return mRdram; }
    const std::vector<FramebufferOperation>& GetFramebufferOperations() const { return mFramebufferOperations; }
    const std::vector<NativeTexture>& GetNativeTextures() const { return mNativeTextures; }

  private:
    uint32_t LowerDisplayList(Gfx* commands, size_t depth);
    uint32_t CopyBytes(const void* source, size_t size, size_t alignment = 8);
    uint32_t CopyTexture(const uint8_t* source, size_t size);
    uint32_t CopyTextureResource(const std::shared_ptr<Texture>& texture, size_t nativeSize, uint32_t nativeWidth,
                                 uint32_t bitsPerTexel, uint8_t format, uint8_t sizeCode, const char* path = nullptr);
    uint32_t CopyVertices(const void* source, size_t count);
    uint32_t CopyMatrix(const MtxS& matrix);
    uint32_t CopyMatrix(const void* source);
    uint32_t CopyMatrixWithReplacement(const void* source);
    uint32_t CopyMovemem(const void* source, size_t size, uint8_t index);
    uint32_t LowerS2dexBackground(const void* source, bool scaled);
    uint32_t ResolveAddress(uintptr_t address, size_t size, size_t alignment = 8);
    uint32_t ResolveTextureAddress(uintptr_t address, size_t size, uint32_t nativeWidth, uint32_t bitsPerTexel,
                                   uint8_t format, uint8_t sizeCode);
    uint32_t ResolveResourcePointer(const void* pointer, size_t size, size_t alignment = 8);
    uint32_t LoadHashResource(uint64_t hash, size_t size, size_t alignment = 8);
    uint32_t LoadPathResource(const char* path, size_t size, size_t alignment = 8);
    uint32_t Allocate(size_t size, size_t alignment);

    std::vector<uint8_t> mRdram;
    uint32_t mNextAddress = 0x200000;
    std::array<uintptr_t, 16> mSegments{};
    std::unordered_map<const void*, uint32_t> mPointerAddresses;
    std::unordered_map<const uint8_t*, std::pair<uint32_t, size_t>> mTextureCopies;
    struct TranslationState {
        std::array<uintptr_t, 16> segments{};
        uint32_t ucode = 4;
        uintptr_t latestDepthImage = 0;
        int activeFramebuffer = 0;
        int textureFramebuffer = -1;
        uint32_t interpolationTarget = 0;
        bool operator==(const TranslationState&) const = default;
    };
    struct CachedList {
        TranslationState input;
        TranslationState output;
        uint32_t address = 0;
    };
    TranslationState CaptureState() const;
    void ApplyState(const TranslationState& state);
    std::unordered_map<Gfx*, std::vector<CachedList>> mTranslatedLists;
    std::unordered_set<Gfx*> mListsInProgress;
    std::vector<std::shared_ptr<Ship::IResource>> mRetainedResources;
    std::vector<std::shared_ptr<std::vector<uint8_t>>> mOwnedNativeTextureData;
    std::unordered_map<Mtx*, MtxF> mMtxReplacements;
    uint32_t mCurrentUcode = 4;
    uintptr_t mLatestNativeDepthImage = 0;
    int mActiveFramebufferId = 0;
    int mCurrentTextureFramebufferId = -1;
    uint32_t mInterpolationIndex = 0;
    uint32_t mInterpolationIndexTarget = 0;
    std::vector<FramebufferOperation> mFramebufferOperations;
    std::vector<NativeTexture> mNativeTextures;
};

} // namespace Fast
