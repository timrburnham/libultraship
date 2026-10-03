#pragma once

#include "fast/backends/gfx_window_manager_api.h"

#include <array>
#include <chrono>

struct SDL_Window;
union SDL_Event;

namespace Fast {
/** SDL window used by RT64's Vulkan renderer. SDL creates the native window only;
 * Vulkan instance, surface, and presentation are owned by RT64.
 */
class SdlVulkanWindow final : public GfxWindowBackend {
  public:
    SdlVulkanWindow() {
        mOnFullscreenChanged = nullptr;
        mOnKeyDown = nullptr;
        mOnKeyUp = nullptr;
        mOnMouseButtonDown = nullptr;
        mOnMouseButtonUp = nullptr;
    }
    ~SdlVulkanWindow() override;

    void Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width, uint32_t height,
              int32_t posX, int32_t posY) override;
    void Close() override;
    void SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                              void (*onAllKeysUp)()) override;
    void SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) override;
    void SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool is_now_fullscreen)) override;
    void SetFullscreen(bool fullscreen) override;
    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override;
    void SetCursorVisibility(bool visibility) override;
    void SetMousePos(int32_t posX, int32_t posY) override;
    void GetMousePos(int32_t* x, int32_t* y) override;
    void GetMouseDelta(int32_t* x, int32_t* y) override;
    void GetMouseWheel(float* x, float* y) override;
    bool GetMouseState(uint32_t btn) override;
    void SetMouseCapture(bool capture) override;
    bool IsMouseCaptured() override;
    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) override;
    void HandleEvents() override;
    bool IsFrameReady() override;
    void SwapBuffersBegin() override;
    void SwapBuffersEnd() override;
    double GetTime() override;
    int GetTargetFps() override;
    void SetTargetFps(int fps) override;
    void SetMaxFrameLatency(int latency) override;
    const char* GetKeyName(int scancode) override;
    bool CanDisableVsync() override;
    bool IsRunning() override;
    void Destroy() override;
    bool IsFullscreen() override;

    SDL_Window* GetWindow() const;

  private:
    void SetFullscreenImpl(bool fullscreen, bool callCallback);
    void HandleSingleEvent(SDL_Event& event);
    int TranslateScancode(int sdlScancode) const;
    int UntranslateScancode(int lusScancode) const;
    void WaitForFrame();

    SDL_Window* mWindow = nullptr;
    bool mSdlVideoInitialized = false;
    bool mFullScreen = false;
    bool mDestroyed = false;
    bool mMouseCaptured = false;
    bool mVsyncEnabled = true;
    uint32_t mWindowWidth = 640;
    uint32_t mWindowHeight = 480;
    float mMouseWheelX = 0.0f;
    float mMouseWheelY = 0.0f;
    std::array<int, 512> mSdlToLusTable{};
    std::array<int, 4> mCursorClip{};
    std::chrono::steady_clock::time_point mStartTime{};
    std::chrono::steady_clock::time_point mNextFrame{};
    void (*mOnAllKeysUp)() = nullptr;
};
} // namespace Fast
