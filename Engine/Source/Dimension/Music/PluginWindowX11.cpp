#ifdef __linux__
#include "Dimension/Music/PluginWindowX11.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <algorithm>
#include <vector>

namespace Dimension::Music::X11
{
    struct WindowHandle
    {
        Window Id = 0;
        std::function<void()> OnClose;
        std::function<void(int, int)> OnResize;
        int Width = 0, Height = 0;
    };

    namespace
    {
        Display* s_Display = nullptr;
        bool s_Tried = false;
        Atom s_DeleteWindow = 0, s_NetWmName = 0, s_Utf8 = 0;
        std::vector<WindowHandle*> s_Windows;

        Display* Connection()
        {
            if (!s_Tried)
            {
                s_Tried = true;
                s_Display = XOpenDisplay(nullptr);
                if (s_Display)
                {
                    s_DeleteWindow = XInternAtom(s_Display, "WM_DELETE_WINDOW", False);
                    s_NetWmName = XInternAtom(s_Display, "_NET_WM_NAME", False);
                    s_Utf8 = XInternAtom(s_Display, "UTF8_STRING", False);
                }
            }
            return s_Display;
        }
    }

    bool Available() { return Connection() != nullptr; }

    WindowHandle* Create(const std::string& title, int width, int height, std::function<void()> onClose, std::function<void(int, int)> onResize)
    {
        Display* d = Connection();
        if (!d) return nullptr;
        int screen = DefaultScreen(d);
        XSetWindowAttributes attr{};
        attr.background_pixel = BlackPixel(d, screen);
        attr.event_mask = StructureNotifyMask;
        Window w = XCreateWindow(d, RootWindow(d, screen), 0, 0, (unsigned)std::max(1, width), (unsigned)std::max(1, height), 0, CopyFromParent, InputOutput,
                                 CopyFromParent, CWBackPixel | CWEventMask, &attr);
        if (!w) return nullptr;
        // Not resizable by dragging (the plug-in decides its size); a title in UTF-8; the close button asks first.
        XSizeHints* hints = XAllocSizeHints();
        hints->flags = PMinSize | PMaxSize;
        hints->min_width = hints->max_width = width;
        hints->min_height = hints->max_height = height;
        XSetWMNormalHints(d, w, hints);
        XFree(hints);
        XStoreName(d, w, title.c_str());
        XChangeProperty(d, w, s_NetWmName, s_Utf8, 8, PropModeReplace, (const unsigned char*)title.data(), (int)title.size());
        XSetWMProtocols(d, w, &s_DeleteWindow, 1);
        auto* handle = new WindowHandle{ w, std::move(onClose), std::move(onResize), width, height };
        s_Windows.push_back(handle);
        XFlush(d);
        return handle;
    }

    void Destroy(WindowHandle* w)
    {
        if (!w) return;
        std::erase(s_Windows, w);
        if (s_Display)
        {
            XDestroyWindow(s_Display, w->Id);
            XFlush(s_Display);
        }
        delete w;
    }

    void Show(WindowHandle* w)
    {
        if (!w || !s_Display) return;
        XMapRaised(s_Display, w->Id);
        XFlush(s_Display);
    }

    void SetClientSize(WindowHandle* w, int width, int height)
    {
        if (!w || !s_Display) return;
        XSizeHints* hints = XAllocSizeHints();
        hints->flags = PMinSize | PMaxSize;
        hints->min_width = hints->max_width = width;
        hints->min_height = hints->max_height = height;
        XSetWMNormalHints(s_Display, w->Id, hints);
        XFree(hints);
        XResizeWindow(s_Display, w->Id, (unsigned)std::max(1, width), (unsigned)std::max(1, height));
        XFlush(s_Display);
    }

    uint64_t NativeId(WindowHandle* w) { return w ? (uint64_t)w->Id : 0; }

    void Pump()
    {
        if (!s_Display) return;
        while (XPending(s_Display))
        {
            XEvent e;
            XNextEvent(s_Display, &e);
            auto it = std::find_if(s_Windows.begin(), s_Windows.end(), [&](WindowHandle* w) { return w->Id == e.xany.window; });
            if (it == s_Windows.end()) continue;
            WindowHandle* w = *it;
            if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == s_DeleteWindow)
            {
                if (w->OnClose) w->OnClose(); // (the owner destroys the window: w is gone after this)
                continue;
            }
            if (e.type == ConfigureNotify && (e.xconfigure.width != w->Width || e.xconfigure.height != w->Height))
            {
                w->Width = e.xconfigure.width;
                w->Height = e.xconfigure.height;
                if (w->OnResize) w->OnResize(w->Width, w->Height);
            }
        }
    }
}
#endif
