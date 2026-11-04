using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using System.Runtime.InteropServices;
using Dimension.Interop;

namespace Dimension
{
    /// <summary>Base class for C# gameplay scripts (the engine's MonoBehaviour).</summary>
    /// <remarks>Public fields of type float, int, bool, string, Vector2/3/4, Color and Entity appear in the Inspector.</remarks>
    public abstract class Behaviour
    {
        public Entity Entity { get; internal set; }
        public Transform Transform => Entity.Transform;
        public string Name { get => Entity.Name; set { var e = Entity; e.Name = value; } }
        internal IntPtr Handle; // the engine's handle to this object

        /// <summary>Whether this script is enabled (its checkbox in the Inspector). A disabled script gets no updates;
        /// turning it off or on calls OnDisable / OnEnable while its object is active.</summary>
        public bool Enabled
        {
            get => Handle != IntPtr.Zero && Entity.ScriptOp(15, Handle) == 1;
            set { if (Handle != IntPtr.Zero) Entity.ScriptOp(value ? 16 : 17, Handle); }
        }
        public string Tag { get => Entity.Tag; set { var e = Entity; e.Tag = value; } }
        public bool CompareTag(string tag) => Entity.CompareTag(tag);

        public virtual void OnCreate() { }
        public virtual void OnUpdate(float deltaTime) { }
        public virtual void OnFixedUpdate(float fixedDeltaTime) { }
        public virtual void OnDestroy() { }
        /// <summary>Called when the script starts counting while playing: after OnCreate when its object is active and
        /// it is enabled, and again each time its object (or a parent) is activated or the script is enabled.</summary>
        public virtual void OnEnable() { }
        /// <summary>Called when the script stops counting: its object or a parent is deactivated, the script is
        /// disabled, or before OnDestroy.</summary>
        public virtual void OnDisable() { }
        public virtual void OnCollisionEnter(Entity other) { }
        public virtual void OnCollisionExit(Entity other) { }
        /// <summary>As OnCollisionEnter(Entity) (which it calls unless overridden), with where and how hard they hit.</summary>
        public virtual void OnCollisionEnter(Collision collision) => OnCollisionEnter(collision.Other);
        /// <summary>Every physics step while touching another collider, except while both bodies sleep (only called on
        /// classes that override it).</summary>
        public virtual void OnCollisionStay(Collision collision) { }
        /// <summary>As OnCollisionExit(Entity) (which it calls unless overridden); a Collision without contacts.</summary>
        public virtual void OnCollisionExit(Collision collision) => OnCollisionExit(collision.Other);
        public virtual void OnTriggerEnter(Entity other) { }
        public virtual void OnTriggerExit(Entity other) { }
        /// <summary>Every physics step while inside a trigger (or something is inside this one), except while both
        /// bodies sleep (only called on classes that override it).</summary>
        public virtual void OnTriggerStay(Entity other) { }
        /// <summary>The pointer moved over this object's collider (seen through the main camera, not through UI).</summary>
        public virtual void OnMouseEnter() { }
        /// <summary>Every frame while the pointer is over this object's collider.</summary>
        public virtual void OnMouseOver() { }
        public virtual void OnMouseExit() { }
        /// <summary>The mouse button went down over this object.</summary>
        public virtual void OnMouseDown() { }
        /// <summary>Every frame while the button pressed on this object is held (even off it): dragging.</summary>
        public virtual void OnMouseDrag() { }
        /// <summary>The button pressed on this object was released (anywhere).</summary>
        public virtual void OnMouseUp() { }
        /// <summary>The button pressed on this object was released over it: a click.</summary>
        public virtual void OnMouseUpAsButton() { }
        /// <summary>Called when a UI Button on this entity is clicked.</summary>
        public virtual void OnClick() { }
        /// <summary>Called when a UI Slider (its value), UI Toggle (1 = on, 0 = off) or UI Dropdown (option index) on this entity changes.</summary>
        public virtual void OnValueChanged(float value) { }
        /// <summary>Called when a Joint or Joint 2D on this entity breaks (its Break Force/Torque was exceeded).</summary>
        public virtual void OnJointBreak(float force) { }
        /// <summary>Called when the text of a UI Input Field on this entity is edited.</summary>
        public virtual void OnTextChanged(string text) { }
        /// <summary>Called when Enter is pressed in a UI Input Field on this entity.</summary>
        public virtual void OnSubmit(string text) { }
        /// <summary>Called when an Animator state event on this entity is reached (the event's function name).</summary>
        public virtual void OnAnimationEvent(string name) { }
        /// <summary>Called instead of OnCreate when the scripts were recompiled and reloaded while playing. Serialized
        /// fields (public and [SerializeField]) keep their values; other state, coroutines and event listeners start over.</summary>
        public virtual void OnReloaded() { }
        /// <summary>Called in the editor to draw the script's gizmos (Gizmos.DrawLine, ...) in the Scene view: while
        /// playing on the live script, otherwise on one holding its saved values (OnCreate has not run).</summary>
        public virtual void OnDrawGizmos() { }
        /// <summary>As OnDrawGizmos, for scripts of the selected objects only.</summary>
        public virtual void OnDrawGizmosSelected() { }

        // ---- Coroutines and delayed calls (like MonoBehaviour) ---------------------------------
        internal CoroutineRunner Coroutines;

        /// <summary>Runs `routine` until its first yield now, then resumes it every frame after OnUpdate.
        /// Yield null (next frame), WaitForSeconds, WaitUntil, WaitWhile, another IEnumerator (run nested) or a Coroutine (wait for it).</summary>
        public Coroutine StartCoroutine(System.Collections.IEnumerator routine) => (Coroutines ??= new CoroutineRunner()).Start(routine, null);
        public void StopCoroutine(Coroutine coroutine) => Coroutines?.Stop(coroutine);
        public void StopAllCoroutines() => Coroutines?.StopAll();

        /// <summary>Calls the parameterless method `methodName` on this script after `delay` seconds.</summary>
        public void Invoke(string methodName, float delay) => StartInvoke(methodName, delay, -1.0f);
        /// <summary>Calls `methodName` after `delay` seconds, then every `rate` seconds.</summary>
        public void InvokeRepeating(string methodName, float delay, float rate) => StartInvoke(methodName, delay, MathF.Max(rate, 1e-4f));
        public void CancelInvoke() => Coroutines?.StopTagged(null);
        public void CancelInvoke(string methodName) => Coroutines?.StopTagged(methodName);
        public bool IsInvoking(string methodName) => Coroutines != null && Coroutines.HasTagged(methodName);

        /// <summary>Destroys `entity` after `delay` seconds.</summary>
        public void Destroy(Entity entity, float delay) => StartCoroutine(DestroyLater(entity, delay));

        private void StartInvoke(string methodName, float delay, float rate)
        {
            var method = GetType().GetMethod(methodName, System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Public |
                                                         System.Reflection.BindingFlags.NonPublic, null, Type.EmptyTypes, null);
            if (method == null)
            {
                Debug.LogError($"{GetType().Name}.Invoke: no parameterless method '{methodName}'");
                return;
            }
            (Coroutines ??= new CoroutineRunner()).Start(InvokeRoutine(method, delay, rate), methodName);
        }

        private System.Collections.IEnumerator InvokeRoutine(System.Reflection.MethodInfo method, float delay, float rate)
        {
            if (delay > 0.0f) yield return new WaitForSeconds(delay);
            do
            {
                method.Invoke(this, null);
                if (rate > 0.0f) yield return new WaitForSeconds(rate);
            } while (rate > 0.0f);
        }

        private static System.Collections.IEnumerator DestroyLater(Entity entity, float delay)
        {
            yield return new WaitForSeconds(delay);
            if (entity.IsValid) entity.Destroy();
        }

        /// <summary>An engine component (Rigidbody, Camera, ...) or another script (a Behaviour class) on this object.</summary>
        public T GetComponent<T>() where T : class => Entity.GetComponent<T>();
        public T[] GetComponents<T>() where T : class => Entity.GetComponents<T>();
        public T GetComponentInChildren<T>(bool includeInactive = false) where T : class => Entity.GetComponentInChildren<T>(includeInactive);
        public T[] GetComponentsInChildren<T>(bool includeInactive = false) where T : class => Entity.GetComponentsInChildren<T>(includeInactive);
        public T GetComponentInParent<T>() where T : class => Entity.GetComponentInParent<T>();
        public T AddComponent<T>() where T : class => Entity.AddComponent<T>();
        /// <summary>The first active object's script or engine component of a type in this scene (null when none).</summary>
        public T FindObjectOfType<T>(bool includeInactive = false) where T : class => Entity.FindObjectOfType<T>(includeInactive);
        public T[] FindObjectsOfType<T>(bool includeInactive = false) where T : class => Entity.FindObjectsOfType<T>(includeInactive);
        public Component GetComponent(string componentName) => Entity.GetComponent(componentName);
        public bool HasComponent<T>() where T : class => Entity.HasComponent<T>();
        public Entity Find(string name) => Entity.Find(name);
        public Entity Instantiate(ulong prefabGuid, Vector3 position) => Entity.Instantiate(prefabGuid, position);
        public Entity Instantiate(ulong prefabGuid, Vector3 position, Quaternion rotation, Entity parent = default) =>
            Entity.Instantiate(prefabGuid, position, rotation, parent);
        /// <summary>A copy of an object (see Entity.Instantiate).</summary>
        public Entity Instantiate(Entity original, Entity parent = default) => Entity.Instantiate(original, parent);
        public Entity Instantiate(Entity original, Vector3 position, Quaternion rotation, Entity parent = default) =>
            Entity.Instantiate(original, position, rotation, parent);
        public T Instantiate<T>(T original, Entity parent = default) where T : Behaviour => Entity.Instantiate(original, parent);
        public T Instantiate<T>(T original, Vector3 position, Quaternion rotation, Entity parent = default) where T : Behaviour =>
            Entity.Instantiate(original, position, rotation, parent);
        public void Destroy(Entity entity) => entity.Destroy();
        /// <summary>Takes an engine component off its object at the end of the frame.</summary>
        public bool Destroy(Component component) => Entity.Destroy(component);
        /// <summary>Takes a script off its object at the end of the frame (Destroy(this) for this one).</summary>
        public bool Destroy(Behaviour script) => Entity.Destroy(script);
    }

    /// <summary>A cell or point on an integer grid (a Tilemap's cells).</summary>
    public readonly record struct Vector2Int(int X, int Y)
    {
        public static Vector2Int Zero => new(0, 0);
        public static Vector2Int One => new(1, 1);
        public static Vector2Int Up => new(0, 1);
        public static Vector2Int Down => new(0, -1);
        public static Vector2Int Left => new(-1, 0);
        public static Vector2Int Right => new(1, 0);
        public static Vector2Int operator +(Vector2Int a, Vector2Int b) => new(a.X + b.X, a.Y + b.Y);
        public static Vector2Int operator -(Vector2Int a, Vector2Int b) => new(a.X - b.X, a.Y - b.Y);
        public static Vector2Int operator *(Vector2Int a, int k) => new(a.X * k, a.Y * k);
        public static implicit operator Vector2(Vector2Int v) => new(v.X, v.Y);
        /// <summary>The cell containing a point (rounded down).</summary>
        public static Vector2Int FloorToInt(Vector2 v) => new((int)MathF.Floor(v.X), (int)MathF.Floor(v.Y));
        public override string ToString() => $"({X}, {Y})";
    }

    /// <summary>A rectangle: X, Y is its top-left corner (screen pixels grow down and right).</summary>
    public readonly record struct Rect(float X, float Y, float Width, float Height)
    {
        public Vector2 Min => new(X, Y);
        public Vector2 Max => new(X + Width, Y + Height);
        public Vector2 Size => new(Width, Height);
        public Vector2 Center => new(X + Width * 0.5f, Y + Height * 0.5f);
        public bool Contains(Vector2 p) => p.X >= X && p.Y >= Y && p.X < X + Width && p.Y < Y + Height;
        public bool Overlaps(Rect o) => o.X < X + Width && X < o.X + o.Width && o.Y < Y + Height && Y < o.Y + o.Height;
        public static Rect MinMax(Vector2 min, Vector2 max) => new(min.X, min.Y, max.X - min.X, max.Y - min.Y);
    }

    /// <summary>RGBA color in sRGB space, components 0..1.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Color
    {
        public float R, G, B, A;
        public Color(float r, float g, float b, float a = 1.0f) { R = r; G = g; B = b; A = a; }
        public static Color White => new(1, 1, 1);
        public static Color Black => new(0, 0, 0);
        public static Color Red => new(1, 0, 0);
        public static Color Green => new(0, 1, 0);
        public static Color Blue => new(0, 0, 1);
        public static Color Lerp(Color a, Color b, float t) =>
            new(a.R + (b.R - a.R) * t, a.G + (b.G - a.G) * t, a.B + (b.B - a.B) * t, a.A + (b.A - a.A) * t);
        /// <summary>Hue/saturation/value to RGB (all 0..1).</summary>
        public static Color FromHSV(float h, float s, float v)
        {
            h = (h % 1.0f + 1.0f) % 1.0f * 6.0f;
            int i = (int)MathF.Floor(h);
            float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
            return i switch { 0 => new(v, t, p), 1 => new(q, v, p), 2 => new(p, v, t), 3 => new(p, q, v), 4 => new(t, p, v), _ => new(v, p, q) };
        }
        public override string ToString() => $"({R:0.###}, {G:0.###}, {B:0.###}, {A:0.###})";
    }

    /// <summary>Handle to a scene entity (GameObject).</summary>
    public readonly unsafe struct Entity : IEquatable<Entity>
    {
        internal readonly IntPtr Scene;
        internal readonly ulong Id;
        internal Entity(IntPtr scene, ulong id) { Scene = scene; Id = id; }

        public static Entity Null => default;
        public bool IsValid => Scene != IntPtr.Zero && Native.Api.IsValid(Scene, Id) != 0;
        public Transform Transform => new Transform { Entity = this };

        public string Name
        {
            get => Native.FromUtf8(Native.Api.GetName(Scene, Id));
            set { var self = this; Native.WithUtf8(value, p => Native.Api.SetName(self.Scene, self.Id, (byte*)p)); }
        }

        public bool HasComponent(string componentName)
        {
            var self = this;
            return Native.WithUtf8(componentName, p => Native.Api.HasComponent(self.Scene, self.Id, (byte*)p)) != 0;
        }
        public bool HasComponent<T>() where T : class => GetComponent<T>() != null;

        /// <summary>Any component by its Inspector name (null if the entity has none), for its numeric fields.</summary>
        public Component GetComponent(string componentName) => HasComponent(componentName) ? new NamedComponent(componentName) { Entity = this } : null;

        // T is an engine component (a Component class) or a script (a Behaviour class).
        private static bool IsScript(Type t) => typeof(Behaviour).IsAssignableFrom(t);
        private Component Wrapper(Type t)
        {
            if (!typeof(Component).IsAssignableFrom(t) || t.IsAbstract)
                throw new ArgumentException($"{t.Name} is neither an engine component nor a script (Behaviour)");
            var c = (Component)Activator.CreateInstance(t);
            c.Entity = this;
            return c;
        }

        /// <summary>An engine component (Rigidbody, Camera, ...) or a script of a type (a Behaviour class, or one deriving
        /// from it) on this object; null when it has none.</summary>
        public T GetComponent<T>() where T : class
        {
            if (IsScript(typeof(T))) return ScriptHost.ScriptsOf(this).OfType<T>().FirstOrDefault();
            Component c = Wrapper(typeof(T));
            return HasComponent(c.NativeName) ? (T)(object)c : null;
        }

        /// <summary>Every script of a type on this object (an engine component: the one, as it has at most one).</summary>
        public T[] GetComponents<T>() where T : class =>
            IsScript(typeof(T)) ? ScriptHost.ScriptsOf(this).OfType<T>().ToArray() : GetComponent<T>() is { } c ? new[] { c } : Array.Empty<T>();

        /// <summary>Adds an engine component (if missing) or a script; a script added while playing starts at once
        /// (OnCreate, OnEnable) and is returned.</summary>
        public T AddComponent<T>() where T : class
        {
            var self = this;
            if (IsScript(typeof(T)))
            {
                IntPtr handle = Native.WithUtf8<IntPtr>(typeof(T).FullName, p => Native.Api.AddScript(self.Scene, self.Id, (byte*)p));
                return handle != IntPtr.Zero ? System.Runtime.InteropServices.GCHandle.FromIntPtr(handle).Target as T : null;
            }
            Component c = Wrapper(typeof(T));
            if (!HasComponent(c.NativeName)) Native.WithUtf8(c.NativeName, p => Native.Api.AddComponent(self.Scene, self.Id, (byte*)p));
            return (T)(object)c;
        }

        /// <summary>The first object's script or engine component of a type in the running script's scene (active objects
        /// unless `includeInactive`); null when none.</summary>
        public static T FindObjectOfType<T>(bool includeInactive = false) where T : class => FindObjectsOfType<T>(includeInactive).FirstOrDefault();
        /// <summary>Every such script or component (scripts in object order, an object's in its order).</summary>
        public static T[] FindObjectsOfType<T>(bool includeInactive = false) where T : class
        {
            IntPtr scene = Native.CurrentScene;
            if (scene == IntPtr.Zero) return Array.Empty<T>();
            if (IsScript(typeof(T))) return ScriptHost.ScriptsInScene(scene, includeInactive).OfType<T>().ToArray();
            var found = new List<ulong>();
            foreach (string name in new Entity(scene, 0).Wrapper(typeof(T)).NativeNames)
            {
                int count = Native.WithUtf8<int>(name, p => Native.Api.EntitiesWithComponent(scene, (byte*)p, null, 0, includeInactive ? 1 : 0));
                var ids = new ulong[count];
                fixed (ulong* q = ids)
                {
                    ulong* at = q;
                    count = Math.Min(count, Native.WithUtf8<int>(name, p => Native.Api.EntitiesWithComponent(scene, (byte*)p, at, ids.Length, includeInactive ? 1 : 0)));
                }
                for (int i = 0; i < count; ++i) if (!found.Contains(ids[i])) found.Add(ids[i]);
            }
            return found.Select(id => (T)(object)new Entity(scene, id).Wrapper(typeof(T))).ToArray();
        }

        public void Destroy() { if (Scene != IntPtr.Zero) Native.Api.Destroy(Scene, Id); }

        // ---- Hierarchy, active state, layer and tag (like Unity's GameObject and Transform) ---------------------
        // Hierarchy ops: 0 parent, 1 child count, 2 child (arg), 3 / 4 set parent keeping the world / local transform,
        // 5 active self, 6 active in hierarchy, 7 set active, 8 layer, 9 set layer, 10 compare tag, 11 set tag,
        // 12 sibling index, 13 set it, 14 first with tag, 15 script enabled (arg: its handle), 16 enable, 17 disable.
        private ulong Op(int op, ulong arg = 0) => Scene == IntPtr.Zero ? ulong.MaxValue : Native.Api.Hierarchy(Scene, Id, op, arg, null);
        private ulong Op(int op, string text)
        {
            if (Scene == IntPtr.Zero) return 0;
            var self = this;
            return Native.WithUtf8<ulong>(text ?? "", p => Native.Api.Hierarchy(self.Scene, self.Id, op, 0, (byte*)p));
        }
        private Entity Wrap(ulong id) => id == ulong.MaxValue ? Null : new Entity(Scene, id);
        internal ulong ScriptOp(int op, IntPtr handle) => Scene == IntPtr.Zero ? 0 : Native.Api.Hierarchy(Scene, Id, op, (ulong)handle, null);

        /// <summary>The parent object (Null for a root).</summary>
        public Entity Parent => Wrap(Op(0));
        /// <summary>The topmost parent (itself for a root).</summary>
        public Entity Root { get { Entity e = this; for (Entity p = e.Parent; p.IsValid; p = p.Parent) e = p; return e; } }
        /// <summary>Moves this object under `parent` (Null: to the scene root), keeping where it is in the world unless
        /// `worldPositionStays` is false (then its local position, rotation and scale stay as they are).</summary>
        public void SetParent(Entity parent, bool worldPositionStays = true) =>
            Op(worldPositionStays ? 3 : 4, parent.IsValid ? parent.Id : ulong.MaxValue);
        public int ChildCount => Scene == IntPtr.Zero ? 0 : (int)Op(1);
        /// <summary>The child at `index` (in Hierarchy order), Null when out of range.</summary>
        public Entity GetChild(int index) => index < 0 ? Null : Wrap(Op(2, (ulong)index));
        /// <summary>The direct children, in Hierarchy order.</summary>
        public Entity[] Children
        {
            get
            {
                var result = new Entity[ChildCount];
                for (int i = 0; i < result.Length; ++i) result[i] = GetChild(i);
                return result;
            }
        }
        /// <summary>A descendant by name, or by a path of names ("Arm/Hand"); Null when there is none.</summary>
        public Entity FindChild(string path)
        {
            Entity at = this;
            foreach (string part in (path ?? "").Split('/', StringSplitOptions.RemoveEmptyEntries))
            {
                Entity next = Null;
                for (int i = 0, n = at.ChildCount; i < n && !next.IsValid; ++i)
                    if (at.GetChild(i).Name == part) next = at.GetChild(i);
                if (!next.IsValid) return Null;
                at = next;
            }
            return at == this ? Null : at;
        }
        /// <summary>Whether this object is somewhere under `ancestor`.</summary>
        public bool IsChildOf(Entity ancestor)
        {
            for (Entity p = Parent; p.IsValid; p = p.Parent)
                if (p == ancestor) return true;
            return false;
        }
        /// <summary>The position among its siblings (the Hierarchy order).</summary>
        public int SiblingIndex { get => (int)Op(12); set => Op(13, (ulong)Math.Max(value, 0)); }

        /// <summary>The object's own Active box (its parents may still turn it off; see ActiveInHierarchy).</summary>
        public bool ActiveSelf => Op(5) == 1;
        /// <summary>Whether the object and all of its parents are active: only then do its scripts update, its
        /// renderers draw and its colliders collide.</summary>
        public bool ActiveInHierarchy => Op(6) == 1;
        /// <summary>Turns the object on or off with its children. Scripts of the objects this switches get OnEnable /
        /// OnDisable right away.</summary>
        public void SetActive(bool active) => Op(7, active ? 1ul : 0ul);

        /// <summary>The physics layer (0-31, named in Project Settings > Physics Layers). Colliders take a new layer at
        /// the next physics step.</summary>
        public int Layer { get => (int)Op(8); set => Op(9, (ulong)Math.Clamp(value, 0, 31)); }
        /// <summary>The tag ("Untagged" when none; setting "" clears it).</summary>
        public string Tag
        {
            get => Scene == IntPtr.Zero ? "" : Native.FromUtf8(Native.Api.GetTag(Scene, Id));
            set => Op(11, value);
        }
        public bool CompareTag(string tag) => Op(10, tag) == 1;

        /// <summary>The first object with a tag in the running script's scene (Null when none).</summary>
        public static Entity FindWithTag(string tag)
        {
            IntPtr scene = Native.CurrentScene;
            if (scene == IntPtr.Zero) return Null;
            ulong id = Native.WithUtf8<ulong>(tag ?? "", p => Native.Api.Hierarchy(scene, 0, 14, 0, (byte*)p));
            return id == ulong.MaxValue ? Null : new Entity(scene, id);
        }
        /// <summary>Every object with a tag (like Unity's FindGameObjectsWithTag).</summary>
        public static Entity[] FindAllWithTag(string tag)
        {
            IntPtr scene = Native.CurrentScene;
            if (scene == IntPtr.Zero) return Array.Empty<Entity>();
            var ids = new ulong[256];
            int count;
            fixed (ulong* ptr = ids)
            {
                ulong* q = ptr;
                count = Native.WithUtf8<int>(tag ?? "", p => Native.Api.FindAllWithTag(scene, (byte*)p, q, 256));
            }
            if (count > ids.Length)
            {
                ids = new ulong[count];
                fixed (ulong* ptr = ids)
                {
                    ulong* q = ptr;
                    int capacity = ids.Length;
                    count = Math.Min(capacity, Native.WithUtf8<int>(tag ?? "", p => Native.Api.FindAllWithTag(scene, (byte*)p, q, capacity)));
                }
            }
            var result = new Entity[count];
            for (int i = 0; i < count; ++i) result[i] = new Entity(scene, ids[i]);
            return result;
        }

        /// <summary>A component on this object or the first of its descendants that has one (depth first, in
        /// Hierarchy order); inactive objects are skipped unless `includeInactive`.</summary>
        public T GetComponentInChildren<T>(bool includeInactive = false) where T : class
        {
            if (!includeInactive && !ActiveInHierarchy) return null;
            if (GetComponent<T>() is { } own) return own;
            for (int i = 0, n = ChildCount; i < n; ++i)
                if (GetChild(i).GetComponentInChildren<T>(includeInactive) is { } found) return found;
            return null;
        }
        /// <summary>The components on this object and all of its descendants (depth first).</summary>
        public T[] GetComponentsInChildren<T>(bool includeInactive = false) where T : class
        {
            var list = new System.Collections.Generic.List<T>();
            CollectInChildren(list, includeInactive);
            return list.ToArray();
        }
        private void CollectInChildren<T>(System.Collections.Generic.List<T> list, bool includeInactive) where T : class
        {
            if (!includeInactive && !ActiveInHierarchy) return;
            list.AddRange(GetComponents<T>());
            for (int i = 0, n = ChildCount; i < n; ++i) GetChild(i).CollectInChildren(list, includeInactive);
        }
        /// <summary>A component on this object or the nearest parent that has one.</summary>
        public T GetComponentInParent<T>() where T : class
        {
            for (Entity e = this; e.IsValid; e = e.Parent)
                if (e.GetComponent<T>() is { } found) return found;
            return null;
        }

        /// <summary>Raised when this entity's collider starts touching another (any script can listen, not only this entity's).</summary>
        public event Action<Entity> CollisionEnter
        {
            add => EngineEvents.Add(this, EngineEvents.Kind.CollisionEnter, value);
            remove => EngineEvents.Remove(this, EngineEvents.Kind.CollisionEnter, value);
        }
        public event Action<Entity> CollisionExit
        {
            add => EngineEvents.Add(this, EngineEvents.Kind.CollisionExit, value);
            remove => EngineEvents.Remove(this, EngineEvents.Kind.CollisionExit, value);
        }
        /// <summary>Raised when this entity enters or touches a trigger, or something enters this trigger.</summary>
        public event Action<Entity> TriggerEnter
        {
            add => EngineEvents.Add(this, EngineEvents.Kind.TriggerEnter, value);
            remove => EngineEvents.Remove(this, EngineEvents.Kind.TriggerEnter, value);
        }
        public event Action<Entity> TriggerExit
        {
            add => EngineEvents.Add(this, EngineEvents.Kind.TriggerExit, value);
            remove => EngineEvents.Remove(this, EngineEvents.Kind.TriggerExit, value);
        }

        /// <summary>Finds an entity by name in the scene of the currently running script.</summary>
        public static Entity Find(string name)
        {
            IntPtr scene = Native.CurrentScene;
            ulong id = Native.WithUtf8<ulong>(name, p => Native.Api.FindEntity(scene, (byte*)p));
            return id == ulong.MaxValue ? Null : new Entity(scene, id);
        }

        // A copy (kind 0: of the object `source`, 1: of the prefab `source`) with the pose parts that are given.
        private static Entity Copy(int kind, ulong source, Vector3? position, Quaternion? rotation, Entity parent)
        {
            IntPtr scene = Native.CurrentScene;
            if (scene == IntPtr.Zero) return Null;
            Vector3 p = position ?? Vector3.Zero;
            Quaternion r = rotation ?? Quaternion.Identity;
            float* pose = stackalloc float[7] { p.X, p.Y, p.Z, r.X, r.Y, r.Z, r.W };
            int flags = (position.HasValue ? 1 : 0) | (rotation.HasValue ? 2 : 0);
            ulong id = Native.Api.InstantiateObject(scene, kind, source, pose, flags, parent.IsValid ? parent.Id : ulong.MaxValue);
            return id == ulong.MaxValue ? Null : new Entity(scene, id);
        }

        /// <summary>A copy of an object with its children, components and scripts (like Unity's Instantiate): at the
        /// original's place in the world, as a root object or under `parent`. While playing its scripts start with the
        /// original's current field values; references inside the copied objects point to the copies.</summary>
        public static Entity Instantiate(Entity original, Entity parent = default) =>
            original.IsValid ? Copy(0, original.Id, null, null, parent) : Null;
        /// <summary>A copy at a world position and rotation.</summary>
        public static Entity Instantiate(Entity original, Vector3 position, Quaternion rotation, Entity parent = default) =>
            original.IsValid ? Copy(0, original.Id, position, rotation, parent) : Null;
        /// <summary>A copy of the object a script is on; the copy's script of the same place (its own copy).</summary>
        public static T Instantiate<T>(T original, Entity parent = default) where T : Behaviour => CopyOf(original, Instantiate(original.Entity, parent));
        public static T Instantiate<T>(T original, Vector3 position, Quaternion rotation, Entity parent = default) where T : Behaviour =>
            CopyOf(original, Instantiate(original.Entity, position, rotation, parent));
        private static T CopyOf<T>(T original, Entity copy) where T : Behaviour
        {
            int index = ScriptHost.ScriptsOf(original.Entity).IndexOf(original);
            var scripts = ScriptHost.ScriptsOf(copy);
            return index >= 0 && index < scripts.Count ? scripts[index] as T : null;
        }
        /// <summary>A prefab instance at a world position and rotation, under `parent` (default: a root object).</summary>
        public static Entity Instantiate(ulong prefabGuid, Vector3 position, Quaternion rotation, Entity parent = default) =>
            Copy(1, prefabGuid, position, rotation, parent);

        /// <summary>Takes an engine component off its object at the end of the frame (a physics component's body is made
        /// again without it). False for Transform and the like, which every object has.</summary>
        public static bool Destroy(Component component)
        {
            if (component == null || !component.Entity.IsValid) return false;
            Entity e = component.Entity;
            return Native.WithUtf8<int>(component.NativeName, p => Native.Api.RemoveComponent(e.Scene, e.Id, (byte*)p, IntPtr.Zero)) != 0;
        }
        /// <summary>Takes a script off its object at the end of the frame (OnDisable, then OnDestroy).</summary>
        public static bool Destroy(Behaviour script) =>
            script != null && script.Handle != IntPtr.Zero && Native.Api.RemoveComponent(script.Entity.Scene, script.Entity.Id, null, script.Handle) != 0;

        /// <summary>Instantiates a prefab asset (by GUID, as shown in the Inspector) at a world position.</summary>
        public static Entity Instantiate(ulong prefabGuid, Vector3 position)
        {
            IntPtr scene = Native.CurrentScene;
            ulong id = Native.Api.Instantiate(scene, prefabGuid, (float*)&position);
            return id == ulong.MaxValue ? Null : new Entity(scene, id);
        }

        public bool Equals(Entity other) => Scene == other.Scene && Id == other.Id;
        public override bool Equals(object obj) => obj is Entity e && Equals(e);
        public override int GetHashCode() => HashCode.Combine(Scene, Id);
        public static bool operator ==(Entity a, Entity b) => a.Equals(b);
        public static bool operator !=(Entity a, Entity b) => !a.Equals(b);
        public override string ToString() => IsValid ? $"Entity({Name})" : "Entity(null)";
    }

    public static unsafe class Debug
    {
        private static void Write(int level, string message) =>
            Native.WithUtf8(message ?? "null", p => Native.Api.Log(level, (byte*)p));
        public static void Log(object message) => Write(1, message?.ToString());
        public static void LogWarning(object message) => Write(2, message?.ToString());
        public static void LogError(object message) => Write(3, message?.ToString());

        /// <summary>
        /// A line from <paramref name="start"/> to <paramref name="end"/> in the editor's Scene and Game views (with the
        /// gizmos; not in builds), until the next update, or for <paramref name="duration"/> seconds of game time.
        /// </summary>
        public static void DrawLine(System.Numerics.Vector3 start, System.Numerics.Vector3 end, Color? color = null, float duration = 0.0f)
        {
            Color c = color ?? Color.White;
            float* data = stackalloc float[11] { start.X, start.Y, start.Z, end.X, end.Y, end.Z, c.R, c.G, c.B, c.A, duration };
            Native.Api.DebugLine(data);
        }

        /// <summary>A line from <paramref name="start"/> along <paramref name="direction"/> (its length the line's); see DrawLine.</summary>
        public static void DrawRay(System.Numerics.Vector3 start, System.Numerics.Vector3 direction, Color? color = null, float duration = 0.0f) =>
            DrawLine(start, start + direction, color, duration);
    }

    /// <summary>
    /// The engine's Profiler (Window > Profiler) from scripts: samples show as sections (nested in the section that
    /// runs the script, such as Scripts), with their time and calls, in its tables and timeline; counters show beside the
    /// frame's draw calls.
    /// </summary>
    /// <example>
    /// <code>
    /// using (Profiler.Sample("Pathfinding")) { FindPath(); }
    /// Profiler.BeginSample("Spawn"); Spawn(); Profiler.EndSample();
    /// Profiler.SetCounter("Enemies", enemies.Count);
    /// </code>
    /// </example>
    public static unsafe class Profiler
    {
        /// <summary>Starts a section named <paramref name="name"/> (until <see cref="EndSample"/>; nest them freely).</summary>
        public static void BeginSample(string name) => Native.WithUtf8(name ?? "Sample", p => Native.Api.ProfilerSample(0, (byte*)p, 0.0f));

        /// <summary>Ends the innermost section begun with <see cref="BeginSample"/>. One left open ends with the frame.</summary>
        public static void EndSample() => Native.Api.ProfilerSample(1, null, 0.0f);

        /// <summary>A section for a <c>using</c> block: begun now, ended when the block is left (even by an exception).</summary>
        public static SampleScope Sample(string name)
        {
            BeginSample(name);
            return new SampleScope();
        }

        /// <summary>Sets a counter shown with this frame in the Profiler (its last value in the frame).</summary>
        public static void SetCounter(string name, float value) => Native.WithUtf8(name ?? "Counter", p => Native.Api.ProfilerSample(2, (byte*)p, value));

        public readonly struct SampleScope : System.IDisposable
        {
            public void Dispose() => EndSample();
        }
    }

    public static unsafe class Time
    {
        /// <summary>Seconds since the previous frame.</summary>
        public static float DeltaTime => Native.Api.GetDeltaTime();
        /// <summary>Seconds since play mode started (stops while paused).</summary>
        public static float time => Native.Api.GetTime(Native.CurrentScene);
        /// <summary>Frames simulated since play mode started.</summary>
        public static ulong FrameCount => Native.Api.GetFrameCount(Native.CurrentScene);
        /// <summary>Game speed: 1 normal, 0.25 slow motion, 0 frozen (a pause menu), up to 100. Scales DeltaTime,
        /// `time`, physics, animation, particles and WaitForSeconds; UI animation keeps real time. Back to 1 when play
        /// starts.</summary>
        public static float TimeScale
        {
            get => Native.Api.TimeControl(Native.CurrentScene, 0, 0.0f);
            set => Native.Api.TimeControl(Native.CurrentScene, 1, value);
        }
        /// <summary>This frame's step in real seconds (before TimeScale): for menus and effects that run while frozen.</summary>
        public static float UnscaledDeltaTime => Native.Api.TimeControl(Native.CurrentScene, 2, 0.0f);
        /// <summary>Real seconds since play mode started (TimeScale does not change it; pausing the editor does).</summary>
        public static float UnscaledTime => Native.Api.TimeControl(Native.CurrentScene, 3, 0.0f);
    }

    /// <summary>The view the game is shown in: the window in a build, the Game view in the editor.</summary>
    public static unsafe class Screen
    {
        internal static Vector2 Size { get { Vector2 v = default; Native.Api.ViewOp(Native.CurrentScene, 0, 3, (float*)&v); return v; } }
        /// <summary>Width in pixels.</summary>
        public static int Width => (int)Size.X;
        /// <summary>Height in pixels.</summary>
        public static int Height => (int)Size.Y;
    }

    public enum CursorLockMode { None = 0, Locked = 1 }

    /// <summary>
    /// The mouse cursor while the game has input (in the editor: while the Game view is focused; Escape gives it back
    /// until the Game view is clicked). It is normal again when play stops.
    /// </summary>
    public static class Cursor
    {
        /// <summary>Locked: hidden and held in place, for mouse look (read Input.MouseDelta). Setting None shows it.</summary>
        public static unsafe CursorLockMode LockState
        {
            get => Native.Api.CursorControl(0, 0) == 2 ? CursorLockMode.Locked : CursorLockMode.None;
            set => Native.Api.CursorControl(1, value == CursorLockMode.Locked ? 2 : 0);
        }
        /// <summary>Whether the cursor shows over the window (a locked cursor is always hidden).</summary>
        public static unsafe bool Visible
        {
            get => Native.Api.CursorControl(0, 0) == 0;
            set
            {
                int mode = Native.Api.CursorControl(0, 0);
                if (mode != 2) Native.Api.CursorControl(1, value ? 0 : 1);
            }
        }
    }

    /// <summary>Key codes (same values as GLFW / the engine's Key enum).</summary>
    public enum KeyCode
    {
        Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
        Alpha0 = 48, Alpha1, Alpha2, Alpha3, Alpha4, Alpha5, Alpha6, Alpha7, Alpha8, Alpha9,
        A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
        Escape = 256, Enter, Tab, Backspace, Insert, Delete, RightArrow, LeftArrow, DownArrow, UpArrow,
        F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
        LeftShift = 340, LeftControl, LeftAlt, LeftSuper, RightShift, RightControl, RightAlt, RightSuper,
    }

    public static unsafe class Input
    {
        private static float Action(int op, string name) => Native.WithUtf8<float>(name ?? "", p => Native.Api.InputAction(op, (byte*)p));

        /// <summary>
        /// Whether an input action (Project Settings > Input, e.g. "Jump", "Fire") is held: any of its keys, mouse
        /// buttons or gamepad controls. Unknown names are not pressed (with a warning in the Console).
        /// </summary>
        public static bool GetButton(string action) => Action(0, action) != 0.0f;
        /// <summary>The action was pressed this frame.</summary>
        public static bool GetButtonDown(string action) => Action(1, action) != 0.0f;
        /// <summary>The action was released this frame.</summary>
        public static bool GetButtonUp(string action) => Action(2, action) != 0.0f;
        /// <summary>An axis action's value, -1..1 (e.g. "Horizontal": A/D, arrows and the left stick added up), smoothed
        /// when the action has a Sensitivity (Project Settings > Input, as Unity's Input Manager smooths keys).</summary>
        public static float GetAxis(string action) => Action(3, action);
        /// <summary>An axis action's value with no smoothing: what its controls say this frame.</summary>
        public static float GetAxisRaw(string action) => Action(4, action);

        private static float Query(int op, int code = 0, int pad = 0) => Native.Api.InputQuery(op, code, pad);

        public static bool GetKey(KeyCode key) => Native.Api.IsKeyDown((int)key) != 0;
        public static bool GetKeyDown(KeyCode key) => Native.Api.IsKeyPressed((int)key) != 0;
        /// <summary>Whether the key came up this frame.</summary>
        public static bool GetKeyUp(KeyCode key) => Query(0, (int)key) != 0.0f;
        /// <summary>Any key, mouse button or gamepad button held.</summary>
        public static bool AnyKey => Query(3) != 0.0f;
        /// <summary>Any key, mouse button or gamepad button went down this frame ("press any key").</summary>
        public static bool AnyKeyDown => Query(4) != 0.0f;
        /// <summary>The text typed this frame, as the keyboard layout makes it, with '\b' for each Backspace and '\n' for
        /// each Enter (empty while a UI text field has the keyboard).</summary>
        public static string InputString => Native.FromUtf8(Native.Api.InputText(0, 0)) ?? "";

        /// <summary>0 = left, 1 = right, 2 = middle.</summary>
        public static bool GetMouseButton(int button) => Native.Api.IsMouseDown(button) != 0;
        /// <summary>Whether the mouse button went down this frame.</summary>
        public static bool GetMouseButtonDown(int button) => Native.Api.IsMousePressed(button) != 0;
        /// <summary>Whether the mouse button came up this frame.</summary>
        public static bool GetMouseButtonUp(int button) => Query(1, button) != 0.0f;
        /// <summary>This frame's scrolling: y the wheel (positive away from you), x sideways (tilt wheels, touchpads).</summary>
        public static Vector2 MouseScrollDelta => new(Query(5), Query(6));
        /// <summary>The mouse in pixels, top-left origin: in the view the game is shown in while playing (the Game view
        /// in the editor, so it matches Camera.ScreenPointToRay and the UI), in the window otherwise.</summary>
        public static Vector2 MousePosition
        {
            get
            {
                Vector2 v;
                if (Application.IsPlaying) Native.Api.ViewOp(Native.CurrentScene, 0, 4, (float*)&v);
                else Native.Api.GetMousePosition((float*)&v);
                return v;
            }
        }
        /// <summary>How far the mouse moved this frame in pixels (keeps reporting while the cursor is locked).</summary>
        public static Vector2 MouseDelta
        {
            get { Vector2 v; Native.Api.ViewOp(IntPtr.Zero, 0, 5, (float*)&v); return v; }
        }

        /// <summary>Gamepad button held (standard Xbox-style layout; pad 0-3).</summary>
        public static bool GetGamepadButton(GamepadButton button, int pad = 0) => Native.Api.GamepadButton((int)button, pad, 0) != 0;
        /// <summary>Gamepad button went down this frame.</summary>
        public static bool GetGamepadButtonDown(GamepadButton button, int pad = 0) => Native.Api.GamepadButton((int)button, pad, 1) != 0;
        /// <summary>Gamepad button came up this frame.</summary>
        public static bool GetGamepadButtonUp(GamepadButton button, int pad = 0) => Query(2, (int)button, pad) != 0.0f;
        /// <summary>Whether a gamepad is plugged in as pad 0-3.</summary>
        public static bool IsGamepadConnected(int pad = 0) => Query(7, 0, pad) != 0.0f;
        /// <summary>A pad's name ("" when none is connected there).</summary>
        public static string GetGamepadName(int pad = 0) => Native.FromUtf8(Native.Api.InputText(1, pad)) ?? "";
        /// <summary>The names of pads 0-3, "" for each slot with none (as Unity's GetJoystickNames).</summary>
        public static string[] GetJoystickNames()
        {
            var names = new string[(int)Query(8)];
            for (int i = 0; i < names.Length; ++i) names[i] = GetGamepadName(i);
            return names;
        }
        /// <summary>How many pads are connected.</summary>
        public static int GamepadCount { get { int n = 0; for (int i = 0, max = (int)Query(8); i < max; ++i) n += IsGamepadConnected(i) ? 1 : 0; return n; } }
        /// <summary>Sticks -1..1 (+Y is down) with a dead zone; triggers 0..1.</summary>
        public static float GetGamepadAxis(GamepadAxis axis, int pad = 0) => Native.Api.GamepadAxis((int)axis, pad);
        /// <summary>Rumble: low-frequency (heavy) and high-frequency (light) motor speeds 0..1. A positive duration
        /// stops it after that many seconds; 0 keeps it until changed. Stops when play stops.</summary>
        public static void SetGamepadVibration(float lowFrequency, float highFrequency, float duration = 0.0f, int pad = 0) =>
            Native.Api.GamepadVibrate(lowFrequency, highFrequency, duration, pad);
        /// <summary>The left stick, or WASD/arrows when no stick is pushed (+Y up).</summary>
        public static Vector2 GetMoveInput(int pad = 0)
        {
            var stick = new Vector2(GetGamepadAxis(GamepadAxis.LeftX, pad), -GetGamepadAxis(GamepadAxis.LeftY, pad));
            if (stick.LengthSquared() > 0.0f) return stick;
            float x = (GetKey(KeyCode.D) || GetKey(KeyCode.RightArrow) ? 1 : 0) - (GetKey(KeyCode.A) || GetKey(KeyCode.LeftArrow) ? 1 : 0);
            float y = (GetKey(KeyCode.W) || GetKey(KeyCode.UpArrow) ? 1 : 0) - (GetKey(KeyCode.S) || GetKey(KeyCode.DownArrow) ? 1 : 0);
            return new Vector2(x, y);
        }
    }

    public enum GamepadButton
    {
        A = 0, B, X, Y, LeftBumper, RightBumper, Back, Start, Guide, LeftThumb, RightThumb, DpadUp, DpadRight, DpadDown, DpadLeft
    }

    public enum GamepadAxis { LeftX = 0, LeftY, RightX, RightY, LeftTrigger, RightTrigger }

    /// <summary>A half-line: Origin plus Direction (normalized) times a distance.</summary>
    public struct Ray
    {
        public Vector3 Origin;
        public Vector3 Direction;
        public Ray(Vector3 origin, Vector3 direction) { Origin = origin; Direction = Vector3.Normalize(direction); }
        public Vector3 GetPoint(float distance) => Origin + Direction * distance;
        public override string ToString() => $"Ray({Origin} -> {Direction})";
    }

    /// <summary>Where two colliders touch (2D: z = 0).</summary>
    public struct ContactPoint
    {
        public Vector3 Point;
        /// <summary>The other object's surface there, pointing toward this object.</summary>
        public Vector3 Normal;
        /// <summary>Negative while they overlap.</summary>
        public float Separation;
    }

    /// <summary>A collision as this object sees it (OnCollisionEnter / Stay / Exit), in 2D and 3D.</summary>
    public sealed class Collision
    {
        public Collision(Entity other, ContactPoint[] contacts, Vector3 normal, Vector3 relativeVelocity, float impulse)
        {
            Other = other;
            Contacts = contacts ?? Array.Empty<ContactPoint>();
            Normal = normal;
            RelativeVelocity = relativeVelocity;
            Impulse = impulse;
        }
        public Entity Other { get; }
        /// <summary>The contact points (none on Exit).</summary>
        public ContactPoint[] Contacts { get; }
        public int ContactCount => Contacts.Length;
        public ContactPoint GetContact(int index) => Contacts[index];
        /// <summary>The contacts' normal, pointing toward this object.</summary>
        public Vector3 Normal { get; }
        /// <summary>This object's velocity minus the other's as they met: <c>RelativeVelocity.Length()</c> is how fast
        /// they hit.</summary>
        public Vector3 RelativeVelocity { get; }
        /// <summary>How hard they hit (N*s): the speed along the normal times the pair's reduced mass, as for an
        /// inelastic hit (about 0 while resting).</summary>
        public float Impulse { get; }
    }

    public struct RaycastHit
    {
        public Entity Entity;
        public Vector3 Point;
        public Vector3 Normal;
        public float Distance;
    }

    /// <summary>Changing terrains while playing.</summary>
    public static unsafe class TerrainTools
    {
        /// <summary>Digs a crater (an explosion's) into the terrain under `point`: a bowl `radius` across and `depth`
        /// deep, with a rim of thrown-up earth, painted with `paintLayer` (3 = dirt; -1 = none) and cleared of trees and
        /// grass. The terrain redraws and collides with its new shape. Returns how many height samples changed.</summary>
        public static int DigCrater(Vector3 point, float radius, float depth, int paintLayer = 3) =>
            Native.Api.TerrainCrater(Native.CurrentScene, (float*)&point, radius, depth, paintLayer);
    }

    /// <summary>The project's physics layers by name (Project Settings > Physics), and masks of them for queries.</summary>
    public static unsafe class LayerMask
    {
        /// <summary>A layer's name ("" for an unused one).</summary>
        public static string LayerToName(int layer) => layer is >= 0 and < 32 ? Native.FromUtf8(Native.Api.LayerName(layer)) : "";
        /// <summary>The layer with a name (-1 when there is none).</summary>
        public static int NameToLayer(string name)
        {
            for (int i = 0; i < 32; ++i)
                if (!string.IsNullOrEmpty(name) && LayerToName(i) == name) return i;
            return -1;
        }
        /// <summary>A mask of the named layers, for Physics queries (unknown names are left out).</summary>
        public static uint GetMask(params string[] names)
        {
            uint mask = 0;
            foreach (string n in names)
                if (NameToLayer(n) is int layer and >= 0) mask |= 1u << layer;
            return mask;
        }
    }

    public static unsafe class Physics
    {
        /// <summary>Layer mask with every layer except "Ignore Raycast" (the default for queries).</summary>
        public const uint DefaultLayers = ~(1u << 1);

        /// <summary>The 3D world's gravity (m/s^2); setting it wakes the bodies resting, so they feel it.</summary>
        public static Vector3 Gravity
        {
            get { Vector3 g; Native.Api.PhysicsSettings(Native.CurrentScene, 0, 0, 0, (float*)&g); return g; }
            set => Native.Api.PhysicsSettings(Native.CurrentScene, 1, 0, 0, (float*)&value);
        }
        /// <summary>Stops (or lets again) objects on two layers colliding while playing, as unticking the pair in the
        /// Layer Collision Matrix would (the project's matrix is left as it is).</summary>
        public static void IgnoreLayerCollision(int layer1, int layer2, bool ignore = true)
        {
            float v = ignore ? 0.0f : 1.0f;
            Native.Api.PhysicsSettings(Native.CurrentScene, 5, layer1, layer2, &v);
        }
        public static bool GetIgnoreLayerCollision(int layer1, int layer2) => Native.Api.PhysicsSettings(Native.CurrentScene, 4, layer1, layer2, null) == 0;
        /// <summary>Stops (or lets again) two objects colliding with each other while playing (touching now, they part).</summary>
        public static void IgnoreCollision(Entity a, Entity b, bool ignore = true) => Native.Api.PhysicsIgnore(a.Scene, a.Id, b.Id, 1, ignore ? 1 : 0);
        public static bool GetIgnoreCollision(Entity a, Entity b) => Native.Api.PhysicsIgnore(a.Scene, a.Id, b.Id, 0, 0) != 0;

        /// <summary>Whether queries given <see cref="QueryTriggerInteraction.UseGlobal"/> (the default) hit trigger colliders.</summary>
        public static bool QueriesHitTriggers { get; set; } = true;
        internal static int Triggers(QueryTriggerInteraction q) =>
            q == QueryTriggerInteraction.Collide || (q == QueryTriggerInteraction.UseGlobal && QueriesHitTriggers) ? 1 : 0;

        private const int MaxResults = 256;
        // PhysicsQuery's hits (point, normal and distance: 7 floats each). Ops: 0 raycast, 1 raycast all, 2 sphere cast,
        // 3 box cast, 4 capsule cast, 5 overlap sphere, 6 overlap box, 7 overlap capsule.
        private static RaycastHit[] Query(int op, float* v, float maxDistance, uint layerMask, QueryTriggerInteraction triggers)
        {
            ulong* ids = stackalloc ulong[MaxResults];
            float* data = stackalloc float[MaxResults * 7];
            IntPtr scene = Native.CurrentScene;
            int count = Math.Min(Native.Api.PhysicsQuery(scene, op, v, maxDistance, layerMask, Triggers(triggers), ids, data, MaxResults), MaxResults);
            var hits = new RaycastHit[count];
            for (int i = 0; i < count; ++i)
            {
                float* d = data + i * 7;
                hits[i] = new RaycastHit { Entity = new Entity(scene, ids[i]), Point = new Vector3(d[0], d[1], d[2]),
                                           Normal = new Vector3(d[3], d[4], d[5]), Distance = d[6] };
            }
            return hits;
        }
        private static bool First(int op, float* v, float maxDistance, uint layerMask, QueryTriggerInteraction triggers, out RaycastHit hit)
        {
            RaycastHit[] hits = Query(op, v, maxDistance, layerMask, triggers);
            hit = hits.Length > 0 ? hits[0] : default;
            return hits.Length > 0;
        }
        private static Entity[] Overlap(int op, float* v, uint layerMask, QueryTriggerInteraction triggers)
        {
            ulong* ids = stackalloc ulong[MaxResults];
            IntPtr scene = Native.CurrentScene;
            int count = Math.Min(Native.Api.PhysicsQuery(scene, op, v, 0.0f, layerMask, Triggers(triggers), ids, null, MaxResults), MaxResults);
            var result = new Entity[count];
            for (int i = 0; i < count; ++i) result[i] = new Entity(scene, ids[i]);
            return result;
        }

        /// <summary>The nearest collider along a ray (direction need not be normalized).</summary>
        public static bool Raycast(Vector3 origin, Vector3 direction, out RaycastHit hit, float maxDistance = 1000.0f, uint layerMask = DefaultLayers,
                                   QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[6] { origin.X, origin.Y, origin.Z, direction.X, direction.Y, direction.Z };
            return First(0, v, maxDistance, layerMask, triggers, out hit);
        }
        public static bool Raycast(Ray ray, out RaycastHit hit, float maxDistance = 1000.0f, uint layerMask = DefaultLayers,
                                   QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal) =>
            Raycast(ray.Origin, ray.Direction, out hit, maxDistance, layerMask, triggers);
        /// <summary>Whether anything is in the way from `start` to `end` (the nearest such collider in hit).</summary>
        public static bool Linecast(Vector3 start, Vector3 end, out RaycastHit hit, uint layerMask = DefaultLayers,
                                    QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            Vector3 d = end - start;
            float length = d.Length();
            if (length <= 0.0f) { hit = default; return false; }
            return Raycast(start, d, out hit, length, layerMask, triggers);
        }
        public static bool Linecast(Vector3 start, Vector3 end, uint layerMask = DefaultLayers, QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal) =>
            Linecast(start, end, out _, layerMask, triggers);

        /// <summary>Every collider along a ray, nearest first (each object once; a ray starting inside one hits it at 0).</summary>
        public static RaycastHit[] RaycastAll(Vector3 origin, Vector3 direction, float maxDistance = 1000.0f, uint layerMask = DefaultLayers,
                                              QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[6] { origin.X, origin.Y, origin.Z, direction.X, direction.Y, direction.Z };
            return Query(1, v, maxDistance, layerMask, triggers);
        }
        public static RaycastHit[] RaycastAll(Ray ray, float maxDistance = 1000.0f, uint layerMask = DefaultLayers,
                                              QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal) =>
            RaycastAll(ray.Origin, ray.Direction, maxDistance, layerMask, triggers);

        /// <summary>Moves a sphere along `direction` and returns the first collider it touches (a thick raycast: Point is
        /// where they touch, Distance how far the sphere moved). A collider it starts inside is hit at distance 0.</summary>
        public static bool SphereCast(Vector3 origin, float radius, Vector3 direction, out RaycastHit hit, float maxDistance = 1000.0f,
                                      uint layerMask = DefaultLayers, QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[7] { origin.X, origin.Y, origin.Z, direction.X, direction.Y, direction.Z, radius };
            return First(2, v, maxDistance, layerMask, triggers, out hit);
        }
        public static bool SphereCast(Ray ray, float radius, out RaycastHit hit, float maxDistance = 1000.0f, uint layerMask = DefaultLayers,
                                      QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal) =>
            SphereCast(ray.Origin, radius, ray.Direction, out hit, maxDistance, layerMask, triggers);

        /// <summary>As SphereCast, for a box `halfExtents` in each direction turned by `orientation`.</summary>
        public static bool BoxCast(Vector3 center, Vector3 halfExtents, Vector3 direction, out RaycastHit hit, Quaternion orientation,
                                   float maxDistance = 1000.0f, uint layerMask = DefaultLayers,
                                   QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[13] { center.X, center.Y, center.Z, direction.X, direction.Y, direction.Z,
                                               halfExtents.X, halfExtents.Y, halfExtents.Z, orientation.X, orientation.Y, orientation.Z, orientation.W };
            return First(3, v, maxDistance, layerMask, triggers, out hit);
        }
        public static bool BoxCast(Vector3 center, Vector3 halfExtents, Vector3 direction, out RaycastHit hit) =>
            BoxCast(center, halfExtents, direction, out hit, Quaternion.Identity);

        /// <summary>As SphereCast, for a capsule whose ends' centers are point1 and point2 (a character's body: its feet's
        /// and head's spheres).</summary>
        public static bool CapsuleCast(Vector3 point1, Vector3 point2, float radius, Vector3 direction, out RaycastHit hit, float maxDistance = 1000.0f,
                                       uint layerMask = DefaultLayers, QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[10] { point1.X, point1.Y, point1.Z, point2.X, point2.Y, point2.Z, direction.X, direction.Y, direction.Z, radius };
            return First(4, v, maxDistance, layerMask, triggers, out hit);
        }

        /// <summary>Entities whose 3D colliders overlap the sphere.</summary>
        public static Entity[] OverlapSphere(Vector3 center, float radius, uint layerMask = DefaultLayers,
                                             QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[4] { center.X, center.Y, center.Z, radius };
            return Overlap(5, v, layerMask, triggers);
        }
        /// <summary>Entities whose 3D colliders overlap a box `halfExtents` in each direction (turned by `orientation`).</summary>
        public static Entity[] OverlapBox(Vector3 center, Vector3 halfExtents, Quaternion orientation, uint layerMask = DefaultLayers,
                                          QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[10] { center.X, center.Y, center.Z, halfExtents.X, halfExtents.Y, halfExtents.Z,
                                               orientation.X, orientation.Y, orientation.Z, orientation.W };
            return Overlap(6, v, layerMask, triggers);
        }
        public static Entity[] OverlapBox(Vector3 center, Vector3 halfExtents) => OverlapBox(center, halfExtents, Quaternion.Identity);
        /// <summary>Entities whose 3D colliders overlap the capsule from point1 to point2 (its ends' centers).</summary>
        public static Entity[] OverlapCapsule(Vector3 point1, Vector3 point2, float radius, uint layerMask = DefaultLayers,
                                              QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[7] { point1.X, point1.Y, point1.Z, point2.X, point2.Y, point2.Z, radius };
            return Overlap(7, v, layerMask, triggers);
        }

        /// <summary>Whether any 3D collider overlaps the sphere / box / capsule (a room to stand in, a spawn point free).</summary>
        public static bool CheckSphere(Vector3 center, float radius, uint layerMask = DefaultLayers,
                                       QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal) =>
            OverlapSphere(center, radius, layerMask, triggers).Length > 0;
        public static bool CheckBox(Vector3 center, Vector3 halfExtents, Quaternion orientation, uint layerMask = DefaultLayers,
                                    QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal) =>
            OverlapBox(center, halfExtents, orientation, layerMask, triggers).Length > 0;
        public static bool CheckBox(Vector3 center, Vector3 halfExtents) => CheckBox(center, halfExtents, Quaternion.Identity);
        public static bool CheckCapsule(Vector3 point1, Vector3 point2, float radius, uint layerMask = DefaultLayers,
                                        QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal) =>
            OverlapCapsule(point1, point2, radius, layerMask, triggers).Length > 0;
    }

    /// <summary>Whether a physics query hits trigger colliders: as Physics.QueriesHitTriggers says (UseGlobal), never
    /// (Ignore) or always (Collide).</summary>
    public enum QueryTriggerInteraction { UseGlobal = 0, Ignore = 1, Collide = 2 }

    /// <summary>An axis-aligned box (a collider's world bounds).</summary>
    public struct Bounds
    {
        public Vector3 Center;
        /// <summary>Half the size.</summary>
        public Vector3 Extents;
        public Bounds(Vector3 center, Vector3 size) { Center = center; Extents = size * 0.5f; }
        public Vector3 Size { get => Extents * 2.0f; set => Extents = value * 0.5f; }
        public Vector3 Min => Center - Extents;
        public Vector3 Max => Center + Extents;
        public static Bounds FromMinMax(Vector3 min, Vector3 max) => new((min + max) * 0.5f, max - min);
        public bool Contains(Vector3 point) =>
            point.X >= Min.X && point.X <= Max.X && point.Y >= Min.Y && point.Y <= Max.Y && point.Z >= Min.Z && point.Z <= Max.Z;
        public bool Intersects(Bounds other) =>
            Min.X <= other.Max.X && Max.X >= other.Min.X && Min.Y <= other.Max.Y && Max.Y >= other.Min.Y && Min.Z <= other.Max.Z && Max.Z >= other.Min.Z;
        /// <summary>Grows to take in a point.</summary>
        public void Encapsulate(Vector3 point) { Vector3 lo = Vector3.Min(Min, point), hi = Vector3.Max(Max, point); this = FromMinMax(lo, hi); }
        public Vector3 ClosestPoint(Vector3 point) => Vector3.Clamp(point, Min, Max);
        public override string ToString() => $"Center {Center}, Extents {Extents}";
    }

    public struct RaycastHit2D
    {
        public Entity Entity;
        public Vector2 Point;
        public Vector2 Normal;
        public float Distance;
    }

    /// <summary>Queries against the 2D colliders (Box / Circle Collider 2D, tilemaps) while playing. Layer masks have
    /// a bit per physics layer.</summary>
    public static unsafe class Physics2D
    {
        /// <summary>Every layer except "Ignore Raycast" (the default for queries).</summary>
        public const uint DefaultLayers = ~(1u << 1);

        /// <summary>The 2D world's gravity (m/s^2); setting it wakes the bodies resting.</summary>
        public static Vector2 Gravity
        {
            get { Vector2 g; Native.Api.PhysicsSettings(Native.CurrentScene, 2, 0, 0, (float*)&g); return g; }
            set => Native.Api.PhysicsSettings(Native.CurrentScene, 3, 0, 0, (float*)&value);
        }
        /// <summary>Stops (or lets again) 2D objects on two layers colliding while playing (the 2D world's own matrix).</summary>
        public static void IgnoreLayerCollision(int layer1, int layer2, bool ignore = true)
        {
            float v = ignore ? 0.0f : 1.0f;
            Native.Api.PhysicsSettings(Native.CurrentScene, 7, layer1, layer2, &v);
        }
        public static bool GetIgnoreLayerCollision(int layer1, int layer2) => Native.Api.PhysicsSettings(Native.CurrentScene, 6, layer1, layer2, null) == 0;
        /// <summary>Stops (or lets again) two 2D objects colliding with each other while playing.</summary>
        public static void IgnoreCollision(Entity a, Entity b, bool ignore = true) => Native.Api.PhysicsIgnore(a.Scene, a.Id, b.Id, 3, ignore ? 1 : 0);
        public static bool GetIgnoreCollision(Entity a, Entity b) => Native.Api.PhysicsIgnore(a.Scene, a.Id, b.Id, 2, 0) != 0;
        private const int MaxResults = 256;

        /// <summary>Whether 2D queries given <see cref="QueryTriggerInteraction.UseGlobal"/> (the default) hit trigger colliders.</summary>
        public static bool QueriesHitTriggers { get; set; } = true;
        private static int Triggers(QueryTriggerInteraction q) =>
            q == QueryTriggerInteraction.Collide || (q == QueryTriggerInteraction.UseGlobal && QueriesHitTriggers) ? 1 : 0;

        // PhysicsQuery's 2D ops: 100 raycast, 101 raycast all, 102 circle cast, 103 box cast (hits: point, normal and
        // distance, 5 floats each), 104 overlap circle, 105 overlap box, 106 overlap point.
        private static RaycastHit2D[] Cast(int op, float* v, float maxDistance, uint layerMask, QueryTriggerInteraction triggers)
        {
            ulong* ids = stackalloc ulong[MaxResults];
            float* data = stackalloc float[MaxResults * 5];
            IntPtr scene = Native.CurrentScene;
            int count = Math.Min(Native.Api.PhysicsQuery(scene, op, v, maxDistance, layerMask, Triggers(triggers), ids, data, MaxResults), MaxResults);
            var hits = new RaycastHit2D[count];
            for (int i = 0; i < count; ++i)
            {
                float* d = data + i * 5;
                hits[i] = new RaycastHit2D { Entity = new Entity(scene, ids[i]), Point = new Vector2(d[0], d[1]), Normal = new Vector2(d[2], d[3]), Distance = d[4] };
            }
            return hits;
        }
        private static bool First(int op, float* v, float maxDistance, uint layerMask, QueryTriggerInteraction triggers, out RaycastHit2D hit)
        {
            RaycastHit2D[] hits = Cast(op, v, maxDistance, layerMask, triggers);
            hit = hits.Length > 0 ? hits[0] : default;
            return hits.Length > 0;
        }

        /// <summary>The nearest collider along a ray from `origin` (direction need not be normalized).</summary>
        public static bool Raycast(Vector2 origin, Vector2 direction, out RaycastHit2D hit, float maxDistance = 1000.0f, uint layerMask = DefaultLayers,
                                   QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[4] { origin.X, origin.Y, direction.X, direction.Y };
            return First(100, v, maxDistance, layerMask, triggers, out hit);
        }
        /// <summary>Every collider along the ray, nearest first (each object once).</summary>
        public static RaycastHit2D[] RaycastAll(Vector2 origin, Vector2 direction, float maxDistance = 1000.0f, uint layerMask = DefaultLayers,
                                                QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[4] { origin.X, origin.Y, direction.X, direction.Y };
            return Cast(101, v, maxDistance, layerMask, triggers);
        }
        /// <summary>Whether anything is in the way from `start` to `end` (the nearest such collider in hit).</summary>
        public static bool Linecast(Vector2 start, Vector2 end, out RaycastHit2D hit, uint layerMask = DefaultLayers,
                                    QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            Vector2 d = end - start;
            float length = d.Length();
            if (length <= 0.0f) { hit = default; return false; }
            return Raycast(start, d, out hit, length, layerMask, triggers);
        }
        public static bool Linecast(Vector2 start, Vector2 end, uint layerMask = DefaultLayers, QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal) =>
            Linecast(start, end, out _, layerMask, triggers);
        /// <summary>Moves a circle along `direction` and returns the first collider it touches (Point is where they touch,
        /// Distance how far it moved; 0 for one it starts in).</summary>
        public static bool CircleCast(Vector2 origin, float radius, Vector2 direction, out RaycastHit2D hit, float maxDistance = 1000.0f,
                                      uint layerMask = DefaultLayers, QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[5] { origin.X, origin.Y, direction.X, direction.Y, radius };
            return First(102, v, maxDistance, layerMask, triggers, out hit);
        }
        /// <summary>As CircleCast, for a box `size` across turned `angleDegrees` counterclockwise.</summary>
        public static bool BoxCast(Vector2 origin, Vector2 size, float angleDegrees, Vector2 direction, out RaycastHit2D hit, float maxDistance = 1000.0f,
                                   uint layerMask = DefaultLayers, QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[7] { origin.X, origin.Y, direction.X, direction.Y, size.X * 0.5f, size.Y * 0.5f, angleDegrees * MathF.PI / 180.0f };
            return First(103, v, maxDistance, layerMask, triggers, out hit);
        }

        private static Entity[] Overlap(int op, float* v, uint layerMask, QueryTriggerInteraction triggers)
        {
            ulong* ids = stackalloc ulong[MaxResults];
            IntPtr scene = Native.CurrentScene;
            int count = Math.Min(Native.Api.PhysicsQuery(scene, op, v, 0.0f, layerMask, Triggers(triggers), ids, null, MaxResults), MaxResults);
            var result = new Entity[count];
            for (int i = 0; i < count; ++i) result[i] = new Entity(scene, ids[i]);
            return result;
        }
        /// <summary>The objects whose colliders overlap a circle.</summary>
        public static Entity[] OverlapCircle(Vector2 center, float radius, uint layerMask = DefaultLayers,
                                             QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[3] { center.X, center.Y, radius };
            return Overlap(104, v, layerMask, triggers);
        }
        /// <summary>The objects whose colliders overlap a box `size` across, turned `angleDegrees` counterclockwise.</summary>
        public static Entity[] OverlapBox(Vector2 center, Vector2 size, float angleDegrees = 0.0f, uint layerMask = DefaultLayers,
                                          QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[5] { center.X, center.Y, size.X * 0.5f, size.Y * 0.5f, angleDegrees * MathF.PI / 180.0f };
            return Overlap(105, v, layerMask, triggers);
        }
        /// <summary>The object whose collider contains a point (Null when none), e.g. under the mouse:
        /// <c>Physics2D.OverlapPoint(Camera.Main.ScreenToWorldPoint(...))</c>.</summary>
        public static Entity OverlapPoint(Vector2 point, uint layerMask = DefaultLayers, QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            Entity[] all = OverlapPointAll(point, layerMask, triggers);
            return all.Length > 0 ? all[0] : Entity.Null;
        }
        public static Entity[] OverlapPointAll(Vector2 point, uint layerMask = DefaultLayers, QueryTriggerInteraction triggers = QueryTriggerInteraction.UseGlobal)
        {
            float* v = stackalloc float[2] { point.X, point.Y };
            return Overlap(106, v, layerMask, triggers);
        }
    }

    /// <summary>Runtime scene switching (scenes come from File > Build Settings). Loads happen at the end of the frame.</summary>
    public static unsafe class Application
    {
        /// <summary>Whether the running script's scene is playing (false for [ExecuteInEditMode] scripts in the editor,
        /// and in editor code).</summary>
        public static bool IsPlaying => Native.CurrentScene != IntPtr.Zero && Native.Api.SceneRunning(Native.CurrentScene) != 0;

        /// <summary>
        /// The folder for the player's data (save games, settings): <c>%APPDATA%\&lt;product name&gt;</c> in a build, the
        /// project's <c>.dimension\PlayerData</c> in the editor (so trying the game never touches a build's saves).
        /// </summary>
        public static string PersistentDataPath => Native.FromUtf8(Native.Api.PlayerPrefs(10, null, null)) ?? "";
    }

    /// <summary>
    /// The game's text in the player's language, from the project's string tables (.csv assets: a column per language,
    /// a row per key). The language starts as the one chosen last, else the system's, else the project's default
    /// (Project Settings > Localization); a text missing in a language comes from the default one.
    /// </summary>
    public static unsafe class Localization
    {
        internal static string Call(int op, string text, Entity entity = default) =>
            Native.WithUtf8<string>(text ?? "", p => Native.FromUtf8(Native.Api.Localization(op, (byte*)p, entity.Scene, entity.Id)));

        /// <summary>The key's text in the current language (the key itself when no table has it).</summary>
        public static string Get(string key) => Call(0, key) ?? key ?? "";
        /// <summary>The key's text with {0}, {1}... filled in (string.Format): <c>Get("score", points)</c>.</summary>
        public static string Get(string key, params object[] args) => string.Format(System.Globalization.CultureInfo.InvariantCulture, Get(key), args);
        public static bool Has(string key) => Call(1, key) != null;

        /// <summary>The current language's code ("en", "fr"...); setting it picks the closest the tables have ("de-AT"
        /// picks "de") and keeps the choice for the next run. UI Texts with a Localized Key follow.</summary>
        public static string Language
        {
            get => Call(2, null) ?? "";
            set => SetLanguage(value);
        }
        /// <summary>Like setting <see cref="Language"/>; false (nothing changes) when no table has the language.</summary>
        public static bool SetLanguage(string code)
        {
            bool ok = Call(3, code) != null;
            if (!ok) Debug.LogWarning($"Localization: no string table has the language '{code}'");
            return ok;
        }

        /// <summary>The languages of the project's tables (codes, in column order).</summary>
        public static string[] Languages
        {
            get
            {
                string list = Call(4, null) ?? "";
                var lines = list.Split('\n', StringSplitOptions.RemoveEmptyEntries);
                var codes = new string[lines.Length];
                for (int i = 0; i < lines.Length; i++) codes[i] = lines[i].Split('\t')[0];
                return codes;
            }
        }
        /// <summary>A language's name from the table headers ("Deutsch" for "de"), or the code.</summary>
        public static string LanguageName(string code) => Call(7, code) ?? code ?? "";
        /// <summary>The system's language as a locale name ("fr-FR").</summary>
        public static string SystemLanguage => Call(5, null) ?? "";
        /// <summary>The project's default language (Project Settings > Localization, else the first column).</summary>
        public static string DefaultLanguage => Call(6, null) ?? "";
    }

    /// <summary>
    /// Small settings kept between runs (like Unity's PlayerPrefs): ints, floats and strings by key, saved in
    /// <see cref="Application.PersistentDataPath"/> whenever one changes. A key of another type, or not set, reads as
    /// the default.
    /// </summary>
    public static unsafe class PlayerPrefs
    {
        private static string Call(int op, string key, string value) =>
            Native.WithUtf8<string>(key ?? "", k => Native.WithUtf8<string>(value ?? "", v => Native.FromUtf8(Native.Api.PlayerPrefs(op, (byte*)k, (byte*)v))));

        private static readonly System.Globalization.CultureInfo Invariant = System.Globalization.CultureInfo.InvariantCulture;

        public static int GetInt(string key, int defaultValue = 0) =>
            Call(0, key, null) is { } s && int.TryParse(s, System.Globalization.NumberStyles.Integer, Invariant, out int v) ? v : defaultValue;
        public static float GetFloat(string key, float defaultValue = 0.0f) =>
            Call(1, key, null) is { } s && float.TryParse(s, System.Globalization.NumberStyles.Float, Invariant, out float v) ? v : defaultValue;
        public static string GetString(string key, string defaultValue = "") => Call(2, key, null) ?? defaultValue;
        public static void SetInt(string key, int value) => Call(3, key, value.ToString(Invariant));
        public static void SetFloat(string key, float value) => Call(4, key, value.ToString("R", Invariant));
        public static void SetString(string key, string value) => Call(5, key, value ?? "");
        public static bool HasKey(string key) => Call(6, key, null) != null;
        public static void DeleteKey(string key) => Call(7, key, null);
        public static void DeleteAll() => Call(8, null, null);
        /// <summary>Changes are written as they are made; this writes again.</summary>
        public static void Save() => Call(9, null, null);
    }

    /// <summary>
    /// Game state saved as JSON files in <see cref="Application.PersistentDataPath"/> (public fields and properties,
    /// nested objects, lists, dictionaries and vectors).
    /// </summary>
    /// <example>
    /// <code>
    /// SaveFile.Write("slot1", new SaveGame { Level = 3, Coins = 120 });
    /// SaveGame game = SaveFile.Read("slot1", new SaveGame());
    /// </code>
    /// </example>
    public static class SaveFile
    {
        private static readonly System.Text.Json.JsonSerializerOptions Options = new() { IncludeFields = true, WriteIndented = true };

        /// <summary>Where a save named <paramref name="name"/> is (<c>name.json</c> in the persistent data folder).</summary>
        public static string PathOf(string name) => System.IO.Path.Combine(Application.PersistentDataPath, name + ".json");

        /// <summary>Writes <paramref name="value"/> (replacing that save whole, so a crash keeps the old one).</summary>
        public static void Write<T>(string name, T value)
        {
            string path = PathOf(name), temp = path + ".tmp";
            System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(path)!);
            System.IO.File.WriteAllText(temp, System.Text.Json.JsonSerializer.Serialize(value, Options));
            System.IO.File.Move(temp, path, true);
        }

        /// <summary>The save, or <paramref name="fallback"/> when there is none or it cannot be read (a warning logged).</summary>
        public static T Read<T>(string name, T fallback = default)
        {
            string path = PathOf(name);
            if (!System.IO.File.Exists(path)) return fallback;
            try { return System.Text.Json.JsonSerializer.Deserialize<T>(System.IO.File.ReadAllText(path), Options) ?? fallback; }
            catch (Exception e)
            {
                Debug.LogWarning($"Save '{name}' could not be read: {e.Message}");
                return fallback;
            }
        }

        public static bool Exists(string name) => System.IO.File.Exists(PathOf(name));
        public static void Delete(string name) { if (Exists(name)) System.IO.File.Delete(PathOf(name)); }
    }

    public enum LoadSceneMode { Single = 0, Additive = 1 }

    /// <summary>A background scene load (SceneManager.LoadSceneAsync).</summary>
    public sealed unsafe class AsyncOperation
    {
        private readonly int m_Id;
        internal AsyncOperation(int id) { m_Id = id; }
        /// <summary>0 to 0.9 while loading in the background; 1 once the scene is active.</summary>
        public float Progress { get { float p; Native.Api.AsyncState(m_Id, &p); return p; } }
        public bool IsDone { get { float p; return Native.Api.AsyncState(m_Id, &p) != 0; } }
        /// <summary>False holds the loaded scene at 0.9 until set back to true.</summary>
        public bool AllowSceneActivation { set => Native.Api.AsyncAllowActivation(m_Id, value ? 1 : 0); }
    }

    public static unsafe class SceneManager
    {
        public static void LoadScene(string name) => Native.WithUtf8(name, p => Native.Api.LoadSceneByName((byte*)p));
        public static void LoadScene(int buildIndex) => Native.Api.LoadSceneByIndex(buildIndex);
        /// <summary>Single destroys everything not marked DontDestroyOnLoad; Additive adds the scene's objects.</summary>
        public static void LoadScene(string name, LoadSceneMode mode) =>
            Native.WithUtf8(name, p => Native.Api.LoadSceneWithMode((byte*)p, -1, (int)mode));
        public static void LoadScene(int buildIndex, LoadSceneMode mode) => Native.Api.LoadSceneWithMode(null, buildIndex, (int)mode);
        /// <summary>Loads in the background (a worker thread reads the scene and decodes its textures and models) and
        /// activates at the end of a frame once ready. Null if there is no such scene. Yield it in a coroutine with
        /// `yield return new WaitUntil(() => op.IsDone)`.</summary>
        public static AsyncOperation LoadSceneAsync(string name, LoadSceneMode mode = LoadSceneMode.Single)
        {
            int id = Native.WithUtf8(name, p => Native.Api.LoadSceneAsync((byte*)p, -1, (int)mode));
            return id != 0 ? new AsyncOperation(id) : null;
        }
        public static AsyncOperation LoadSceneAsync(int buildIndex, LoadSceneMode mode = LoadSceneMode.Single)
        {
            int id = Native.Api.LoadSceneAsync(null, buildIndex, (int)mode);
            return id != 0 ? new AsyncOperation(id) : null;
        }
        /// <summary>Removes the objects of a scene loaded additively (or the first scene).</summary>
        public static void UnloadScene(string name) => Native.WithUtf8(name, p => Native.Api.UnloadScene((byte*)p));
        /// <summary>Keeps the entity's root object (and its children) alive across single-mode loads.</summary>
        public static void DontDestroyOnLoad(Entity entity) => Native.Api.DontDestroyOnLoad(entity.Scene, entity.Id);
        /// <summary>Names of the loaded scenes (the active one first; "DontDestroyOnLoad" holds persistent objects).</summary>
        public static string[] LoadedSceneNames
        {
            get
            {
                IntPtr scene = Native.CurrentScene;
                int n = Native.Api.GetLoadedSceneCount(scene);
                var names = new string[n];
                for (int i = 0; i < n; i++) names[i] = Native.FromUtf8(Native.Api.GetLoadedSceneName(scene, i));
                return names;
            }
        }
        /// <summary>Name of the scene the calling script runs in.</summary>
        public static string ActiveSceneName => Native.FromUtf8(Native.Api.GetSceneName(Native.CurrentScene));
        public static int SceneCountInBuild => Native.Api.GetSceneCount();
    }
}
