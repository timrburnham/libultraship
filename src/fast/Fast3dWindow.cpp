#include "fast/Fast3dWindow.h"

#include "ship/Context.h"
#include "ship/config/Config.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/config/ConsoleVariable.h"
#include "fast/interpreter.h"
#include "fast/backends/gfx_sdl.h"
#include "fast/backends/gfx_dxgi.h"
#include "fast/backends/gfx_opengl.h"
#include "fast/backends/gfx_metal.h"
#include "fast/backends/gfx_direct3d_common.h"
#include "fast/backends/gfx_direct3d11.h"
#include "fast/backends/gfx_window_manager_api.h"

#include <fstream>
#ifdef SHIP_USE_RT64
#include "fast/rt64/Renderer.h"
#include "fast/rt64/SdlVulkanWindow.h"
#endif

namespace Fast {

extern void GfxSetInstance(std::shared_ptr<Interpreter> gfx);

Fast3dWindow::Fast3dWindow(std::shared_ptr<Ship::Gui> gui, std::shared_ptr<FastMouseStateManager> mouseStateManager)
    : Ship::Window(gui, mouseStateManager) {
    mWindowManagerApi = nullptr;
    mRenderingApi = nullptr;
    mInterpreter = std::make_shared<Interpreter>();
    GfxSetInstance(mInterpreter);

#ifdef SHIP_USE_RT64
    AddAvailableWindowBackend(Ship::WindowBackend::RT64_SDL_VULKAN);
#else
#ifdef _WIN32
    AddAvailableWindowBackend(Ship::WindowBackend::FAST3D_DXGI_DX11);
#endif
#ifdef __APPLE__
    if (Metal_IsSupported()) {
        AddAvailableWindowBackend(Ship::WindowBackend::FAST3D_SDL_METAL);
    }
#endif
    AddAvailableWindowBackend(Ship::WindowBackend::FAST3D_SDL_OPENGL);
#endif
}

Fast3dWindow::Fast3dWindow(std::shared_ptr<Ship::Gui> gui)
    : Fast3dWindow(gui, std::make_shared<FastMouseStateManager>()) {
}

Fast3dWindow::Fast3dWindow(std::vector<std::shared_ptr<Ship::GuiWindow>> guiWindows)
    : Fast3dWindow(std::make_shared<Ship::Gui>(guiWindows)) {
}

Fast3dWindow::Fast3dWindow() : Fast3dWindow(std::vector<std::shared_ptr<Ship::GuiWindow>>()) {
}

Fast3dWindow::~Fast3dWindow() {
    SPDLOG_DEBUG("destruct fast3dwindow");
#ifdef SHIP_USE_RT64
    mInterpreter->TextureCacheClear();
    mRt64.reset();
    mInterpreter->mRapi = nullptr;
    if (mWindowManagerApi) mWindowManagerApi->Destroy();
#else
    mInterpreter->Destroy();
    delete mRenderingApi;
#endif
    delete mWindowManagerApi;
}

void Fast3dWindow::Init() {
    bool gameMode = false;

#ifdef __linux__
    std::ifstream osReleaseFile("/etc/os-release");
    if (osReleaseFile.is_open()) {
        std::string line;
        while (std::getline(osReleaseFile, line)) {
            if (line.find("VARIANT_ID") != std::string::npos) {
                if (line.find("steamdeck") != std::string::npos) {
                    gameMode = std::getenv("XDG_CURRENT_DESKTOP") != nullptr &&
                               std::string(std::getenv("XDG_CURRENT_DESKTOP")) == "gamescope";
                }
                break;
            }
        }
    }
#elif defined(__ANDROID__) || defined(__IOS__)
    gameMode = true;
#endif

    bool isFullscreen;
    uint32_t width, height;
    int32_t posX, posY;

    isFullscreen = Ship::Context::GetInstance()->GetConfig()->GetBool("Window.Fullscreen.Enabled", false) || gameMode;
    posX = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.PositionX", 100);
    posY = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.PositionY", 100);

    if (isFullscreen) {
        width = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Fullscreen.Width", gameMode ? 1280 : 1920);
        height = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Fullscreen.Height", gameMode ? 800 : 1080);
    } else {
        width = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Width", 640);
        height = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Height", 480);
    }
    Ship::Context::GetInstance()->GetWindow()->SetFullscreenScancode(
        Ship::Context::GetInstance()->GetConfig()->GetInt("Shortcuts.Fullscreen", Ship::KbScancode::LUS_KB_F11));
    Ship::Context::GetInstance()->GetWindow()->SetMouseCaptureScancode(
        Ship::Context::GetInstance()->GetConfig()->GetInt("Shortcuts.MouseCapture", Ship::KbScancode::LUS_KB_F2));

    InitWindowManager();
#ifdef SHIP_USE_RT64
    mWindowManagerApi->Init(Ship::Context::GetInstance()->GetName().c_str(), "RT64 Vulkan", isFullscreen, width, height, posX, posY);
    mRt64 = std::make_unique<Rt64Renderer>(static_cast<SdlVulkanWindow*>(mWindowManagerApi)->GetWindow());
    mRenderingApi = mRt64->GetGuiApi();
    mInterpreter->mRapi = mRenderingApi;
    mInterpreter->mWapi = mWindowManagerApi;
    mInterpreter->mCurDimensions.width = 320;
    mInterpreter->mCurDimensions.height = 240;
    mInterpreter->mCurDimensions.internal_mul = 1.0f;
    mInterpreter->mNativeDimensions.width = 320;
    mInterpreter->mNativeDimensions.height = 240;
    mInterpreter->mCurDimensions.aspect_ratio = 4.0f / 3.0f;
    Ship::GuiWindowInitData guiData{};
    guiData.Opengl.Window = static_cast<SdlVulkanWindow*>(mWindowManagerApi)->GetWindow();
    GetGui()->Init(guiData);
#else
    mInterpreter->Init(mWindowManagerApi, mRenderingApi, Ship::Context::GetInstance()->GetName().c_str(), isFullscreen,
                       width, height, posX, posY);
#endif
    mWindowManagerApi->SetFullscreenChangedCallback(OnFullscreenChanged);
    mWindowManagerApi->SetKeyboardCallbacks(KeyDown, KeyUp, AllKeysUp);
    mWindowManagerApi->SetMouseCallbacks(MouseButtonDown, MouseButtonUp);

    SetTextureFilter((FilteringMode)Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(
        CVAR_TEXTURE_FILTER, FILTER_THREE_POINT));
}

int32_t Fast3dWindow::GetTargetFps() {
#ifdef SHIP_USE_RT64
    return mWindowManagerApi->GetTargetFps();
#else
    return mInterpreter->GetTargetFps();
#endif
}

void Fast3dWindow::SetTargetFps(int32_t fps) {
#ifdef SHIP_USE_RT64
    mWindowManagerApi->SetTargetFps(fps);
#else
    mInterpreter->SetTargetFps(fps);
#endif
}

void Fast3dWindow::SetMaximumFrameLatency(int32_t latency) {
#ifdef SHIP_USE_RT64
    mWindowManagerApi->SetMaxFrameLatency(latency);
#else
    mInterpreter->SetMaxFrameLatency(latency);
#endif
}

void Fast3dWindow::GetPixelDepthPrepare(float x, float y) {
#ifdef SHIP_USE_RT64
    // RT64 fullSync synchronizes the native depth buffer before returning.
#else
    mInterpreter->GetPixelDepthPrepare(x, y);
#endif
}

uint16_t Fast3dWindow::GetPixelDepth(float x, float y) {
#ifdef SHIP_USE_RT64
    return mRt64->GetDepth(x,y);
#else
    return mInterpreter->GetPixelDepth(x, y);
#endif
}

void Fast3dWindow::InitWindowManager() {
#ifdef SHIP_USE_RT64
    SetWindowBackend(Ship::WindowBackend::RT64_SDL_VULKAN);
    mWindowManagerApi = new SdlVulkanWindow();
#else
    SetWindowBackend(Ship::Context::GetInstance()->GetConfig()->GetWindowBackend());

    switch (GetWindowBackend()) {
#ifdef ENABLE_DX11
        case Ship::WindowBackend::FAST3D_DXGI_DX11:
            mWindowManagerApi = new GfxWindowBackendDXGI();
            mRenderingApi = new GfxRenderingAPIDX11(static_cast<GfxWindowBackendDXGI*>(mWindowManagerApi));
            break;
#endif
#ifdef ENABLE_OPENGL
        case Ship::WindowBackend::FAST3D_SDL_OPENGL:
            mRenderingApi = new GfxRenderingAPIOGL();
            mWindowManagerApi = new GfxWindowBackendSDL2();
            break;
#endif
#ifdef __APPLE__
        case Ship::WindowBackend::FAST3D_SDL_METAL:
            mRenderingApi = new GfxRenderingAPIMetal();
            mWindowManagerApi = new GfxWindowBackendSDL2();
            break;
#endif
        default:
            SPDLOG_ERROR("Could not load the correct rendering backend");
            break;
    }
#endif
}

void Fast3dWindow::SetTextureFilter(FilteringMode filteringMode) {
#ifdef SHIP_USE_RT64
    mRt64->SetTextureFilter(filteringMode);
#else
    mInterpreter->GetCurrentRenderingAPI()->SetTextureFilter(filteringMode);
#endif
}

void Fast3dWindow::EnableSRGBMode() {
#ifdef SHIP_USE_RT64
    // RT64 handles the swapchain color format.
#else
    mInterpreter->mRapi->SetSrgbMode();
#endif
}

void Fast3dWindow::SetRendererUCode(UcodeHandlers ucode) {
#ifdef SHIP_USE_RT64
    if (ucode != UcodeHandlers::ucode_f3dex2) throw std::runtime_error("RT64 requires F3DEX2 display lists");
#else
    gfx_set_target_ucode(ucode);
#endif
}

void Fast3dWindow::Close() {
    mWindowManagerApi->Close();
}

void Fast3dWindow::RunGuiOnly() {
#ifdef SHIP_USE_RT64
    // The caller owns StartDraw/EndDraw; RT64 presents in EndFrame.
#else
    mInterpreter->RunGuiOnly();
#endif
}

void Fast3dWindow::StartFrame() {
#ifdef SHIP_USE_RT64
    mWindowManagerApi->HandleEvents();
#else
    mInterpreter->StartFrame();
#endif
}

void Fast3dWindow::EndFrame() {
#ifdef SHIP_USE_RT64
    mWindowManagerApi->SwapBuffersBegin();
    mRt64->Present();
    mWindowManagerApi->SwapBuffersEnd();
#else
    mInterpreter->EndFrame();
#endif
}

bool Fast3dWindow::IsFrameReady() {
    return mWindowManagerApi->IsFrameReady();
}

bool Fast3dWindow::DrawAndRunGraphicsCommands(Gfx* commands, const std::unordered_map<Mtx*, MtxF>& mtxReplacements) {
    std::shared_ptr<Window> wnd = Ship::Context::GetInstance()->GetWindow();

    // Skip dropped frames
    if (!wnd->IsFrameReady()) {
        return false;
    }

    auto gui = wnd->GetGui();
    // Setup mouse state manager
    wnd->GetMouseStateManager()->StartFrame();
    // Submit game work before locking GUI data consumed by RT64's presentation thread.
#ifdef SHIP_USE_RT64
    mRt64->Run(commands, mtxReplacements, mInterpreter->mInterpolationIndex);
    gui->StartDraw();
#else
    gui->StartDraw();
    mInterpreter->StartFrame();
    mInterpreter->Run(commands, mtxReplacements);
#endif
    // Renders the game frame buffer to the final window and finishes the GUI
    gui->EndDraw();
    // Finalize swap buffers
#ifdef SHIP_USE_RT64
    mWindowManagerApi->SwapBuffersBegin();
    mRt64->Present();
    mWindowManagerApi->SwapBuffersEnd();
#else
    mInterpreter->EndFrame();
#endif
    return true;
}

void Fast3dWindow::HandleEvents() {
    mWindowManagerApi->HandleEvents();
}

void Fast3dWindow::SetCursorVisibility(bool visible) {
    mWindowManagerApi->SetCursorVisibility(visible);
}

uint32_t Fast3dWindow::GetWidth() {
    uint32_t width, height;
    int32_t posX, posY;
    mWindowManagerApi->GetDimensions(&width, &height, &posX, &posY);
    return width;
}

uint32_t Fast3dWindow::GetHeight() {
    uint32_t width, height;
    int32_t posX, posY;
    mWindowManagerApi->GetDimensions(&width, &height, &posX, &posY);
    return height;
}

float Fast3dWindow::GetAspectRatio() {
    return mInterpreter->mCurDimensions.aspect_ratio;
}

int32_t Fast3dWindow::GetPosX() {
    uint32_t width, height;
    int32_t posX, posY;
    mWindowManagerApi->GetDimensions(&width, &height, &posX, &posY);
    return posX;
}

int32_t Fast3dWindow::GetPosY() {
    uint32_t width, height;
    int32_t posX, posY;
    mWindowManagerApi->GetDimensions(&width, &height, &posX, &posY);
    return posY;
}

void Fast3dWindow::SetMousePos(Ship::Coords pos) {
    mWindowManagerApi->SetMousePos(pos.x, pos.y);
}

Ship::Coords Fast3dWindow::GetMousePos() {
    int32_t x, y;
    mWindowManagerApi->GetMousePos(&x, &y);
    return { x, y };
}

Ship::Coords Fast3dWindow::GetMouseDelta() {
    int32_t x, y;
    mWindowManagerApi->GetMouseDelta(&x, &y);
    return { x, y };
}

Ship::CoordsF Fast3dWindow::GetMouseWheel() {
    float x, y;
    mWindowManagerApi->GetMouseWheel(&x, &y);
    return { x, y };
}

bool Fast3dWindow::GetMouseState(Ship::MouseBtn btn) {
    return mWindowManagerApi->GetMouseState(static_cast<uint32_t>(btn));
}

void Fast3dWindow::SetMouseCapture(bool capture) {
    mWindowManagerApi->SetMouseCapture(capture);
}

bool Fast3dWindow::IsMouseCaptured() {
    return mWindowManagerApi->IsMouseCaptured();
}

uint32_t Fast3dWindow::GetCurrentRefreshRate() {
    uint32_t refreshRate;
    mWindowManagerApi->GetActiveWindowRefreshRate(&refreshRate);
    return refreshRate;
}

bool Fast3dWindow::SupportsWindowedFullscreen() {
#ifdef __APPLE__
    return false;
#endif

    if (GetWindowBackend() == Ship::WindowBackend::FAST3D_SDL_OPENGL) {
        return true;
    }

    return false;
}

bool Fast3dWindow::CanDisableVerticalSync() {
    return mWindowManagerApi->CanDisableVsync();
}

void Fast3dWindow::SetResolutionMultiplier(float multiplier) {
#ifdef SHIP_USE_RT64
    mRt64->SetResolution(multiplier); mInterpreter->mCurDimensions.internal_mul = multiplier;
#else
    mInterpreter->SetResolutionMultiplier(multiplier);
#endif
}

void Fast3dWindow::SetMsaaLevel(uint32_t value) {
#ifdef SHIP_USE_RT64
    mRt64->SetMsaa(value);
#else
    mInterpreter->SetMsaaLevel(value);
#endif
}

void Fast3dWindow::SetFullscreen(bool isFullscreen) {
    // Save current window position before fullscreening
    SaveWindowToConfig();
    mWindowManagerApi->SetFullscreen(isFullscreen);
}

bool Fast3dWindow::IsFullscreen() {
    return mWindowManagerApi->IsFullscreen();
}

bool Fast3dWindow::IsRunning() {
    return mWindowManagerApi->IsRunning();
}

uintptr_t Fast3dWindow::GetGfxFrameBuffer() {
#ifdef SHIP_USE_RT64
    return 0; // RT64 presents the game directly before the GUI overlay.
#else
    return mInterpreter->mGfxFrameBuffer;
#endif
}

const char* Fast3dWindow::GetKeyName(int32_t scancode) {
    return mWindowManagerApi->GetKeyName(scancode);
}

bool Fast3dWindow::KeyUp(int32_t scancode) {
    if (scancode == Ship::Context::GetInstance()->GetWindow()->GetFullscreenScancode()) {
        Ship::Context::GetInstance()->GetWindow()->ToggleFullscreen();
    }

    if (scancode == Ship::Context::GetInstance()->GetWindow()->GetMouseCaptureScancode()) {
        Ship::Context::GetInstance()->GetWindow()->GetMouseStateManager()->ToggleMouseCaptureOverride();
    }

    Ship::Context::GetInstance()->GetWindow()->SetLastScancode(-1);
    return Ship::Context::GetInstance()->GetControlDeck()->ProcessKeyboardEvent(
        Ship::KbEventType::LUS_KB_EVENT_KEY_UP, static_cast<Ship::KbScancode>(scancode));
}

bool Fast3dWindow::KeyDown(int32_t scancode) {
    bool isProcessed = Ship::Context::GetInstance()->GetControlDeck()->ProcessKeyboardEvent(
        Ship::KbEventType::LUS_KB_EVENT_KEY_DOWN, static_cast<Ship::KbScancode>(scancode));
    Ship::Context::GetInstance()->GetWindow()->SetLastScancode(scancode);

    return isProcessed;
}

void Fast3dWindow::AllKeysUp() {
    Ship::Context::GetInstance()->GetControlDeck()->ProcessKeyboardEvent(Ship::KbEventType::LUS_KB_EVENT_ALL_KEYS_UP,
                                                                         Ship::KbScancode::LUS_KB_UNKNOWN);
}

bool Fast3dWindow::MouseButtonUp(int button) {
    return Ship::Context::GetInstance()->GetControlDeck()->ProcessMouseButtonEvent(false,
                                                                                   static_cast<Ship::MouseBtn>(button));
}

bool Fast3dWindow::MouseButtonDown(int button) {
    bool isProcessed = Ship::Context::GetInstance()->GetControlDeck()->ProcessMouseButtonEvent(
        true, static_cast<Ship::MouseBtn>(button));
    return isProcessed;
}

void Fast3dWindow::OnFullscreenChanged(bool isNowFullscreen) {
    std::shared_ptr<Window> wnd = Ship::Context::GetInstance()->GetWindow();

    // Re-save fullscreen enabled after
    Ship::Context::GetInstance()->GetConfig()->SetBool("Window.Fullscreen.Enabled", isNowFullscreen);
}

std::weak_ptr<Interpreter> Fast3dWindow::GetInterpreterWeak() const {
    return mInterpreter;
}

} // namespace Fast
