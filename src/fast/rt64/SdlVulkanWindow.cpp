#include "fast/rt64/SdlVulkanWindow.h"

#include <algorithm>
#include <cstdio>
#include <thread>

#include "ship/Context.h"
#include "ship/config/Config.h"
#include "ship/window/FileDropMgr.h"
#include "ship/window/gui/Gui.h"

#if defined(_WIN32)
#include <SDL.h>
#include <SDL_vulkan.h>
#elif defined(__APPLE__)
#include <SDL.h>
#include <SDL_vulkan.h>
#else
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#endif

namespace Fast {
namespace {
constexpr int kScancodeCount = 512;

void BindScancode(std::array<int, kScancodeCount>& table, SDL_Scancode sdl, int lus) {
    if (sdl >= 0 && sdl < kScancodeCount) {
        table[static_cast<size_t>(sdl)] = lus;
    }
}
} // namespace

SdlVulkanWindow::~SdlVulkanWindow() {
    Destroy();
}

void SdlVulkanWindow::Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width,
                           uint32_t height, int32_t posX, int32_t posY) {
    if (mWindow != nullptr) {
        return;
    }
    mDestroyed = false;
#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        SPDLOG_ERROR("Unable to initialize SDL video: {}", SDL_GetError());
        mIsRunning = false;
        return;
    }
    mSdlVideoInitialized = true;
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    char title[512];
    std::snprintf(title, sizeof(title), "%s (%s)", gameName != nullptr ? gameName : "Shipwright",
                  apiName != nullptr ? apiName : "Vulkan");
    Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_VULKAN;
#if defined(__IOS__)
    flags = SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_VULKAN;
#endif
    mWindow = SDL_CreateWindow(title, posX, posY, static_cast<int>(width), static_cast<int>(height), flags);
    if (mWindow == nullptr) {
        SPDLOG_ERROR("Unable to create SDL Vulkan window: {}", SDL_GetError());
        mIsRunning = false;
        Destroy();
        return;
    }

    mWindowWidth = width;
    mWindowHeight = height;
    mIsRunning = true;
    mFullScreen = false;
    mDestroyed = false;
    mStartTime = std::chrono::steady_clock::now();
    mNextFrame = mStartTime;

    for (int i = 0; i < kScancodeCount; ++i) {
        mSdlToLusTable[static_cast<size_t>(i)] = 0;
    }
#define MAP(sdl, lus) BindScancode(mSdlToLusTable, SDL_SCANCODE_##sdl, Ship::LUS_KB_##lus)
    MAP(ESCAPE, ESCAPE);
    MAP(1, 1); MAP(2, 2); MAP(3, 3); MAP(4, 4); MAP(5, 5); MAP(6, 6); MAP(7, 7); MAP(8, 8); MAP(9, 9); MAP(0, 0);
    MAP(MINUS, OEM_MINUS); MAP(EQUALS, OEM_PLUS); MAP(BACKSPACE, BACKSPACE); MAP(TAB, TAB);
    MAP(Q, Q); MAP(W, W); MAP(E, E); MAP(R, R); MAP(T, T); MAP(Y, Y); MAP(U, U); MAP(I, I); MAP(O, O); MAP(P, P);
    MAP(LEFTBRACKET, OEM_4); MAP(RIGHTBRACKET, OEM_6); MAP(RETURN, ENTER); MAP(LCTRL, CONTROL); MAP(RCTRL, CONTROL);
    MAP(A, A); MAP(S, S); MAP(D, D); MAP(F, F); MAP(G, G); MAP(H, H); MAP(J, J); MAP(K, K); MAP(L, L);
    MAP(SEMICOLON, OEM_1); MAP(APOSTROPHE, OEM_7); MAP(GRAVE, OEM_3); MAP(LSHIFT, SHIFT); MAP(RSHIFT, RSHIFT);
    MAP(BACKSLASH, OEM_5); MAP(Z, Z); MAP(X, X); MAP(C, C); MAP(V, V); MAP(B, B); MAP(N, N); MAP(M, M);
    MAP(COMMA, OEM_COMMA); MAP(PERIOD, OEM_PERIOD); MAP(SLASH, OEM_2); MAP(PRINTSCREEN, PRINTSCREEN); MAP(LALT, ALT);
    MAP(RALT, ALT); MAP(SPACE, SPACE); MAP(CAPSLOCK, CAPSLOCK); MAP(F1, F1); MAP(F2, F2); MAP(F3, F3); MAP(F4, F4);
    MAP(F5, F5); MAP(F6, F6); MAP(F7, F7); MAP(F8, F8); MAP(F9, F9); MAP(F10, F10); MAP(F11, F11); MAP(F12, F12);
    MAP(F13, F13); MAP(F14, F14); MAP(F15, F15); MAP(F16, F16); MAP(F17, F17); MAP(F18, F18); MAP(F19, F19);
    MAP(PAUSE, PAUSE); MAP(SCROLLLOCK, SCROLL); MAP(NUMLOCKCLEAR, NUMPAD7); MAP(KP_7, NUMPAD7); MAP(KP_8, NUMPAD8);
    MAP(KP_9, NUMPAD9); MAP(KP_MINUS, SUBTRACT); MAP(KP_4, NUMPAD4); MAP(KP_5, NUMPAD5); MAP(KP_6, NUMPAD6);
    MAP(KP_PLUS, ADD); MAP(KP_1, NUMPAD1); MAP(KP_2, NUMPAD2); MAP(KP_3, NUMPAD3); MAP(KP_0, NUMPAD0);
    MAP(KP_PERIOD, NUMPAD_DEL); MAP(KP_MULTIPLY, MULTIPLY);
    MAP(UP, ARROWKEY_UP); MAP(LEFT, ARROWKEY_LEFT); MAP(RIGHT, ARROWKEY_RIGHT); MAP(DOWN, ARROWKEY_DOWN);
#undef MAP
    // The legacy keyboard API distinguishes right-side modifiers and keypad enter/divide.
    mSdlToLusTable[SDL_SCANCODE_RCTRL] = Ship::LUS_KB_CONTROL + 0x100;
    mSdlToLusTable[SDL_SCANCODE_RALT] = Ship::LUS_KB_ALT + 0x100;
    mSdlToLusTable[SDL_SCANCODE_KP_ENTER] = Ship::LUS_KB_ENTER + 0x100;
    mSdlToLusTable[SDL_SCANCODE_KP_DIVIDE] = Ship::LUS_KB_OEM_2 + 0x100;

    if (startFullScreen) {
        SetFullscreenImpl(true, false);
    }
}

void SdlVulkanWindow::Close() {
    mIsRunning = false;
}

void SdlVulkanWindow::SetKeyboardCallbacks(bool (*onKeyDown)(int), bool (*onKeyUp)(int), void (*onAllKeysUp)()) {
    mOnKeyDown = onKeyDown;
    mOnKeyUp = onKeyUp;
    mOnAllKeysUp = onAllKeysUp;
}

void SdlVulkanWindow::SetMouseCallbacks(bool (*onMouseButtonDown)(int), bool (*onMouseButtonUp)(int)) {
    mOnMouseButtonDown = onMouseButtonDown;
    mOnMouseButtonUp = onMouseButtonUp;
}

void SdlVulkanWindow::SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool)) {
    mOnFullscreenChanged = onFullscreenChanged;
}

void SdlVulkanWindow::SetFullscreenImpl(bool fullscreen, bool callCallback) {
    if (mWindow == nullptr || mFullScreen == fullscreen) {
        return;
    }
    if (SDL_SetWindowFullscreen(mWindow, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0) {
        mFullScreen = fullscreen;
        if (!fullscreen) {
            auto context = Ship::Context::GetInstance();
            if (context != nullptr && context->GetConfig() != nullptr) {
                const auto config = context->GetConfig();
                SDL_SetWindowSize(mWindow, config->GetInt("Window.Width", 640), config->GetInt("Window.Height", 480));
                SDL_SetWindowPosition(mWindow, config->GetInt("Window.PositionX", 100),
                                      config->GetInt("Window.PositionY", 100));
            }
        }
        if (callCallback && mOnFullscreenChanged != nullptr) {
            mOnFullscreenChanged(fullscreen);
        }
    } else {
        SPDLOG_ERROR("Failed to change SDL fullscreen state: {}", SDL_GetError());
    }
}

void SdlVulkanWindow::SetFullscreen(bool fullscreen) { SetFullscreenImpl(fullscreen, true); }

void SdlVulkanWindow::GetActiveWindowRefreshRate(uint32_t* refreshRate) {
    if (refreshRate == nullptr) return;
    const int display = mWindow != nullptr ? SDL_GetWindowDisplayIndex(mWindow) : 0;
    SDL_DisplayMode mode{};
    *refreshRate = display >= 0 && SDL_GetCurrentDisplayMode(display, &mode) == 0 && mode.refresh_rate > 0
                       ? static_cast<uint32_t>(mode.refresh_rate)
                       : 60;
}

void SdlVulkanWindow::SetCursorVisibility(bool visibility) { SDL_ShowCursor(visibility ? SDL_ENABLE : SDL_DISABLE); }
void SdlVulkanWindow::SetMousePos(int32_t posX, int32_t posY) { if (mWindow) SDL_WarpMouseInWindow(mWindow, posX, posY); }
void SdlVulkanWindow::GetMousePos(int32_t* x, int32_t* y) { SDL_GetMouseState(x, y); }
void SdlVulkanWindow::GetMouseDelta(int32_t* x, int32_t* y) { SDL_GetRelativeMouseState(x, y); }

void SdlVulkanWindow::GetMouseWheel(float* x, float* y) {
    if (x) *x = mMouseWheelX;
    if (y) *y = mMouseWheelY;
    mMouseWheelX = mMouseWheelY = 0.0f;
}

bool SdlVulkanWindow::GetMouseState(uint32_t btn) {
    if (btn >= 32) return false;
    const Uint32 state = SDL_GetMouseState(nullptr, nullptr);
    const Uint32 mask = SDL_BUTTON(static_cast<Uint8>(btn + 1));
    return (state & mask) != 0;
}

void SdlVulkanWindow::SetMouseCapture(bool capture) {
    if (mWindow == nullptr) return;
    SDL_SetRelativeMouseMode(capture ? SDL_TRUE : SDL_FALSE);
    mMouseCaptured = capture && SDL_GetRelativeMouseMode() == SDL_TRUE;
    if (mMouseCaptured) {
        int w = 0, h = 0;
        SDL_GetWindowSize(mWindow, &w, &h);
        mCursorClip = {(w / 2) - 1, (h / 2) - 1, 2, 2};
        SDL_Rect clip{mCursorClip[0], mCursorClip[1], mCursorClip[2], mCursorClip[3]};
        SDL_SetWindowMouseRect(mWindow, &clip);
    } else {
        SDL_SetWindowMouseRect(mWindow, nullptr);
    }
}

bool SdlVulkanWindow::IsMouseCaptured() { return SDL_GetRelativeMouseMode() == SDL_TRUE; }

void SdlVulkanWindow::GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    if (mWindow == nullptr) return;
    int w = 0, h = 0, x = 0, y = 0;
    SDL_Vulkan_GetDrawableSize(mWindow, &w, &h);
    SDL_GetWindowPosition(mWindow, &x, &y);
    if (width) *width = static_cast<uint32_t>(std::max(w, 0));
    if (height) *height = static_cast<uint32_t>(std::max(h, 0));
    if (posX) *posX = x;
    if (posY) *posY = y;
}

int SdlVulkanWindow::TranslateScancode(int sdlScancode) const {
    return sdlScancode >= 0 && sdlScancode < kScancodeCount ? mSdlToLusTable[static_cast<size_t>(sdlScancode)] : 0;
}

int SdlVulkanWindow::UntranslateScancode(int lusScancode) const {
    for (int i = 0; i < kScancodeCount; ++i) {
        if (mSdlToLusTable[static_cast<size_t>(i)] == lusScancode) return i;
    }
    return SDL_SCANCODE_UNKNOWN;
}

void SdlVulkanWindow::HandleSingleEvent(SDL_Event& event) {
    auto context = Ship::Context::GetInstance();
    if (context != nullptr && context->GetWindow() != nullptr && context->GetWindow()->GetGui() != nullptr) {
        Ship::WindowEvent guiEvent;
        guiEvent.Sdl = {&event};
        context->GetWindow()->GetGui()->HandleWindowEvents(guiEvent);
    }
    switch (event.type) {
        case SDL_KEYDOWN:
            if (mOnKeyDown != nullptr) mOnKeyDown(TranslateScancode(event.key.keysym.scancode));
            break;
        case SDL_KEYUP:
            if (mOnKeyUp != nullptr) mOnKeyUp(TranslateScancode(event.key.keysym.scancode));
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button >= SDL_BUTTON_LEFT && event.button.button <= SDL_BUTTON_X2 && mOnMouseButtonDown)
                mOnMouseButtonDown(event.button.button - 1);
            break;
        case SDL_MOUSEBUTTONUP:
            if (event.button.button >= SDL_BUTTON_LEFT && event.button.button <= SDL_BUTTON_X2 && mOnMouseButtonUp)
                mOnMouseButtonUp(event.button.button - 1);
            break;
        case SDL_MOUSEWHEEL:
            mMouseWheelX = static_cast<float>(event.wheel.x);
            mMouseWheelY = static_cast<float>(event.wheel.y);
#if SDL_VERSION_ATLEAST(2, 0, 18)
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                mMouseWheelX = -mMouseWheelX;
                mMouseWheelY = -mMouseWheelY;
            }
#endif
            break;
        case SDL_WINDOWEVENT:
            if (event.window.windowID == SDL_GetWindowID(mWindow)) {
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) Close();
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST && mOnAllKeysUp != nullptr) mOnAllKeysUp();
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    int w = 0, h = 0;
                    SDL_Vulkan_GetDrawableSize(mWindow, &w, &h);
                    mWindowWidth = static_cast<uint32_t>(std::max(w, 0));
                    mWindowHeight = static_cast<uint32_t>(std::max(h, 0));
                }
            }
            break;
        case SDL_DROPFILE:
            if (context != nullptr && context->GetFileDropMgr() != nullptr)
                context->GetFileDropMgr()->SetDroppedFile(event.drop.file);
            SDL_free(event.drop.file);
            event.drop.file = nullptr;
            break;
        case SDL_QUIT:
            Close();
            break;
    }
}

void SdlVulkanWindow::HandleEvents() {
    if (mWindow == nullptr) return;
    SDL_PumpEvents();
    SDL_Event event;
    // Let SDL game-controller input handling consume its own device lifecycle events.
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_CONTROLLERDEVICEADDED - 1) > 0)
        HandleSingleEvent(event);
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_CONTROLLERDEVICEREMOVED + 1, SDL_LASTEVENT) > 0)
        HandleSingleEvent(event);
}

bool SdlVulkanWindow::IsFrameReady() { return mIsRunning; }

void SdlVulkanWindow::WaitForFrame() {
    const int fps = std::max(1, static_cast<int>(mTargetFps));
    const auto interval = std::chrono::microseconds(1000000 / fps);
    const auto now = std::chrono::steady_clock::now();
    if (mNextFrame < now - interval) mNextFrame = now;
    mNextFrame += interval;
    std::this_thread::sleep_until(mNextFrame);
}

void SdlVulkanWindow::SwapBuffersBegin() { WaitForFrame(); }
void SdlVulkanWindow::SwapBuffersEnd() {}
double SdlVulkanWindow::GetTime() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - mStartTime).count();
}
int SdlVulkanWindow::GetTargetFps() { return static_cast<int>(mTargetFps); }
void SdlVulkanWindow::SetTargetFps(int fps) { mTargetFps = static_cast<uint32_t>(std::clamp(fps, 1, 1000)); }
void SdlVulkanWindow::SetMaxFrameLatency(int) {}
const char* SdlVulkanWindow::GetKeyName(int scancode) {
    return SDL_GetScancodeName(static_cast<SDL_Scancode>(UntranslateScancode(scancode)));
}
bool SdlVulkanWindow::CanDisableVsync() { return true; }
bool SdlVulkanWindow::IsRunning() { return mIsRunning; }
void SdlVulkanWindow::Destroy() {
    if (mDestroyed) return;
    mDestroyed = true;
    if (mWindow != nullptr) {
        SDL_DestroyWindow(mWindow);
        mWindow = nullptr;
    }
    if (mSdlVideoInitialized) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        mSdlVideoInitialized = false;
    }
}
bool SdlVulkanWindow::IsFullscreen() { return mFullScreen; }
SDL_Window* SdlVulkanWindow::GetWindow() const { return mWindow; }
} // namespace Fast
