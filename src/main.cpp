#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "resource.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kWindowClass[] = L"DesktopFlyNativeWindow";
constexpr wchar_t kMutexName[] = L"Local\\DesktopFly_5Minute_Instance";
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT kFrameIntervalMs = 16;
constexpr ULONGLONG kLifetimeMs = 300000;
constexpr int kFrameCount = 64;
constexpr double kPi = 3.14159265358979323846;

struct Frame {
    std::unique_ptr<Bitmap> bitmap;
    HBITMAP handle = nullptr;

    Frame() = default;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&& other) noexcept : bitmap(std::move(other.bitmap)), handle(other.handle) {
        other.handle = nullptr;
    }
    Frame& operator=(Frame&& other) noexcept {
        if (this != &other) {
            if (handle) DeleteObject(handle);
            bitmap = std::move(other.bitmap);
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    ~Frame() {
        if (handle) DeleteObject(handle);
    }
};

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HANDLE g_mutex = nullptr;
ULONG_PTR g_gdiplusToken = 0;
bool g_comInitialized = false;
HDC g_screenDc = nullptr;
HDC g_memoryDc = nullptr;
std::vector<Frame> g_flightFrames;
std::vector<Frame> g_groomFrames;
std::vector<Frame> g_flightFlutterFrames;
std::vector<Frame> g_groomFlutterFrames;
Bitmap* g_currentBitmap = nullptr;

std::mt19937 g_random;
RECT g_bounds{};
int g_pixelSize = 52;
double g_dpiScale = 1.0;
double g_x = 0.0;
double g_y = 0.0;
double g_vx = 0.0;
double g_vy = 0.0;
bool g_flying = true;
bool g_grooming = false;
int g_restFrame = 0;
ULONGLONG g_startedAt = 0;
ULONGLONG g_lastTick = 0;
ULONGLONG g_nextStop = 0;
ULONGLONG g_stopUntil = 0;
ULONGLONG g_nextTurn = 0;
ULONGLONG g_nextSpeedChange = 0;
ULONGLONG g_nextMotionChange = 0;
ULONGLONG g_boostUntil = 0;
ULONGLONG g_nextGroomChange = 0;
ULONGLONG g_nextWingFlick = 0;
ULONGLONG g_wingFlickUntil = 0;
double g_targetSpeed = 0.0;

enum class MotionMode {
    Crawl,
    Flight,
};

MotionMode g_motionMode = MotionMode::Flight;

double RandomUnit() {
    return std::generate_canonical<double, 53>(g_random);
}

double RandomRange(double low, double high) {
    return low + RandomUnit() * (high - low);
}

ULONGLONG RandomRestDuration() {
    const double roll = RandomUnit();
    if (roll < 0.58) return static_cast<ULONGLONG>(RandomRange(1800.0, 6000.0));
    if (roll < 0.90) return static_cast<ULONGLONG>(RandomRange(6000.0, 18000.0));
    return static_cast<ULONGLONG>(RandomRange(18000.0, 45000.0));
}

double MotionSpeed(MotionMode mode, bool boosted = false) {
    if (boosted) return RandomRange(760.0, 1120.0) * g_dpiScale;
    if (mode == MotionMode::Crawl) return RandomRange(22.0, 78.0) * g_dpiScale;
    return RandomRange(360.0, 920.0) * g_dpiScale;
}

void ScheduleMotion(ULONGLONG now, MotionMode mode, bool boosted = false) {
    g_motionMode = mode;
    g_targetSpeed = MotionSpeed(mode, boosted);
    g_nextTurn = now + static_cast<ULONGLONG>(
        mode == MotionMode::Crawl ? RandomRange(420.0, 1250.0) : RandomRange(90.0, 420.0));
    g_nextSpeedChange = now + static_cast<ULONGLONG>(
        mode == MotionMode::Crawl ? RandomRange(700.0, 2200.0) : RandomRange(180.0, 750.0));
    g_nextMotionChange = now + static_cast<ULONGLONG>(
        mode == MotionMode::Crawl ? RandomRange(2500.0, 9000.0) : RandomRange(1800.0, 6500.0));
}

std::unique_ptr<Bitmap> LoadPngResource(int resourceId) {
    HRSRC resource = FindResourceW(g_instance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) return nullptr;
    HGLOBAL loaded = LoadResource(g_instance, resource);
    if (!loaded) return nullptr;
    const DWORD size = SizeofResource(g_instance, resource);
    const void* source = LockResource(loaded);
    if (!source || size == 0) return nullptr;

    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy) return nullptr;
    void* destination = GlobalLock(copy);
    if (!destination) {
        GlobalFree(copy);
        return nullptr;
    }
    CopyMemory(destination, source, size);
    GlobalUnlock(copy);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(copy, TRUE, &stream))) {
        GlobalFree(copy);
        return nullptr;
    }

    std::unique_ptr<Bitmap> decoded(Bitmap::FromStream(stream, FALSE));
    if (!decoded || decoded->GetLastStatus() != Ok) {
        stream->Release();
        return nullptr;
    }
    std::unique_ptr<Bitmap> owned(decoded->Clone(0, 0, decoded->GetWidth(), decoded->GetHeight(), PixelFormat32bppPARGB));
    stream->Release();
    if (!owned || owned->GetLastStatus() != Ok) return nullptr;
    return owned;
}

Frame MakeRotatedFrame(Bitmap* source, double degrees) {
    Frame frame;
    frame.bitmap = std::make_unique<Bitmap>(g_pixelSize, g_pixelSize, PixelFormat32bppPARGB);
    Graphics graphics(frame.bitmap.get());
    graphics.SetCompositingMode(CompositingModeSourceCopy);
    graphics.Clear(Color(0, 0, 0, 0));
    graphics.SetCompositingMode(CompositingModeSourceOver);
    graphics.SetCompositingQuality(CompositingQualityHighQuality);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetSmoothingMode(SmoothingModeHighQuality);
    graphics.TranslateTransform(g_pixelSize / 2.0f, g_pixelSize / 2.0f);
    graphics.RotateTransform(static_cast<REAL>(degrees));
    graphics.TranslateTransform(-g_pixelSize / 2.0f, -g_pixelSize / 2.0f);
    graphics.DrawImage(source, Rect(0, 0, g_pixelSize, g_pixelSize),
                       0, 0, source->GetWidth(), source->GetHeight(), UnitPixel);
    graphics.Flush(FlushIntentionSync);
    frame.bitmap->GetHBITMAP(Color(0, 0, 0, 0), &frame.handle);
    return frame;
}

void DrawWingGhost(Graphics& target, Bitmap* source, bool leftWing) {
    Bitmap layer(g_pixelSize, g_pixelSize, PixelFormat32bppPARGB);
    Graphics layerGraphics(&layer);
    layerGraphics.SetCompositingMode(CompositingModeSourceCopy);
    layerGraphics.Clear(Color(0, 0, 0, 0));
    layerGraphics.SetCompositingMode(CompositingModeSourceOver);
    layerGraphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);

    const auto point = [](double x, double y) {
        return PointF(static_cast<REAL>(x * g_pixelSize), static_cast<REAL>(y * g_pixelSize));
    };
    PointF wingPoints[6];
    const double normalizedX[6] = {0.46, 0.34, 0.25, 0.29, 0.41, 0.47};
    const double normalizedY[6] = {0.41, 0.45, 0.63, 0.78, 0.66, 0.45};
    for (int index = 0; index < 6; ++index) {
        const double x = leftWing ? normalizedX[index] : 1.0 - normalizedX[index];
        wingPoints[index] = point(x, normalizedY[index]);
    }

    GraphicsPath wingPath;
    wingPath.AddPolygon(wingPoints, 6);
    layerGraphics.SetClip(&wingPath);
    layerGraphics.DrawImage(source, Rect(0, 0, g_pixelSize, g_pixelSize),
                            0, 0, source->GetWidth(), source->GetHeight(), UnitPixel);

    ColorMatrix alphaMatrix = {{
        {1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.52f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    }};
    ImageAttributes attributes;
    attributes.SetColorMatrix(&alphaMatrix, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);

    const REAL rootX = static_cast<REAL>((leftWing ? 0.45 : 0.55) * g_pixelSize);
    const REAL rootY = static_cast<REAL>(0.42 * g_pixelSize);
    const GraphicsState state = target.Save();
    target.TranslateTransform(rootX, rootY);
    target.RotateTransform(leftWing ? 7.0f : -7.0f);
    target.TranslateTransform(-rootX, -rootY);
    target.DrawImage(&layer, Rect(0, 0, g_pixelSize, g_pixelSize),
                     0, 0, g_pixelSize, g_pixelSize, UnitPixel, &attributes);
    target.Restore(state);
}

std::unique_ptr<Bitmap> MakeWingFlutterSource(Bitmap* source) {
    auto composed = std::make_unique<Bitmap>(g_pixelSize, g_pixelSize, PixelFormat32bppPARGB);
    Graphics graphics(composed.get());
    graphics.SetCompositingMode(CompositingModeSourceCopy);
    graphics.Clear(Color(0, 0, 0, 0));
    graphics.SetCompositingMode(CompositingModeSourceOver);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    graphics.DrawImage(source, Rect(0, 0, g_pixelSize, g_pixelSize),
                       0, 0, source->GetWidth(), source->GetHeight(), UnitPixel);
    DrawWingGhost(graphics, source, true);
    DrawWingGhost(graphics, source, false);
    graphics.Flush(FlushIntentionSync);
    return composed;
}

bool BuildFrames() {
    auto flight = LoadPngResource(IDR_FLY_FLIGHT);
    auto groom = LoadPngResource(IDR_FLY_GROOM);
    if (!flight || !groom) return false;
    auto flightFlutter = MakeWingFlutterSource(flight.get());
    auto groomFlutter = MakeWingFlutterSource(groom.get());
    if (!flightFlutter || !groomFlutter) return false;
    g_flightFrames.reserve(kFrameCount);
    g_groomFrames.reserve(kFrameCount);
    g_flightFlutterFrames.reserve(kFrameCount);
    g_groomFlutterFrames.reserve(kFrameCount);
    for (int index = 0; index < kFrameCount; ++index) {
        const double angle = 360.0 * index / kFrameCount;
        Frame flightFrame = MakeRotatedFrame(flight.get(), angle);
        Frame groomFrame = MakeRotatedFrame(groom.get(), angle);
        Frame flightFlutterFrame = MakeRotatedFrame(flightFlutter.get(), angle);
        Frame groomFlutterFrame = MakeRotatedFrame(groomFlutter.get(), angle);
        if (!flightFrame.handle || !groomFrame.handle ||
            !flightFlutterFrame.handle || !groomFlutterFrame.handle) return false;
        g_flightFrames.emplace_back(std::move(flightFrame));
        g_groomFrames.emplace_back(std::move(groomFrame));
        g_flightFlutterFrames.emplace_back(std::move(flightFlutterFrame));
        g_groomFlutterFrames.emplace_back(std::move(groomFlutterFrame));
    }
    return true;
}

int HeadingFrame(double jitterDegrees = 0.0) {
    double degrees = std::atan2(g_vy, g_vx) * 180.0 / kPi + 90.0 + jitterDegrees;
    while (degrees < 0.0) degrees += 360.0;
    while (degrees >= 360.0) degrees -= 360.0;
    return static_cast<int>(std::lround(degrees / 360.0 * kFrameCount)) % kFrameCount;
}

void Present(Frame& frame) {
    if (!g_window || !g_screenDc || !g_memoryDc || !frame.handle) return;
    HGDIOBJ previous = SelectObject(g_memoryDc, frame.handle);
    POINT destination{static_cast<LONG>(std::lround(g_x)), static_cast<LONG>(std::lround(g_y))};
    POINT source{0, 0};
    SIZE size{g_pixelSize, g_pixelSize};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_window, g_screenDc, &destination, &size, g_memoryDc,
                        &source, 0, &blend, ULW_ALPHA);
    SelectObject(g_memoryDc, previous);
    g_currentBitmap = frame.bitmap.get();
}

void StartFlight(bool clicked) {
    const ULONGLONG now = GetTickCount64();
    const double angle = RandomRange(0.0, kPi * 2.0);
    const MotionMode mode = clicked || RandomUnit() >= 0.38
        ? MotionMode::Flight : MotionMode::Crawl;
    const double speed = MotionSpeed(mode, clicked);
    g_vx = std::cos(angle) * speed;
    g_vy = std::sin(angle) * speed;
    g_flying = true;
    g_grooming = false;
    g_nextStop = now + static_cast<ULONGLONG>(RandomRange(3500.0, 14000.0));
    ScheduleMotion(now, mode, clicked);
    if (clicked) g_boostUntil = now + 1100;
}

void ClampAndBounce() {
    const double left = static_cast<double>(g_bounds.left);
    const double top = static_cast<double>(g_bounds.top);
    const double right = static_cast<double>(g_bounds.right - g_pixelSize);
    const double bottom = static_cast<double>(g_bounds.bottom - g_pixelSize);
    if (g_x < left) { g_x = left; g_vx = std::abs(g_vx); }
    if (g_x > right) { g_x = right; g_vx = -std::abs(g_vx); }
    if (g_y < top) { g_y = top; g_vy = std::abs(g_vy); }
    if (g_y > bottom) { g_y = bottom; g_vy = -std::abs(g_vy); }
}

void Tick() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_startedAt >= kLifetimeMs) {
        DestroyWindow(g_window);
        return;
    }
    double delta = static_cast<double>(now - g_lastTick) / 1000.0;
    g_lastTick = now;
    delta = std::min(delta, 0.08);

    if (g_flying) {
        const bool boosted = now < g_boostUntil;
        if (!boosted && now >= g_nextMotionChange) {
            const MotionMode nextMode = g_motionMode == MotionMode::Flight && RandomUnit() < 0.52
                ? MotionMode::Crawl : MotionMode::Flight;
            ScheduleMotion(now, nextMode);
        }

        if (now >= g_nextTurn) {
            const double turn = g_motionMode == MotionMode::Crawl ? 0.55 : 1.65;
            const double heading = std::atan2(g_vy, g_vx) + RandomRange(-turn, turn);
            const double speed = std::hypot(g_vx, g_vy);
            g_vx = std::cos(heading) * speed;
            g_vy = std::sin(heading) * speed;
            g_nextTurn = now + static_cast<ULONGLONG>(g_motionMode == MotionMode::Crawl
                ? RandomRange(420.0, 1250.0) : RandomRange(90.0, 420.0));
        }

        if (now >= g_nextSpeedChange) {
            g_targetSpeed = MotionSpeed(g_motionMode, boosted);
            g_nextSpeedChange = now + static_cast<ULONGLONG>(g_motionMode == MotionMode::Crawl
                ? RandomRange(700.0, 2200.0) : RandomRange(180.0, 750.0));
        }

        const double speed = std::max(1.0, std::hypot(g_vx, g_vy));
        const double response = g_motionMode == MotionMode::Crawl ? 2.2 : 5.5;
        const double easedSpeed = speed + (g_targetSpeed - speed) * (1.0 - std::exp(-response * delta));
        g_vx *= easedSpeed / speed;
        g_vy *= easedSpeed / speed;

        g_x += g_vx * delta;
        g_y += g_vy * delta;
        ClampAndBounce();
        const double jitter = ((now / 18) % 2 == 0) ? -3.8 : 3.8;
        Present(g_flightFrames[HeadingFrame(jitter)]);

        if (now >= g_nextStop) {
            g_flying = false;
            g_grooming = true;
            g_stopUntil = now + RandomRestDuration();
            g_restFrame = HeadingFrame();
            g_nextGroomChange = now + static_cast<ULONGLONG>(RandomRange(1200.0, 3200.0));
            g_nextWingFlick = now + static_cast<ULONGLONG>(RandomRange(120.0, 650.0));
            g_wingFlickUntil = 0;
        }
    } else {
        if (now >= g_nextGroomChange) {
            g_grooming = !g_grooming;
            g_nextGroomChange = now + static_cast<ULONGLONG>(g_grooming
                ? RandomRange(1100.0, 3400.0) : RandomRange(300.0, 1300.0));
        }
        if (now >= g_nextWingFlick) {
            g_wingFlickUntil = now + static_cast<ULONGLONG>(RandomRange(140.0, 420.0));
            g_nextWingFlick = g_wingFlickUntil + static_cast<ULONGLONG>(RandomRange(350.0, 1800.0));
        }

        const bool rubPose = g_grooming && ((now / 83) % 2 == 0);
        const bool wingFlick = now < g_wingFlickUntil && ((now / 42) % 2 == 0);
        if (rubPose) {
            Present(wingFlick ? g_groomFlutterFrames[g_restFrame] : g_groomFrames[g_restFrame]);
        } else {
            Present(wingFlick ? g_flightFlutterFrames[g_restFrame] : g_flightFrames[g_restFrame]);
        }
        if (now >= g_stopUntil) StartFlight(false);
    }
}

void ConfigureDpi() {
    using SetContextFn = BOOL(WINAPI*)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setContext = reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setContext) {
        setContext(reinterpret_cast<HANDLE>(-4));
    } else {
        SetProcessDPIAware();
    }

    UINT dpi = 96;
    using GetDpiFn = UINT(WINAPI*)();
    auto getDpi = reinterpret_cast<GetDpiFn>(GetProcAddress(user32, "GetDpiForSystem"));
    if (getDpi) {
        dpi = getDpi();
    } else {
        HDC dc = GetDC(nullptr);
        if (dc) {
            dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
            ReleaseDC(nullptr, dc);
        }
    }
    if (dpi < 72) dpi = 96;
    g_dpiScale = static_cast<double>(dpi) / 96.0;
    g_pixelSize = static_cast<int>(std::lround(dpi * 13.0 / 25.4));
    g_pixelSize = std::clamp(g_pixelSize, 42, 104);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_NCHITTEST: {
            if (!g_currentBitmap) return HTTRANSPARENT;
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT windowRect{};
            GetWindowRect(window, &windowRect);
            const int localX = point.x - windowRect.left;
            const int localY = point.y - windowRect.top;
            if (localX < 0 || localY < 0 || localX >= g_pixelSize || localY >= g_pixelSize)
                return HTTRANSPARENT;
            Color pixel;
            if (g_currentBitmap->GetPixel(localX, localY, &pixel) != Ok || pixel.GetA() < 24)
                return HTTRANSPARENT;
            return HTCLIENT;
        }
        case WM_LBUTTONDOWN:
            StartFlight(true);
            return 0;
        case WM_TIMER:
            if (wParam == kAnimationTimer) Tick();
            return 0;
        case WM_DESTROY:
            KillTimer(window, kAnimationTimer);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

void Cleanup() {
    g_currentBitmap = nullptr;
    g_flightFrames.clear();
    g_groomFrames.clear();
    g_flightFlutterFrames.clear();
    g_groomFlutterFrames.clear();
    if (g_memoryDc) { DeleteDC(g_memoryDc); g_memoryDc = nullptr; }
    if (g_screenDc) { ReleaseDC(nullptr, g_screenDc); g_screenDc = nullptr; }
    if (g_gdiplusToken) { GdiplusShutdown(g_gdiplusToken); g_gdiplusToken = 0; }
    if (g_comInitialized) { CoUninitialize(); g_comInitialized = false; }
    if (g_mutex) { CloseHandle(g_mutex); g_mutex = nullptr; }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!g_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_mutex) CloseHandle(g_mutex);
        return 0;
    }

    ConfigureDpi();
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_comInitialized = SUCCEEDED(comResult);
    GdiplusStartupInput startupInput;
    if (GdiplusStartup(&g_gdiplusToken, &startupInput, nullptr) != Ok) {
        MessageBoxW(nullptr, L"Windows graphics could not be initialized.", L"Desktop Fly", MB_OK | MB_ICONERROR);
        Cleanup();
        return 1;
    }
    if (!BuildFrames()) {
        MessageBoxW(nullptr, L"The embedded fly images could not be loaded.", L"Desktop Fly", MB_OK | MB_ICONERROR);
        Cleanup();
        return 2;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        MessageBoxW(nullptr, L"The desktop animation window could not be registered.", L"Desktop Fly", MB_OK | MB_ICONERROR);
        Cleanup();
        return 3;
    }

    g_bounds.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_bounds.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_bounds.right = g_bounds.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_bounds.bottom = g_bounds.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    std::random_device randomDevice;
    g_random.seed(randomDevice() ^ static_cast<unsigned>(GetTickCount64()));
    g_x = RandomRange(g_bounds.left, std::max(g_bounds.left + 1, g_bounds.right - g_pixelSize));
    g_y = RandomRange(g_bounds.top, std::max(g_bounds.top + 1, g_bounds.bottom - g_pixelSize));

    g_window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass, L"", WS_POPUP, static_cast<int>(g_x), static_cast<int>(g_y),
        g_pixelSize, g_pixelSize, nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        MessageBoxW(nullptr, L"The desktop animation window could not be created.", L"Desktop Fly", MB_OK | MB_ICONERROR);
        Cleanup();
        return 4;
    }

    g_screenDc = GetDC(nullptr);
    g_memoryDc = CreateCompatibleDC(g_screenDc);
    if (!g_screenDc || !g_memoryDc) {
        MessageBoxW(nullptr, L"The transparent desktop graphics surface could not be created.", L"Desktop Fly", MB_OK | MB_ICONERROR);
        DestroyWindow(g_window);
        Cleanup();
        return 5;
    }

    SetWindowPos(g_window, HWND_TOPMOST, static_cast<int>(g_x), static_cast<int>(g_y),
                 g_pixelSize, g_pixelSize, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_startedAt = g_lastTick = GetTickCount64();
    StartFlight(false);
    Present(g_flightFrames[HeadingFrame()]);
    SetTimer(g_window, kAnimationTimer, kFrameIntervalMs, nullptr);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    Cleanup();
    return 0;
}
