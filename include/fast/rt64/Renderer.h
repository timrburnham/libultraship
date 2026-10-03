#pragma once
#include <memory>
#include <unordered_map>
#include <imgui.h>
#include "fast/types.h"
#include "fast/backends/gfx_rendering_api.h"
union Gfx;
struct SDL_Window;
namespace Fast {
// Owns RT64's memory, device, queues and GUI bridge for one native window.
class Rt64Renderer {
  public:
    explicit Rt64Renderer(SDL_Window* window);
    ~Rt64Renderer();
    GfxRenderingAPI* GetGuiApi();
    void Run(Gfx* commands, const std::unordered_map<Mtx*, MtxF>& replacements, uint32_t interpolationIndex = 0);
    void BeginGuiFrame();
    void EndGuiFrame();
    void Present();
    void SetResolution(float multiplier);
    void SetMsaa(uint32_t samples);
    void SetTextureFilter(FilteringMode mode);
    uint16_t GetDepth(float x, float y);
    uint16_t GetNativeColor(uint32_t x, uint32_t y) const;
  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
}
