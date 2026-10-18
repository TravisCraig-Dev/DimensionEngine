using System;
using System.Collections;
using System.Collections.Generic;

namespace Dimension
{
    /// <summary>Base of objects a coroutine can yield to wait on.</summary>
    public abstract class YieldInstruction
    {
        internal abstract bool KeepWaiting(float deltaTime);
    }

    /// <summary>Suspends a coroutine for the given number of (scaled) seconds.</summary>
    public sealed class WaitForSeconds : YieldInstruction
    {
        private float m_Remaining;
        public WaitForSeconds(float seconds) { m_Remaining = seconds; }
        internal override bool KeepWaiting(float deltaTime) => (m_Remaining -= deltaTime) > 0.0f;
    }

    /// <summary>Suspends a coroutine until the predicate returns true (checked once per frame).</summary>
    public sealed class WaitUntil : YieldInstruction
    {
        private readonly Func<bool> m_Predicate;
        public WaitUntil(Func<bool> predicate) { m_Predicate = predicate; }
        internal override bool KeepWaiting(float deltaTime) => !m_Predicate();
    }

    /// <summary>Suspends a coroutine while the predicate returns true.</summary>
    public sealed class WaitWhile : YieldInstruction
    {
        private readonly Func<bool> m_Predicate;
        public WaitWhile(Func<bool> predicate) { m_Predicate = predicate; }
        internal override bool KeepWaiting(float deltaTime) => m_Predicate();
    }

    /// <summary>A running coroutine. Yield it from another coroutine to wait for it to finish.</summary>
    public sealed class Coroutine : YieldInstruction
    {
        internal readonly Stack<IEnumerator> Stack = new();
        internal YieldInstruction Waiting;
        internal bool Fresh;   // started this frame: first resume happens next frame
        internal string Tag;   // Invoke method name
        public bool IsDone { get; internal set; }
        internal override bool KeepWaiting(float deltaTime) => !IsDone;
    }

    /// <summary>Per-behaviour coroutine scheduler, ticked after OnUpdate.</summary>
    internal sealed class CoroutineRunner
    {
        private readonly List<Coroutine> m_Active = new();

        public Coroutine Start(IEnumerator routine, string tag)
        {
            var c = new Coroutine { Tag = tag, Fresh = true };
            if (routine == null) { c.IsDone = true; return c; }
            c.Stack.Push(routine);
            m_Active.Add(c);
            Run(c); // like Unity: runs synchronously until the first yield
            return c;
        }

        public void Stop(Coroutine c)
        {
            if (c == null) return;
            c.IsDone = true;
            m_Active.Remove(c);
        }

        public void StopAll()
        {
            foreach (var c in m_Active) c.IsDone = true;
            m_Active.Clear();
        }

        public void StopTagged(string tag) // null = every Invoke
        {
            foreach (var c in m_Active)
                if (c.Tag != null && (tag == null || c.Tag == tag)) c.IsDone = true;
            m_Active.RemoveAll(c => c.IsDone);
        }

        public bool HasTagged(string tag) => m_Active.Exists(c => !c.IsDone && c.Tag == tag);

        public void Tick(float deltaTime)
        {
            if (m_Active.Count == 0) return;
            foreach (var c in m_Active.ToArray()) // coroutines may start or stop others
            {
                if (c.IsDone) continue;
                if (c.Fresh) { c.Fresh = false; continue; }
                if (c.Waiting != null)
                {
                    if (c.Waiting.KeepWaiting(deltaTime)) continue;
                    c.Waiting = null;
                }
                Run(c);
            }
            m_Active.RemoveAll(c => c.IsDone);
        }

        private static void Run(Coroutine c)
        {
            try
            {
                while (c.Stack.Count > 0 && !c.IsDone)
                {
                    IEnumerator top = c.Stack.Peek();
                    if (!top.MoveNext())
                    {
                        c.Stack.Pop();
                        continue;
                    }
                    switch (top.Current)
                    {
                        case null: return; // resume next frame
                        case IEnumerator nested: c.Stack.Push(nested); continue;
                        case Coroutine other when other.IsDone: continue;
                        case YieldInstruction wait: c.Waiting = wait; return;
                        default: return; // unknown values wait one frame
                    }
                }
            }
            catch (Exception e)
            {
                Debug.LogError($"Coroutine failed: {e}");
            }
            c.IsDone = true;
        }
    }

    /// <summary>Math helpers in the style of Unity's Mathf.</summary>
    public static class Mathf
    {
        public const float PI = MathF.PI;
        public const float Deg2Rad = MathF.PI / 180.0f;
        public const float Rad2Deg = 180.0f / MathF.PI;
        public const float Epsilon = 1e-6f;

        public static float Clamp(float v, float min, float max) => v < min ? min : v > max ? max : v;
        public static int Clamp(int v, int min, int max) => v < min ? min : v > max ? max : v;
        public static float Clamp01(float v) => Clamp(v, 0.0f, 1.0f);
        public static float Lerp(float a, float b, float t) => a + (b - a) * Clamp01(t);
        public static float LerpUnclamped(float a, float b, float t) => a + (b - a) * t;
        public static float InverseLerp(float a, float b, float v) => MathF.Abs(b - a) < Epsilon ? 0.0f : Clamp01((v - a) / (b - a));
        public static float SmoothStep(float from, float to, float t)
        {
            t = Clamp01(t);
            t = t * t * (3.0f - 2.0f * t);
            return from + (to - from) * t;
        }
        public static float MoveTowards(float current, float target, float maxDelta) =>
            MathF.Abs(target - current) <= maxDelta ? target : current + MathF.Sign(target - current) * maxDelta;
        public static float Repeat(float t, float length) => Clamp(t - MathF.Floor(t / length) * length, 0.0f, length);
        public static float PingPong(float t, float length)
        {
            t = Repeat(t, length * 2.0f);
            return length - MathF.Abs(t - length);
        }
        public static bool Approximately(float a, float b) => MathF.Abs(a - b) < MathF.Max(1e-6f * MathF.Max(MathF.Abs(a), MathF.Abs(b)), Epsilon * 8);
        /// <summary>Critically damped spring towards `target` (Game Programming Gems 4).</summary>
        public static float SmoothDamp(float current, float target, ref float velocity, float smoothTime, float deltaTime)
        {
            smoothTime = MathF.Max(0.0001f, smoothTime);
            float omega = 2.0f / smoothTime, x = omega * deltaTime;
            float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
            float change = current - target;
            float temp = (velocity + omega * change) * deltaTime;
            velocity = (velocity - omega * temp) * exp;
            return target + (change + temp) * exp;
        }

        /// <summary>2D gradient noise in [0, 1].</summary>
        public static float PerlinNoise(float x, float y)
        {
            int xi = (int)MathF.Floor(x) & 255, yi = (int)MathF.Floor(y) & 255;
            float xf = x - MathF.Floor(x), yf = y - MathF.Floor(y);
            float u = Fade(xf), v = Fade(yf);
            int aa = P[P[xi] + yi], ab = P[P[xi] + yi + 1], ba = P[P[xi + 1] + yi], bb = P[P[xi + 1] + yi + 1];
            float x1 = LerpUnclamped(Grad(aa, xf, yf), Grad(ba, xf - 1, yf), u);
            float x2 = LerpUnclamped(Grad(ab, xf, yf - 1), Grad(bb, xf - 1, yf - 1), u);
            return Clamp01((LerpUnclamped(x1, x2, v) + 1.0f) * 0.5f);
        }

        private static float Fade(float t) => t * t * t * (t * (t * 6 - 15) + 10);
        private static float Grad(int hash, float x, float y) => (hash & 3) switch { 0 => x + y, 1 => -x + y, 2 => x - y, _ => -x - y };
        private static readonly int[] P = BuildPermutation();
        private static int[] BuildPermutation()
        {
            var perm = new int[256];
            for (int i = 0; i < 256; ++i) perm[i] = i;
            var rng = new System.Random(1337);
            for (int i = 255; i > 0; --i) { int j = rng.Next(i + 1); (perm[i], perm[j]) = (perm[j], perm[i]); }
            var p = new int[512];
            for (int i = 0; i < 512; ++i) p[i] = perm[i & 255];
            return p;
        }
    }

    /// <summary>Random numbers in the style of Unity's Random (shared generator; call InitState to seed).</summary>
    public static class Random
    {
        private static System.Random s_Rng = new();
        public static void InitState(int seed) => s_Rng = new System.Random(seed);
        /// <summary>Uniform in [0, 1].</summary>
        public static float Value => (float)s_Rng.NextDouble();
        /// <summary>Uniform float in [min, max].</summary>
        public static float Range(float min, float max) => min + (max - min) * Value;
        /// <summary>Uniform int in [min, max) like Unity.</summary>
        public static int Range(int min, int max) => max <= min ? min : s_Rng.Next(min, max);
        public static System.Numerics.Vector2 InsideUnitCircle
        {
            get
            {
                float a = Range(0.0f, MathF.Tau), r = MathF.Sqrt(Value);
                return new System.Numerics.Vector2(MathF.Cos(a) * r, MathF.Sin(a) * r);
            }
        }
        public static System.Numerics.Vector3 OnUnitSphere
        {
            get
            {
                float z = Range(-1.0f, 1.0f), a = Range(0.0f, MathF.Tau), s = MathF.Sqrt(1.0f - z * z);
                return new System.Numerics.Vector3(s * MathF.Cos(a), s * MathF.Sin(a), z);
            }
        }
        public static System.Numerics.Vector3 InsideUnitSphere => OnUnitSphere * MathF.Cbrt(Value);
        public static Color ColorHSV(float saturation = 1.0f, float value = 1.0f) => Color.FromHSV(Value, saturation, value);
    }
}
