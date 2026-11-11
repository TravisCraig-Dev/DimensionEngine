using System;
using System.Collections;
using System.Collections.Generic;

namespace Dimension
{
    /// <summary>Marks a method as a unit test (like NUnit's [Test] in Unity's Test Runner): a public method without
    /// parameters, static or on a class with a parameterless constructor. Window > Test Runner runs them in edit mode.</summary>
    [AttributeUsage(AttributeTargets.Method)]
    public sealed class TestAttribute : Attribute { }

    /// <summary>Marks a play-mode test (like Unity's [UnityTest]): a public method without parameters returning
    /// IEnumerator. The Test Runner enters play mode on an empty scene for each one and resumes it once a frame; it yields
    /// what a coroutine can (null for a frame, WaitForSeconds, WaitUntil, a SceneManager.LoadSceneAsync operation ...).
    /// It fails on an exception, a failed Assert, an error logged meanwhile (see LogAssert), or after TimeoutSeconds of
    /// real time.</summary>
    [AttributeUsage(AttributeTargets.Method)]
    public sealed class PlayTestAttribute : Attribute
    {
        public float TimeoutSeconds { get; set; } = 30.0f;
    }

    /// <summary>A method run before each test of its class (edit and play mode).</summary>
    [AttributeUsage(AttributeTargets.Method)]
    public sealed class SetUpAttribute : Attribute { }

    /// <summary>A method run after each test of its class, also when the test failed.</summary>
    [AttributeUsage(AttributeTargets.Method)]
    public sealed class TearDownAttribute : Attribute { }

    /// <summary>Errors logged while a test runs fail it (as in Unity) unless expected here first.</summary>
    public static class LogAssert
    {
        internal static readonly List<string> Expected = new();
        internal static bool Ignore;
        internal static string Unexpected; // the first unexpected error of the running test
        internal static bool Active;

        /// <summary>An error containing this text is expected (once) in the running test; the test fails if it does not
        /// come.</summary>
        public static void Expect(string messageContains) => Expected.Add(messageContains ?? "");
        /// <summary>Errors logged in the running test do not fail it.</summary>
        public static bool IgnoreFailingMessages { get => Ignore; set => Ignore = value; }
        /// <summary>Fails now if an error that was not expected has been logged so far.</summary>
        public static void NoUnexpectedReceived()
        {
            if (Unexpected != null) Assert.Fail($"Error logged: {Unexpected}");
        }

        internal static void Begin()
        {
            Expected.Clear();
            Ignore = false;
            Unexpected = null;
            Active = true;
        }

        // The test's verdict on logged errors: null when fine.
        internal static string End()
        {
            Active = false;
            if (Unexpected != null && !Ignore) return $"Error logged: {Unexpected}";
            if (Expected.Count > 0) return $"Expected an error containing \"{Expected[0]}\", none was logged";
            return null;
        }

        internal static void OnError(string message)
        {
            if (!Active) return;
            int i = Expected.FindIndex(e => message.Contains(e, StringComparison.Ordinal));
            if (i >= 0) Expected.RemoveAt(i);
            else Unexpected ??= message;
        }
    }

    // Finding and running tests for the Test Runner (ScriptHost.RunTests / PlayTest).
    internal static class TestRunner
    {
        public static string Name(System.Reflection.MethodInfo m) => $"{m.DeclaringType.FullName}.{m.Name}";

        public static List<System.Reflection.MethodInfo> Find(System.Reflection.Assembly assembly)
        {
            var tests = new List<System.Reflection.MethodInfo>();
            if (assembly == null) return tests;
            Type[] types;
            try { types = assembly.GetTypes(); }
            catch (System.Reflection.ReflectionTypeLoadException e) { types = Array.FindAll(e.Types, t => t != null); }
            const System.Reflection.BindingFlags flags = System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static |
                                                         System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.DeclaredOnly;
            foreach (Type type in types)
                foreach (System.Reflection.MethodInfo m in type.GetMethods(flags))
                {
                    if (m.GetParameters().Length != 0) continue;
                    if (m.IsDefined(typeof(TestAttribute), false) ||
                        (m.IsDefined(typeof(PlayTestAttribute), false) && typeof(IEnumerator).IsAssignableFrom(m.ReturnType)))
                        tests.Add(m);
                }
            tests.Sort((a, b) => string.CompareOrdinal(Name(a), Name(b)));
            return tests;
        }

        public static bool IsPlay(System.Reflection.MethodInfo m) => m.IsDefined(typeof(PlayTestAttribute), false);

        public static string Describe(Exception e)
        {
            if (e is System.Reflection.TargetInvocationException { InnerException: not null } t) e = t.InnerException;
            return e is AssertionException ? e.Message : $"{e.GetType().Name}: {e.Message}";
        }

        // The class's [SetUp] or [TearDown] methods, on the test's object.
        public static void RunFixture(Type type, object target, Type attribute)
        {
            for (Type t = type; t != null && t != typeof(object); t = t.BaseType)
                foreach (var m in t.GetMethods(System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic |
                                               System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Static |
                                               System.Reflection.BindingFlags.DeclaredOnly))
                    if (m.IsDefined(attribute, false) && m.GetParameters().Length == 0 && (m.IsStatic || target != null))
                        m.Invoke(m.IsStatic ? null : target, null);
        }

        // An edit-mode test, start to end: (passed, message).
        public static (bool, string) RunEdit(System.Reflection.MethodInfo m)
        {
            object target = null;
            string failure = null;
            LogAssert.Begin();
            try
            {
                target = m.IsStatic ? null : Activator.CreateInstance(m.DeclaringType);
                RunFixture(m.DeclaringType, target, typeof(SetUpAttribute));
                m.Invoke(target, null);
            }
            catch (Exception e) { failure = Describe(e); }
            try { RunFixture(m.DeclaringType, target, typeof(TearDownAttribute)); }
            catch (Exception e) { failure ??= "TearDown: " + Describe(e); }
            string logs = LogAssert.End();
            failure ??= logs;
            return (failure == null, failure ?? "");
        }
    }

    // The play-mode test running now: begun once play starts on its empty scene, then resumed once a frame.
    internal sealed class PlayTestDriver
    {
        private readonly Stack<IEnumerator> m_Stack = new();
        private readonly System.Reflection.MethodInfo m_Method;
        private object m_Target;
        private YieldInstruction m_Waiting;
        private readonly System.Diagnostics.Stopwatch m_Watch = new();
        private readonly float m_Timeout;
        private bool m_Started;
        public int Frames;

        public static PlayTestDriver Current;

        public PlayTestDriver(System.Reflection.MethodInfo method)
        {
            m_Method = method;
            m_Timeout = method.GetCustomAttributes(typeof(PlayTestAttribute), false) is PlayTestAttribute[] { Length: > 0 } a ? a[0].TimeoutSeconds : 30.0f;
        }

        // null: still running; otherwise the failure ("" when it passed).
        public string Step(float deltaTime)
        {
            string failure = null;
            try
            {
                if (!m_Started)
                {
                    m_Started = true;
                    m_Watch.Start();
                    LogAssert.Begin();
                    m_Target = m_Method.IsStatic ? null : Activator.CreateInstance(m_Method.DeclaringType);
                    TestRunner.RunFixture(m_Method.DeclaringType, m_Target, typeof(SetUpAttribute));
                    if (m_Method.Invoke(m_Target, null) is IEnumerator routine) m_Stack.Push(routine);
                }
                else
                {
                    ++Frames;
                    if (m_Waiting != null)
                    {
                        // WaitForFixedUpdate / WaitForEndOfFrame: the frame has passed both points, go on.
                        if (m_Waiting is not (WaitForFixedUpdate or WaitForEndOfFrame) && m_Waiting.Wait(deltaTime)) return CheckTimeout();
                        m_Waiting = null;
                    }
                }
                while (m_Stack.Count > 0)
                {
                    IEnumerator top = m_Stack.Peek();
                    if (!top.MoveNext())
                    {
                        m_Stack.Pop();
                        continue;
                    }
                    switch (top.Current)
                    {
                        case IEnumerator nested: m_Stack.Push(nested); continue;
                        case Coroutine c when c.IsDone: continue;
                        case YieldInstruction wait: m_Waiting = wait; return CheckTimeout();
                        default: return CheckTimeout(); // null and anything else: the next frame
                    }
                }
            }
            catch (Exception e) { failure = TestRunner.Describe(e); }
            if (failure == null && LogAssert.Unexpected != null && !LogAssert.Ignore) failure = $"Error logged: {LogAssert.Unexpected}";
            return Finish(failure);
        }

        private string CheckTimeout()
        {
            if (LogAssert.Unexpected != null && !LogAssert.Ignore) return Finish($"Error logged: {LogAssert.Unexpected}");
            return m_Watch.Elapsed.TotalSeconds > m_Timeout ? Finish($"Timed out after {m_Timeout:0.#} s ({Frames} frames)") : null;
        }

        public string Finish(string failure)
        {
            m_Stack.Clear();
            try { TestRunner.RunFixture(m_Method.DeclaringType, m_Target, typeof(TearDownAttribute)); }
            catch (Exception e) { failure ??= "TearDown: " + TestRunner.Describe(e); }
            string logs = LogAssert.End();
            failure ??= logs;
            m_Watch.Stop();
            return failure ?? "";
        }

        public double Milliseconds => m_Watch.Elapsed.TotalMilliseconds;
    }

    /// <summary>Thrown by Assert when a check fails (the test fails with its message).</summary>
    public sealed class AssertionException : Exception
    {
        public AssertionException(string message) : base(message) { }
    }

    /// <summary>Checks for tests (NUnit's Assert, the common part).</summary>
    public static class Assert
    {
        public static void IsTrue(bool condition, string message = null)
        {
            if (!condition) Fail(message ?? "Expected true, was false");
        }
        public static void IsFalse(bool condition, string message = null)
        {
            if (condition) Fail(message ?? "Expected false, was true");
        }
        public static void AreEqual(object expected, object actual, string message = null)
        {
            if (!Equals(expected, actual)) Fail(message ?? $"Expected {Show(expected)}, was {Show(actual)}");
        }
        public static void AreNotEqual(object notExpected, object actual, string message = null)
        {
            if (Equals(notExpected, actual)) Fail(message ?? $"Expected anything but {Show(notExpected)}");
        }
        /// <summary>Floats within a tolerance.</summary>
        public static void AreEqual(float expected, float actual, float delta, string message = null)
        {
            if (!(Math.Abs(expected - actual) <= delta)) Fail(message ?? $"Expected {expected} (within {delta}), was {actual}");
        }
        public static void IsNull(object value, string message = null)
        {
            if (value != null) Fail(message ?? $"Expected null, was {Show(value)}");
        }
        public static void IsNotNull(object value, string message = null)
        {
            if (value == null) Fail(message ?? "Expected a value, was null");
        }
        public static void Greater(IComparable a, IComparable b, string message = null)
        {
            if (a.CompareTo(b) <= 0) Fail(message ?? $"Expected {a} > {b}");
        }
        public static void Less(IComparable a, IComparable b, string message = null)
        {
            if (a.CompareTo(b) >= 0) Fail(message ?? $"Expected {a} < {b}");
        }
        public static void Contains(object item, IEnumerable collection, string message = null)
        {
            foreach (object o in collection)
                if (Equals(o, item)) return;
            Fail(message ?? $"Expected the collection to contain {Show(item)}");
        }
        /// <summary>The action throws T (or a type deriving from it); returns the exception.</summary>
        public static T Throws<T>(Action action, string message = null) where T : Exception
        {
            try { action(); }
            catch (T e) { return e; }
            catch (Exception e) { Fail(message ?? $"Expected {typeof(T).Name}, got {e.GetType().Name}: {e.Message}"); }
            Fail(message ?? $"Expected {typeof(T).Name}, nothing was thrown");
            return null;
        }
        public static void Fail(string message) => throw new AssertionException(message);

        private static string Show(object o) => o == null ? "null" : o is string s ? $"\"{s}\"" : o.ToString();
    }
}
