#include "Dimension/Platform/SplashScreen.h"
#include "Dimension/Core/Log.h"

#ifdef _WIN32
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace Dimension::SplashScreen
{
    namespace
    {
        constexpr UINT kStatusChanged = WM_APP + 1;

        struct State
        {
            std::thread Thread;
            std::mutex Mutex;
            std::wstring Title, Subtitle, Status;
            std::atomic<HWND> Window{ nullptr };
            std::chrono::steady_clock::time_point Start;
            float Dpi = 1.0f;
            std::vector<uint8_t> Background; // Background(Dpi) as top-down BGRA rows (the splash thread's)
            int BackgroundWidth = 0, BackgroundHeight = 0;
        };
        State& S()
        {
            static State s;
            return s;
        }

        std::wstring Wide(const std::string& text)
        {
            if (text.empty()) return {};
            int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
            std::wstring out(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n);
            return out;
        }

        int Px(float v) { return (int)std::lround(v * S().Dpi); }

        HFONT Font(int height, int weight)
        {
            return CreateFontW(-Px((float)height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        }

        void Text(HDC dc, const std::wstring& text, HFONT font, COLORREF color, RECT r, UINT format = DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX)
        {
            HGDIOBJ old = SelectObject(dc, font);
            SetTextColor(dc, color);
            DrawTextW(dc, text.c_str(), (int)text.size(), &r, format);
            SelectObject(dc, old);
        }

        void Fill(HDC dc, RECT r, COLORREF color)
        {
            HBRUSH b = CreateSolidBrush(color);
            FillRect(dc, &r, b);
            DeleteObject(b);
        }

        void Paint(HWND hwnd)
        {
            State& s = S();
            PAINTSTRUCT ps;
            HDC screen = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            int w = client.right, h = client.bottom;
            // Drawn off screen, then copied (no flicker as the bar moves).
            HDC dc = CreateCompatibleDC(screen);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, w, h);
            HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
            SetBkMode(dc, TRANSPARENT);

            // Background color, accent line and logo (drawn by SplashScreenImage.cpp, as on Linux).
            Fill(dc, client, RGB(30, 33, 41));
            if (!s.Background.empty())
            {
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = s.BackgroundWidth;
                bi.bmiHeader.biHeight = -s.BackgroundHeight; // top-down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                SetDIBitsToDevice(dc, 0, 0, s.BackgroundWidth, s.BackgroundHeight, 0, 0, 0, s.BackgroundHeight,
                                  s.Background.data(), &bi, DIB_RGB_COLORS);
            }
            std::wstring title, subtitle, status;
            {
                std::lock_guard lock(s.Mutex);
                title = s.Title, subtitle = s.Subtitle, status = s.Status;
            }
            using namespace Layout;
            HFONT big = Font(TitleSize, FW_SEMIBOLD), mid = Font(SubtitleSize, FW_NORMAL), note = Font(StatusSize, FW_NORMAL);
            int left = Px((float)TextLeft), right = Px((float)TextRight);
            Text(dc, title, big, RGB(236, 238, 242), { left, Px((float)TitleTop), right, Px((float)(SubtitleTop - 2)) });
            Text(dc, subtitle, mid, RGB(160, 168, 182), { left, Px((float)SubtitleTop), right, Px((float)(SubtitleTop + SubtitleSize + 8)) });

            // Status and a moving bar (loading has no known length: a segment sweeping across).
            int barTop = Px((float)BarTop), barH = Px((float)BarHeight);
            Text(dc, status.empty() ? L"Loading..." : status, note, RGB(190, 196, 206), { left, Px((float)StatusTop), right, barTop - Px(6) });
            Fill(dc, { left, barTop, right, barTop + barH }, RGB(48, 53, 64));
            float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - s.Start).count();
            float phase = std::fmod(t / 1.6f, 1.0f); // a sweep every 1.6 s
            int span = right - left, seg = span / 4;
            int x0 = left + (int)((span + seg) * phase) - seg;
            Fill(dc, { std::max(x0, left), barTop, std::min(x0 + seg, right), barTop + barH }, RGB(58, 121, 187));
            DeleteObject(big);
            DeleteObject(mid);
            DeleteObject(note);

            BitBlt(screen, 0, 0, w, h, dc, 0, 0, SRCCOPY);
            SelectObject(dc, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(dc);
            EndPaint(hwnd, &ps);
        }

        LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            switch (msg)
            {
            case WM_PAINT: Paint(hwnd); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_TIMER:
            case kStatusChanged: InvalidateRect(hwnd, nullptr, FALSE); return 0;
            case WM_NCHITTEST: return HTCAPTION; // dragged by anywhere
            case WM_CLOSE: DestroyWindow(hwnd); return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        void Run()
        {
            State& s = S();
            // Sharp on high-DPI screens (the process may not be DPI aware yet: GLFW sets that later).
            using SetContext = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
            if (auto set = (SetContext)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"))
                set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.style = CS_DROPSHADOW;
            wc.lpfnWndProc = Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_APPSTARTING);
            wc.lpszClassName = L"DimensionSplash";
            RegisterClassExW(&wc);
            // Centered on the primary monitor's work area, sized for its DPI.
            POINT origin{ 0, 0 };
            HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(monitor, &mi);
            using GetDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
            UINT dpiX = 96, dpiY = 96;
            if (HMODULE shcore = LoadLibraryW(L"shcore.dll"))
                if (auto get = (GetDpi)GetProcAddress(shcore, "GetDpiForMonitor")) get(monitor, 0, &dpiX, &dpiY);
            s.Dpi = dpiX / 96.0f;
            int w = Px((float)Layout::Width), h = Px((float)Layout::Height);
            {
                AppIcon::Image background = Background(s.Dpi);
                for (size_t i = 0; i + 3 < background.Rgba.size(); i += 4) std::swap(background.Rgba[i], background.Rgba[i + 2]); // to BGRA
                s.BackgroundWidth = background.Width, s.BackgroundHeight = background.Height;
                s.Background = std::move(background.Rgba);
            }
            RECT work = mi.rcWork;
            int x = (work.left + work.right - w) / 2, y = (work.top + work.bottom - h) / 2;
            std::wstring title;
            {
                std::lock_guard lock(s.Mutex);
                title = s.Title;
            }
            HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, title.c_str(), WS_POPUP, x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
            if (!hwnd) return;
            // Rounded corners on Windows 11 (DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2).
            using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
            if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
                if (auto set = (SetAttribute)GetProcAddress(dwm, "DwmSetWindowAttribute"))
                {
                    int round = 2;
                    set(hwnd, 33, &round, sizeof(round));
                }
            s.Window = hwnd;
            ShowWindow(hwnd, SW_SHOWNORMAL);
            UpdateWindow(hwnd);
            SetTimer(hwnd, 1, 30, nullptr);
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            s.Window = nullptr;
            UnregisterClassW(wc.lpszClassName, wc.hInstance);
        }
    }

    void Show(const std::string& title, const std::string& subtitle)
    {
        State& s = S();
        if (s.Thread.joinable()) return;
        {
            std::lock_guard lock(s.Mutex);
            s.Title = Wide(title);
            s.Subtitle = Wide(subtitle);
            s.Status.clear();
        }
        s.Start = std::chrono::steady_clock::now();
        s.Thread = std::thread(Run);
    }

    void SetStatus(const std::string& status)
    {
        State& s = S();
        {
            std::lock_guard lock(s.Mutex);
            s.Status = Wide(status);
        }
        if (HWND hwnd = s.Window) PostMessageW(hwnd, kStatusChanged, 0, 0);
    }

    void Close()
    {
        State& s = S();
        if (!s.Thread.joinable()) return;
        // (the window may still be on its way: wait briefly for it)
        for (int i = 0; i < 200 && !s.Window && s.Thread.joinable(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (HWND hwnd = s.Window) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        s.Thread.join();
        Log::Trace("Splash screen shown for {:.2f} s", std::chrono::duration<float>(std::chrono::steady_clock::now() - s.Start).count());
    }

    bool IsShown() { return S().Thread.joinable(); }
}
#elif !defined(__linux__) // (SplashScreenLinux.cpp)
namespace Dimension::SplashScreen
{
    void Show(const std::string&, const std::string&) {}
    void SetStatus(const std::string&) {}
    void Close() {}
    bool IsShown() { return false; }
}
#endif
