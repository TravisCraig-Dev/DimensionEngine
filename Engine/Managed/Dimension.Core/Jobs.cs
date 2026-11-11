using System;
using System.Collections;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace Dimension
{
    // The C# job system (like Unity's): small structs of work over NativeArrays, scheduled on worker threads with
    // dependencies between them. Jobs run on the .NET thread pool as soon as what they depend on is done; Complete()
    // waits for one on the main thread. Safety checks (as Unity's) catch races: the main thread cannot use an array a
    // scheduled job writes (or write one it reads) until that job is completed, two jobs using an array where one writes
    // must depend on each other, and a job cannot write an array its field marks [ReadOnly]. Engine objects (entities,
    // components) are main-thread only: copy what a job needs into NativeArrays.

    /// <summary>A job run once (Schedule, Run).</summary>
    public interface IJob
    {
        void Execute();
    }

    /// <summary>A job run for each index of a range, split into batches over the workers (Schedule(length, batch)).</summary>
    public interface IJobParallelFor
    {
        void Execute(int index);
    }

    /// <summary>A job run for each index: on one worker in order (Schedule) or split over the workers (ScheduleParallel).</summary>
    public interface IJobFor
    {
        void Execute(int index);
    }

    /// <summary>A job's NativeArray field that it only reads: other jobs may read it at the same time, and writing it
    /// inside the job throws.</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class ReadOnlyAttribute : Attribute { }

    /// <summary>A job's NativeArray field that it only writes (it counts as written for the safety checks).</summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class WriteOnlyAttribute : Attribute { }

    /// <summary>How long a NativeArray is meant to live (all are freed by Dispose; kept for Unity's signatures).</summary>
    public enum Allocator { None = 0, Temp = 2, TempJob = 3, Persistent = 4 }

    public enum NativeArrayOptions { ClearMemory = 1, UninitializedMemory = 0 }

    // A scheduled job: its task, and the jobs it waited for (for the safety checks' "depends on").
    internal sealed class JobNode
    {
        public Task Task;
        public JobNode[] Dependencies;
        public string Name;
        public Action MainThreadFinish; // run once on the main thread after it ran (Complete, or the frame's end)
        public bool IsCompleted => Task != null && Task.IsCompleted; // (no task yet: being scheduled)

        // Its and its dependencies' main-thread finish actions (they ran: the caller waited).
        public void Finish()
        {
            foreach (JobNode d in Dependencies) d.Finish();
            Action finish = MainThreadFinish;
            MainThreadFinish = null;
            finish?.Invoke();
        }

        public bool DependsOn(JobNode other)
        {
            if (other == null || other == this) return other == this;
            if (other.IsCompleted) return true; // done before this one started: no race
            foreach (JobNode d in Dependencies)
                if (d == other || d.DependsOn(other)) return true;
            return false;
        }
    }

    /// <summary>A scheduled job (or several combined): pass it as a dependency, Complete() it before using its results.</summary>
    public readonly struct JobHandle
    {
        internal readonly JobNode Node;
        internal JobHandle(JobNode node) { Node = node; }

        /// <summary>Whether it has finished (Complete() still has to be called before using its arrays in Unity; here
        /// finished is enough).</summary>
        public bool IsCompleted => Node == null || Node.IsCompleted;

        /// <summary>Waits on this thread until the job (and those it depends on) has run.</summary>
        public void Complete()
        {
            if (Node == null) return;
            if (JobSystem.InJob) throw new InvalidOperationException("JobHandle.Complete cannot be called from inside a job");
            Node.Task.Wait();
            Node.Finish();
        }

        /// <summary>One handle done when all of these are.</summary>
        public static JobHandle CombineDependencies(params JobHandle[] handles) => JobSystem.Combine(handles);
        public static JobHandle CombineDependencies(JobHandle a, JobHandle b) => JobSystem.Combine(new[] { a, b });
        public static JobHandle CombineDependencies(JobHandle a, JobHandle b, JobHandle c) => JobSystem.Combine(new[] { a, b, c });
        /// <summary>Waits for all of them.</summary>
        public static void CompleteAll(params JobHandle[] handles)
        {
            foreach (JobHandle h in handles) h.Complete();
        }
        /// <summary>Whether `handle` waits for `dependency` (directly, through others, or because it already ran).</summary>
        public static bool CheckFenceIsDependencyOrDidSyncFence(JobHandle dependency, JobHandle handle) =>
            dependency.Node == null || (handle.Node != null && handle.Node.DependsOn(dependency.Node));
        /// <summary>Jobs start as soon as they are scheduled here; kept for Unity's code.</summary>
        public static void ScheduleBatchedJobs() { }
    }

    // A NativeArray's shared state: its memory and which scheduled jobs write and read it.
    // (a container's state derives from it: NativeList's grows its buffer, NativeHashMap's holds its table)
    internal class NativeSafety
    {
        public IntPtr Buffer;
        public int Length;
        public bool Disposed;
        public JobNode Writer;
        public readonly List<JobNode> Readers = new();
        public readonly object Lock = new();
        public string Kind = "NativeArray";

        public void Prune()
        {
            if (Writer != null && Writer.IsCompleted) Writer = null;
            Readers.RemoveAll(r => r.IsCompleted);
        }

        // Frees what it holds (under Lock, once).
        public virtual unsafe void Release()
        {
            if (Buffer != IntPtr.Zero) NativeMemory.Free((void*)Buffer);
            Buffer = IntPtr.Zero;
        }

        // The main thread may read it unless a scheduled job writes it.
        public void CheckRead()
        {
            if (Disposed) throw new ObjectDisposedException($"The {Kind} has been disposed");
            if (JobSystem.InJob) return;
            JobNode writer = Writer;
            if (writer != null && !writer.IsCompleted)
                throw new InvalidOperationException($"The {Kind} is being written by the scheduled job {writer.Name}: call Complete() on its handle before reading it");
        }

        // The main thread may write it unless a scheduled job uses it; a job may unless its field is [ReadOnly].
        public void CheckWrite(bool readOnlyCopy)
        {
            if (Disposed) throw new ObjectDisposedException($"The {Kind} has been disposed");
            if (JobSystem.InJob)
            {
                if (readOnlyCopy) throw new InvalidOperationException($"The {Kind} is [ReadOnly] in this job: it cannot be written");
                return;
            }
            lock (Lock)
            {
                Prune();
                JobNode user = Writer ?? (Readers.Count > 0 ? Readers[0] : null);
                if (user != null) throw new InvalidOperationException($"The {Kind} is used by the scheduled job {user.Name}: call Complete() on its handle before writing it");
            }
        }

        public void CheckDispose()
        {
            if (Disposed) throw new ObjectDisposedException($"The {Kind} has been disposed already");
            if (JobSystem.InJob) return;
            lock (Lock)
            {
                Prune();
                if (Writer != null || Readers.Count > 0)
                    throw new InvalidOperationException($"The {Kind} is used by the scheduled job {(Writer ?? Readers[0]).Name}: Complete it (or Dispose(handle)) first");
            }
        }
    }

    // What the job system needs from any native container in a job's fields.
    internal interface INativeContainer
    {
        NativeSafety Safety { get; }
        object AsReadOnly();
    }

    /// <summary>A fixed-length array of unmanaged values in native memory, to share with jobs (like Unity's). Copies of
    /// the struct share the same memory; Dispose frees it (Dispose(handle) once a job is done with it).</summary>
    public readonly unsafe struct NativeArray<T> : IEnumerable<T>, IDisposable, INativeContainer, IEquatable<NativeArray<T>> where T : unmanaged
    {
        // (readonly: copies share the state, so `using var` and readonly fields work as with any handle)
        private readonly NativeSafety m_Safety;
        private readonly bool m_ReadOnly; // (the copy a job gets for a [ReadOnly] field)
        private readonly bool m_View;     // (NativeList.AsArray: the list's memory, disposed with it)

        internal NativeArray(NativeSafety state, bool view, bool readOnly = false)
        {
            m_Safety = state;
            m_ReadOnly = readOnly;
            m_View = view;
        }

        public NativeArray(int length, Allocator allocator, NativeArrayOptions options = NativeArrayOptions.ClearMemory)
        {
            if (length < 0) throw new ArgumentOutOfRangeException(nameof(length), "Length must be 0 or more");
            if (allocator == Allocator.None) throw new ArgumentException("Allocator.None cannot allocate", nameof(allocator));
            nuint bytes = (nuint)length * (nuint)sizeof(T);
            void* p = options == NativeArrayOptions.ClearMemory ? NativeMemory.AllocZeroed(Math.Max(bytes, 1)) : NativeMemory.Alloc(Math.Max(bytes, 1));
            m_Safety = new NativeSafety { Buffer = (IntPtr)p, Length = length };
            m_ReadOnly = false;
            m_View = false;
            JobSystem.Track(m_Safety, allocator);
        }

        /// <summary>A native copy of a managed array.</summary>
        public NativeArray(T[] array, Allocator allocator) : this(array.Length, allocator, NativeArrayOptions.UninitializedMemory)
        {
            array.AsSpan().CopyTo(new Span<T>((void*)m_Safety.Buffer, array.Length));
        }

        /// <summary>A native copy of another NativeArray.</summary>
        public NativeArray(NativeArray<T> array, Allocator allocator) : this(array.Length, allocator, NativeArrayOptions.UninitializedMemory)
        {
            array.CheckRead();
            new ReadOnlySpan<T>((void*)array.m_Safety.Buffer, array.Length).CopyTo(new Span<T>((void*)m_Safety.Buffer, array.Length));
        }

        public bool IsCreated => m_Safety != null && !m_Safety.Disposed;
        public int Length => m_Safety?.Length ?? 0;

        public T this[int index]
        {
            get
            {
                CheckRead();
                if ((uint)index >= (uint)m_Safety.Length) throw new IndexOutOfRangeException($"Index {index} is out of range of '{m_Safety.Length}' Length.");
                return ((T*)m_Safety.Buffer)[index];
            }
            set
            {
                CheckWrite();
                if ((uint)index >= (uint)m_Safety.Length) throw new IndexOutOfRangeException($"Index {index} is out of range of '{m_Safety.Length}' Length.");
                ((T*)m_Safety.Buffer)[index] = value;
            }
        }

        /// <summary>The memory as a span (checked for writing, as it can be written through).</summary>
        public Span<T> AsSpan()
        {
            CheckWrite();
            return new Span<T>((void*)m_Safety.Buffer, m_Safety.Length);
        }

        /// <summary>The memory as a read-only span.</summary>
        public ReadOnlySpan<T> AsReadOnlySpan()
        {
            CheckRead();
            return new ReadOnlySpan<T>((void*)m_Safety.Buffer, m_Safety.Length);
        }

        public void CopyFrom(T[] array)
        {
            if (array.Length != Length) throw new ArgumentException($"Lengths differ ({array.Length} into {Length})");
            array.AsSpan().CopyTo(AsSpan());
        }
        public void CopyFrom(NativeArray<T> array)
        {
            if (array.Length != Length) throw new ArgumentException($"Lengths differ ({array.Length} into {Length})");
            array.AsReadOnlySpan().CopyTo(AsSpan());
        }
        public void CopyTo(T[] array)
        {
            if (array.Length != Length) throw new ArgumentException($"Lengths differ ({Length} into {array.Length})");
            AsReadOnlySpan().CopyTo(array);
        }
        public T[] ToArray() => AsReadOnlySpan().ToArray();

        /// <summary>Frees the memory (no job may still use it).</summary>
        public void Dispose()
        {
            if (m_Safety == null) throw new ObjectDisposedException("The NativeArray was never created");
            if (m_View) throw new InvalidOperationException("This NativeArray is a NativeList's AsArray(): dispose the list instead");
            m_Safety.CheckDispose();
            JobSystem.Free(m_Safety);
        }

        /// <summary>Frees the memory once the job is done; returns a handle for that.</summary>
        public JobHandle Dispose(JobHandle inputDeps)
        {
            return JobSystem.DisposeAfter(m_Safety ?? throw new ObjectDisposedException("The NativeArray was never created"), inputDeps);
        }

        private void CheckRead() => (m_Safety ?? throw new ObjectDisposedException("The NativeArray was never created")).CheckRead();
        private void CheckWrite() => (m_Safety ?? throw new ObjectDisposedException("The NativeArray was never created")).CheckWrite(m_ReadOnly);

        NativeSafety INativeContainer.Safety => m_Safety;
        object INativeContainer.AsReadOnly() => new NativeArray<T>(m_Safety, m_View, readOnly: true);

        public IEnumerator<T> GetEnumerator()
        {
            for (int i = 0; i < Length; ++i) yield return this[i];
        }
        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
        public bool Equals(NativeArray<T> other) => m_Safety == other.m_Safety;
        public override bool Equals(object obj) => obj is NativeArray<T> other && Equals(other);
        public override int GetHashCode() => m_Safety?.GetHashCode() ?? 0;
        public static bool operator ==(NativeArray<T> a, NativeArray<T> b) => a.Equals(b);
        public static bool operator !=(NativeArray<T> a, NativeArray<T> b) => !a.Equals(b);
    }

    /// <summary>Scheduling for IJob (Schedule on a worker, Run here).</summary>
    public static class IJobExtensions
    {
        public static JobHandle Schedule<T>(this T job, JobHandle dependsOn = default) where T : struct, IJob
        {
            T copy = JobSystem.Prepare(job, out var uses);
            return JobSystem.ScheduleAction(() => copy.Execute(), dependsOn, typeof(T).Name, uses);
        }

        /// <summary>Runs it now on this thread (after what it uses is complete).</summary>
        public static void Run<T>(this T job) where T : struct, IJob
        {
            JobSystem.CompleteUsers(job);
            job.Execute();
        }
    }

    /// <summary>Scheduling for IJobParallelFor: Execute(index) for 0..length-1, in batches of innerloopBatchCount
    /// indices spread over the workers.</summary>
    public static class IJobParallelForExtensions
    {
        public static JobHandle Schedule<T>(this T job, int arrayLength, int innerloopBatchCount, JobHandle dependsOn = default)
            where T : struct, IJobParallelFor
        {
            T copy = JobSystem.Prepare(job, out var uses);
            return JobSystem.ScheduleBatches(arrayLength, innerloopBatchCount, (start, end) =>
            {
                for (int i = start; i < end; ++i) copy.Execute(i);
            }, dependsOn, typeof(T).Name, uses);
        }

        public static void Run<T>(this T job, int arrayLength) where T : struct, IJobParallelFor
        {
            JobSystem.CompleteUsers(job);
            for (int i = 0; i < arrayLength; ++i) job.Execute(i);
        }
    }

    /// <summary>Scheduling for IJobFor: in order on one worker (Schedule), spread over the workers (ScheduleParallel), or
    /// here (Run).</summary>
    public static class IJobForExtensions
    {
        public static JobHandle Schedule<T>(this T job, int arrayLength, JobHandle dependsOn = default) where T : struct, IJobFor
        {
            T copy = JobSystem.Prepare(job, out var uses);
            return JobSystem.ScheduleAction(() =>
            {
                for (int i = 0; i < arrayLength; ++i) copy.Execute(i);
            }, dependsOn, typeof(T).Name, uses);
        }

        public static JobHandle ScheduleParallel<T>(this T job, int arrayLength, int innerloopBatchCount, JobHandle dependsOn = default)
            where T : struct, IJobFor
        {
            T copy = JobSystem.Prepare(job, out var uses);
            return JobSystem.ScheduleBatches(arrayLength, innerloopBatchCount, (start, end) =>
            {
                for (int i = start; i < end; ++i) copy.Execute(i);
            }, dependsOn, typeof(T).Name, uses);
        }

        public static void Run<T>(this T job, int arrayLength) where T : struct, IJobFor
        {
            JobSystem.CompleteUsers(job);
            for (int i = 0; i < arrayLength; ++i) job.Execute(i);
        }
    }

    // A job type's NativeArray (container) fields and whether each is [ReadOnly], found once per type.
    internal static class JobFields<T> where T : struct
    {
        public static readonly (FieldInfo Field, bool ReadOnly)[] Containers = Array.ConvertAll(
            Array.FindAll(typeof(T).GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic),
                          f => typeof(INativeContainer).IsAssignableFrom(f.FieldType)),
            f => (f, f.IsDefined(typeof(ReadOnlyAttribute), false)));
    }

    /// <summary>The job system's settings and state.</summary>
    public static class JobSystem
    {
        [ThreadStatic] private static bool t_InJob;
        internal static bool InJob => t_InJob;

        // Jobs not yet finished (completed before the scripts unload: their code is in the scripts' assemblies).
        private static readonly ConcurrentDictionary<JobNode, byte> s_Running = new();
        // Arrays not yet disposed, with their allocator (a count for leak checks).
        private static readonly ConcurrentDictionary<NativeSafety, Allocator> s_Live = new();

        /// <summary>How many worker threads run jobs at once.</summary>
        public static int WorkerCount => Math.Max(1, Environment.ProcessorCount - 1);
        /// <summary>NativeArrays created and not yet disposed.</summary>
        public static int LiveNativeArrays => s_Live.Count;
        /// <summary>Jobs scheduled and not yet finished.</summary>
        public static int RunningJobs => s_Running.Count;

        internal static void Track(NativeSafety safety, Allocator allocator) => s_Live[safety] = allocator;

        internal static void Free(NativeSafety safety)
        {
            lock (safety.Lock)
            {
                if (safety.Disposed) return;
                safety.Disposed = true;
                safety.Release();
            }
            s_Live.TryRemove(safety, out _);
        }

        // A container's Dispose(handle): freed once the job is done.
        internal static JobHandle DisposeAfter(NativeSafety safety, JobHandle inputDeps) =>
            ScheduleAction(() => Free(safety), inputDeps, safety.Kind + ".Dispose", null);

        internal static JobHandle Combine(JobHandle[] handles)
        {
            var nodes = new List<JobNode>();
            foreach (JobHandle h in handles)
                if (h.Node != null && !h.Node.IsCompleted) nodes.Add(h.Node);
            if (nodes.Count == 0) return default;
            if (nodes.Count == 1) return new JobHandle(nodes[0]);
            var node = new JobNode { Dependencies = nodes.ToArray(), Name = "CombineDependencies" };
            node.Task = Task.WhenAll(Array.ConvertAll(node.Dependencies, d => d.Task));
            return new JobHandle(node);
        }

        // The native containers a job's fields hold (and whether it only reads each); a copy of the job whose
        // [ReadOnly] fields refuse writes.
        internal static T Prepare<T>(T job, out List<(NativeSafety, bool)> uses) where T : struct
        {
            if (InJob) throw new InvalidOperationException("Jobs cannot be scheduled from inside a job");
            uses = new List<(NativeSafety, bool)>();
            var fields = JobFields<T>.Containers;
            if (fields.Length == 0) return job;
            object boxed = job;
            bool changed = false;
            foreach (var (f, readOnly) in fields)
            {
                if (f.GetValue(boxed) is not INativeContainer c) continue;
                if (c.Safety == null || c.Safety.Disposed)
                    throw new InvalidOperationException($"{typeof(T).Name}.{f.Name}: the NativeArray has been disposed (or was never created)");
                uses.Add((c.Safety, readOnly));
                if (readOnly)
                {
                    f.SetValue(boxed, c.AsReadOnly());
                    changed = true;
                }
            }
            return changed ? (T)boxed : job;
        }

        // Before running a job here: the jobs using its arrays must be done (as Unity's Run does).
        internal static void CompleteUsers<T>(T job) where T : struct
        {
            if (InJob) throw new InvalidOperationException("Jobs cannot be run from inside a job");
            foreach (var (f, _) in JobFields<T>.Containers)
                if (f.GetValue(job) is INativeContainer { Safety: { } s })
                {
                    JobNode[] users;
                    lock (s.Lock)
                    {
                        s.Prune();
                        users = s.Readers.ToArray();
                        if (s.Writer != null) Array.Resize(ref users, users.Length + 1);
                        if (s.Writer != null) users[^1] = s.Writer;
                    }
                    foreach (JobNode u in users) u.Task.Wait();
                }
        }

        // Jobs with a main-thread finish not yet run (transform jobs nobody completed): run at the frame's end.
        private static readonly List<JobNode> s_Unfinished = new();

        internal static void AddFinish(JobHandle handle, Action finish)
        {
            handle.Node.MainThreadFinish = finish;
            s_Unfinished.Add(handle.Node);
        }

        // The frame's end (main thread): finished jobs' write-backs.
        internal static void FinishCompleted()
        {
            if (s_Unfinished.Count == 0) return;
            foreach (JobNode node in s_Unfinished.ToArray())
                if (node.IsCompleted) node.Finish();
            s_Unfinished.RemoveAll(n => n.MainThreadFinish == null);
        }

        // Job names as UTF-8 for the Profiler (made once per name, kept for the session).
        private static readonly ConcurrentDictionary<string, IntPtr> s_ProfileNames = new();

        // Times work on this worker thread as a span on its row of the Profiler's timeline.
        internal static unsafe void Profiled(string name, Action work)
        {
            long start = Interop.Native.Api.ProfilerWorker(0, null, 0);
            try { work(); }
            finally
            {
                IntPtr utf8 = s_ProfileNames.GetOrAdd(name, n => Marshal.StringToCoTaskMemUTF8(n));
                Interop.Native.Api.ProfilerWorker(1, (byte*)utf8, start);
            }
        }

        internal static JobHandle ScheduleAction(Action body, JobHandle dependsOn, string name, List<(NativeSafety, bool)> uses, bool profile = true)
        {
            JobNode dep = dependsOn.Node != null && !dependsOn.Node.IsCompleted ? dependsOn.Node : null;
            var node = new JobNode { Dependencies = dep != null ? new[] { dep } : Array.Empty<JobNode>(), Name = name };
            if (uses != null) Claim(node, uses);
            Action run = () =>
            {
                t_InJob = true;
                try
                {
                    if (profile) Profiled(name, body);
                    else body(); // (a parallel job's batches are timed one by one)
                }
                catch (AggregateException e) when (e.InnerExceptions.Count > 0) // (batches of a parallel job)
                {
                    string more = e.InnerExceptions.Count > 1 ? $" (and {e.InnerExceptions.Count - 1} more batches)" : "";
                    Debug.LogError($"Job {name} failed{more}: {e.InnerExceptions[0]}");
                }
                catch (Exception e) { Debug.LogError($"Job {name} failed: {e}"); }
                finally
                {
                    t_InJob = false;
                    s_Running.TryRemove(node, out _);
                }
            };
            s_Running[node] = 0;
            node.Task = dep == null ? Task.Run(run) : dep.Task.ContinueWith(_ => run(), TaskScheduler.Default);
            return new JobHandle(node);
        }

        internal static JobHandle ScheduleBatches(int length, int batch, Action<int, int> body, JobHandle dependsOn, string name, List<(NativeSafety, bool)> uses)
        {
            if (length < 0) throw new ArgumentOutOfRangeException(nameof(length));
            batch = Math.Max(1, batch);
            int batches = (length + batch - 1) / batch;
            return ScheduleAction(() =>
            {
                if (batches <= 1)
                {
                    Profiled(name, () => body(0, length));
                    return;
                }
                Parallel.For(0, batches, new ParallelOptions { MaxDegreeOfParallelism = WorkerCount + 1 }, b =>
                {
                    bool was = t_InJob;
                    t_InJob = true;
                    try { Profiled(name, () => body(b * batch, Math.Min(length, (b + 1) * batch))); }
                    finally { t_InJob = was; }
                });
            }, dependsOn, name, uses, profile: false);
        }

        // The safety checks at Schedule: an array this job writes may not be in use by a job it does not depend on, nor
        // may one it reads be written by such a job. Then this job is its writer or one of its readers.
        private static void Claim(JobNode node, List<(NativeSafety Safety, bool ReadOnly)> uses)
        {
            foreach (var (s, readOnly) in uses)
                lock (s.Lock)
                {
                    s.Prune();
                    if (s.Writer != null && !node.DependsOn(s.Writer))
                        throw new InvalidOperationException(
                            $"The job {node.Name} uses a NativeArray that the scheduled job {s.Writer.Name} writes: pass that job's handle as a dependency");
                    if (!readOnly)
                        foreach (JobNode r in s.Readers)
                            if (!node.DependsOn(r))
                                throw new InvalidOperationException(
                                    $"The job {node.Name} writes a NativeArray that the scheduled job {r.Name} reads: pass that job's handle as a dependency");
                }
            foreach (var (s, readOnly) in uses)
                lock (s.Lock)
                {
                    if (readOnly) s.Readers.Add(node);
                    else
                    {
                        s.Writer = node;
                        s.Readers.Clear(); // (they all come before this one)
                    }
                }
        }

        /// <summary>Waits for every scheduled job (the engine does this before the scripts unload).</summary>
        public static void CompleteAll()
        {
            foreach (JobNode node in s_Running.Keys)
                try { node.Task?.Wait(); }
                catch (Exception) { }
        }

        // Before the scripts unload: their jobs finish, and arrays they never disposed are freed (with a warning, as
        // Unity's leak detection gives).
        internal static void Shutdown()
        {
            CompleteAll();
            s_Unfinished.Clear(); // (their objects belong to the scene being torn down)
            int leaked = 0;
            foreach (NativeSafety s in s_Live.Keys)
            {
                Free(s);
                ++leaked;
            }
            if (leaked > 0) Debug.LogWarning($"{leaked} NativeArray(s) were not disposed; freed as the scripts unloaded (call Dispose when done with one)");
        }
    }
}
