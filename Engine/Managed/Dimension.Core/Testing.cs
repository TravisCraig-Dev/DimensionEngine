using System;
using System.Collections;
using System.Collections.Generic;

namespace Dimension
{
    /// <summary>Marks a method as a unit test (like NUnit's [Test] in Unity's Test Runner): a public method without
    /// parameters, static or on a class with a parameterless constructor. Window > Test Runner runs them in edit mode.</summary>
    [AttributeUsage(AttributeTargets.Method)]
    public sealed class TestAttribute : Attribute { }

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
