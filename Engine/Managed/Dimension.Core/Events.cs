using System;
using System.Collections.Generic;
using Dimension.Interop;

namespace Dimension
{
    // C# events on engine objects (like UnityEvent listeners): any script can subscribe to another entity's UI widget
    // (UIButton.Clicked, UISlider.ValueChanged, ...) or collisions (Entity.CollisionEnter, ...), with += and -=.
    // Listeners end when play stops or the scripts reload.
    internal static class EngineEvents
    {
        // Matches Scene::EventKind.
        internal enum Kind { Click = 0, ValueChanged = 1, TextChanged = 2, Submit = 3, CollisionEnter = 4, CollisionExit = 5, TriggerEnter = 6, TriggerExit = 7 }

        // Listeners in subscription order. One list per (entity, kind): widgets give the same event different types
        // (a slider's float, a toggle's bool, a dropdown's int), so delegates are kept as they were added.
        private static readonly Dictionary<(IntPtr Scene, ulong Id, Kind Kind), List<Delegate>> s_Listeners = new();

        internal static void Add(Entity e, Kind kind, Delegate listener)
        {
            if (listener == null || e.Scene == IntPtr.Zero) return;
            var key = (e.Scene, e.Id, kind);
            if (!s_Listeners.TryGetValue(key, out var list)) s_Listeners[key] = list = new List<Delegate>();
            list.Add(listener);
        }

        internal static void Remove(Entity e, Kind kind, Delegate listener)
        {
            if (listener == null || !s_Listeners.TryGetValue((e.Scene, e.Id, kind), out var list)) return;
            int i = list.LastIndexOf(listener);
            if (i >= 0) list.RemoveAt(i);
            if (list.Count == 0) s_Listeners.Remove((e.Scene, e.Id, kind));
        }

        internal static void ClearScene(IntPtr scene)
        {
            var keys = new List<(IntPtr, ulong, Kind)>();
            foreach (var key in s_Listeners.Keys)
                if (key.Scene == scene) keys.Add(key);
            foreach (var key in keys) s_Listeners.Remove(key);
        }

        internal static void Clear() => s_Listeners.Clear();

        internal static void Raise(IntPtr scene, ulong id, Kind kind, float value, ulong other, string text)
        {
            if (!s_Listeners.TryGetValue((scene, id, kind), out var list)) return;
            Native.CurrentScene = scene;
            var otherEntity = new Entity(scene, other);
            // A copy: listeners may subscribe or unsubscribe while being called.
            foreach (Delegate listener in list.ToArray())
            {
                try
                {
                    switch (listener)
                    {
                        case Action a: a(); break;
                        case Action<float> f: f(value); break;
                        case Action<bool> b: b(value != 0.0f); break;
                        case Action<int> i: i((int)value); break;
                        case Action<string> s: s(text ?? string.Empty); break;
                        case Action<Entity> o: o(otherEntity); break;
                    }
                }
                catch (Exception e) { Debug.LogError($"{listener.Method.DeclaringType?.Name}.{listener.Method.Name}: {e}"); }
            }
        }
    }
}
