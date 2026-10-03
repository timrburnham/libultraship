// RT64 and libultra expose global N64 typedefs; isolate the RT64 names.
#define Vp_t Rt64Vp_t
#define OSTask_t Rt64OSTask_t
#include "hle/rt64_application.h"
#include "hle/rt64_workload_queue.h"
#include "gbi/rt64_gbi_f3dex2.h"
#include "gbi/rt64_gbi_s2dex2.h"
#include "gbi/rt64_gbi_rdp.h"
#include "rhi/rt64_render_hooks.h"
#include "gui/rt64_inspector.h"
#include "shared/rt64_fb_common.h"
#include "plume_vulkan.h"
#include "imgui/backends/imgui_impl_vulkan.h"
#undef Vp_t
#undef OSTask_t
#include "fast/rt64/Renderer.h"
#include "fast/rt64/DisplayListTranslator.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Fast {
namespace {
using namespace RT64;
// RT64's debugger shortcuts must not consume Ship's F1/F2 menu/input keys.
struct NativeApplication : Application {
    using Application::Application;
    bool sdlEventFilter(SDL_Event*) override { return false; }
};
class GuiApi final : public GfxRenderingAPI {
  public:
    struct GuiTexture {
        std::unique_ptr<RenderTexture> texture;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
    };
    Application* app;
    std::unordered_map<uint32_t, GuiTexture> textures;
    uint32_t nextTexture = 0, selectedTexture = 0;
    int nextFramebuffer = 0;
    FilteringMode filter = FILTER_THREE_POINT;
    VkSampler sampler = VK_NULL_HANDLE;
    explicit GuiApi(Application* app) : app(app) {
        auto* device = static_cast<plume::VulkanDevice*>(app->device.get());
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 1.0f;
        if (vkCreateSampler(device->vk, &info, nullptr, &sampler) != VK_SUCCESS) {
            throw std::runtime_error("RT64 GUI sampler creation failed");
        }
    }
    ~GuiApi() override {
        auto* device = static_cast<plume::VulkanDevice*>(app->device.get());
        for (auto& [id, texture] : textures) {
            if (texture.descriptor) ImGui_ImplVulkan_RemoveTexture(texture.descriptor);
        }
        textures.clear();
        vkDestroySampler(device->vk, sampler, nullptr);
    }
    void upload(const uint8_t* bytes, uint32_t width, uint32_t height) {
        if (!bytes || !width || !height || width > 8192 || height > 8192)
            throw std::invalid_argument("Invalid RT64 GUI texture");
        auto texture = app->device->createTexture(RenderTextureDesc::Texture2D(width,height,1,RenderFormat::R8G8B8A8_UNORM));
        const uint32_t pitch = (width * 4 + 255) & ~255u;
        auto upload = app->device->createBuffer(RenderBufferDesc::UploadBuffer(size_t(pitch)*height));
        auto* dst = static_cast<uint8_t*>(upload->map());
        for (uint32_t row=0;row<height;row++) std::memcpy(dst+row*pitch,bytes+row*width*4,width*4);
        upload->unmap();
        RenderWorker worker(app->device.get(), "Ship GUI upload", RenderCommandListType::DIRECT);
        worker.commandList->begin();
        worker.commandList->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(texture.get(),RenderTextureLayout::COPY_DEST));
        worker.commandList->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.get()),RenderTextureCopyLocation::PlacedFootprint(upload.get(),RenderFormat::R8G8B8A8_UNORM,width,height,1,pitch/4));
        worker.commandList->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(texture.get(),RenderTextureLayout::SHADER_READ));
        worker.commandList->end(); worker.execute(); worker.wait();
        auto* vkTexture = static_cast<plume::VulkanTexture*>(texture.get());
        auto descriptor = ImGui_ImplVulkan_AddTexture(sampler,vkTexture->imageView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!descriptor) throw std::runtime_error("RT64 GUI descriptor allocation failed");
        // NewTexture gives every upload a new ID; old textures survive queued frames.
        if (textures.contains(selectedTexture)) throw std::logic_error("RT64 GUI texture ID uploaded twice");
        textures.emplace(selectedTexture,GuiTexture{std::move(texture),descriptor});
    }
    const char* GetName() override { return "RT64 Vulkan"; }
    int GetMaxTextureSize() override { return 8192; }
    GfxClipParameters GetClipParameters() override { return {true,false}; }
    void UnloadShader(ShaderProgram* oldPrg) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: UnloadShader"); }
    void LoadShader(ShaderProgram* newPrg) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: LoadShader"); }
    ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint32_t shaderId1) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: CreateAndLoadNewShader"); }
    ShaderProgram* LookupShader(uint64_t shaderId0, uint32_t shaderId1) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: LookupShader"); }
    void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: ShaderGetInfo"); }
    uint32_t NewTexture() override { return ++nextTexture; }
    void SelectTexture(int tile, uint32_t textureId) override { selectedTexture = textureId; }
    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override { upload(rgba32Buf, width, height); }
    void SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) override {  }
    void SetDepthTestAndMask(bool depth_test, bool z_upd) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: SetDepthTestAndMask"); }
    void SetZmodeDecal(bool decal) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: SetZmodeDecal"); }
    void SetViewport(int x, int y, int width, int height) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: SetViewport"); }
    void SetScissor(int x, int y, int width, int height) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: SetScissor"); }
    void SetUseAlpha(bool useAlpha) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: SetUseAlpha"); }
    void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: DrawTriangles"); }
    void Init() override {  }
    void OnResize() override {  }
    void StartFrame() override {  }
    void EndFrame() override {  }
    void FinishRender() override {  }
    int CreateFramebuffer() override { return ++nextFramebuffer; }
    void UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                             bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                             bool can_extract_depth) override { DisplayListTranslator::DefineFramebuffer(fb_id, width, height); }
    void StartDrawToFramebuffer(int fbId, float noiseScale) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: StartDrawToFramebuffer"); }
    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0,
                                 int dstY0, int dstX1, int dstY1) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: CopyFramebuffer"); }
    void ClearFramebuffer(bool color, bool depth) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: ClearFramebuffer"); }
    void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: ReadFramebufferToCPU"); }
    void ResolveMSAAColorBuffer(int fbIdTarger, int fbIdSrc) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: ResolveMSAAColorBuffer"); }
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: GetPixelDepth"); }
    void* GetFramebufferTextureId(int fbId) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: GetFramebufferTextureId"); }
    void SelectTextureFb(int fbId) override { throw std::logic_error("Fast3D rendering is unavailable in the RT64 build: SelectTextureFb"); }
    void DeleteTexture(uint32_t texId) override { /* Retain descriptors until all presentation work has ended. */ }
    void SetTextureFilter(FilteringMode mode) override { filter = mode; }
    FilteringMode GetTextureFilter() override { return filter; }
    void SetSrgbMode() override {  }
    ImTextureID GetTextureById(int id) override { auto it = textures.find(id); return it == textures.end() ? (ImTextureID)nullptr : (ImTextureID)it->second.descriptor; }
};
}
struct Rt64Renderer::Impl {
    DisplayListTranslator translator;
    std::array<uint8_t,64> header{};
    std::array<uint8_t,4096> dmem{}, imem{};
    std::array<uint32_t,28> registers{};
    std::unique_ptr<NativeApplication> app;
    std::unique_ptr<Inspector> gui;
    std::unique_ptr<GuiApi> guiApi;
    GBI gbi{}, spriteGbi{};
    std::unordered_map<uint32_t, std::vector<uint8_t>> framebufferNativeTextureData;
    struct FramebufferImage {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };
    std::unordered_map<int, FramebufferImage> framebufferImages;
    std::unordered_set<int> framebufferReadbackReady;
    uint32_t pendingMsaa = 1;
    uint32_t pendingWidth = 0, pendingHeight = 0;
    float pendingX = 0, pendingY = 0, pendingPresentationWidth = 0, pendingPresentationHeight = 0;
    uint32_t nativeExtraGeometryMode = 0;
    uint64_t frames = 0;
    uint64_t testFrameLimit = 0;
    uint64_t captureStartFrame = 0;
    uint64_t testKeyFrame = 60;
    SDL_Scancode testKey = SDL_SCANCODE_UNKNOWN;
    struct ScheduledTestKey {
        uint64_t frame;
        SDL_Scancode scancode;
    };
    std::vector<ScheduledTestKey> scheduledTestKeys;
    static Impl* active;
    static void NativeWideRectangle(State* state, RT64::DisplayList** dl) {
        if (!active) throw std::logic_error("No active RT64 window");
        const auto& rectangles = active->translator.GetNativeWideRectangles();
        const uint32_t index = (*dl)->w1;
        if (index >= rectangles.size()) throw std::out_of_range("Native RT64 wide rectangle index");
        const auto& rect = rectangles[index];
        const auto previousAspect = state->rdp->extended.global.rectAspect;
        // Ship's wide coordinates already express placement relative to the
        // native screen center; preserve their pixel size in the wider image.
        state->rdp->setRectAspect(G_EX_ASPECT_ADJUST);
        if (rect.fill) {
            state->rdp->fillRect(rect.ulx, rect.uly, rect.lrx, rect.lry);
        }
        else {
            state->rdp->drawTexRect(rect.ulx, rect.uly, rect.lrx, rect.lry, rect.tile,
                                   rect.uls, rect.ult, rect.dsdx, rect.dtdy, rect.flip);
        }
        state->rdp->setRectAspect(previousAspect);
    }
    static void NativeUcode(State* state, RT64::DisplayList** dl) {
        if (!active) throw std::logic_error("No active RT64 window");
        const uint32_t index=(*dl)->w0 & 0xffffffu;
        GBI* target=index==4?&active->gbi:index==5?&active->spriteGbi:nullptr;
        if (!target) throw std::runtime_error("Unsupported native microcode index: "+std::to_string(index));
        state->flush(); state->ext.interpreter->hleGBI=target; state->rsp->setGBI(target);
        if (target->resetFromLoad) target->resetFromLoad(state);
    }
    static void ExtraGeometryMode(State* state, RT64::DisplayList** dl) {
        if (!active) throw std::logic_error("No active RT64 window");
        state->flush();
        active->nativeExtraGeometryMode=(active->nativeExtraGeometryMode & ((*dl)->w0 & 0xffffffu)) | (*dl)->w1;
        state->rsp->nativeInvertCulling=(active->nativeExtraGeometryMode & 1u)!=0;
        state->rsp->forceBranch((active->nativeExtraGeometryMode & 2u)!=0);
    }
    static void Grayscale(State* state, RT64::DisplayList** dl) {
        state->flush(); state->nativeGrayscaleEnabled=(*dl)->w1!=0;
        state->updateDrawStatusAttribute(DrawAttribute::PrimColor);
    }
    static void GrayscaleColor(State* state, RT64::DisplayList** dl) {
        state->flush(); const uint32_t color=(*dl)->w1;
        state->nativeGrayscaleColor=hlslpp::float4(float((color>>24)&255),float((color>>16)&255),float((color>>8)&255),float(color&255))/255.0f;
        state->updateDrawStatusAttribute(DrawAttribute::PrimColor);
    }
    static void NativeSetOtherMode(State* state, RT64::DisplayList** dl) {
        const uint32_t high = (*dl)->p0(0, 24);
        const uint32_t low = (*dl)->w1;
        state->rsp->setOtherMode(high, low);
    }
    static void FullSyncPreservingRsp(State* state) {
        auto* queue = state->ext.workloadQueue;
        auto& current = queue->workloads[queue->writeCursor];
        RT64::DrawData cached = current.drawData;

        state->fullSync();

        auto& next = queue->workloads[queue->writeCursor];
        cached.faceIndices.clear();
        cached.triPosFloats.clear();
        cached.triTcFloats.clear();
        cached.triColorFloats.clear();
        cached.rdpParams.clear();
        cached.renderParams.clear();
        cached.extraParams.clear();
        cached.rdpTiles.clear();
        cached.gpuTiles.clear();
        cached.callTiles.clear();
        cached.loadOperations.clear();
        next.drawData = std::move(cached);
        next.resetDrawDataRanges();
        state->resetDrawCall();
    }
    static FramebufferImage CaptureFramebuffer(State* state,
                                               const DisplayListTranslator::NativeFramebuffer& source) {
        // The synchronous RAM renderer deliberately uses native 4:3. Wait
        // for the display workload instead, then retain its full GPU image.
        state->ext.workloadQueue->waitForWorkloadId(state->workloadId);
        std::scoped_lock lock(state->ext.sharedQueueResources->managerMutex);
        auto& target = state->ext.sharedQueueResources->renderTargetManager.get(
            RenderTargetKey(source.address, source.width, G_IM_SIZ_16b, Framebuffer::Type::Color));
        FramebufferImage image;
        if (target.isEmpty()) return image;
        auto* worker = state->ext.framebufferGraphicsWorker;
        const size_t imageBytes = size_t(target.width) * target.height * 4;
        auto output = worker->device->createBuffer(RenderBufferDesc::DefaultBuffer(imageBytes,
            RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS | RenderBufferFlag::FORMATTED));
        auto outputView = output->createBufferFormattedView(RenderFormat::R32_UINT);
        auto readback = worker->device->createBuffer(RenderBufferDesc::ReadbackBuffer(imageBytes));
        FramebufferWriteDescriptorBufferSet outputSet(worker->device);
        outputSet.setBuffer(outputSet.gOutput, output.get(), imageBytes, outputView.get());
        FramebufferWriteDescriptorTextureSet inputSet(worker->device);
        const auto& shader = state->ext.shaderLibrary->fbWriteColor;
        interop::FbCommonCB constants{};
        constants.resolution = { target.width, target.height };
        constants.fmt = G_IM_FMT_RGBA;
        constants.siz = G_IM_SIZ_32b;
        constants.ditherPattern = 3; // Disabled; preserve the captured image.
        constants.usesHDR = target.usesHDR;
        constants.sourceScale = { 1.0f, 1.0f }; // Capture every display pixel, not the native 320x240 slice.
        worker->commandList->begin();
        target.resolveTarget(worker, state->ext.shaderLibrary);
        auto* texture = target.getResolvedTexture();
        inputSet.setTexture(inputSet.gInput, texture, RenderTextureLayout::SHADER_READ,
                            target.getResolvedTextureView());
        worker->commandList->barriers(RenderBarrierStage::COMPUTE,
            RenderBufferBarrier(output.get(), RenderBufferAccess::WRITE),
            RenderTextureBarrier(texture, RenderTextureLayout::SHADER_READ));
        worker->commandList->setPipeline(shader.pipeline.get());
        worker->commandList->setComputePipelineLayout(shader.pipelineLayout.get());
        worker->commandList->setComputePushConstants(0, &constants);
        worker->commandList->setComputeDescriptorSet(outputSet.get(), 0);
        worker->commandList->setComputeDescriptorSet(inputSet.get(), 1);
        worker->commandList->dispatch((target.width + FB_COMMON_WORKGROUP_SIZE - 1) / FB_COMMON_WORKGROUP_SIZE,
                                     (target.height + FB_COMMON_WORKGROUP_SIZE - 1) / FB_COMMON_WORKGROUP_SIZE, 1);
        const RenderBufferBarrier copyBarriers[] = {
            RenderBufferBarrier(output.get(), RenderBufferAccess::READ),
            RenderBufferBarrier(readback.get(), RenderBufferAccess::WRITE)
        };
        worker->commandList->barriers(RenderBarrierStage::COPY, copyBarriers, 2);
        worker->commandList->copyBuffer(readback.get(), output.get());
        worker->commandList->end();
        worker->execute(); worker->wait();
        image.width = target.width; image.height = target.height;
        image.rgba.resize(imageBytes);
        // RT64's RGBA32 write shader emits RGBA byte order in host memory.
        std::memcpy(image.rgba.data(), readback->map(), imageBytes);
        readback->unmap();
        return image;
    }
    static void FramebufferOperation(State* state, RT64::DisplayList** dl) {
        if (!active) throw std::logic_error("No active RT64 window");
        const auto& operations=active->translator.GetFramebufferOperations();
        const uint32_t index=(*dl)->w1;
        if (index>=operations.size()) throw std::out_of_range("RT64 framebuffer operation index");
        const auto& operation=operations[index];
        if (operation.oncePerFrame && operation.copiedFlag && *reinterpret_cast<uint8_t*>(operation.copiedFlag)) return;
        if (operation.type==DisplayListTranslator::FramebufferOperation::Type::Invalidate) {
            active->framebufferReadbackReady.erase(operation.sourceId);
            active->framebufferImages.erase(operation.sourceId);
            return;
        }
        const auto source=DisplayListTranslator::GetFramebuffer(operation.sourceId);
        if (!source) throw std::runtime_error("Unknown RT64 framebuffer source");
        // ResetFB flushes the preview before the game loads its UI vertices. A
        // second full sync here would move those cached RSP indices into a new
        // workload while they still refer to the previous one.
        if (operation.type==DisplayListTranslator::FramebufferOperation::Type::Flush) {
            if (active->framebufferReadbackReady.contains(operation.sourceId)) return;
            FullSyncPreservingRsp(state);
            active->framebufferReadbackReady.insert(operation.sourceId);
            return;
        }
        const bool alreadySynchronized = operation.type==DisplayListTranslator::FramebufferOperation::Type::BindTexture &&
                                         active->framebufferReadbackReady.contains(operation.sourceId);
        if (!alreadySynchronized) FullSyncPreservingRsp(state);
        auto& memory=active->translator.GetRdram();
        const auto read=[&](uint32_t x,uint32_t y) {
            uint16_t color;
            std::memcpy(&color,memory.data()+((source->address+(y*source->width+x)*2)^2),2);
            return color;
        };
        if (operation.type==DisplayListTranslator::FramebufferOperation::Type::BindTexture) {
            if (operation.format != G_IM_FMT_RGBA || operation.sizeCode != G_IM_SIZ_16b ||
                operation.width == 0 || operation.width > UINT16_MAX || operation.tile >= RDP_TILES) {
                throw std::invalid_argument("Unsupported RT64 framebuffer texture format or dimensions");
            }
            const auto& tile = state->rdp->tiles[operation.tile];
            if (tile.lrt < tile.ult) throw std::invalid_argument("Invalid RT64 framebuffer render-tile height");
            const uint32_t logicalHeight = ((tile.lrt - tile.ult) >> 2) + 1;
            auto& rgba = active->framebufferNativeTextureData[source->address];
            rgba.resize(size_t(source->width) * source->height * 4);
            for (uint32_t y = 0; y < source->height; ++y) {
                for (uint32_t x = 0; x < source->width; ++x) {
                    const uint16_t pixel = read(x, y);
                    const uint8_t r5 = static_cast<uint8_t>((pixel >> 11) & 0x1fu);
                    const uint8_t g5 = static_cast<uint8_t>((pixel >> 6) & 0x1fu);
                    const uint8_t b5 = static_cast<uint8_t>((pixel >> 1) & 0x1fu);
                    const size_t offset = (size_t(y) * source->width + x) * 4;
                    rgba[offset + 0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
                    rgba[offset + 1] = static_cast<uint8_t>((g5 << 3) | (g5 >> 2));
                    rgba[offset + 2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
                    rgba[offset + 3] = (pixel & 1u) ? 255 : 0;
                }
            }
            RT64::NativeTexture native{};
            native.address = source->address;
            native.rgba = rgba.data();
            native.byteCount = rgba.size();
            native.width = source->width;
            native.height = source->height;
            native.nativeWidth = operation.width;
            native.nativeHeight = logicalHeight;
            const auto image = active->framebufferImages.find(operation.sourceId);
            if (operation.height && image != active->framebufferImages.end() && !image->second.rgba.empty()) {
                native.rgba = image->second.rgba.data();
                native.byteCount = image->second.rgba.size();
                native.width = image->second.width;
                native.height = image->second.height;
            }
            state->rdp->registerNativeTexture(native);
            state->rdp->setTextureImage(operation.format, operation.sizeCode,
                                        static_cast<uint16_t>(operation.width), source->address);
            state->rdp->loadTile(operation.tile, tile.uls, tile.ult, tile.lrs, tile.lrt);
        } else if (operation.type==DisplayListTranslator::FramebufferOperation::Type::Copy) {
            const auto destination=DisplayListTranslator::GetFramebuffer(operation.destinationId);
            if (!destination) throw std::runtime_error("Unknown RT64 framebuffer destination");
            if (operation.sourceId == 0) {
                active->framebufferImages[operation.destinationId] = CaptureFramebuffer(state, *source);
            } else {
                const auto image = active->framebufferImages.find(operation.sourceId);
                if (image != active->framebufferImages.end())
                    active->framebufferImages[operation.destinationId] = image->second;
                else active->framebufferImages.erase(operation.destinationId);
            }
            std::vector<uint16_t> pixels(destination->width*destination->height);
            for (uint32_t y=0;y<destination->height;y++) for (uint32_t x=0;x<destination->width;x++)
                pixels[y*destination->width+x]=read(x*source->width/destination->width,y*source->height/destination->height);
            for (uint32_t n=0;n<pixels.size();n++) std::memcpy(memory.data()+((destination->address+n*2)^2),&pixels[n],2);
            if (operation.copiedFlag) *reinterpret_cast<uint8_t*>(operation.copiedFlag)=1;
            state->rdramCheckPending=true;
            state->checkRDRAM();
        } else {
            if (!operation.destination || !operation.width || !operation.height) throw std::invalid_argument("Invalid RT64 framebuffer readback");
            auto* output=reinterpret_cast<uint16_t*>(operation.destination);
            for (uint32_t y=0;y<operation.height;y++) for (uint32_t x=0;x<operation.width;x++) {
                const uint32_t sx=std::min(operation.x+x,source->width-1), sy=std::min(operation.y+y,source->height-1);
                uint16_t color=read(sx,sy);
                if (operation.byteSwap) color=uint16_t((color<<8)|(color>>8));
                output[y*operation.width+x]=color;
            }
        }
    }
    static void Draw(RenderCommandList* list, RenderFramebuffer*) { if (active && active->gui) active->gui->draw(list); }
    static void Deinit() { if (active) { active->guiApi.reset(); active->gui.reset(); } }
    explicit Impl(SDL_Window* window) {
        if (active) throw std::logic_error("Only one RT64 window is supported");
        if (const char* limit=std::getenv("SHIP_RT64_TEST_FRAMES")) testFrameLimit=std::strtoull(limit,nullptr,10);
        if (testFrameLimit) {
            if (const char* start=std::getenv("SHIP_RT64_CAPTURE_START")) captureStartFrame=std::strtoull(start,nullptr,10);
            if (const char* key=std::getenv("SHIP_RT64_TEST_KEY")) testKey=SDL_GetScancodeFromName(key);
            if (const char* frame=std::getenv("SHIP_RT64_TEST_KEY_FRAME")) testKeyFrame=std::strtoull(frame,nullptr,10);
            if (const char* schedule = std::getenv("SHIP_RT64_TEST_KEYS")) {
                const std::string scheduleText(schedule);
                size_t begin = 0;
                while (begin < scheduleText.size()) {
                    const size_t end = scheduleText.find(',', begin);
                    const size_t tokenEnd = end == std::string::npos ? scheduleText.size() : end;
                    const size_t separator = scheduleText.find(':', begin);
                    if (separator == std::string::npos || separator >= tokenEnd || separator == begin ||
                        separator + 1 == tokenEnd) {
                        throw std::invalid_argument("SHIP_RT64_TEST_KEYS entries must use frame:Scancode syntax");
                    }
                    const std::string frameText = scheduleText.substr(begin, separator - begin);
                    char* frameEnd = nullptr;
                    const unsigned long long frame = std::strtoull(frameText.c_str(), &frameEnd, 10);
                    if (frameEnd == frameText.c_str() || *frameEnd != '\0') {
                        throw std::invalid_argument("SHIP_RT64_TEST_KEYS contains an invalid frame number");
                    }
                    const std::string keyName = scheduleText.substr(separator + 1, tokenEnd - separator - 1);
                    const SDL_Scancode scancode = SDL_GetScancodeFromName(keyName.c_str());
                    if (scancode == SDL_SCANCODE_UNKNOWN) {
                        throw std::invalid_argument("SHIP_RT64_TEST_KEYS contains an unknown SDL scancode: " +
                                                    keyName);
                    }
                    scheduledTestKeys.push_back({static_cast<uint64_t>(frame), scancode});
                    begin = tokenEnd + (end == std::string::npos ? 0 : 1);
                }
            }
        }
        Application::Core core{};
        core.window = window; core.HEADER=header.data(); core.DMEM=dmem.data(); core.IMEM=imem.data();
        core.RDRAM=translator.GetRdram().data();
        unsigned n=0;
#define REG(name) core.name = &registers[n++]
        REG(MI_INTR_REG); REG(DPC_START_REG); REG(DPC_END_REG); REG(DPC_CURRENT_REG); REG(DPC_STATUS_REG);
        REG(DPC_CLOCK_REG); REG(DPC_BUFBUSY_REG); REG(DPC_PIPEBUSY_REG); REG(DPC_TMEM_REG);
        REG(VI_STATUS_REG); REG(VI_ORIGIN_REG); REG(VI_WIDTH_REG); REG(VI_INTR_REG); REG(VI_V_CURRENT_LINE_REG);
        REG(VI_TIMING_REG); REG(VI_V_SYNC_REG); REG(VI_H_SYNC_REG); REG(VI_LEAP_REG); REG(VI_H_START_REG);
        REG(VI_V_START_REG); REG(VI_V_BURST_REG); REG(VI_X_SCALE_REG); REG(VI_Y_SCALE_REG);
#undef REG
        core.checkInterrupts=[]{};
        *core.VI_STATUS_REG=2; *core.VI_ORIGIN_REG=DisplayListTranslator::ColorAddress+320*2;
        *core.VI_WIDTH_REG=320; *core.VI_V_SYNC_REG=525; *core.VI_H_SYNC_REG=3093;
        *core.VI_H_START_REG=(108u<<16)|748; *core.VI_V_START_REG=(37u<<16)|517;
        *core.VI_X_SCALE_REG=512; *core.VI_Y_SCALE_REG=1024;
        ApplicationConfiguration config; config.appId="ship-rt64"; config.dataPath=".rt64"; config.detectDataPath=false; config.useConfigurationFile=false;
        app=std::make_unique<NativeApplication>(core,config);
        app->userConfig.graphicsAPI=UserConfiguration::GraphicsAPI::Vulkan;
        app->userConfig.developerMode=false;
        app->emulatorConfig.framebuffer.renderToRAM=true;
        app->enhancementConfig.presentation.mode=EnhancementConfiguration::Presentation::Mode::SkipBuffering;
        auto result=app->setup(0);
        if (result!=Application::SetupResult::Success) {
            app->end(); throw std::runtime_error("RT64 Vulkan setup failed: "+std::to_string(int(result)));
        }
        gbi.ucode=GBIUCode::F3DEX2; GBI_RDP::setup(&gbi,true); GBI_F3DEX2::setup(&gbi);
        // Ship's native GBI can emit the OTR point-lighting geometry bit and
        // positional Light records. RT64 otherwise treats those records as
        // directional lights even though its RSP path supports point lights.
        gbi.flags.pointLighting = true;
        spriteGbi.ucode=GBIUCode::S2DEX2; GBI_RDP::setup(&spriteGbi,true); GBI_S2DEX2::setup(&spriteGbi);
        for (GBI* nativeGbi : { &gbi, &spriteGbi }) {
            nativeGbi->map[0xdd]=NativeUcode;
            nativeGbi->map[0xef]=NativeSetOtherMode;
            nativeGbi->map[0x39]=Grayscale;
            nativeGbi->map[0x3a]=ExtraGeometryMode;
            nativeGbi->map[0x40]=GrayscaleColor;
            nativeGbi->map[DisplayListTranslator::NativeFramebufferOperationOpcode]=FramebufferOperation;
            nativeGbi->map[DisplayListTranslator::NativeWideRectangleOpcode]=NativeWideRectangle;
        }
        app->state->rsp->nativeTextureCoordinates = true;
        app->state->rsp->nativeLightLayout = true;
        app->interpreter->hleGBI=&gbi; app->state->rsp->setGBI(&gbi);
        gui=std::make_unique<Inspector>(app->device.get(),app->swapChain.get(),UserConfiguration::GraphicsAPI::Vulkan,window);
        guiApi=std::make_unique<GuiApi>(app.get());
        active=this; SetRenderHooks(nullptr,Draw,Deinit);
    }
    ~Impl() { if (app) app->end(); SetRenderHooks(nullptr,nullptr,nullptr); active=nullptr; }
};
Rt64Renderer::Impl* Rt64Renderer::Impl::active=nullptr;
Rt64Renderer::Rt64Renderer(SDL_Window* window) : mImpl(std::make_unique<Impl>(window)) {}
Rt64Renderer::~Rt64Renderer()=default;
GfxRenderingAPI* Rt64Renderer::GetGuiApi() { return mImpl->guiApi.get(); }
void Rt64Renderer::Run(Gfx* commands,const std::unordered_map<Mtx*,MtxF>& replacements, uint32_t interpolationIndex) {
    auto& i=*mImpl;
    if (i.pendingWidth && i.pendingHeight) {
        auto& config = i.app->userConfig;
        const double multiplier = double(i.pendingHeight) / 240.0;
        const double ratio = double(i.pendingWidth) / i.pendingHeight;
        if (config.resolution != UserConfiguration::Resolution::Manual ||
            config.resolutionMultiplier != multiplier || config.aspectRatio != UserConfiguration::AspectRatio::Manual ||
            config.aspectTarget != ratio) {
            config.resolution = UserConfiguration::Resolution::Manual;
            config.resolutionMultiplier = multiplier;
            config.aspectRatio = UserConfiguration::AspectRatio::Manual;
            config.aspectTarget = ratio;
            i.app->updateUserConfig(false);
        }
    }
    if (i.pendingPresentationWidth > 0 && i.pendingPresentationHeight > 0) {
        auto& presentation = i.app->enhancementConfig.presentation;
        if (presentation.nativeViewportX != i.pendingX || presentation.nativeViewportY != i.pendingY ||
            presentation.nativeViewportWidth != i.pendingPresentationWidth ||
            presentation.nativeViewportHeight != i.pendingPresentationHeight) {
            presentation.nativeViewportX = i.pendingX;
            presentation.nativeViewportY = i.pendingY;
            presentation.nativeViewportWidth = i.pendingPresentationWidth;
            presentation.nativeViewportHeight = i.pendingPresentationHeight;
            i.app->updateEnhancementConfig();
        }
    }
    auto mode=i.pendingMsaa>=8?UserConfiguration::Antialiasing::MSAA8X:i.pendingMsaa>=4?UserConfiguration::Antialiasing::MSAA4X:i.pendingMsaa>=2?UserConfiguration::Antialiasing::MSAA2X:UserConfiguration::Antialiasing::None;
    if (i.app->userConfig.antialiasing!=mode) { i.app->userConfig.antialiasing=mode; i.app->updateUserConfig(true); i.app->updateMultisampling(); }
    auto address=i.translator.Translate(commands,replacements,interpolationIndex);
    i.app->state->rdp->clearNativeTextures();
    i.framebufferNativeTextureData.clear();
    i.framebufferReadbackReady.clear();
    for (const auto& texture : i.translator.GetNativeTextures()) {
        RT64::NativeTexture native;
        native.address=texture.address; native.rgba=texture.rgba; native.byteCount=texture.byteCount;
        native.width=texture.width; native.height=texture.height;
        native.nativeWidth=texture.nativeWidth; native.nativeHeight=texture.nativeHeight;
        i.app->state->rdp->registerNativeTexture(native);
    }
    i.app->interpreter->hleGBI=&i.gbi; i.app->state->rsp->setGBI(&i.gbi);
    if (i.gbi.resetFromTask) i.gbi.resetFromTask(i.app->state.get());
    i.app->processDisplayLists(i.translator.GetRdram().data(),address,0,true);
    // A bounded launch can validate game frames without relying on desktop input.
    if (i.testFrameLimit) ++i.frames;
    for (const auto& scheduledKey : i.scheduledTestKeys) {
        if (i.frames == scheduledKey.frame || i.frames == scheduledKey.frame + 6) {
            SDL_Event event{};
            event.type = i.frames == scheduledKey.frame ? SDL_KEYDOWN : SDL_KEYUP;
            event.key.state = event.type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
            event.key.keysym.scancode = scheduledKey.scancode;
            event.key.keysym.sym = SDL_GetKeyFromScancode(scheduledKey.scancode);
            SDL_PushEvent(&event);
        }
    }
    if (i.testKey!=SDL_SCANCODE_UNKNOWN && (i.frames==i.testKeyFrame || i.frames==i.testKeyFrame+6)) {
        SDL_Event event{};
        event.type=i.frames==i.testKeyFrame?SDL_KEYDOWN:SDL_KEYUP;
        event.key.state=event.type==SDL_KEYDOWN?SDL_PRESSED:SDL_RELEASED;
        event.key.keysym.scancode=i.testKey;
        event.key.keysym.sym=SDL_GetKeyFromScancode(i.testKey);
        SDL_PushEvent(&event);
    }
    if (i.testFrameLimit && (i.frames==i.testFrameLimit ||
        (i.captureStartFrame && i.frames>=i.captureStartFrame))) {
        if (const char* path=std::getenv("SHIP_RT64_CAPTURE")) {
            const std::string capturePath = i.captureStartFrame ?
                std::string(path) + "." + std::to_string(i.frames) + ".ppm" : std::string(path);
            std::ofstream capture(capturePath,std::ios::binary);
            if (!capture) throw std::runtime_error("Unable to open RT64 frame capture");
            capture << "P6\n320 240\n255\n";
            for (uint32_t y=0;y<240;y++) for (uint32_t x=0;x<320;x++) {
                const uint16_t color=GetNativeColor(x,y);
                const char rgb[]={char(((color>>11)&31)*255/31),char(((color>>6)&31)*255/31),char(((color>>1)&31)*255/31)};
                capture.write(rgb,3);
            }
        }
    }
    if (i.testFrameLimit && i.frames==i.testFrameLimit) {
        SDL_Event event{}; event.type=SDL_QUIT; SDL_PushEvent(&event);
    }
}
void Rt64Renderer::BeginGuiFrame() { mImpl->gui->newFrame(mImpl->app->presentGraphicsWorker.get()); }
void Rt64Renderer::EndGuiFrame() { mImpl->gui->endFrame(); }
void Rt64Renderer::Present() { mImpl->app->updateScreen(); }
void Rt64Renderer::SetResolution(float multiplier) {
    auto& a=*mImpl->app; a.userConfig.resolution=UserConfiguration::Resolution::Manual;
    a.userConfig.resolutionMultiplier=std::max(multiplier,1.0f); a.updateUserConfig(false);
}
void Rt64Renderer::SetRenderSize(uint32_t width, uint32_t height) {
    mImpl->pendingWidth = width;
    mImpl->pendingHeight = height;
}
void Rt64Renderer::SetPresentationRect(float x, float y, float width, float height) {
    mImpl->pendingX = x;
    mImpl->pendingY = y;
    mImpl->pendingPresentationWidth = width;
    mImpl->pendingPresentationHeight = height;
}
void Rt64Renderer::SetMsaa(uint32_t samples) {
    // Apply after the GUI frame has released the presentation thread's mutex.
    mImpl->pendingMsaa=samples;
}
void Rt64Renderer::SetTextureFilter(FilteringMode mode) {
    mImpl->guiApi->filter=mode; mImpl->app->userConfig.threePointFiltering=(mode==FILTER_THREE_POINT); mImpl->app->updateUserConfig(false);
}
uint16_t Rt64Renderer::GetNativeColor(uint32_t x,uint32_t y) const {
    if (x>=320 || y>=240) throw std::out_of_range("Native RT64 framebuffer pixel");
    uint16_t result; std::memcpy(&result,mImpl->translator.GetRdram().data()+((DisplayListTranslator::ColorAddress+(y*320+x)*2)^2),2); return result;
}
uint16_t Rt64Renderer::GetDepth(float x,float y) {
    if (x<0 || y<0 || x>=320 || y>=240) return 0xffff;
    uint16_t encoded;
    std::memcpy(&encoded,
                mImpl->translator.GetRdram().data() +
                    ((DisplayListTranslator::DepthAddress + (uint32_t(y) * 320 + uint32_t(x)) * 2) ^ 2),
                sizeof(encoded));

    // N64 depth memory uses a 16-bit exponent/mantissa representation. The
    // legacy renderer API returns normalized depth scaled to 0..65532; decode
    // the RDRAM value back to that linear range for callers such as light glow.
    const uint32_t exponent = (encoded & 0xe000u) >> 13;
    const uint32_t mantissa = (encoded & 0x1ffcu) >> 2;
    const uint32_t shiftedMantissa = mantissa << (6u - std::min(6u, exponent));
    const uint32_t mantissaBias = 0x40000u - (0x40000u >> exponent);
    const float normalized = static_cast<float>(shiftedMantissa + mantissaBias) / 262143.0f;
    const float scaled = std::clamp(normalized * 65532.0f, 0.0f, 65532.0f);
    return static_cast<uint16_t>(scaled);
}
}
