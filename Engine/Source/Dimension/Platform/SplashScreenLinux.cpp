#include "Dimension/Platform/SplashScreen.h"
#include "Dimension/Core/Log.h"

#ifdef __linux__
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <poll.h>
#include <thread>

// The splash screen on Linux: Render's picture in an X11 window of its own (its own display connection, on its own
// thread), typed as a splash screen so window managers show it undecorated and on top. Under Wayland it goes through
// XWayland; without an X server (no DISPLAY) nothing is shown.
namespace Dimension::SplashScreen
{
    namespace
    {
        struct State
        {
            std::thread Thread;
            std::mutex Mutex;
            std::string Title, Subtitle, Step;
            std::atomic<bool> Quit{ false };
            std::chrono::steady_clock::time_point Start;
        };
        State& S()
        {
            static State s;
            return s;
        }

        // Xft.dpi (what desktops set for their scaling) / 96; 1 when unset.
        float DpiScale(Display* d)
        {
            float scale = 1.0f;
            XrmInitialize();
            if (const char* resources = XResourceManagerString(d))
                if (XrmDatabase db = XrmGetStringDatabase(resources))
                {
                    char* type = nullptr;
                    XrmValue value{};
                    if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value) && value.addr)
                        scale = (float)std::atof(value.addr) / 96.0f;
                    XrmDestroyDatabase(db);
                }
            return scale >= 0.5f && scale <= 4.0f ? scale : 1.0f;
        }

        // The primary monitor's rectangle through XRandR (loaded at run time: nothing more is linked), else the screen.
        void PrimaryMonitor(Display* d, ::Window root, int& x, int& y, int& w, int& h)
        {
            x = y = 0;
            w = DisplayWidth(d, DefaultScreen(d)), h = DisplayHeight(d, DefaultScreen(d));
            struct Monitor // XRRMonitorInfo
            {
                Atom name;
                Bool primary, automatic;
                int noutput, x, y, width, height, mwidth, mheight;
                void* outputs;
            };
            using GetMonitors = Monitor* (*)(Display*, ::Window, Bool, int*);
            using FreeMonitors = void (*)(Monitor*);
            void* lib = dlopen("libXrandr.so.2", RTLD_LAZY | RTLD_LOCAL);
            if (!lib) return;
            auto get = (GetMonitors)dlsym(lib, "XRRGetMonitors");
            auto release = (FreeMonitors)dlsym(lib, "XRRFreeMonitors");
            int count = 0;
            if (Monitor* monitors = get && release ? get(d, root, True, &count) : nullptr)
            {
                for (int i = 0; i < count; ++i)
                    if (monitors[i].primary || i == 0)
                        x = monitors[i].x, y = monitors[i].y, w = monitors[i].width, h = monitors[i].height;
                release(monitors);
            }
            dlclose(lib);
        }

        // Shifts that place 8-bit channels into a TrueColor visual's masks.
        int Shift(unsigned long mask)
        {
            int shift = 0;
            while (mask && !(mask & 1ul)) mask >>= 1, ++shift;
            int bits = 0;
            while (mask & 1ul) mask >>= 1, ++bits;
            return shift + bits - 8;
        }

        void Run()
        {
            State& s = S();
            Display* d = XOpenDisplay(nullptr);
            if (!d) return;
            int screen = DefaultScreen(d);
            ::Window root = RootWindow(d, screen);
            Visual* visual = DefaultVisual(d, screen);
            int depth = DefaultDepth(d, screen);
            if (visual->c_class != TrueColor || depth < 24)
            {
                XCloseDisplay(d);
                return;
            }
            float scale = DpiScale(d);
            std::string title, subtitle, status;
            {
                std::lock_guard lock(s.Mutex);
                title = s.Title, subtitle = s.Subtitle;
            }
            AppIcon::Image picture = Render(scale, title, subtitle, "", 0.0f);
            int w = picture.Width, h = picture.Height;
            int mx, my, mw, mh;
            PrimaryMonitor(d, root, mx, my, mw, mh);
            int x = mx + (mw - w) / 2, y = my + (mh - h) / 2;

            XSetWindowAttributes attributes{};
            attributes.background_pixel = BlackPixel(d, screen);
            attributes.event_mask = ExposureMask | ButtonPressMask | StructureNotifyMask;
            ::Window window = XCreateWindow(d, root, x, y, (unsigned)w, (unsigned)h, 0, depth, InputOutput, visual,
                                          CWBackPixel | CWEventMask, &attributes);
            auto atom = [&](const char* name) { return XInternAtom(d, name, False); };
            Atom utf8 = atom("UTF8_STRING");
            XStoreName(d, window, title.c_str());
            XChangeProperty(d, window, atom("_NET_WM_NAME"), utf8, 8, PropModeReplace, (const unsigned char*)title.data(), (int)title.size());
            Atom type = atom("_NET_WM_WINDOW_TYPE_SPLASH");
            XChangeProperty(d, window, atom("_NET_WM_WINDOW_TYPE"), XA_ATOM, 32, PropModeReplace, (const unsigned char*)&type, 1);
            long motif[5] = { 2, 0, 0, 0, 0 }; // _MOTIF_WM_HINTS: decorations given, none
            XChangeProperty(d, window, atom("_MOTIF_WM_HINTS"), atom("_MOTIF_WM_HINTS"), 32, PropModeReplace, (const unsigned char*)motif, 5);
            XClassHint* classHint = XAllocClassHint();
            classHint->res_name = (char*)"dimension-editor";
            classHint->res_class = (char*)"Dimension";
            XSetClassHint(d, window, classHint);
            XFree(classHint);
            XSizeHints* sizeHints = XAllocSizeHints();
            sizeHints->flags = PPosition | PMinSize | PMaxSize;
            sizeHints->x = x, sizeHints->y = y;
            sizeHints->min_width = sizeHints->max_width = w;
            sizeHints->min_height = sizeHints->max_height = h;
            XSetWMNormalHints(d, window, sizeHints);
            XFree(sizeHints);
            Cursor cursor = XCreateFontCursor(d, XC_watch);
            XDefineCursor(d, window, cursor);
            Atom deleteWindow = atom("WM_DELETE_WINDOW");
            XSetWMProtocols(d, window, &deleteWindow, 1);
            XMapRaised(d, window);

            // The picture in the visual's pixel format (32 bits per pixel for depth 24 and 32).
            int rs = Shift(visual->red_mask), gs = Shift(visual->green_mask), bs = Shift(visual->blue_mask);
            auto* pixels = (uint32_t*)std::malloc((size_t)w * h * 4);
            XImage* image = XCreateImage(d, visual, (unsigned)depth, ZPixmap, 0, (char*)pixels, (unsigned)w, (unsigned)h, 32, 0);
            GC gc = XCreateGC(d, window, 0, nullptr);
            auto place = [](uint8_t v, int shift, unsigned long mask) {
                unsigned long p = shift >= 0 ? (unsigned long)v << shift : (unsigned long)v >> -shift;
                return p & mask;
            };
            Atom moveResize = atom("_NET_WM_MOVERESIZE");
            bool mapped = false;
            while (!s.Quit && image)
            {
                while (XPending(d))
                {
                    XEvent e;
                    XNextEvent(d, &e);
                    if (e.type == MapNotify) mapped = true;
                    else if (e.type == ButtonPress && e.xbutton.button == Button1)
                    {
                        // Dragged by anywhere: hand the move to the window manager.
                        XUngrabPointer(d, CurrentTime);
                        XEvent m{};
                        m.xclient.type = ClientMessage;
                        m.xclient.window = window;
                        m.xclient.message_type = moveResize;
                        m.xclient.format = 32;
                        m.xclient.data.l[0] = e.xbutton.x_root;
                        m.xclient.data.l[1] = e.xbutton.y_root;
                        m.xclient.data.l[2] = 8; // _NET_WM_MOVERESIZE_MOVE
                        m.xclient.data.l[3] = Button1;
                        m.xclient.data.l[4] = 1;
                        XSendEvent(d, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &m);
                    }
                }
                if (mapped)
                {
                    {
                        std::lock_guard lock(s.Mutex);
                        status = s.Step;
                    }
                    float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - s.Start).count();
                    picture = Render(scale, title, subtitle, status, t / 1.6f); // a sweep every 1.6 s
                    for (size_t i = 0, n = (size_t)w * h; i < n; ++i)
                    {
                        const uint8_t* p = &picture.Rgba[i * 4];
                        pixels[i] = (uint32_t)(place(p[0], rs, visual->red_mask) | place(p[1], gs, visual->green_mask) |
                                               place(p[2], bs, visual->blue_mask) | (depth == 32 ? 0xFF000000u : 0u));
                    }
                    XPutImage(d, window, gc, image, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
                    XFlush(d);
                }
                pollfd fd{ ConnectionNumber(d), POLLIN, 0 };
                poll(&fd, 1, 30);
            }
            if (image) XDestroyImage(image); // (frees the pixels)
            else std::free(pixels);
            XFreeGC(d, gc);
            XFreeCursor(d, cursor);
            XDestroyWindow(d, window);
            XCloseDisplay(d);
        }
    }

    void Show(const std::string& title, const std::string& subtitle)
    {
        State& s = S();
        if (s.Thread.joinable()) return;
        const char* display = std::getenv("DISPLAY");
        if (!display || !*display) return;
        XInitThreads(); // (before any other Xlib call: GLFW's connection is used from the main thread meanwhile)
        {
            std::lock_guard lock(s.Mutex);
            s.Title = title;
            s.Subtitle = subtitle;
            s.Step.clear();
        }
        s.Quit = false;
        s.Start = std::chrono::steady_clock::now();
        s.Thread = std::thread(Run);
    }

    void SetStatus(const std::string& status)
    {
        State& s = S();
        std::lock_guard lock(s.Mutex);
        s.Step = status;
    }

    void Close()
    {
        State& s = S();
        if (!s.Thread.joinable()) return;
        s.Quit = true;
        s.Thread.join();
        Log::Trace("Splash screen shown for {:.2f} s", std::chrono::duration<float>(std::chrono::steady_clock::now() - s.Start).count());
    }

    bool IsShown() { return S().Thread.joinable(); }
}
#endif
