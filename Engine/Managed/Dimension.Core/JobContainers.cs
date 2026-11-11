using System;
using System.Collections;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Threading;

namespace Dimension
{
    // More containers for jobs (like Unity's Collections package) and jobs that move objects. Each is a struct whose
    // copies share one state, with NativeArray's safety checks; a ParallelWriter lets every batch of a parallel job add
    // at once.

    internal sealed class NativeListState : NativeSafety
    {
        public int Capacity;
    }

    /// <summary>A growable list of unmanaged values in native memory (like Unity's NativeList). Add in a job writing it
    /// alone, or AddNoResize through AsParallelWriter() from a parallel job (up to Capacity).</summary>
    public unsafe struct NativeList<T> : IEnumerable<T>, IDisposable, INativeContainer where T : unmanaged
    {
        private NativeListState m_State;
        private bool m_ReadOnly;

        public NativeList(Allocator allocator) : this(8, allocator) { }

        public NativeList(int initialCapacity, Allocator allocator)
        {
            if (allocator == Allocator.None) throw new ArgumentException("Allocator.None cannot allocate", nameof(allocator));
            int capacity = Math.Max(1, initialCapacity);
            m_State = new NativeListState { Kind = "NativeList", Capacity = capacity, Buffer = (IntPtr)NativeMemory.AllocZeroed((nuint)capacity * (nuint)sizeof(T)) };
            m_ReadOnly = false;
            JobSystem.Track(m_State, allocator);
        }

        public bool IsCreated => m_State != null && !m_State.Disposed;
        public bool IsEmpty => Length == 0;

        /// <summary>How many values it holds; setting it grows (with zeros) or shrinks it.</summary>
        public int Length
        {
            get
            {
                State.CheckRead();
                return Volatile.Read(ref m_State.Length);
            }
            set => Resize(value, NativeArrayOptions.ClearMemory);
        }

        /// <summary>How many it holds before its memory grows (AddNoResize's limit).</summary>
        public int Capacity
        {
            get
            {
                State.CheckRead();
                return m_State.Capacity;
            }
            set
            {
                State.CheckWrite(m_ReadOnly);
                if (value < m_State.Length) throw new ArgumentOutOfRangeException(nameof(value), "Capacity cannot be less than Length");
                SetCapacity(value);
            }
        }

        public T this[int index]
        {
            get
            {
                State.CheckRead();
                if ((uint)index >= (uint)m_State.Length) throw new IndexOutOfRangeException($"Index {index} is out of range of '{m_State.Length}' Length.");
                return ((T*)m_State.Buffer)[index];
            }
            set
            {
                State.CheckWrite(m_ReadOnly);
                if ((uint)index >= (uint)m_State.Length) throw new IndexOutOfRangeException($"Index {index} is out of range of '{m_State.Length}' Length.");
                ((T*)m_State.Buffer)[index] = value;
            }
        }

        public void Add(T value)
        {
            State.CheckWrite(m_ReadOnly);
            if (m_State.Length == m_State.Capacity) SetCapacity(m_State.Capacity * 2);
            ((T*)m_State.Buffer)[m_State.Length++] = value;
        }

        public void AddRange(NativeArray<T> values) => AddRange(values.AsReadOnlySpan());

        public void AddRange(ReadOnlySpan<T> values)
        {
            State.CheckWrite(m_ReadOnly);
            int needed = m_State.Length + values.Length;
            if (needed > m_State.Capacity) SetCapacity(Math.Max(needed, m_State.Capacity * 2));
            values.CopyTo(new Span<T>((T*)m_State.Buffer + m_State.Length, values.Length));
            m_State.Length = needed;
        }

        /// <summary>Removes one, moving the later ones down (keeps the order).</summary>
        public void RemoveAt(int index)
        {
            State.CheckWrite(m_ReadOnly);
            if ((uint)index >= (uint)m_State.Length) throw new IndexOutOfRangeException($"Index {index} is out of range of '{m_State.Length}' Length.");
            var span = new Span<T>((void*)m_State.Buffer, m_State.Length);
            span[(index + 1)..].CopyTo(span[index..]);
            --m_State.Length;
        }

        /// <summary>Removes one by moving the last into its place (fast; changes the order).</summary>
        public void RemoveAtSwapBack(int index)
        {
            State.CheckWrite(m_ReadOnly);
            if ((uint)index >= (uint)m_State.Length) throw new IndexOutOfRangeException($"Index {index} is out of range of '{m_State.Length}' Length.");
            T* p = (T*)m_State.Buffer;
            p[index] = p[--m_State.Length];
        }

        public void Clear()
        {
            State.CheckWrite(m_ReadOnly);
            m_State.Length = 0;
        }

        public void Resize(int length, NativeArrayOptions options)
        {
            State.CheckWrite(m_ReadOnly);
            if (length < 0) throw new ArgumentOutOfRangeException(nameof(length));
            if (length > m_State.Capacity) SetCapacity(Math.Max(length, m_State.Capacity * 2));
            if (length > m_State.Length && options == NativeArrayOptions.ClearMemory)
                new Span<T>((T*)m_State.Buffer + m_State.Length, length - m_State.Length).Clear();
            m_State.Length = length;
        }

        /// <summary>Its values as a NativeArray using the same memory and safety (valid until it grows; do not
        /// dispose it, dispose the list).</summary>
        public NativeArray<T> AsArray() => new NativeArray<T>(State, view: true);

        public Span<T> AsSpan()
        {
            State.CheckWrite(m_ReadOnly);
            return new Span<T>((void*)m_State.Buffer, m_State.Length);
        }

        public T[] ToArray()
        {
            State.CheckRead();
            return new ReadOnlySpan<T>((void*)m_State.Buffer, m_State.Length).ToArray();
        }

        public NativeArray<T> ToArray(Allocator allocator)
        {
            State.CheckRead();
            var copy = new NativeArray<T>(m_State.Length, allocator, NativeArrayOptions.UninitializedMemory);
            new ReadOnlySpan<T>((void*)m_State.Buffer, m_State.Length).CopyTo(copy.AsSpan());
            return copy;
        }

        /// <summary>For parallel jobs: AddNoResize from every batch at once (the list must have the room).</summary>
        public ParallelWriter AsParallelWriter() => new ParallelWriter(State);

        public void Dispose()
        {
            State.CheckDispose();
            JobSystem.Free(m_State);
        }

        public JobHandle Dispose(JobHandle inputDeps) => JobSystem.DisposeAfter(State, inputDeps);

        private NativeListState State => m_State ?? throw new ObjectDisposedException("The NativeList was never created");

        private void SetCapacity(int capacity)
        {
            capacity = Math.Max(1, capacity);
            m_State.Buffer = (IntPtr)NativeMemory.Realloc((void*)m_State.Buffer, (nuint)capacity * (nuint)sizeof(T));
            m_State.Capacity = capacity;
        }

        NativeSafety INativeContainer.Safety => m_State;
        object INativeContainer.AsReadOnly()
        {
            NativeList<T> copy = this;
            copy.m_ReadOnly = true;
            return copy;
        }

        public IEnumerator<T> GetEnumerator()
        {
            for (int i = 0; i < Length; ++i) yield return this[i];
        }
        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

        /// <summary>Adds from many threads at once, within the list's Capacity.</summary>
        public struct ParallelWriter : INativeContainer
        {
            private readonly NativeListState m_State;
            internal ParallelWriter(NativeListState state) { m_State = state; }

            public void AddNoResize(T value)
            {
                m_State.CheckWrite(false);
                int index = Interlocked.Increment(ref m_State.Length) - 1;
                if (index >= m_State.Capacity)
                {
                    Interlocked.Decrement(ref m_State.Length);
                    throw new InvalidOperationException($"AddNoResize: the NativeList is full ({m_State.Capacity}); give it more Capacity before the job");
                }
                ((T*)m_State.Buffer)[index] = value;
            }

            NativeSafety INativeContainer.Safety => m_State;
            object INativeContainer.AsReadOnly() => this;
        }
    }

    internal sealed class NativeHashMapState<TKey, TValue> : NativeSafety
    {
        public Dictionary<TKey, TValue> Map;
        public readonly object Sync = new(); // (the parallel writer's)
        public override void Release() => Map = null;
    }

    /// <summary>Unmanaged keys to unmanaged values (like Unity's NativeHashMap), usable in jobs: written by one job, or
    /// added to from a parallel job through AsParallelWriter(). (Its table is kept by the .NET runtime.)</summary>
    public struct NativeHashMap<TKey, TValue> : IDisposable, INativeContainer, IEnumerable<KeyValuePair<TKey, TValue>>
        where TKey : unmanaged, IEquatable<TKey> where TValue : unmanaged
    {
        private NativeHashMapState<TKey, TValue> m_State;
        private bool m_ReadOnly;

        public NativeHashMap(int initialCapacity, Allocator allocator)
        {
            if (allocator == Allocator.None) throw new ArgumentException("Allocator.None cannot allocate", nameof(allocator));
            m_State = new NativeHashMapState<TKey, TValue> { Kind = "NativeHashMap", Map = new Dictionary<TKey, TValue>(Math.Max(0, initialCapacity)) };
            m_ReadOnly = false;
            JobSystem.Track(m_State, allocator);
        }

        public bool IsCreated => m_State != null && !m_State.Disposed;
        public int Count
        {
            get
            {
                Read();
                return m_State.Map.Count;
            }
        }
        public bool IsEmpty => Count == 0;

        public TValue this[TKey key]
        {
            get
            {
                Read();
                return m_State.Map.TryGetValue(key, out TValue v) ? v : throw new KeyNotFoundException($"Key {key} is not in the NativeHashMap");
            }
            set
            {
                Write();
                m_State.Map[key] = value;
            }
        }

        /// <summary>Adds unless the key is there already (false then).</summary>
        public bool TryAdd(TKey key, TValue value)
        {
            Write();
            return m_State.Map.TryAdd(key, value);
        }

        public void Add(TKey key, TValue value)
        {
            if (!TryAdd(key, value)) throw new ArgumentException($"Key {key} is already in the NativeHashMap");
        }

        public bool TryGetValue(TKey key, out TValue value)
        {
            Read();
            return m_State.Map.TryGetValue(key, out value);
        }

        public bool ContainsKey(TKey key)
        {
            Read();
            return m_State.Map.ContainsKey(key);
        }

        public bool Remove(TKey key)
        {
            Write();
            return m_State.Map.Remove(key);
        }

        public void Clear()
        {
            Write();
            m_State.Map.Clear();
        }

        public NativeArray<TKey> GetKeyArray(Allocator allocator)
        {
            Read();
            var keys = new NativeArray<TKey>(m_State.Map.Count, allocator, NativeArrayOptions.UninitializedMemory);
            int i = 0;
            foreach (TKey k in m_State.Map.Keys) keys[i++] = k;
            return keys;
        }

        public NativeArray<TValue> GetValueArray(Allocator allocator)
        {
            Read();
            var values = new NativeArray<TValue>(m_State.Map.Count, allocator, NativeArrayOptions.UninitializedMemory);
            int i = 0;
            foreach (TValue v in m_State.Map.Values) values[i++] = v;
            return values;
        }

        /// <summary>For parallel jobs: TryAdd from every batch at once.</summary>
        public ParallelWriter AsParallelWriter() => new ParallelWriter(State);

        public void Dispose()
        {
            State.CheckDispose();
            JobSystem.Free(m_State);
        }

        public JobHandle Dispose(JobHandle inputDeps) => JobSystem.DisposeAfter(State, inputDeps);

        private NativeHashMapState<TKey, TValue> State => m_State ?? throw new ObjectDisposedException("The NativeHashMap was never created");
        private void Read() => State.CheckRead();
        private void Write() => State.CheckWrite(m_ReadOnly);

        NativeSafety INativeContainer.Safety => m_State;
        object INativeContainer.AsReadOnly()
        {
            var copy = this;
            copy.m_ReadOnly = true;
            return copy;
        }

        public IEnumerator<KeyValuePair<TKey, TValue>> GetEnumerator()
        {
            Read();
            return m_State.Map.GetEnumerator();
        }
        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

        /// <summary>Adds from many threads at once.</summary>
        public struct ParallelWriter : INativeContainer
        {
            private readonly NativeHashMapState<TKey, TValue> m_State;
            internal ParallelWriter(NativeHashMapState<TKey, TValue> state) { m_State = state; }

            public bool TryAdd(TKey key, TValue value)
            {
                m_State.CheckWrite(false);
                lock (m_State.Sync) return m_State.Map.TryAdd(key, value);
            }

            NativeSafety INativeContainer.Safety => m_State;
            object INativeContainer.AsReadOnly() => this;
        }
    }

    internal sealed class NativeQueueState<T> : NativeSafety
    {
        public ConcurrentQueue<T> Queue;
        public override void Release() => Queue = null;
    }

    /// <summary>A first-in first-out queue of unmanaged values (like Unity's NativeQueue), usable in jobs; a parallel job
    /// enqueues from every batch through AsParallelWriter().</summary>
    public struct NativeQueue<T> : IDisposable, INativeContainer where T : unmanaged
    {
        private NativeQueueState<T> m_State;
        private bool m_ReadOnly;

        public NativeQueue(Allocator allocator)
        {
            if (allocator == Allocator.None) throw new ArgumentException("Allocator.None cannot allocate", nameof(allocator));
            m_State = new NativeQueueState<T> { Kind = "NativeQueue", Queue = new ConcurrentQueue<T>() };
            m_ReadOnly = false;
            JobSystem.Track(m_State, allocator);
        }

        public bool IsCreated => m_State != null && !m_State.Disposed;
        public int Count
        {
            get
            {
                State.CheckRead();
                return m_State.Queue.Count;
            }
        }
        public bool IsEmpty() => Count == 0;

        public void Enqueue(T value)
        {
            State.CheckWrite(m_ReadOnly);
            m_State.Queue.Enqueue(value);
        }

        public bool TryDequeue(out T value)
        {
            State.CheckWrite(m_ReadOnly);
            return m_State.Queue.TryDequeue(out value);
        }

        public T Dequeue() => TryDequeue(out T v) ? v : throw new InvalidOperationException("The NativeQueue is empty");

        public T Peek()
        {
            State.CheckRead();
            return m_State.Queue.TryPeek(out T v) ? v : throw new InvalidOperationException("The NativeQueue is empty");
        }

        public void Clear()
        {
            State.CheckWrite(m_ReadOnly);
            m_State.Queue.Clear();
        }

        public NativeArray<T> ToArray(Allocator allocator)
        {
            State.CheckRead();
            return new NativeArray<T>(m_State.Queue.ToArray(), allocator);
        }

        public ParallelWriter AsParallelWriter() => new ParallelWriter(State);

        public void Dispose()
        {
            State.CheckDispose();
            JobSystem.Free(m_State);
        }

        public JobHandle Dispose(JobHandle inputDeps) => JobSystem.DisposeAfter(State, inputDeps);

        private NativeQueueState<T> State => m_State ?? throw new ObjectDisposedException("The NativeQueue was never created");

        NativeSafety INativeContainer.Safety => m_State;
        object INativeContainer.AsReadOnly()
        {
            var copy = this;
            copy.m_ReadOnly = true;
            return copy;
        }

        /// <summary>Enqueues from many threads at once.</summary>
        public struct ParallelWriter : INativeContainer
        {
            private readonly NativeQueueState<T> m_State;
            internal ParallelWriter(NativeQueueState<T> state) { m_State = state; }

            public void Enqueue(T value)
            {
                m_State.CheckWrite(false);
                m_State.Queue.Enqueue(value);
            }

            NativeSafety INativeContainer.Safety => m_State;
            object INativeContainer.AsReadOnly() => this;
        }
    }

    // ---- Jobs that move objects ------------------------------------------------------------------------------------

    internal sealed class TransformArrayState : NativeSafety
    {
        public List<Entity> Entities = new();
        public override void Release() => Entities = null;
    }

    /// <summary>The objects a transform job moves (like Unity's TransformAccessArray). Dispose it when done.</summary>
    public struct TransformAccessArray : IDisposable, INativeContainer
    {
        private TransformArrayState m_State;

        public TransformAccessArray(int capacity, int desiredJobCount = -1)
        {
            m_State = new TransformArrayState { Kind = "TransformAccessArray" };
            m_State.Entities.Capacity = Math.Max(0, capacity);
            JobSystem.Track(m_State, Allocator.Persistent);
        }

        public TransformAccessArray(Entity[] entities, int desiredJobCount = -1) : this(entities.Length, desiredJobCount)
        {
            m_State.Entities.AddRange(entities);
        }

        public bool IsCreated => m_State != null && !m_State.Disposed;
        public int Length
        {
            get
            {
                State.CheckRead();
                return m_State.Entities.Count;
            }
        }

        public Entity this[int index]
        {
            get
            {
                State.CheckRead();
                return m_State.Entities[index];
            }
            set
            {
                State.CheckWrite(false);
                m_State.Entities[index] = value;
            }
        }

        public void Add(Entity entity)
        {
            State.CheckWrite(false);
            m_State.Entities.Add(entity);
        }

        public void Add(Transform transform) => Add(transform.Entity);

        public void RemoveAtSwapBack(int index)
        {
            State.CheckWrite(false);
            var list = m_State.Entities;
            list[index] = list[^1];
            list.RemoveAt(list.Count - 1);
        }

        public void Dispose()
        {
            State.CheckDispose();
            JobSystem.Free(m_State);
        }

        internal TransformArrayState State => m_State ?? throw new ObjectDisposedException("The TransformAccessArray was never created");
        NativeSafety INativeContainer.Safety => m_State;
        object INativeContainer.AsReadOnly() => this;
    }

    // One object's transform as a transform job sees it: its local values, its parent's world matrix and rotation (to
    // give and take world values), and which values the job set.
    internal struct TransformData
    {
        public Vector3 LocalPosition, Scale;
        public Quaternion LocalRotation, ParentRotation;
        public Matrix4x4 Parent, InverseParent;
        public bool Valid;
        public byte Dirty; // 1 position, 2 rotation, 4 scale
    }

    /// <summary>An object's transform inside a transform job: read and set like Transform (Position, LocalRotation and
    /// Scale are local, WorldPosition and Rotation in the world). Values are as they were when the job was scheduled;
    /// what the job sets is written back on the main thread when it is completed (or at the frame's end).</summary>
    public readonly struct TransformAccess
    {
        private readonly TransformData[] m_Data;
        private readonly int m_Index;
        private readonly bool m_ReadOnly;
        internal TransformAccess(TransformData[] data, int index, bool readOnly)
        {
            m_Data = data;
            m_Index = index;
            m_ReadOnly = readOnly;
        }

        private ref TransformData D => ref m_Data[m_Index];
        private ref TransformData W
        {
            get
            {
                if (m_ReadOnly) throw new InvalidOperationException("This transform job was scheduled read-only (ScheduleReadOnly): it cannot move objects");
                return ref m_Data[m_Index];
            }
        }

        /// <summary>Whether the object exists (false for one destroyed before the job: setting it does nothing).</summary>
        public bool IsValid => D.Valid;

        public Vector3 Position
        {
            get => D.LocalPosition;
            set
            {
                W.LocalPosition = value;
                W.Dirty |= 1;
            }
        }

        public Vector3 WorldPosition
        {
            get => Vector3.Transform(D.LocalPosition, D.Parent);
            set
            {
                W.LocalPosition = Vector3.Transform(value, D.InverseParent);
                W.Dirty |= 1;
            }
        }

        public Quaternion LocalRotation
        {
            get => D.LocalRotation;
            set
            {
                W.LocalRotation = Quaternion.Normalize(value);
                W.Dirty |= 2;
            }
        }

        public Quaternion Rotation
        {
            get => D.ParentRotation * D.LocalRotation;
            set
            {
                W.LocalRotation = Quaternion.Normalize(Quaternion.Inverse(D.ParentRotation) * value);
                W.Dirty |= 2;
            }
        }

        public Vector3 Scale
        {
            get => D.Scale;
            set
            {
                W.Scale = value;
                W.Dirty |= 4;
            }
        }
    }

    /// <summary>A job run for each object of a TransformAccessArray, spread over the workers (like Unity's).</summary>
    public interface IJobParallelForTransform
    {
        void Execute(int index, TransformAccess transform);
    }

    public static class IJobParallelForTransformExtensions
    {
        /// <summary>Moves the objects on the workers; what the job sets is written back when it is completed.</summary>
        public static JobHandle Schedule<T>(this T job, TransformAccessArray transforms, JobHandle dependsOn = default)
            where T : struct, IJobParallelForTransform => Schedule(job, transforms, dependsOn, false);

        /// <summary>Reads the objects on the workers (setting a value throws); nothing is written back.</summary>
        public static JobHandle ScheduleReadOnly<T>(this T job, TransformAccessArray transforms, int batchSize, JobHandle dependsOn = default)
            where T : struct, IJobParallelForTransform => Schedule(job, transforms, dependsOn, true, batchSize);

        /// <summary>Runs it here, setting the objects at once.</summary>
        public static void Run<T>(this T job, TransformAccessArray transforms) where T : struct, IJobParallelForTransform
        {
            JobSystem.CompleteUsers(job);
            Entity[] entities = transforms.State.Entities.ToArray();
            TransformData[] data = Snapshot(entities);
            for (int i = 0; i < data.Length; ++i)
                if (data[i].Valid) job.Execute(i, new TransformAccess(data, i, false));
            Apply(entities, data);
        }

        private static JobHandle Schedule<T>(T job, TransformAccessArray transforms, JobHandle dependsOn, bool readOnly, int batchSize = 32)
            where T : struct, IJobParallelForTransform
        {
            T copy = JobSystem.Prepare(job, out var uses);
            TransformArrayState state = transforms.State;
            state.CheckRead();
            uses.Add((state, readOnly));
            Entity[] entities = state.Entities.ToArray();
            TransformData[] data = Snapshot(entities); // (the main thread reads the objects now)
            JobHandle handle = JobSystem.ScheduleBatches(data.Length, batchSize, (start, end) =>
            {
                for (int i = start; i < end; ++i)
                    if (data[i].Valid) copy.Execute(i, new TransformAccess(data, i, readOnly));
            }, dependsOn, typeof(T).Name, uses);
            if (!readOnly) JobSystem.AddFinish(handle, () => Apply(entities, data));
            return handle;
        }

        private static TransformData[] Snapshot(Entity[] entities)
        {
            var data = new TransformData[entities.Length];
            for (int i = 0; i < entities.Length; ++i)
            {
                Entity e = entities[i];
                if (!e.IsValid) continue;
                Transform t = e.Transform;
                ref TransformData d = ref data[i];
                d.Valid = true;
                d.LocalPosition = t.Position;
                d.LocalRotation = t.LocalRotation;
                d.Scale = t.Scale;
                Entity parent = e.Parent;
                d.Parent = parent.IsValid ? parent.Transform.LocalToWorld : Matrix4x4.Identity;
                if (!Matrix4x4.Invert(d.Parent, out d.InverseParent)) d.InverseParent = Matrix4x4.Identity;
                d.ParentRotation = Quaternion.Normalize(t.Rotation * Quaternion.Inverse(d.LocalRotation)); // (world = parent * local)
            }
            return data;
        }

        private static void Apply(Entity[] entities, TransformData[] data)
        {
            for (int i = 0; i < entities.Length; ++i)
            {
                ref TransformData d = ref data[i];
                if (!d.Valid || d.Dirty == 0 || !entities[i].IsValid) continue;
                Transform t = entities[i].Transform;
                if ((d.Dirty & 1) != 0) t.Position = d.LocalPosition;
                if ((d.Dirty & 2) != 0) t.LocalRotation = d.LocalRotation;
                if ((d.Dirty & 4) != 0) t.Scale = d.Scale;
            }
        }
    }
}
