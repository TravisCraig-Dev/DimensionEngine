#pragma once
#include "Dimension/Core/Log.h"
#include <coroutine>
#include <exception>
#include <functional>
#include <utility>

namespace Dimension
{
    // C++20 coroutines for native scripts (Unity's IEnumerator coroutines). A member function returning Coroutine
    // can co_await waits and other coroutines; ScriptableEntity::StartCoroutine runs it, ticking it after OnUpdate.
    //
    //   Coroutine Blink() {
    //       for (int i = 0; i < 6; ++i) { Get<SpriteComponent>().Color.a = i % 2; co_await WaitForSeconds(0.25f); }
    //   }
    //   void OnCreate() override { StartCoroutine(Blink()); }
    //
    // Like Unity, a started coroutine runs at once until its first wait. Waits count scaled time from the frame
    // after they begin.
    class Coroutine
    {
    public:
        struct promise_type
        {
            // While set, the coroutine is suspended until it returns false (called once per tick with the frame time).
            std::function<bool(float)> Wait;
            std::coroutine_handle<promise_type> Child; // a nested coroutine being awaited (owned)

            Coroutine get_return_object() { return Coroutine(std::coroutine_handle<promise_type>::from_promise(*this)); }
            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_always final_suspend() noexcept { return {}; }
            void return_void() {}
            void unhandled_exception()
            {
                try { std::rethrow_exception(std::current_exception()); }
                catch (const std::exception& e) { Log::Error("Coroutine threw: {}", e.what()); }
                catch (...) { Log::Error("Coroutine threw an exception"); }
            }
            ~promise_type()
            {
                if (Child) Child.destroy();
            }
        };
        using Handle = std::coroutine_handle<promise_type>;

        Coroutine() = default;
        explicit Coroutine(Handle h) : m_Handle(h) {}
        Coroutine(Coroutine&& other) noexcept : m_Handle(std::exchange(other.m_Handle, {})) {}
        Coroutine& operator=(Coroutine&& other) noexcept
        {
            if (this != &other)
            {
                if (m_Handle) m_Handle.destroy();
                m_Handle = std::exchange(other.m_Handle, {});
            }
            return *this;
        }
        Coroutine(const Coroutine&) = delete;
        Coroutine& operator=(const Coroutine&) = delete;
        ~Coroutine()
        {
            if (m_Handle) m_Handle.destroy();
        }

        bool IsValid() const { return (bool)m_Handle; }
        bool IsDone() const { return !m_Handle || m_Handle.done(); }

        // Runs until the first wait (StartCoroutine).
        void Start()
        {
            if (m_Handle && !m_Handle.done()) m_Handle.resume();
        }

        // Advances by one frame: finishes waits and nested coroutines that are due, then resumes. Returns true while
        // the coroutine is still running.
        bool Tick(float dt) { return m_Handle && Step(m_Handle, dt); }

        // co_await another coroutine: it starts at once and the caller resumes when it finishes.
        bool await_ready() const noexcept { return IsDone(); }
        bool await_suspend(Handle parent)
        {
            Start();
            if (IsDone()) return false; // finished synchronously: continue the caller right away
            parent.promise().Child = std::exchange(m_Handle, {});
            return true;
        }
        void await_resume() const noexcept {}

    private:
        static bool Step(Handle h, float dt)
        {
            auto& p = h.promise();
            if (p.Child)
            {
                if (Step(p.Child, dt)) return true;
                p.Child.destroy();
                p.Child = {};
            }
            else if (p.Wait)
            {
                if (p.Wait(dt)) return true;
                p.Wait = nullptr;
            }
            if (!h.done()) h.resume();
            return !h.done();
        }

        Handle m_Handle;
    };

    namespace Detail
    {
        // Awaitable that suspends with a wait function (returns true while still waiting).
        template <class Fn> struct WaitAwaiter
        {
            Fn Waiting;
            bool await_ready() const noexcept { return false; }
            void await_suspend(Coroutine::Handle h) { h.promise().Wait = std::move(Waiting); }
            void await_resume() const noexcept {}
        };
    }

    // Suspends for `seconds` of scaled time.
    inline auto WaitForSeconds(float seconds)
    {
        auto wait = [remaining = seconds](float dt) mutable { return (remaining -= dt) > 0.0f; };
        return Detail::WaitAwaiter<decltype(wait)>{ std::move(wait) };
    }
    // Suspends until the next frame.
    inline auto NextFrame()
    {
        auto wait = [](float) { return false; };
        return Detail::WaitAwaiter<decltype(wait)>{ wait };
    }
    // Suspends until the predicate returns true (checked once per frame).
    inline auto WaitUntil(std::function<bool()> predicate)
    {
        auto wait = [p = std::move(predicate)](float) { return !p(); };
        return Detail::WaitAwaiter<decltype(wait)>{ std::move(wait) };
    }
    // Suspends while the predicate returns true.
    inline auto WaitWhile(std::function<bool()> predicate)
    {
        auto wait = [p = std::move(predicate)](float) { return p(); };
        return Detail::WaitAwaiter<decltype(wait)>{ std::move(wait) };
    }
}
