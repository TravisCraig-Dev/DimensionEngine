// VST3 hosting: modules, plug-in instances (processor + controller), their editor windows, and the rack a song's
// plug-ins live in. The only file that sees the VST3 SDK (compiled with RELEASE=1, which the SDK wants).
#include "Dimension/Music/Plugin.h"
#include "Dimension/Music/Song.h"
#include "Dimension/Core/Log.h"

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#pragma comment(lib, "ole32.lib")
#else
#include "Dimension/Music/PluginWindowX11.h"
#include <chrono>
#include <poll.h>
#endif

#include <algorithm>
#include <cstring>
#include <random>

namespace Dimension::Music
{
    using namespace Steinberg;
    using namespace Steinberg::Vst;

    namespace
    {
        std::string ToUtf8(const TChar* s) { return StringConvert::convert(std::u16string(reinterpret_cast<const char16_t*>(s))); }

        // One host object for every plug-in (its name, and the message/attribute objects it makes for them).
#ifndef _WIN32
        // The host's run loop for plug-in editors (Linux::IRunLoop): plug-ins register file descriptors (their own X
        // connections) and timers; PollEvents runs them each frame.
        struct RunLoop
        {
            std::vector<std::pair<int, IPtr<Linux::IEventHandler>>> Fds;
            struct Timer
            {
                IPtr<Linux::ITimerHandler> Handler;
                std::chrono::milliseconds Interval;
                std::chrono::steady_clock::time_point Due;
            };
            std::vector<Timer> Timers;
        };
        RunLoop& Loop()
        {
            static RunLoop loop;
            return loop;
        }

        class HostRunLoop : public U::ImplementsNonDestroyable<U::Directly<Linux::IRunLoop>>
        {
        public:
            tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* handler, Linux::FileDescriptor fd) override
            {
                if (!handler) return kInvalidArgument;
                Loop().Fds.emplace_back(fd, handler);
                return kResultTrue;
            }
            tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* handler) override
            {
                auto& fds = Loop().Fds;
                size_t before = fds.size();
                std::erase_if(fds, [&](const auto& f) { return f.second.get() == handler; });
                return fds.size() != before ? kResultTrue : kResultFalse;
            }
            tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler, Linux::TimerInterval ms) override
            {
                if (!handler || ms == 0) return kInvalidArgument;
                auto interval = std::chrono::milliseconds(ms);
                Loop().Timers.push_back({ handler, interval, std::chrono::steady_clock::now() + interval });
                return kResultTrue;
            }
            tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override
            {
                auto& timers = Loop().Timers;
                size_t before = timers.size();
                std::erase_if(timers, [&](const auto& t) { return t.Handler.get() == handler; });
                return timers.size() != before ? kResultTrue : kResultFalse;
            }
        };
        HostRunLoop& TheRunLoop()
        {
            static HostRunLoop loop;
            return loop;
        }

        // The host application object plug-ins get when they start: it also answers for the run loop (plug-ins built
        // with VSTGUI take it from there as they initialize, before any editor opens).
        class LinuxHostApplication : public HostApplication
        {
        public:
            tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
            {
                if (FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid))
                {
                    *obj = static_cast<Linux::IRunLoop*>(&TheRunLoop());
                    return kResultOk;
                }
                return HostApplication::queryInterface(iid, obj);
            }
        };
#endif

        FUnknown* HostContext()
        {
#ifdef _WIN32
            // Plug-ins (VSTGUI: Direct2D, drag and drop) expect COM/OLE on the host's thread before their modules load.
            // A thread already in another apartment (the C# runtime makes the main thread MTA) keeps it.
            static thread_local bool ole = [] {
                OleInitialize(nullptr);
                return true;
            }();
            (void)ole;
#endif
#ifdef _WIN32
            static IPtr<HostApplication> host = owned(new HostApplication());
#else
            static IPtr<HostApplication> host = owned(new LinuxHostApplication());
#endif
            static bool set = [] {
                PluginContextFactory::instance().setPluginContext(host.get());
                return true;
            }();
            (void)set;
            return host.get();
        }

        // Modules stay loaded once used (instances and scans share them; unloading under a live instance would crash).
        std::mutex s_ModuleMutex;
        std::map<std::string, VST3::Hosting::Module::Ptr> s_Modules;

        VST3::Hosting::Module::Ptr LoadModule(const fs::path& path, std::string& error)
        {
            HostContext();
            std::string key = fs::path(path).lexically_normal().string();
            std::lock_guard lock(s_ModuleMutex);
            if (auto it = s_Modules.find(key); it != s_Modules.end()) return it->second;
            auto module = VST3::Hosting::Module::create(path.string(), error);
            if (module)
            {
                // The factory gets the host context as hosts give it (plug-ins built with VSTGUI take the Linux run loop
                // from it here).
                module->getFactory().setHostContext(HostContext());
                s_Modules[key] = module;
            }
            return module;
        }

        std::atomic<int> s_OpenEditors{ 0 }; // (editor windows open: the editor keeps its frame rate up for them)

        bool IsAudioClass(const VST3::Hosting::ClassInfo& info) { return info.category() == kVstAudioEffectClass; }
        bool IsInstrumentClass(const VST3::Hosting::ClassInfo& info) { return info.subCategoriesString().find("Instrument") != std::string::npos; }
    }

    // ---- Finding plug-ins ----

    bool Plugins::IsSupported()
    {
#if defined(_WIN32) || defined(__linux__)
        return true;
#else
        return false;
#endif
    }

    std::vector<fs::path> Plugins::DefaultFolders()
    {
        std::vector<fs::path> out;
#ifdef _WIN32
        if (const char* local = std::getenv("LOCALAPPDATA")) out.push_back(fs::path(local) / "Programs" / "Common" / "VST3");
        if (const char* common = std::getenv("CommonProgramFiles")) out.push_back(fs::path(common) / "VST3");
#else
        if (const char* home = std::getenv("HOME")) out.push_back(fs::path(home) / ".vst3");
        out.push_back("/usr/lib/vst3");
        out.push_back("/usr/local/lib/vst3");
#endif
        return out;
    }

    std::vector<PluginInfo> Plugins::ScanModule(const fs::path& module, std::string* error)
    {
        std::vector<PluginInfo> out;
        std::string err;
        auto m = LoadModule(module, err);
        if (!m)
        {
            if (error) *error = module.filename().string() + ": " + (err.empty() ? "cannot be loaded" : err);
            return out;
        }
        for (const auto& info : m->getFactory().classInfos())
        {
            if (!IsAudioClass(info)) continue;
            PluginInfo p;
            p.Name = info.name();
            p.Vendor = info.vendor();
            p.Version = info.version();
            p.ClassId = info.ID().toString();
            p.SubCategories = info.subCategoriesString();
            p.Module = module;
            p.Instrument = IsInstrumentClass(info);
            out.push_back(std::move(p));
        }
        return out;
    }

    std::vector<PluginInfo> Plugins::Scan(const std::vector<fs::path>& folders, std::vector<std::string>* errors)
    {
        std::vector<PluginInfo> out;
        for (const fs::path& folder : folders)
        {
            std::error_code ec;
            if (!fs::is_directory(folder, ec)) continue;
            // A .vst3 bundle is a folder: it is the module (not searched inside); a plain .vst3 file is one too.
            fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec))
            {
                if (it->path().extension() != ".vst3") continue;
                if (it->is_directory(ec)) it.disable_recursion_pending();
                std::string error;
                auto found = ScanModule(it->path(), &error);
                if (!error.empty() && errors) errors->push_back(error);
                out.insert(out.end(), found.begin(), found.end());
            }
        }
        std::sort(out.begin(), out.end(), [](const PluginInfo& a, const PluginInfo& b) { return a.Name < b.Name; });
        return out;
    }

    // ---- Editor windows ----

    namespace
    {
#ifdef _WIN32
        // A top-level window (owned by the editor's) holding a plug-in's view.
        class PluginWindow
        {
        public:
            std::function<void()> OnClose;
            std::function<void(int, int)> OnResize;

            static std::unique_ptr<PluginWindow> Create(const std::string& title, int width, int height)
            {
                static const wchar_t* kClass = L"DimensionPluginWindow";
                static bool registered = [] {
                    WNDCLASSEXW wc{ sizeof(wc) };
                    wc.lpfnWndProc = Proc;
                    wc.hInstance = GetModuleHandleW(nullptr);
                    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
                    wc.lpszClassName = kClass;
                    return RegisterClassExW(&wc) != 0;
                }();
                if (!registered) return nullptr;
                auto w = std::unique_ptr<PluginWindow>(new PluginWindow());
                DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
                RECT r{ 0, 0, width, height };
                AdjustWindowRect(&r, style, FALSE);
                std::wstring wtitle(title.begin(), title.end());
                int n = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
                if (n > 0)
                {
                    wtitle.resize(n - 1);
                    MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wtitle.data(), n);
                }
                w->m_Hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, wtitle.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                            GetActiveWindow(), nullptr, GetModuleHandleW(nullptr), nullptr);
                if (!w->m_Hwnd) return nullptr;
                w->m_Style = style;
                SetWindowLongPtrW(w->m_Hwnd, GWLP_USERDATA, (LONG_PTR)w.get());
                return w;
            }
            ~PluginWindow()
            {
                if (m_Hwnd)
                {
                    SetWindowLongPtrW(m_Hwnd, GWLP_USERDATA, 0);
                    DestroyWindow(m_Hwnd);
                }
            }
            void Show() { ShowWindow(m_Hwnd, SW_SHOWNORMAL); }
            void* Native() const { return m_Hwnd; }
            void SetClientSize(int width, int height)
            {
                RECT r{ 0, 0, width, height };
                AdjustWindowRect(&r, m_Style, FALSE);
                SetWindowPos(m_Hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            static const char* PlatformType() { return kPlatformTypeHWND; }

        private:
            static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                auto* w = (PluginWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
                if (w && msg == WM_CLOSE)
                {
                    if (w->OnClose) w->OnClose(); // (the instance destroys the window)
                    return 0;
                }
                if (w && msg == WM_SIZE && w->OnResize) w->OnResize(LOWORD(lp), HIWORD(lp));
                return DefWindowProcW(hwnd, msg, wp, lp);
            }
            HWND m_Hwnd = nullptr;
            DWORD m_Style = 0;
        };
#else
        // Linux: a top-level X11 window the plug-in's view embeds itself in (PluginWindowX11.cpp).
        class PluginWindow
        {
        public:
            std::function<void()> OnClose;
            std::function<void(int, int)> OnResize;

            static std::unique_ptr<PluginWindow> Create(const std::string& title, int width, int height)
            {
                auto w = std::unique_ptr<PluginWindow>(new PluginWindow());
                PluginWindow* self = w.get();
                w->m_Handle = X11::Create(title, width, height, [self] { if (self->OnClose) self->OnClose(); },
                                          [self](int cw, int ch) { if (self->OnResize) self->OnResize(cw, ch); });
                if (!w->m_Handle) return nullptr;
                return w;
            }
            ~PluginWindow() { X11::Destroy(m_Handle); }
            void Show() { X11::Show(m_Handle); }
            void* Native() const { return (void*)(uintptr_t)X11::NativeId(m_Handle); }
            void SetClientSize(int width, int height) { X11::SetClientSize(m_Handle, width, height); }
            static const char* PlatformType() { return kPlatformTypeX11EmbedWindowID; }

        private:
            X11::WindowHandle* m_Handle = nullptr;
        };

#endif
    }

    // ---- Instances ----

    namespace
    {
        class Vst3Instance;

        // The controller tells the host when its editor moves a parameter: the processor gets it too.
        class ComponentHandler : public U::Implements<U::Directly<IComponentHandler>>
        {
        public:
            explicit ComponentHandler(Vst3Instance* owner) : m_Owner(owner) {}
            tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
            tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override;
            tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
            tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
            Vst3Instance* m_Owner;
        };

        // The editor window asks for a new size; on Linux the frame is also the host's run loop.
#ifdef _WIN32
        class PlugFrame : public U::Implements<U::Directly<IPlugFrame>>
#else
        class PlugFrame : public U::Implements<U::Directly<IPlugFrame, Linux::IRunLoop>>
#endif
        {
        public:
            explicit PlugFrame(Vst3Instance* owner) : m_Owner(owner) {}
            tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override;
#ifndef _WIN32
            tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* handler, Linux::FileDescriptor fd) override
            {
                return TheRunLoop().registerEventHandler(handler, fd);
            }
            tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* handler) override { return TheRunLoop().unregisterEventHandler(handler); }
            tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler, Linux::TimerInterval ms) override
            {
                return TheRunLoop().registerTimer(handler, ms);
            }
            tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override { return TheRunLoop().unregisterTimer(handler); }
#endif
            Vst3Instance* m_Owner;
        };

        class Vst3Instance : public PluginInstance
        {
        public:
            bool Init(const fs::path& module, const std::string& classId, double sampleRate, int maxBlock, std::string& error)
            {
                m_Module = LoadModule(module, error);
                if (!m_Module) return false;
                auto uid = VST3::UID::fromString(classId);
                const VST3::Hosting::ClassInfo* found = nullptr;
                auto infos = m_Module->getFactory().classInfos();
                for (const auto& info : infos)
                    if (IsAudioClass(info) && uid && info.ID() == *uid) found = &info;
                if (!found)
                {
                    error = "no plug-in " + classId + " in " + module.filename().string();
                    return false;
                }
                m_Name = found->name();
                m_Instrument = IsInstrumentClass(*found);
                m_Provider = owned(new PlugProvider(m_Module->getFactory(), *found, true));
                if (!m_Provider->initialize())
                {
                    error = "'" + m_Name + "' would not start";
                    return false;
                }
                m_Component = m_Provider->getComponentPtr();
                m_Controller = m_Provider->getControllerPtr();
                m_Processor = U::cast<IAudioProcessor>(m_Component);
                if (!m_Component || !m_Processor)
                {
                    error = "'" + m_Name + "' has no audio processor";
                    return false;
                }
                m_Handler = owned(new ComponentHandler(this));
                if (m_Controller) m_Controller->setComponentHandler(m_Handler);

                // Stereo main buses (inputs only for effects), the first event input (notes) on.
                m_SampleRate = sampleRate;
                m_MaxBlock = std::max(16, maxBlock);
                int32 ins = m_Component->getBusCount(kAudio, kInput), outs = m_Component->getBusCount(kAudio, kOutput);
                std::vector<SpeakerArrangement> inArr(ins, SpeakerArr::kStereo), outArr(outs, SpeakerArr::kStereo);
                m_Processor->setBusArrangements(inArr.data(), ins, outArr.data(), outs);
                for (int32 i = 0; i < ins; ++i) m_Component->activateBus(kAudio, kInput, i, i == 0);
                for (int32 i = 0; i < outs; ++i) m_Component->activateBus(kAudio, kOutput, i, i == 0);
                if (m_Component->getBusCount(kEvent, kInput) > 0) m_Component->activateBus(kEvent, kInput, 0, true);
                ProcessSetup setup{ kRealtime, kSample32, m_MaxBlock, sampleRate };
                if (m_Processor->setupProcessing(setup) != kResultOk)
                {
                    error = "'" + m_Name + "' cannot process at " + std::to_string((int)sampleRate) + " Hz";
                    return false;
                }
                m_Data.prepare(*m_Component, 0, kSample32); // (0: the buses get our buffers each block, not their own)
                m_Data.inputEvents = &m_Events;
                m_Data.inputParameterChanges = &m_Changes;
                m_Data.processContext = &m_Context;
                m_Changes.setMaxParameters(512);
                m_HasInput = ins > 0;
                for (auto& b : m_Buffers) b.assign(m_MaxBlock, 0.0f);
                m_Component->setActive(true);
                m_Processor->setProcessing(true);
                m_Active = true;
                SyncControllerState();
                return true;
            }

            ~Vst3Instance() override
            {
                CloseEditor();
                if (m_Active)
                {
                    m_Processor->setProcessing(false);
                    m_Component->setActive(false);
                }
                if (m_Controller) m_Controller->setComponentHandler(nullptr);
                m_Data.unprepare();
                m_Processor = nullptr;
                m_Controller = nullptr;
                m_Component = nullptr;
                m_Provider = nullptr; // (terminates and disconnects the processor and controller)
            }

            const std::string& GetName() const override { return m_Name; }
            bool IsInstrument() const override { return m_Instrument; }
            double GetSampleRate() const override { return m_SampleRate; }

            // ---- State ----
            bool GetState(std::vector<uint8_t>& component, std::vector<uint8_t>& controller) override
            {
                std::lock_guard lock(m_StateMutex);
                auto read = [](auto* obj, std::vector<uint8_t>& out) {
                    out.clear();
                    if (!obj) return true;
                    IPtr<MemoryStream> stream = owned(new MemoryStream());
                    if (obj->getState(stream) != kResultOk) return false;
                    out.assign((const uint8_t*)stream->getData(), (const uint8_t*)stream->getData() + stream->getSize());
                    return true;
                };
                bool ok = read(m_Component.get(), component);
                read(m_Controller.get(), controller);
                return ok;
            }

            bool SetState(const std::vector<uint8_t>& component, const std::vector<uint8_t>& controller) override
            {
                std::lock_guard lock(m_StateMutex);
                bool ok = true;
                if (!component.empty())
                {
                    IPtr<MemoryStream> stream = owned(new MemoryStream());
                    stream->write((void*)component.data(), (int32)component.size(), nullptr);
                    stream->seek(0, IBStream::kIBSeekSet, nullptr);
                    ok = m_Component->setState(stream) == kResultOk;
                    if (m_Controller)
                    {
                        stream->seek(0, IBStream::kIBSeekSet, nullptr);
                        m_Controller->setComponentState(stream);
                    }
                }
                if (!controller.empty() && m_Controller)
                {
                    IPtr<MemoryStream> stream = owned(new MemoryStream());
                    stream->write((void*)controller.data(), (int32)controller.size(), nullptr);
                    stream->seek(0, IBStream::kIBSeekSet, nullptr);
                    m_Controller->setState(stream);
                }
                return ok;
            }

            void SyncControllerState()
            {
                // The controller starts from the processor's state (as hosts do after making both).
                if (!m_Controller) return;
                IPtr<MemoryStream> stream = owned(new MemoryStream());
                if (m_Component->getState(stream) != kResultOk) return;
                stream->seek(0, IBStream::kIBSeekSet, nullptr);
                m_Controller->setComponentState(stream);
            }

            // ---- Parameters ----
            std::vector<PluginParameter> Parameters() override
            {
                std::vector<PluginParameter> out;
                if (!m_Controller) return out;
                int32 count = m_Controller->getParameterCount();
                for (int32 i = 0; i < count; ++i)
                {
                    ParameterInfo info{};
                    if (m_Controller->getParameterInfo(i, info) != kResultOk) continue;
                    PluginParameter p;
                    p.Id = info.id;
                    p.Title = ToUtf8(info.title);
                    p.Units = ToUtf8(info.units);
                    p.Default = info.defaultNormalizedValue;
                    p.Steps = info.stepCount;
                    p.Automatable = (info.flags & ParameterInfo::kCanAutomate) != 0;
                    p.ReadOnly = (info.flags & ParameterInfo::kIsReadOnly) != 0;
                    p.Hidden = (info.flags & ParameterInfo::kIsHidden) != 0;
                    p.Bypass = (info.flags & ParameterInfo::kIsBypass) != 0;
                    out.push_back(std::move(p));
                }
                return out;
            }

            double GetParameter(uint32_t id) override { return m_Controller ? m_Controller->getParamNormalized(id) : 0.0; }

            void SetParameter(uint32_t id, double value) override
            {
                if (m_Controller) m_Controller->setParamNormalized(id, value);
                PushToProcessor(id, value);
            }

            std::string FormatParameter(uint32_t id, double value) override
            {
                if (!m_Controller) return {};
                String128 text{};
                if (m_Controller->getParamStringByValue(id, value, text) != kResultOk) return {};
                return ToUtf8(text);
            }

            // From the main thread (the UI, the plug-in's editor) to the processor's next block.
            void PushToProcessor(uint32_t id, double value)
            {
                std::lock_guard lock(m_PendingMutex);
                for (auto& p : m_Pending)
                    if (p.first == id)
                    {
                        p.second = value;
                        return;
                    }
                m_Pending.emplace_back(id, value);
            }

            void OnEdit(uint32_t id, double value)
            {
                PushToProcessor(id, value);
                m_Edited = true;
            }

            // ---- Audio thread ----
            void QueueNote(int key, float velocity, bool on, int frame) override
            {
                Event e{};
                e.busIndex = 0;
                e.sampleOffset = std::max(0, frame);
                if (on)
                {
                    e.type = Event::kNoteOnEvent;
                    e.noteOn.channel = 0;
                    e.noteOn.pitch = (int16)std::clamp(key, 0, 127);
                    e.noteOn.velocity = std::clamp(velocity, 0.0f, 1.0f);
                    e.noteOn.noteId = -1;
                }
                else
                {
                    e.type = Event::kNoteOffEvent;
                    e.noteOff.channel = 0;
                    e.noteOff.pitch = (int16)std::clamp(key, 0, 127);
                    e.noteOff.velocity = 0.0f;
                    e.noteOff.noteId = -1;
                }
                m_Events.addEvent(e);
            }

            void QueueParameter(uint32_t id, double value, int frame) override
            {
                int32 index = 0;
                if (IParamValueQueue* q = m_Changes.addParameterData(id, index))
                {
                    int32 point = 0;
                    q->addPoint(std::max(0, frame), std::clamp(value, 0.0, 1.0), point);
                }
            }

            void AllNotesOff() override
            {
                for (int key = 0; key < 128; ++key) QueueNote(key, 0.0f, false, 0);
            }

            void Process(float* stereo, uint32_t frames, const PluginTransport& t) override
            {
                if (!m_Active || frames == 0) return;
                frames = std::min<uint32_t>(frames, (uint32_t)m_MaxBlock);
                // The main thread's parameter changes (skipped this block if it is writing them right now).
                if (m_PendingMutex.try_lock())
                {
                    for (auto& [id, value] : m_Pending) QueueParameter(id, value, 0);
                    m_Pending.clear();
                    m_PendingMutex.unlock();
                }
                std::unique_lock stateLock(m_StateMutex, std::try_to_lock);
                if (!stateLock.owns_lock())
                {
                    // (its state is being saved or loaded on the main thread: this block passes through)
                    if (m_Instrument) std::fill(stereo, stereo + frames * 2, 0.0f);
                    m_Events.clear();
                    m_Changes.clearQueue();
                    return;
                }
                for (uint32_t f = 0; f < frames; ++f)
                {
                    m_Buffers[0][f] = stereo[f * 2];
                    m_Buffers[1][f] = stereo[f * 2 + 1];
                    m_Buffers[2][f] = m_Buffers[3][f] = 0.0f;
                }
                Sample32* ins[2] = { m_Buffers[0].data(), m_Buffers[1].data() };
                Sample32* outs[2] = { m_Buffers[2].data(), m_Buffers[3].data() };
                if (m_HasInput && m_Data.numInputs > 0) m_Data.setChannelBuffers(kInput, 0, ins, 2);
                if (m_Data.numOutputs > 0) m_Data.setChannelBuffers(kOutput, 0, outs, 2);
                m_Context = {};
                m_Context.state = ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid | ProcessContext::kTimeSigValid |
                                  (t.Playing ? ProcessContext::kPlaying : 0);
                m_Context.sampleRate = m_SampleRate;
                m_Context.tempo = t.Tempo;
                m_Context.projectTimeMusic = t.Quarters;
                m_Context.projectTimeSamples = t.Samples;
                m_Context.timeSigNumerator = t.BeatsPerBar;
                m_Context.timeSigDenominator = 4;
                m_Context.continousTimeSamples = m_Continuous;
                m_Continuous += frames;
                m_Data.numSamples = (int32)frames;
                if (m_Processor->process(m_Data) == kResultOk && m_Data.numOutputs > 0)
                    for (uint32_t f = 0; f < frames; ++f)
                    {
                        stereo[f * 2] = m_Buffers[2][f];
                        stereo[f * 2 + 1] = m_Buffers[3][f];
                    }
                else if (m_Instrument)
                    std::fill(stereo, stereo + frames * 2, 0.0f);
                m_Events.clear();
                m_Changes.clearQueue();
            }

            // ---- Editor ----
            bool HasEditor() override
            {
                if (!m_Controller) return false;
#ifndef _WIN32
                if (!X11::Available()) return false; // (no X display to put a window on)
#endif
                IPtr<IPlugView> view = owned(m_Controller->createView(ViewType::kEditor));
                return view && view->isPlatformTypeSupported(PluginWindow::PlatformType()) == kResultTrue;
            }

            bool OpenEditor(const std::string& title) override
            {
                if (m_Window) return true;
                if (!m_Controller) return false;
                m_View = owned(m_Controller->createView(ViewType::kEditor));
                if (!m_View || m_View->isPlatformTypeSupported(PluginWindow::PlatformType()) != kResultTrue)
                {
                    m_View = nullptr;
                    return false;
                }
                ViewRect r{ 0, 0, 640, 400 };
                m_View->getSize(&r);
                m_Window = PluginWindow::Create(title, std::max(100, r.getWidth()), std::max(60, r.getHeight()));
                if (!m_Window)
                {
                    m_View = nullptr;
                    return false;
                }
                m_Frame = owned(new PlugFrame(this));
                m_View->setFrame(m_Frame);
                tresult attachedResult = m_View->attached(m_Window->Native(), PluginWindow::PlatformType());
                if (attachedResult != kResultOk)
                {
                    m_View->setFrame(nullptr);
                    m_View = nullptr;
                    m_Window.reset();
                    return false;
                }
                m_Window->OnClose = [this] { CloseEditor(); };
                m_Window->OnResize = [this](int w, int h) {
                    if (!m_View || m_Resizing) return;
                    ViewRect now{};
                    m_View->getSize(&now);
                    if (now.getWidth() == w && now.getHeight() == h) return;
                    ViewRect want{ 0, 0, w, h };
                    m_View->onSize(&want);
                };
                m_Window->Show();
                ++s_OpenEditors;
                return true;
            }

            void CloseEditor() override
            {
                if (m_View)
                {
                    m_View->setFrame(nullptr);
                    m_View->removed();
                    m_View = nullptr;
                }
                if (m_Window) --s_OpenEditors;
                m_Window.reset();
            }

            bool IsEditorOpen() const override { return m_Window != nullptr; }

            tresult ResizeView(IPlugView* view, ViewRect* size)
            {
                if (!size || view != m_View.get() || !m_Window) return kInvalidArgument;
                m_Resizing = true;
                m_Window->SetClientSize(size->getWidth(), size->getHeight());
                m_Resizing = false;
                view->onSize(size);
                return kResultTrue;
            }

        private:
            VST3::Hosting::Module::Ptr m_Module;
            IPtr<PlugProvider> m_Provider;
            IPtr<IComponent> m_Component;
            IPtr<IAudioProcessor> m_Processor;
            IPtr<IEditController> m_Controller;
            IPtr<ComponentHandler> m_Handler;
            IPtr<PlugFrame> m_Frame;
            IPtr<IPlugView> m_View;
            std::unique_ptr<PluginWindow> m_Window;
            bool m_Resizing = false;
            std::string m_Name;
            bool m_Instrument = false, m_HasInput = false, m_Active = false;
            double m_SampleRate = 48000.0;
            int m_MaxBlock = 512;
            HostProcessData m_Data;
            EventList m_Events{ 512 };
            ParameterChanges m_Changes;
            ProcessContext m_Context{};
            int64 m_Continuous = 0;
            std::vector<float> m_Buffers[4]; // in L, in R, out L, out R
            std::mutex m_PendingMutex, m_StateMutex;
            std::vector<std::pair<uint32_t, double>> m_Pending;
        };

        tresult PLUGIN_API ComponentHandler::performEdit(ParamID id, ParamValue value)
        {
            m_Owner->OnEdit(id, value);
            return kResultOk;
        }

        tresult PLUGIN_API PlugFrame::resizeView(IPlugView* view, ViewRect* newSize) { return m_Owner->ResizeView(view, newSize); }
    }

    std::shared_ptr<PluginInstance> PluginInstance::Create(const fs::path& module, const std::string& classId, double sampleRate, int maxBlock, std::string& error)
    {
        auto instance = std::make_shared<Vst3Instance>();
        if (!instance->Init(module, classId, sampleRate, maxBlock, error))
        {
            if (error.empty()) error = "the plug-in could not be loaded";
            return nullptr;
        }
        return instance;
    }

    void Plugins::PollEvents()
    {
#ifndef _WIN32
        X11::Pump();
        // Copies: handlers may (un)register others while they run.
        auto fds = Loop().Fds;
        if (!fds.empty())
        {
            std::vector<pollfd> polls;
            for (auto& [fd, handler] : fds) polls.push_back({ fd, POLLIN, 0 });
            if (::poll(polls.data(), (nfds_t)polls.size(), 0) > 0)
                for (size_t i = 0; i < polls.size(); ++i)
                    if (polls[i].revents & (POLLIN | POLLHUP | POLLERR)) fds[i].second->onFDIsSet(fds[i].first);
        }
        auto now = std::chrono::steady_clock::now();
        auto timers = Loop().Timers;
        for (auto& t : timers)
            if (now >= t.Due)
            {
                for (auto& live : Loop().Timers)
                    if (live.Handler == t.Handler) live.Due = now + live.Interval;
                t.Handler->onTimer();
            }
#endif
    }

    bool Plugins::AnyEditorOpen() { return s_OpenEditors.load() > 0; }

    // ---- The rack ----

    std::shared_ptr<PluginInstance> PluginRack::Get(uint64_t id) const
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_Instances.find(id);
        return it != m_Instances.end() ? it->second : nullptr;
    }

    std::shared_ptr<PluginInstance> PluginRack::Ensure(uint64_t id, const fs::path& module, const std::string& classId, const std::string& state,
                                                       const std::string& controllerState)
    {
        if (auto existing = Get(id)) return existing;
        if (m_Failed.contains(id)) return nullptr;
        std::string error;
        auto instance = PluginInstance::Create(module, classId, m_SampleRate, m_MaxBlock, error);
        if (!instance)
        {
            m_Failed[id] = error;
            m_Errors.push_back(error);
            Log::Warn("Plug-in: {}", error);
            return nullptr;
        }
        if (!state.empty() || !controllerState.empty()) instance->SetState(Base64Decode(state), Base64Decode(controllerState));
        std::lock_guard lock(m_Mutex);
        m_Instances[id] = instance;
        return instance;
    }

    void PluginRack::Keep(const std::vector<uint64_t>& ids)
    {
        std::vector<std::shared_ptr<PluginInstance>> dropped; // (let go outside the lock)
        std::lock_guard lock(m_Mutex);
        for (auto it = m_Instances.begin(); it != m_Instances.end();)
            if (std::find(ids.begin(), ids.end(), it->first) == ids.end())
            {
                dropped.push_back(it->second);
                it = m_Instances.erase(it);
            }
            else ++it;
    }

    void PluginRack::Sync(const Song& song)
    {
        std::vector<uint64_t> ids;
        for (const PluginRef* p : song.Plugins())
        {
            ids.push_back(p->Id);
            Ensure(p->Id, fs::path(p->Module), p->ClassId, p->State, p->ControllerState);
        }
        Keep(ids);
    }

    void PluginRack::StoreStates(Song& song)
    {
        for (PluginRef* p : song.Plugins())
            if (auto instance = Get(p->Id))
            {
                std::vector<uint8_t> component, controller;
                if (instance->GetState(component, controller))
                {
                    p->State = Base64Encode(component);
                    p->ControllerState = Base64Encode(controller);
                }
            }
    }

    bool PluginRack::TakeEdited()
    {
        bool edited = false;
        std::lock_guard lock(m_Mutex);
        for (auto& [id, instance] : m_Instances) edited |= instance->TakeEdited();
        return edited;
    }

    void PluginRack::Clear()
    {
        std::map<uint64_t, std::shared_ptr<PluginInstance>> dropped;
        {
            std::lock_guard lock(m_Mutex);
            dropped.swap(m_Instances);
        }
        m_Failed.clear();
        m_Errors.clear();
    }

    // ---- Helpers ----

    std::string Base64Encode(const std::vector<uint8_t>& bytes)
    {
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((bytes.size() + 2) / 3 * 4);
        for (size_t i = 0; i < bytes.size(); i += 3)
        {
            uint32_t v = (uint32_t)bytes[i] << 16;
            if (i + 1 < bytes.size()) v |= (uint32_t)bytes[i + 1] << 8;
            if (i + 2 < bytes.size()) v |= bytes[i + 2];
            out += table[(v >> 18) & 63];
            out += table[(v >> 12) & 63];
            out += i + 1 < bytes.size() ? table[(v >> 6) & 63] : '=';
            out += i + 2 < bytes.size() ? table[v & 63] : '=';
        }
        return out;
    }

    std::vector<uint8_t> Base64Decode(const std::string& text)
    {
        auto value = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };
        std::vector<uint8_t> out;
        uint32_t acc = 0;
        int bits = 0;
        for (char c : text)
        {
            int v = value(c);
            if (v < 0) continue;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out.push_back((uint8_t)((acc >> bits) & 0xFF));
            }
        }
        return out;
    }

    uint64_t NewPluginId()
    {
        static std::mt19937_64 rng{ std::random_device{}() };
        uint64_t id = 0;
        while (id == 0) id = rng() >> 11; // (fits a JSON double exactly)
        return id;
    }
}
