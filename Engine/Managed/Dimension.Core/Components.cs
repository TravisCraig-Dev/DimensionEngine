using System;
using System.Numerics;
using Dimension.Interop;

namespace Dimension
{
    /// <summary>Thin wrapper over an engine component on an entity.</summary>
    public abstract unsafe class Component
    {
        public Entity Entity { get; internal set; }
        /// <summary>Its object's Transform (like Unity's component.transform).</summary>
        public Transform Transform => Entity.Transform;
        /// <summary>Component name in the engine registry (as shown in the Inspector).</summary>
        internal abstract string NativeName { get; }
        /// <summary>Every engine component this class stands for (Light: the three kinds), for FindObjectsOfType.</summary>
        internal virtual string[] NativeNames => new[] { NativeName };

        // Any numeric field shown in the Inspector, by its label (e.g. "Max Steer Angle"): bools, ints, enums (by index),
        // floats, vectors and colors. Missing fields read as 0.
        private int ReadField(string field, float* values)
        {
            var e = Entity;
            string component = NativeName;
            IntPtr v = (IntPtr)values;
            return Native.WithUtf8(component, c => Native.WithUtf8(field, f => Native.Api.GetComponentField(e.Scene, e.Id, (byte*)c, (byte*)f, (float*)v)));
        }
        private bool WriteField(string field, float* values, int count)
        {
            var e = Entity;
            string component = NativeName;
            IntPtr v = (IntPtr)values;
            return Native.WithUtf8(component, c => Native.WithUtf8(field, f => Native.Api.SetComponentField(e.Scene, e.Id, (byte*)c, (byte*)f, (float*)v, count))) != 0;
        }
        public float GetFloat(string field) { float* v = stackalloc float[4]; return ReadField(field, v) > 0 ? v[0] : 0.0f; }
        public bool SetFloat(string field, float value) => WriteField(field, &value, 1);
        public int GetInt(string field) => (int)GetFloat(field);
        public bool SetInt(string field, int value) => SetFloat(field, value);
        public bool GetBool(string field) => GetFloat(field) != 0.0f;
        public bool SetBool(string field, bool value) => SetFloat(field, value ? 1.0f : 0.0f);
        public Vector2 GetVector2(string field) { float* v = stackalloc float[4]; ReadField(field, v); return new Vector2(v[0], v[1]); }
        public bool SetVector2(string field, Vector2 value) => WriteField(field, (float*)&value, 2);
        public Vector3 GetVector3(string field) { float* v = stackalloc float[4]; ReadField(field, v); return new Vector3(v[0], v[1], v[2]); }
        public bool SetVector3(string field, Vector3 value) => WriteField(field, (float*)&value, 3);
        public Vector4 GetVector4(string field) { float* v = stackalloc float[4]; ReadField(field, v); return new Vector4(v[0], v[1], v[2], v[3]); }
        public bool SetVector4(string field, Vector4 value) => WriteField(field, (float*)&value, 4);

        // An asset field (a texture, mesh, clip...) by its label, as its GUID: 0 when empty, ulong.MaxValue when missing.
        private ulong AssetField(string field, int op, ulong guid)
        {
            var e = Entity;
            string component = NativeName;
            return Native.WithUtf8(component, c => Native.WithUtf8(field, f => Native.Api.ComponentAsset(e.Scene, e.Id, (byte*)c, (byte*)f, op, guid)));
        }
        /// <summary>The GUID of the asset an asset field refers to (0: none; ulong.MaxValue: no such asset field).</summary>
        public ulong GetAssetGuid(string field) => AssetField(field, 0, 0);
        /// <summary>Points an asset field at another asset (GUID 0 empties it); false when there is no such asset field.</summary>
        public bool SetAssetGuid(string field, ulong guid) => AssetField(field, 1, guid) != ulong.MaxValue;

        // An object field (a joint's Connected Body) by its label: the object, or default when empty or missing.
        private ulong EntityField(string field, int op, ulong target)
        {
            var e = Entity;
            string component = NativeName;
            return Native.WithUtf8(component, c => Native.WithUtf8(field, f => Native.Api.ComponentEntity(e.Scene, e.Id, (byte*)c, (byte*)f, op, target)));
        }
        public Entity GetEntity(string field) { ulong id = EntityField(field, 0, 0); return id == ulong.MaxValue ? default : new Entity(Entity.Scene, id); }
        public void SetEntity(string field, Entity value) => EntityField(field, 1, value.IsValid ? value.Id : ulong.MaxValue);

        // A text field (an Audio Source's Output) by its label: its text, or null when there is no such field.
        private string StringField(string field, int op, string value)
        {
            var e = Entity;
            string component = NativeName;
            return Native.WithUtf8(component, c => Native.WithUtf8(field, f => Native.WithUtf8(value ?? "", v =>
                Native.FromUtf8(Native.Api.ComponentString(e.Scene, e.Id, (byte*)c, (byte*)f, op, (byte*)v)))));
        }
        public string GetString(string field) => StringField(field, 0, null);
        /// <summary>False when there is no such text field.</summary>
        public bool SetString(string field, string value) => StringField(field, 1, value) != null;
    }

    /// <summary>Any engine component by its Inspector name, for components without their own class
    /// (<c>entity.GetComponent("Point Light").SetFloat("Intensity", 2)</c>).</summary>
    public sealed class NamedComponent : Component
    {
        private readonly string m_Name;
        public NamedComponent() : this("") { }
        public NamedComponent(string name) { m_Name = name; }
        internal override string NativeName => m_Name;
    }

    public unsafe class Transform : Component
    {
        internal override string NativeName => "Transform";

        private void Get(out Vector3 p, out Vector3 r, out Vector3 s)
        {
            Vector3 a, b, c;
            Native.Api.GetTransform(Entity.Scene, Entity.Id, (float*)&a, (float*)&b, (float*)&c);
            p = a; r = b; s = c;
        }

        private void Set(Vector3 p, Vector3 r, Vector3 s) => Native.Api.SetTransform(Entity.Scene, Entity.Id, (float*)&p, (float*)&r, (float*)&s);

        /// <summary>Local position.</summary>
        public Vector3 Position
        {
            get { Get(out var p, out _, out _); return p; }
            set { Get(out _, out var r, out var s); Set(value, r, s); }
        }

        /// <summary>Local rotation as Euler angles in degrees.</summary>
        public Vector3 EulerAngles
        {
            get { Get(out _, out var r, out _); return r * (180.0f / System.MathF.PI); }
            set { Get(out var p, out _, out var s); Set(p, value * (System.MathF.PI / 180.0f), s); }
        }

        public Vector3 Scale
        {
            get { Get(out _, out _, out var s); return s; }
            set { Get(out var p, out var r, out _); Set(p, r, value); }
        }

        public Vector3 WorldPosition
        {
            get { Vector3 v; Native.Api.GetWorldPosition(Entity.Scene, Entity.Id, (float*)&v); return v; }
        }

        public void Translate(Vector3 delta) => Position += delta;
        public void Rotate(Vector3 eulerDegrees) => EulerAngles += eulerDegrees;

        /// <summary>World transform (row-major System.Numerics layout: translation in M41..M43).</summary>
        public Matrix4x4 LocalToWorld
        {
            get
            {
                Matrix4x4 m;
                Native.Api.GetWorldMatrix(Entity.Scene, Entity.Id, (float*)&m); // column-major glm == row-major System.Numerics
                return m;
            }
        }

        /// <summary>Its rotation (local: relative to its parent) as a quaternion, as EulerAngles is.</summary>
        public Quaternion LocalRotation { get => Quaternion.Euler(EulerAngles); set => EulerAngles = value.EulerAngles; }
        /// <summary>Its rotation in the world (its parents' too); setting it turns it so.</summary>
        public Quaternion Rotation
        {
            get
            {
                Quaternion r = LocalRotation;
                for (Entity p = Entity.Parent; p.IsValid; p = p.Parent) r = Quaternion.Euler(p.Transform.EulerAngles) * r;
                return r;
            }
            set
            {
                Entity parent = Entity.Parent;
                LocalRotation = parent.IsValid ? Quaternion.Inverse(parent.Transform.Rotation) * value : value;
            }
        }
        /// <summary>Turns it by `rotation` (in its own space, after its current rotation).</summary>
        public void Rotate(Quaternion rotation) => LocalRotation = LocalRotation * rotation;

        /// <summary>World-space forward (-Z), right (+X) and up (+Y) directions.</summary>
        public Vector3 Forward => Axis(new Vector3(0, 0, -1));
        public Vector3 Right => Axis(Vector3.UnitX);
        public Vector3 Up => Axis(Vector3.UnitY);

        private Vector3 Axis(Vector3 local)
        {
            Vector3 v = Vector3.TransformNormal(local, LocalToWorld);
            return v.LengthSquared() > 1e-12f ? Vector3.Normalize(v) : local;
        }

        /// <summary>Rotates so forward (-Z) points at `target` (world space, root entities), keeping no roll.</summary>
        public void LookAt(Vector3 target)
        {
            Vector3 dir = target - WorldPosition;
            if (dir.LengthSquared() < 1e-12f) return;
            dir = Vector3.Normalize(dir);
            float pitch = System.MathF.Asin(System.Math.Clamp(dir.Y, -1.0f, 1.0f));
            float yaw = System.MathF.Atan2(-dir.X, -dir.Z);
            EulerAngles = new Vector3(pitch, yaw, 0.0f) * (180.0f / System.MathF.PI);
        }
    }

    /// <summary>How a body moves: not at all (Static), by physics (Dynamic), or where scripts put it (Kinematic).</summary>
    public enum RigidbodyType { Static = 0, Dynamic = 1, Kinematic = 2 }

    /// <summary>Discrete: checked where it is each step (thin walls can be passed at speed); Continuous: swept along its motion.</summary>
    public enum CollisionDetectionMode { Discrete = 0, Continuous = 1 }
    /// <summary>None: its object jumps a physics step at a time; Interpolate: it moves smoothly between steps (a step behind).</summary>
    public enum RigidbodyInterpolation { None = 0, Interpolate = 1 }

    /// <summary>Axes a dynamic 3D body may not move along or turn about (as Unity's).</summary>
    [Flags]
    public enum RigidbodyConstraints
    {
        None = 0,
        FreezePositionX = 2, FreezePositionY = 4, FreezePositionZ = 8,
        FreezeRotationX = 16, FreezeRotationY = 32, FreezeRotationZ = 64,
        FreezePosition = 14, FreezeRotation = 112, FreezeAll = 126,
    }

    /// <summary>
    /// A 2D body. Its settings apply while playing, keeping its motion and joints. A kinematic body moved by
    /// <see cref="MovePosition"/> (or by setting its Transform) travels there during the next physics step, so it
    /// pushes and carries what it meets; <see cref="Position"/> and <see cref="Rotation"/> put it there at once.
    /// </summary>
    public unsafe class Rigidbody2D : Component
    {
        internal override string NativeName => "Rigidbody 2D";
        private void Refresh() => Native.Api.RigidbodyOp(Entity.Scene, Entity.Id, 0, null);
        private void Move(Vector2 position, float degrees, bool teleport)
        {
            float* v = stackalloc float[6] { position.X, position.Y, 0.0f, 0.0f, 0.0f, degrees * MathF.PI / 180.0f };
            Native.Api.RigidbodyOp(Entity.Scene, Entity.Id, teleport ? 1 : 2, v);
        }

        public RigidbodyType BodyType { get => (RigidbodyType)GetInt("Body Type"); set { SetInt("Body Type", (int)value); Refresh(); } }
        /// <summary>Moved only by scripts (true), or by physics (false: Dynamic).</summary>
        public bool IsKinematic { get => BodyType == RigidbodyType.Kinematic; set => BodyType = value ? RigidbodyType.Kinematic : RigidbodyType.Dynamic; }
        /// <summary>Its mass in kg, from its colliders' densities; setting it scales those densities.</summary>
        public float Mass
        {
            get => Native.Api.RigidbodyOp(Entity.Scene, Entity.Id, 3, null);
            set { float m = value; Native.Api.RigidbodyOp(Entity.Scene, Entity.Id, 4, &m); }
        }
        /// <summary>How strongly gravity pulls it (0 floats, -1 falls up).</summary>
        public float GravityScale { get => GetFloat("Gravity Scale"); set { SetFloat("Gravity Scale", value); Refresh(); } }
        /// <summary>How quickly it slows down by itself (Linear Damping).</summary>
        public float Drag { get => GetFloat("Linear Damping"); set { SetFloat("Linear Damping", value); Refresh(); } }
        public float AngularDrag { get => GetFloat("Angular Damping"); set { SetFloat("Angular Damping", value); Refresh(); } }
        /// <summary>Never turns (Fixed Rotation).</summary>
        public bool FreezeRotation { get => GetBool("Fixed Rotation"); set { SetBool("Fixed Rotation", value); Refresh(); } }
        /// <summary>Where the body is (bodies move their objects as if they had no parent); setting it moves it there at once.</summary>
        public Vector2 Position
        {
            get { Vector3 p = Transform.Position; return new Vector2(p.X, p.Y); }
            set => Move(value, Rotation, true);
        }
        /// <summary>Its angle in degrees (counterclockwise); setting it turns the body at once.</summary>
        public float Rotation { get => Transform.EulerAngles.Z; set => Move(Position, value, true); }
        /// <summary>Kinematic: travels to a world position during the next physics step (pushing and carrying what it
        /// meets); others are put there at once.</summary>
        public void MovePosition(Vector2 position) => Move(position, Rotation, false);
        public void MoveRotation(float degrees) => Move(Position, degrees, false);

        private Vector2 Op2(int op, Vector2 v = default) { Native.Api.Body2DOp(Entity.Scene, Entity.Id, op, (float*)&v); return v; }
        /// <summary>Continuous: a bullet, swept against moving bodies too.</summary>
        public CollisionDetectionMode CollisionDetectionMode
        {
            get => (CollisionDetectionMode)GetInt("Collision Detection");
            set { SetInt("Collision Detection", (int)value); Refresh(); }
        }
        public RigidbodyInterpolation Interpolation
        {
            get => (RigidbodyInterpolation)GetInt("Interpolation");
            set { SetInt("Interpolation", (int)value); Refresh(); }
        }
        /// <summary>Whether it falls asleep at rest, and below which speed (m/s).</summary>
        public bool CanSleep { get => GetBool("Can Sleep"); set { SetBool("Can Sleep", value); Refresh(); } }
        public float SleepThreshold { get => GetFloat("Sleep Threshold"); set { SetFloat("Sleep Threshold", MathF.Max(value, 0)); Refresh(); } }
        /// <summary>Puts it to sleep now (until something touches or moves it).</summary>
        public void Sleep() => Op2(0);
        public void WakeUp() => Op2(7);
        /// <summary>The velocity of a world point moving with it (a wheel's rim, a swinging plank's end).</summary>
        public Vector2 GetPointVelocity(Vector2 worldPoint) => Op2(1, worldPoint);
        /// <summary>Its center of mass in its own space (set: until its colliders change), and in the world.</summary>
        public Vector2 CenterOfMass { get => Op2(2); set => Op2(3, value); }
        public Vector2 WorldCenterOfMass => Op2(4);
        /// <summary>How hard it is to spin (kg m^2, about its center of mass); set: until its colliders change.</summary>
        public float Inertia
        {
            get { float* v = stackalloc float[2]; Native.Api.Body2DOp(Entity.Scene, Entity.Id, 5, v); return v[0]; }
            set { float* v = stackalloc float[2] { value, 0 }; Native.Api.Body2DOp(Entity.Scene, Entity.Id, 6, v); }
        }
        public Vector2 Velocity
        {
            get { Vector2 v; Native.Api.GetVelocity2D(Entity.Scene, Entity.Id, (float*)&v); return v; }
            set => Native.Api.SetVelocity2D(Entity.Scene, Entity.Id, (float*)&value);
        }
        public void AddForce(Vector2 force) => Native.Api.ApplyForce2D(Entity.Scene, Entity.Id, (float*)&force);
        public void AddImpulse(Vector2 impulse) => Native.Api.ApplyImpulse2D(Entity.Scene, Entity.Id, (float*)&impulse);
        /// <summary>Spin in degrees per second, counterclockwise.</summary>
        public float AngularVelocity
        {
            get => Native.Api.Body2D(Entity.Scene, Entity.Id, 0, 0.0f) * 180.0f / MathF.PI;
            set => Native.Api.Body2D(Entity.Scene, Entity.Id, 1, value * MathF.PI / 180.0f);
        }
        /// <summary>Turns the body (N*m, counterclockwise) for this step, like AddForce for rotation.</summary>
        public void AddTorque(float torque) => Native.Api.Body2D(Entity.Scene, Entity.Id, 2, torque);
    }

    /// <summary>
    /// Any 3D collider (GetComponent&lt;Collider&gt;() finds whichever the object has). Changes while playing apply to its
    /// body at once, keeping its motion.
    /// </summary>
    public unsafe class Collider : Component
    {
        private static readonly string[] s_Kinds = { "Box Collider", "Sphere Collider", "Capsule Collider", "Mesh Collider" };
        internal override string NativeName => Array.Find(s_Kinds, k => Entity.HasComponent(k)) ?? s_Kinds[0];
        internal override string[] NativeNames => GetType() == typeof(Collider) ? s_Kinds : new[] { NativeName };
        private protected void Changed() => Native.Api.RefreshColliders(Entity.Scene, Entity.Id);
        private protected void Set(string field, float v) { SetFloat(field, v); Changed(); }
        private protected void Set(string field, Vector3 v) { SetVector3(field, v); Changed(); }

        /// <summary>A trigger: reports OnTriggerEnter/Stay/Exit instead of colliding.</summary>
        public bool IsTrigger { get => GetBool("Is Trigger"); set { SetBool("Is Trigger", value); Changed(); } }
        public float Friction { get => GetFloat("Friction"); set => Set("Friction", value); }
        /// <summary>How much of the speed it keeps off a hit (0 a thud, 1 a full bounce).</summary>
        public float Bounciness { get => GetFloat("Bounciness"); set => Set("Bounciness", value); }

        /// <summary>The point on the object's colliders nearest to `position` (`position` itself when inside them; while
        /// playing).</summary>
        public Vector3 ClosestPoint(Vector3 position)
        {
            float* v = stackalloc float[6] { position.X, position.Y, position.Z, 0, 0, 0 };
            return Native.Api.ColliderQuery(Entity.Scene, Entity.Id, 0, v) != 0 ? new Vector3(v[0], v[1], v[2]) : position;
        }
        /// <summary>The world box around the object's colliders now (while playing; empty at its position otherwise).</summary>
        public Bounds Bounds
        {
            get
            {
                float* v = stackalloc float[6];
                return Native.Api.ColliderQuery(Entity.Scene, Entity.Id, 1, v) != 0
                    ? Bounds.FromMinMax(new Vector3(v[0], v[1], v[2]), new Vector3(v[3], v[4], v[5]))
                    : new Bounds(Transform.WorldPosition, Vector3.Zero);
            }
        }
    }
    public class BoxCollider : Collider
    {
        internal override string NativeName => "Box Collider";
        /// <summary>Its middle, in the object's space.</summary>
        public Vector3 Center { get => GetVector3("Offset"); set => Set("Offset", value); }
        public Vector3 Size { get => GetVector3("Size"); set => Set("Size", value); }
    }
    public class SphereCollider : Collider
    {
        internal override string NativeName => "Sphere Collider";
        public Vector3 Center { get => GetVector3("Offset"); set => Set("Offset", value); }
        public float Radius { get => GetFloat("Radius"); set => Set("Radius", value); }
    }
    /// <summary>Upright (along the object's Y), Height from end to end.</summary>
    public class CapsuleCollider : Collider
    {
        internal override string NativeName => "Capsule Collider";
        public Vector3 Center { get => GetVector3("Offset"); set => Set("Offset", value); }
        public float Radius { get => GetFloat("Radius"); set => Set("Radius", value); }
        public float Height { get => GetFloat("Height"); set => Set("Height", value); }
    }
    public class MeshCollider : Collider
    {
        internal override string NativeName => "Mesh Collider";
        /// <summary>Its convex hull instead of the exact triangles (dynamic bodies always use the hull).</summary>
        public bool Convex { get => GetBool("Convex"); set { SetBool("Convex", value); Changed(); } }
    }

    /// <summary>Any 2D collider (GetComponent&lt;Collider2D&gt;() finds whichever the object has); changes while playing
    /// apply to its body at once.</summary>
    public unsafe class Collider2D : Component
    {
        private static readonly string[] s_Kinds = { "Box Collider 2D", "Circle Collider 2D" };
        internal override string NativeName => Array.Find(s_Kinds, k => Entity.HasComponent(k)) ?? s_Kinds[0];
        internal override string[] NativeNames => GetType() == typeof(Collider2D) ? s_Kinds : new[] { NativeName };
        private protected void Changed() => Native.Api.RefreshColliders(Entity.Scene, Entity.Id);
        private protected void Set(string field, float v) { SetFloat(field, v); Changed(); }

        public bool IsTrigger { get => GetBool("Is Trigger"); set { SetBool("Is Trigger", value); Changed(); } }
        public float Friction { get => GetFloat("Friction"); set => Set("Friction", value); }
        public float Bounciness { get => GetFloat("Bounciness"); set => Set("Bounciness", value); }
        /// <summary>Mass per square meter (the body's mass follows its shapes').</summary>
        public float Density { get => GetFloat("Density"); set => Set("Density", value); }
        public Vector2 Offset { get => GetVector2("Offset"); set { SetVector2("Offset", value); Changed(); } }

        /// <summary>The point on the object's colliders nearest to `position` (`position` itself when inside one).</summary>
        public Vector2 ClosestPoint(Vector2 position)
        {
            float* v = stackalloc float[4] { position.X, position.Y, 0, 0 };
            return Native.Api.ColliderQuery(Entity.Scene, Entity.Id, 2, v) != 0 ? new Vector2(v[0], v[1]) : position;
        }
        /// <summary>The world box around the object's colliders now (z: 0 thick, at the object's depth).</summary>
        public Bounds Bounds
        {
            get
            {
                float* v = stackalloc float[4];
                float z = Transform.WorldPosition.Z;
                return Native.Api.ColliderQuery(Entity.Scene, Entity.Id, 3, v) != 0
                    ? Bounds.FromMinMax(new Vector3(v[0], v[1], z), new Vector3(v[2], v[3], z))
                    : new Bounds(Transform.WorldPosition, Vector3.Zero);
            }
        }
    }
    public class BoxCollider2D : Collider2D
    {
        internal override string NativeName => "Box Collider 2D";
        public Vector2 Size { get => GetVector2("Size"); set { SetVector2("Size", value); Changed(); } }
    }
    public class CircleCollider2D : Collider2D
    {
        internal override string NativeName => "Circle Collider 2D";
        public float Radius { get => GetFloat("Radius"); set => Set("Radius", value); }
    }

    /// <summary>
    /// A 3D body. Its settings apply while playing, keeping its motion and joints. A kinematic body moved by
    /// <see cref="MovePosition"/> (or by setting its Transform) travels there during the next physics step, pushing what
    /// it meets; <see cref="Position"/> and <see cref="Rotation"/> put it there at once.
    /// </summary>
    public unsafe class Rigidbody : Component
    {
        internal override string NativeName => "Rigidbody";
        private void Refresh() => Native.Api.RigidbodyOp(Entity.Scene, Entity.Id, 0, null);
        private void Move(Vector3 position, Vector3 eulerDegrees, bool teleport)
        {
            Vector3 r = eulerDegrees * (MathF.PI / 180.0f);
            float* v = stackalloc float[6] { position.X, position.Y, position.Z, r.X, r.Y, r.Z };
            Native.Api.RigidbodyOp(Entity.Scene, Entity.Id, teleport ? 1 : 2, v);
        }

        public RigidbodyType BodyType { get => (RigidbodyType)GetInt("Body Type"); set { SetInt("Body Type", (int)value); Refresh(); } }
        /// <summary>Moved only by scripts (true), or by physics (false: Dynamic).</summary>
        public bool IsKinematic { get => BodyType == RigidbodyType.Kinematic; set => BodyType = value ? RigidbodyType.Kinematic : RigidbodyType.Dynamic; }
        /// <summary>In kg (spread as its colliders' shapes spread it).</summary>
        public float Mass { get => GetFloat("Mass"); set { SetFloat("Mass", value); Refresh(); } }
        /// <summary>How quickly it slows down by itself (Linear Damping).</summary>
        public float Drag { get => GetFloat("Linear Damping"); set { SetFloat("Linear Damping", value); Refresh(); } }
        public float AngularDrag { get => GetFloat("Angular Damping"); set { SetFloat("Angular Damping", value); Refresh(); } }
        public bool UseGravity { get => GetBool("Use Gravity"); set { SetBool("Use Gravity", value); Refresh(); } }
        public RigidbodyConstraints Constraints { get => (RigidbodyConstraints)GetInt("Constraints"); set { SetInt("Constraints", (int)value); Refresh(); } }
        /// <summary>Never turns (all of Constraints' rotation axes).</summary>
        public bool FreezeRotation
        {
            get => (Constraints & RigidbodyConstraints.FreezeRotation) == RigidbodyConstraints.FreezeRotation;
            set => Constraints = value ? Constraints | RigidbodyConstraints.FreezeRotation : Constraints & ~RigidbodyConstraints.FreezeRotation;
        }
        private (Vector3 Position, Vector3 Euler) Pose()
        {
            float* d = stackalloc float[6];
            Native.Api.Body3D(Entity.Scene, Entity.Id, 14, d);
            return (new Vector3(d[0], d[1], d[2]), new Vector3(d[3], d[4], d[5]) * (180.0f / MathF.PI));
        }
        /// <summary>Where the body is (bodies move their objects as if they had no parent; an interpolating one's object
        /// lags a step behind this); setting it moves it there at once.</summary>
        public Vector3 Position { get => Pose().Position; set => Move(value, Rotation, true); }
        /// <summary>Its rotation as Euler angles in degrees; setting it turns the body at once.</summary>
        public Vector3 Rotation { get => Pose().Euler; set => Move(Position, value, true); }
        /// <summary>Kinematic: travels to a world position during the next physics step (pushing what it meets); others
        /// are put there at once.</summary>
        public void MovePosition(Vector3 position) => Move(position, Rotation, false);
        public void MoveRotation(Vector3 eulerDegrees) => Move(Position, eulerDegrees, false);

        private Vector3 Op3(int op, Vector3 v = default)
        {
            float* d = stackalloc float[6] { v.X, v.Y, v.Z, 0, 0, 0 };
            Native.Api.Body3D(Entity.Scene, Entity.Id, op, d);
            return new Vector3(d[0], d[1], d[2]);
        }
        /// <summary>Continuous: swept along its motion, so at speed it stops at thin walls instead of passing them.</summary>
        public CollisionDetectionMode CollisionDetectionMode
        {
            get => (CollisionDetectionMode)GetInt("Collision Detection");
            set { SetInt("Collision Detection", (int)value); Refresh(); }
        }
        /// <summary>Interpolate: its object moves smoothly between physics steps (a step behind; Position reads that).</summary>
        public RigidbodyInterpolation Interpolation
        {
            get => (RigidbodyInterpolation)GetInt("Interpolation");
            set { SetInt("Interpolation", (int)value); Refresh(); }
        }
        /// <summary>The fastest it may spin (radians per second).</summary>
        public float MaxAngularVelocity { get => GetFloat("Max Angular Velocity"); set { SetFloat("Max Angular Velocity", MathF.Max(value, 0)); Refresh(); } }
        /// <summary>Whether it falls asleep at rest (below Physics.SleepThreshold).</summary>
        public bool CanSleep { get => GetBool("Can Sleep"); set { SetBool("Can Sleep", value); Refresh(); } }
        /// <summary>Puts it to sleep now (until something touches or moves it).</summary>
        public void Sleep() => Op3(7);
        /// <summary>The velocity of a world point moving with it (a wheel's rim, a door's edge).</summary>
        public Vector3 GetPointVelocity(Vector3 worldPoint) => Op3(8, worldPoint);
        /// <summary>Its center of mass in its own space; setting it moves it there (Center Of Mass Offset, kept).</summary>
        public Vector3 CenterOfMass { get => Op3(9); set => Op3(10, value); }
        /// <summary>Back to where its colliders put it.</summary>
        public void ResetCenterOfMass() { SetVector3("Center Of Mass Offset", Vector3.Zero); Native.Api.RefreshColliders(Entity.Scene, Entity.Id); }
        public Vector3 WorldCenterOfMass => Op3(11);
        /// <summary>How hard it is to spin about its principal axes (kg m^2); set: until its colliders or mass change.</summary>
        public Vector3 InertiaTensor { get => Op3(12); set => Op3(13, value); }
        public Vector3 Velocity
        {
            get { Vector3 v; Native.Api.GetVelocity3D(Entity.Scene, Entity.Id, (float*)&v); return v; }
            set => Native.Api.SetVelocity3D(Entity.Scene, Entity.Id, (float*)&value);
        }
        public void AddForce(Vector3 force) => Native.Api.AddForce3D(Entity.Scene, Entity.Id, (float*)&force);
        public void AddImpulse(Vector3 impulse) => Native.Api.AddImpulse3D(Entity.Scene, Entity.Id, (float*)&impulse);
        /// <summary>Moved by physics (Body Type Dynamic). Colliders without a Rigidbody get a static one while playing.</summary>
        public bool IsDynamic => BodyType == RigidbodyType.Dynamic;
        /// <summary>At rest for a while, so not simulated until something touches or moves it (setting Velocity wakes it).</summary>
        public bool IsSleeping { get { float* v = stackalloc float[6]; Native.Api.Body3D(Entity.Scene, Entity.Id, 6, v); return v[0] != 0.0f; } }
        public void WakeUp() { float* v = stackalloc float[6]; Native.Api.Body3D(Entity.Scene, Entity.Id, 5, v); }
        /// <summary>Spin about the world axes in radians per second (as Unity's 3D Rigidbody).</summary>
        public Vector3 AngularVelocity
        {
            get { Vector3 w = default; Native.Api.Body3D(Entity.Scene, Entity.Id, 0, (float*)&w); return w; }
            set => Native.Api.Body3D(Entity.Scene, Entity.Id, 1, (float*)&value);
        }
        /// <summary>Turns the body (N*m about world axes) for this step, like AddForce for rotation.</summary>
        public void AddTorque(Vector3 torque) => Native.Api.Body3D(Entity.Scene, Entity.Id, 2, (float*)&torque);
        /// <summary>A force at a world point: pushes the body and, off its center of mass, turns it too.</summary>
        public void AddForceAtPosition(Vector3 force, Vector3 position)
        {
            float* v = stackalloc float[6] { force.X, force.Y, force.Z, position.X, position.Y, position.Z };
            Native.Api.Body3D(Entity.Scene, Entity.Id, 3, v);
        }
        /// <summary>An instant push at a world point (a hit where something struck it).</summary>
        public void AddImpulseAtPosition(Vector3 impulse, Vector3 position)
        {
            float* v = stackalloc float[6] { impulse.X, impulse.Y, impulse.Z, position.X, position.Y, position.Z };
            Native.Api.Body3D(Entity.Scene, Entity.Id, 4, v);
        }
    }

    /// <summary>
    /// Draws a mesh: with a <see cref="Material"/> asset, that material's look; without one, the simple fields here
    /// (<see cref="Texture"/>, <see cref="Color"/>, <see cref="Smoothness"/>, <see cref="Emission"/>). <see cref="Tint"/>
    /// recolors either, for this object only.
    /// </summary>
    public unsafe class MeshRenderer : Component
    {
        internal override string NativeName => "Mesh Renderer";
        public Color Color
        {
            get { Color c; Native.Api.GetColor(Entity.Scene, Entity.Id, (float*)&c); return c; }
            set => Native.Api.SetColor(Entity.Scene, Entity.Id, (float*)&value);
        }
        /// <summary>The mesh it draws (the built-in shapes too: a cube is GUID 1, a sphere 2).</summary>
        public Mesh Mesh { get => new(GetAssetGuid("Mesh")); set => SetAssetGuid("Mesh", value.Guid); }
        /// <summary>Its own material, as Unity's renderer.material: the first time it is read, the material it uses (or its
        /// simple look) is copied for this object alone, so changing it leaves the others alone; setting one uses that
        /// one (shared).</summary>
        public Material Material
        {
            get => new(Native.Api.RendererMaterialInstance(Entity.Scene, Entity.Id));
            set => SetAssetGuid("Material", value.Guid);
        }
        /// <summary>The material it uses, shared with every object using it (none: the simple fields below); changing it
        /// changes all of them.</summary>
        public Material SharedMaterial { get => new(GetAssetGuid("Material")); set => SetAssetGuid("Material", value.Guid); }
        /// <summary>The albedo texture of the simple look (no Material).</summary>
        public Texture Texture { get => new(GetAssetGuid("Albedo")); set => SetAssetGuid("Albedo", value.Guid); }
        public Vector2 Tiling { get => GetVector2("Tiling"); set => SetVector2("Tiling", value); }
        /// <summary>0 rough to 1 polished (the simple look).</summary>
        public float Smoothness { get => GetFloat("Smoothness"); set => SetFloat("Smoothness", value); }
        /// <summary>How much it glows in its color (0: not at all; above 1 blooms).</summary>
        public float Emission { get => GetFloat("Emission"); set => SetFloat("Emission", value); }
        public bool CastShadows { get => GetBool("Cast Shadows"); set => SetBool("Cast Shadows", value); }
        /// <summary>Multiplies the color of whatever it draws, a material asset's too (white: as it is); for this object
        /// only, so flashing one crate leaves the others sharing its material alone.</summary>
        public Color Tint
        {
            get { Vector4 v = GetVector4("Tint"); return new Color(v.X, v.Y, v.Z, v.W); }
            set => SetVector4("Tint", new Vector4(value.R, value.G, value.B, value.A));
        }

        // Ops: 0 count, 1 get slot, 2 set slot, 3 clear.
        private ulong Slots(int op, int slot = 0, ulong guid = 0) => Native.Api.RendererMaterials(Entity.Scene, Entity.Id, op, slot, guid);
        /// <summary>How many material slots are overridden (slot N replaces the model's material N; see SetMaterial).</summary>
        public int MaterialOverrideCount => (int)Slots(0);
        /// <summary>The material overriding a slot of the model (none: Material, or the model's own).</summary>
        public Material GetMaterial(int slot) => new(Slots(1, slot));
        /// <summary>Overrides one material slot of a model with several (none puts its own back).</summary>
        public void SetMaterial(int slot, Material material) => Slots(2, slot, material.Guid);
        public void ClearMaterialOverrides() => Slots(3);
    }

    public enum DecalBlend { Alpha = 0, Multiply = 1, Additive = 2 }

    /// <summary>Projects a texture onto the surfaces in front of it (along its -Z), like Unity's URP Decal Projector:
    /// scorch marks, dirt, paint, footprints.</summary>
    public unsafe class Decal : Component
    {
        internal override string NativeName => "Decal";
        /// <summary>The picture projected (none: a soft round spot).</summary>
        public Texture Texture { get => new(GetAssetGuid("Texture")); set => SetAssetGuid("Texture", value.Guid); }
        /// <summary>Its tint; alpha is the opacity (fade it out by lowering it).</summary>
        public Color Color { get { Vector4 v = GetVector4("Color"); return new Color(v.X, v.Y, v.Z, v.W); } set => SetVector4("Color", new Vector4(value.R, value.G, value.B, value.A)); }
        /// <summary>Width and height of the picture, and how deep it projects.</summary>
        public Vector3 Size { get => GetVector3("Size"); set => SetVector3("Size", value); }
        public DecalBlend Blend { get => (DecalBlend)GetInt("Blend"); set => SetInt("Blend", (int)value); }
        /// <summary>Surfaces tilted away from it fade out from this cosine on (0-1).</summary>
        public float AngleFade { get => GetFloat("Angle Fade"); set => SetFloat("Angle Fade", value); }
        public int SortingOrder { get => GetInt("Sorting Order"); set => SetInt("Sorting Order", value); }
    }

    public enum TextAlignment { Left = 0, Center = 1, Right = 2 }

    /// <summary>Text in the 3D world (a Text (3D) component, like Unity's TextMesh): labels, signs, damage numbers.</summary>
    public unsafe class TextMesh : Component
    {
        internal override string NativeName => "Text (3D)";
        public string Text
        {
            get => Native.FromUtf8(Native.Api.GetUIText(Entity.Scene, Entity.Id));
            set { var e = Entity; Native.WithUtf8(value ?? "", p => Native.Api.SetUIText(e.Scene, e.Id, (byte*)p)); }
        }
        public Color Color { get { Vector4 v = GetVector4("Color"); return new Color(v.X, v.Y, v.Z, v.W); } set => SetVector4("Color", new Vector4(value.R, value.G, value.B, value.A)); }
        /// <summary>The height of a line in world units.</summary>
        public float FontSize { get => GetFloat("Font Size"); set => SetFloat("Font Size", value); }
        public TextAlignment Alignment { get => (TextAlignment)GetInt("Alignment"); set => SetInt("Alignment", (int)value); }
        /// <summary>Whether tags like &lt;b&gt; and &lt;color=#ff8000&gt; style it.</summary>
        public bool RichText { get => GetBool("Rich Text"); set => SetBool("Rich Text", value); }
        public int SortingOrder { get => GetInt("Sorting Order"); set => SetInt("Sorting Order", value); }
    }

    /// <summary>
    /// A camera; <see cref="Main"/> is the one the scene renders with. Screen points are in pixels of the view the game
    /// is shown in (<see cref="Screen.Width"/> x <see cref="Screen.Height"/>), with the origin at the top-left like
    /// <see cref="Input.MousePosition"/>; their z is the distance in front of the camera in world units.
    /// </summary>
    public unsafe class Camera : Component
    {
        internal override string NativeName => "Camera";

        /// <summary>The camera the running scene renders with (its primary one), or null; setting another makes that one
        /// primary (a cut: the view switches to it this frame).</summary>
        public static Camera Main
        {
            get
            {
                IntPtr scene = Native.CurrentScene;
                ulong id = scene == IntPtr.Zero ? ulong.MaxValue : Native.Api.MainCamera(scene);
                return id == ulong.MaxValue ? null : new Camera { Entity = new Entity(scene, id) };
            }
            set { if (value != null) value.MakeMain(); }
        }
        /// <summary>Every active camera in the scene.</summary>
        public static Camera[] AllCameras => Entity.FindObjectsOfType<Camera>();
        /// <summary>Makes this the camera the game shows (marked Primary; the others not).</summary>
        public void MakeMain() => Native.Api.SetMainCamera(Entity.Scene, Entity.Id);
        /// <summary>Whether the game shows this camera now.</summary>
        public bool IsMain => Native.Api.MainCamera(Entity.Scene) == Entity.Id;

        /// <summary>Viewport points run from (0, 0) at the top-left of the view to (1, 1) at its bottom-right (as screen
        /// points do in pixels); z is the distance in front of the camera.</summary>
        public Vector3 WorldToViewportPoint(Vector3 world)
        {
            Vector3 s = WorldToScreenPoint(world);
            Vector2 size = Screen.Size;
            return new Vector3(s.X / MathF.Max(size.X, 1.0f), s.Y / MathF.Max(size.Y, 1.0f), s.Z);
        }
        public Vector3 ViewportToWorldPoint(Vector3 viewport) => ScreenToWorldPoint(ViewportToScreenPoint(viewport));
        public Ray ViewportPointToRay(Vector2 viewport) { Vector3 s = ViewportToScreenPoint(new Vector3(viewport, 0)); return ScreenPointToRay(new Vector2(s.X, s.Y)); }
        public static Vector3 ViewportToScreenPoint(Vector3 viewport) { Vector2 size = Screen.Size; return new Vector3(viewport.X * size.X, viewport.Y * size.Y, viewport.Z); }
        public static Vector3 ScreenToViewportPoint(Vector3 screen) { Vector2 size = Screen.Size; return new Vector3(screen.X / MathF.Max(size.X, 1.0f), screen.Y / MathF.Max(size.Y, 1.0f), screen.Z); }
        /// <summary>The view's width over its height.</summary>
        public float Aspect { get { Vector2 size = Screen.Size; return size.X / MathF.Max(size.Y, 1.0f); } }

        public float NearClipPlane { get => GetFloat("Near Clip"); set => SetFloat("Near Clip", value); }
        public float FarClipPlane { get => GetFloat("Far Clip"); set => SetFloat("Far Clip", value); }
        /// <summary>The color it clears to (with Clear Flags Solid Color).</summary>
        public Color BackgroundColor
        {
            get { Vector4 v = GetVector4("Background"); return new Color(v.X, v.Y, v.Z, v.W); }
            set => SetVector4("Background", new Vector4(value.R, value.G, value.B, value.A));
        }
        public CameraClearFlags ClearFlags { get => (CameraClearFlags)GetInt("Clear Flags"); set => SetInt("Clear Flags", (int)value); }

        /// <summary>Where a world point shows on screen (z: its distance in front of the camera, negative behind it).</summary>
        public Vector3 WorldToScreenPoint(Vector3 world)
        {
            Native.Api.ViewOp(Entity.Scene, Entity.Id, 0, (float*)&world);
            return world;
        }
        /// <summary>The world point under a screen point, `screen.Z` world units in front of the camera.</summary>
        public Vector3 ScreenToWorldPoint(Vector3 screen)
        {
            Native.Api.ViewOp(Entity.Scene, Entity.Id, 1, (float*)&screen);
            return screen;
        }
        /// <summary>The ray from the camera through a screen point (e.g. the mouse), for Physics.Raycast.</summary>
        public Ray ScreenPointToRay(Vector2 screen)
        {
            float* v = stackalloc float[6];
            v[0] = screen.X; v[1] = screen.Y;
            Native.Api.ViewOp(Entity.Scene, Entity.Id, 2, v);
            return new Ray(new Vector3(v[0], v[1], v[2]), new Vector3(v[3], v[4], v[5]));
        }

        /// <summary>Vertical field of view in degrees (perspective cameras).</summary>
        public float FieldOfView { get => GetFloat("Field Of View"); set => SetFloat("Field Of View", value); }
        /// <summary>Half the view's height in world units (orthographic cameras).</summary>
        public float OrthographicSize { get => GetFloat("Orthographic Size"); set => SetFloat("Orthographic Size", value); }
        public bool Orthographic { get => GetInt("Projection") == 1; set => SetInt("Projection", value ? 1 : 0); }
        /// <summary>Where on the screen it draws (Unity's rect, but from the top-left): x, y, width, height as fractions of
        /// the screen. A camera other than the main one draws over it when this is smaller than the screen (picture in
        /// picture, split screen, a minimap).</summary>
        public Rect Rect
        {
            get { Vector4 v = GetVector4("Viewport Rect"); return new Rect(v.X, v.Y, v.Z, v.W); }
            set => SetVector4("Viewport Rect", new Vector4(value.X, value.Y, value.Width, value.Height));
        }
        /// <summary>The same in pixels.</summary>
        public Rect PixelRect
        {
            get { Rect r = Rect; Vector2 s = Screen.Size; return new Rect(r.X * s.X, r.Y * s.Y, r.Width * s.X, r.Height * s.Y); }
            set { Vector2 s = Screen.Size; Rect = new Rect(value.X / s.X, value.Y / s.Y, value.Width / s.X, value.Height / s.Y); }
        }
        /// <summary>Of the cameras drawn over the main one, higher ones go on top.</summary>
        public float Depth { get => GetFloat("Depth"); set => SetFloat("Depth", value); }
        /// <summary>The layers it draws (LayerMask.GetMask("Default", "Player"); -1 everything).</summary>
        public int CullingMask { get => GetInt("Culling Mask"); set => SetInt("Culling Mask", value); }
        /// <summary>A Render Texture asset it draws into instead of the screen (default: none, the screen). Materials,
        /// sprites and UI Images showing that texture show this camera's view.</summary>
        public Texture TargetTexture { get => new(GetAssetGuid("Target Texture")); set => SetAssetGuid("Target Texture", value.Guid); }
        /// <summary>With a Target Texture: it draws only when <see cref="Render"/> asks, not every frame.</summary>
        public bool ManualRender { get => GetBool("Manual Render"); set => SetBool("Manual Render", value); }
        /// <summary>Draws it into its Target Texture this frame (with ManualRender: a snapshot; Texture2D.ReadPixels sees
        /// it the frame after).</summary>
        public void Render() { ulong id = Entity.Id; Native.Api.TextureOp(Entity.Scene, 9, &id, null, null, 0, 0); }
    }

    /// <summary>What a camera's image starts from; Depth Only adds a camera drawn over another to the image below.</summary>
    public enum CameraClearFlags { SolidColor = 0, Skybox = 1, DepthOnly = 2 }

    public unsafe class SpriteRenderer : Component
    {
        internal override string NativeName => "Sprite Renderer";
        public Color Color
        {
            get { Color c; Native.Api.GetColor(Entity.Scene, Entity.Id, (float*)&c); return c; }
            set => Native.Api.SetColor(Entity.Scene, Entity.Id, (float*)&value);
        }
        /// <summary>The texture it draws.</summary>
        public Texture Sprite { get => new(GetAssetGuid("Sprite")); set => SetAssetGuid("Sprite", value.Guid); }
        public bool FlipX { get => GetBool("Flip X"); set => SetBool("Flip X", value); }
        public bool FlipY { get => GetBool("Flip Y"); set => SetBool("Flip Y", value); }
    }

    /// <summary>
    /// Where a UI element sits (like Unity's RectTransform). Anchors and the pivot are fractions of the parent's rect
    /// (0,0 is its top-left); <see cref="AnchoredPosition"/> is the pivot's offset from the anchors and <see cref="Size"/>
    /// is added to the span between them (so it is the size when both anchors are the same point), in canvas units.
    /// </summary>
    public unsafe class UIRect : Component
    {
        internal override string NativeName => "UI Rect";
        public Vector2 AnchoredPosition { get => GetVector2("Position"); set => SetVector2("Position", value); }
        /// <summary>Added to the anchors' span (Unity's sizeDelta); the size itself when the anchors coincide.</summary>
        public Vector2 Size { get => GetVector2("Size"); set => SetVector2("Size", value); }
        public Vector2 AnchorMin { get => GetVector2("Anchor Min"); set => SetVector2("Anchor Min", Clamp01(value)); }
        public Vector2 AnchorMax { get => GetVector2("Anchor Max"); set => SetVector2("Anchor Max", Clamp01(value)); }
        public Vector2 Pivot { get => GetVector2("Pivot"); set => SetVector2("Pivot", Clamp01(value)); }
        private static Vector2 Clamp01(Vector2 v) => Vector2.Clamp(v, Vector2.Zero, Vector2.One);

        /// <summary>Both anchors at once (e.g. (0,0)-(1,1) to stretch over the parent).</summary>
        public void SetAnchors(Vector2 min, Vector2 max) { AnchorMin = min; AnchorMax = max; }

        /// <summary>
        /// Where it is on screen in pixels (top-left origin, like <see cref="Input.MousePosition"/>), as laid out at the
        /// start of this frame: changes made since show next frame.
        /// </summary>
        public Rect ScreenRect
        {
            get
            {
                float* v = stackalloc float[5];
                if (Native.Api.UIRectScreen(Entity.Scene, Entity.Id, v) == 0) return default;
                return new Rect(v[0], v[1], v[2] - v[0], v[3] - v[1]);
            }
        }
        /// <summary>Canvas units to screen pixels for this element (its canvas's scale).</summary>
        public float Scale { get { float* v = stackalloc float[5]; return Native.Api.UIRectScreen(Entity.Scene, Entity.Id, v) != 0 ? v[4] : 1.0f; } }
        /// <summary>Whether a screen point (the mouse) is over it (see <see cref="ScreenRect"/>).</summary>
        public bool ContainsScreenPoint(Vector2 screen) => ScreenRect.Contains(screen);

        /// <summary>
        /// A screen point (pixels) in this element's own space, in canvas units from its pivot (Unity's
        /// RectTransformUtility.ScreenPointToLocalPointInRectangle; y grows down here, as anchors do). With
        /// <see cref="LocalRect"/> it says where on the element a click landed (a slider of your own, a map).
        /// </summary>
        public Vector2 ScreenPointToLocalPoint(Vector2 screen)
        {
            float* v = stackalloc float[5];
            if (Native.Api.UIRectScreen(Entity.Scene, Entity.Id, v) == 0) return default;
            Vector2 min = new(v[0], v[1]), max = new(v[2], v[3]);
            Vector2 pivot = min + Pivot * (max - min);
            return (screen - pivot) / MathF.Max(v[4], 1e-4f);
        }
        /// <summary>A screen point as a fraction of this element (0,0 its top-left corner, 1,1 its bottom-right).</summary>
        public Vector2 ScreenPointToNormalized(Vector2 screen)
        {
            Rect r = ScreenRect;
            if (r.Width <= 0 || r.Height <= 0) return default;
            return new Vector2((screen.X - r.X) / r.Width, (screen.Y - r.Y) / r.Height);
        }
        /// <summary>Its rect in its own space (canvas units, from its pivot; Unity's rect.rect), as laid out.</summary>
        public Rect LocalRect
        {
            get
            {
                Rect r = ScreenRect;
                float s = MathF.Max(Scale, 1e-4f);
                Vector2 size = new(r.Width / s, r.Height / s);
                return new Rect(-Pivot.X * size.X, -Pivot.Y * size.Y, size.X, size.Y);
            }
        }
    }

    /// <summary>
    /// A sprite sheet's flip-book on a Sprite Renderer: it plays FrameCount frames from FirstFrame (the sheet's frames
    /// are numbered row by row) at FramesPerSecond.
    /// </summary>
    public unsafe class SpriteAnimator : Component
    {
        internal override string NativeName => "Sprite Animator";
        /// <summary>Its speed; changing it while playing goes on from the frame it is on.</summary>
        public float FramesPerSecond
        {
            get => GetFloat("Frames Per Second");
            set
            {
                float before = FramesPerSecond, time = Time;
                SetFloat("Frames Per Second", value);
                float after = FramesPerSecond; // within the Inspector's range
                if (before > 0.0f && after > 0.0f && after != before) Time = time * before / after;
            }
        }
        public int FirstFrame { get => GetInt("First Frame"); set => SetInt("First Frame", value); }
        public int FrameCount { get => GetInt("Frame Count"); set => SetInt("Frame Count", value); }
        /// <summary>Starts over after the last frame; otherwise it stops there (IsPlaying turns false).</summary>
        public bool Loop { get => GetBool("Loop"); set => SetBool("Loop", value); }
        public bool IsPlaying => GetBool("Playing");

        /// <summary>Plays from the first frame.</summary>
        public void Play() { SetBool("Playing", true); Time = 0.0f; }
        /// <summary>Plays another run of the sheet's frames (a clip: e.g. a walk cycle's frames) from its start.</summary>
        public void Play(int firstFrame, int frameCount, bool loop = true)
        {
            FirstFrame = firstFrame;
            FrameCount = frameCount;
            Loop = loop;
            Play();
        }
        /// <summary>Holds the frame it is on; Resume goes on from there.</summary>
        public void Pause() => SetBool("Playing", false);
        public void Resume() => SetBool("Playing", true);
        /// <summary>Stops and goes back to the first frame.</summary>
        public void Stop() { SetBool("Playing", false); Time = 0.0f; }

        /// <summary>Seconds into the run; setting it jumps there and shows that frame at once.</summary>
        public float Time
        {
            get => Native.Api.SpriteAnimatorTime(Entity.Scene, Entity.Id, 0, 0.0f);
            set => Native.Api.SpriteAnimatorTime(Entity.Scene, Entity.Id, 1, value);
        }
        /// <summary>The frame it shows, counted from FirstFrame (0 to FrameCount - 1); setting it jumps there.</summary>
        public int Frame
        {
            get
            {
                int count = Math.Max(FrameCount, 1), step = (int)(Time * FramesPerSecond);
                return Loop ? step % count : Math.Min(step, count - 1);
            }
            set { float fps = FramesPerSecond; if (fps > 0.0f) Time = (value + 0.5f) / fps; }
        }
    }

    /// <summary>
    /// A grid of tiles from a sprite sheet (like Unity's Tilemap): cell (x, y) is CellSize wide in the object's XY plane,
    /// from its origin, and shows the sheet's tile number `tile` (counted row by row; -1 is an empty cell). Its collider
    /// follows the tiles painted or cleared while playing (from the next physics step).
    /// </summary>
    public unsafe class Tilemap : Component
    {
        internal override string NativeName => "Tilemap";
        private int Op(int op, int x = 0, int y = 0, int tile = 0, float* io = null) => Native.Api.TilemapOp(Entity.Scene, Entity.Id, op, x, y, tile, io);

        /// <summary>The tile in a cell (-1: empty).</summary>
        public int GetTile(int x, int y) => Op(0, x, y);
        public int GetTile(Vector2Int cell) => GetTile(cell.X, cell.Y);
        public bool HasTile(Vector2Int cell) => GetTile(cell) >= 0;
        /// <summary>Paints a cell with a tile of the sheet (a negative tile empties it).</summary>
        public void SetTile(int x, int y, int tile) => Op(1, x, y, tile);
        public void SetTile(Vector2Int cell, int tile) => SetTile(cell.X, cell.Y, tile);
        public void ClearTile(Vector2Int cell) => SetTile(cell.X, cell.Y, -1);
        /// <summary>Paints every cell of a rectangle (min and max inclusive).</summary>
        public void BoxFill(Vector2Int min, Vector2Int max, int tile)
        {
            for (int y = Math.Min(min.Y, max.Y); y <= Math.Max(min.Y, max.Y); ++y)
                for (int x = Math.Min(min.X, max.X); x <= Math.Max(min.X, max.X); ++x) SetTile(x, y, tile);
        }
        /// <summary>Empties every cell.</summary>
        public void ClearAllTiles() => Op(2);
        /// <summary>How many cells are painted.</summary>
        public int TileCount => Op(3);

        /// <summary>The center of a cell in world space.</summary>
        public Vector3 CellToWorld(Vector2Int cell) { Vector3 p; Op(4, cell.X, cell.Y, 0, (float*)&p); return p; }
        /// <summary>The cell under a world point (e.g. the mouse, from Camera.ScreenToWorldPoint).</summary>
        public Vector2Int WorldToCell(Vector3 world) { Op(5, 0, 0, 0, (float*)&world); return new Vector2Int((int)world.X, (int)world.Y); }
        /// <summary>The smallest and largest painted cells (inclusive), or false when nothing is painted.</summary>
        public bool GetCellBounds(out Vector2Int min, out Vector2Int max)
        {
            float* v = stackalloc float[4];
            bool any = Op(6, 0, 0, 0, v) != 0;
            min = new Vector2Int((int)v[0], (int)v[1]);
            max = new Vector2Int((int)v[2], (int)v[3]);
            return any;
        }
        public float CellSize { get => GetFloat("Cell Size"); set => SetFloat("Cell Size", value); }
        public Color Color { get { Vector4 v = GetVector4("Color"); return new Color(v.X, v.Y, v.Z, v.W); } set => SetVector4("Color", new Vector4(value.R, value.G, value.B, value.A)); }
    }

    /// <summary>A Terrain (its height field), for putting things on the ground.</summary>
    public unsafe class Terrain : Component
    {
        internal override string NativeName => "Terrain";

        /// <summary>The scene's first active terrain (null when there is none).</summary>
        public static Terrain Active => Entity.FindObjectOfType<Terrain>();

        /// <summary>The ground under (or over) a world point, along the terrain's up axis, and the surface normal there;
        /// false outside the terrain's area. Holes are ignored.</summary>
        public bool TrySample(Vector3 worldPosition, out Vector3 ground, out Vector3 normal)
        {
            float* v = stackalloc float[6];
            bool inside = Native.Api.TerrainSample(Entity.Scene, Entity.Id, (float*)&worldPosition, v) != 0;
            ground = new Vector3(v[0], v[1], v[2]);
            normal = new Vector3(v[3], v[4], v[5]);
            return inside;
        }
        /// <summary>
        /// The world height (Y) of the ground under a point (Unity's SampleHeight gives it from the terrain's position;
        /// this is the world Y, ready for a position). Outside the terrain: the point's own Y.
        /// </summary>
        public float SampleHeight(Vector3 worldPosition) => TrySample(worldPosition, out Vector3 g, out _) ? g.Y : worldPosition.Y;
        /// <summary>The surface normal under a point (up outside the terrain).</summary>
        public Vector3 GetNormal(Vector3 worldPosition) => TrySample(worldPosition, out _, out Vector3 n) ? n : Vector3.UnitY;
        /// <summary>Meters along X and Z, centered on the object.</summary>
        public Vector2 Size => GetVector2("Size");
    }

    /// <summary>
    /// A 2D joint between this object's Rigidbody 2D and ConnectedBody's (none: the world); changes to what it connects or
    /// its shape while playing make it again (as Joint does); the motor and break limits apply as they are.
    /// </summary>
    public unsafe class Joint2D : Component
    {
        internal override string NativeName => "Joint 2D";
        private float State(int i) { float* s = stackalloc float[4]; Native.Api.GetJointState(Entity.Scene, Entity.Id, s); return s[i]; }
        private void Rebuild() => Native.Api.RebuildJoint(Entity.Scene, Entity.Id);
        private void Shape(string field, float v) { SetFloat(field, v); Rebuild(); }
        private void Shape(string field, Vector2 v) { SetVector2(field, v); Rebuild(); }

        public Joint2DType Type { get => (Joint2DType)GetInt("Type"); set { SetInt("Type", (int)value); Rebuild(); } }
        public Entity ConnectedBody { get => GetEntity("Connected Body"); set { SetEntity("Connected Body", value); Rebuild(); } }
        public Vector2 Anchor { get => GetVector2("Anchor"); set => Shape("Anchor", value); }
        public Vector2 ConnectedAnchor { get => GetVector2("Connected Anchor"); set => Shape("Connected Anchor", value); }
        public bool UseLimits { get => GetBool("Use Limits"); set { SetBool("Use Limits", value); Rebuild(); } }
        /// <summary>Hinge limits in degrees.</summary>
        public float LowerAngle { get => GetFloat("Lower Angle"); set => Shape("Lower Angle", value); }
        public float UpperAngle { get => GetFloat("Upper Angle"); set => Shape("Upper Angle", value); }
        /// <summary>Distance joints: the length (0: the distance when it is made).</summary>
        public float Distance { get => GetFloat("Distance"); set => Shape("Distance", value); }
        /// <summary>Slider axis, in this object's space, and its travel from where it was made.</summary>
        public Vector2 Axis { get => GetVector2("Axis"); set => Shape("Axis", value); }
        public float LowerTranslation { get => GetFloat("Lower Translation"); set => Shape("Lower Translation", value); }
        public float UpperTranslation { get => GetFloat("Upper Translation"); set => Shape("Upper Translation", value); }
        public float SpringFrequency { get => GetFloat("Spring Frequency"); set => Shape("Spring Frequency", value); }
        public float SpringDamping { get => GetFloat("Spring Damping"); set => Shape("Spring Damping", value); }
        public bool CollideConnected { get => GetBool("Collide Connected"); set { SetBool("Collide Connected", value); Rebuild(); } }
        /// <summary>Hinge: drives toward MotorSpeed degrees per second; slider: units per second.</summary>
        public bool UseMotor { get => GetBool("Use Motor"); set => SetBool("Use Motor", value); }
        public float MotorSpeed { get => GetFloat("Motor Speed"); set => SetFloat("Motor Speed", value); }
        public float MotorMaxTorque { get => GetFloat("Motor Max Torque"); set => SetFloat("Motor Max Torque", value); }
        public float BreakForce { get => GetFloat("Break Force"); set => SetFloat("Break Force", value); }
        public float BreakTorque { get => GetFloat("Break Torque"); set => SetFloat("Break Torque", value); }
        public bool IsBroken { get { float* s = stackalloc float[4]; return Native.Api.GetJointState(Entity.Scene, Entity.Id, s) == 1; } }
        /// <summary>Reaction force (N) and torque (N*m) during the last physics step.</summary>
        public float Force => State(2);
        public float Torque => State(3);
        /// <summary>Makes the joint again as it is set now (mending a broken one).</summary>
        public void Reconnect() => Rebuild();
    }

    public enum LightType { Directional = 0, Point = 1, Spot = 2 }

    /// <summary>
    /// A Directional, Point or Spot Light (like Unity's Light: one class for the three; <see cref="Type"/> says which).
    /// Values are kept within the Inspector's ranges; ones a kind does not have (a directional light's Range) read 0.
    /// AddComponent&lt;Light&gt;() adds a Point Light.
    /// </summary>
    public unsafe class Light : Component
    {
        private static readonly string[] s_Kinds = { "Directional Light", "Point Light", "Spot Light" };
        internal override string NativeName => s_Kinds[(int)Type];
        internal override string[] NativeNames => s_Kinds;

        public LightType Type
        {
            get
            {
                if (Entity.HasComponent(s_Kinds[0])) return LightType.Directional;
                return Entity.HasComponent(s_Kinds[2]) ? LightType.Spot : LightType.Point;
            }
        }
        /// <summary>Its color (sRGB, as in the Inspector; alpha is always 1).</summary>
        public Color Color
        {
            get { Vector3 c = GetVector3("Color"); return new Color(c.X, c.Y, c.Z); }
            set => SetVector3("Color", new Vector3(value.R, value.G, value.B));
        }
        public float Intensity { get => GetFloat("Intensity"); set => SetFloat("Intensity", value); }
        /// <summary>How far a point or spot light reaches, in world units.</summary>
        public float Range { get => GetFloat("Range"); set => SetFloat("Range", value); }
        /// <summary>A spot light's full cone angle in degrees, and that of its fully lit core.</summary>
        public float SpotAngle { get => GetFloat("Spot Angle"); set => SetFloat("Spot Angle", value); }
        public float InnerSpotAngle { get => GetFloat("Inner Spot Angle"); set => SetFloat("Inner Spot Angle", value); }
        /// <summary>Whether it casts shadows (point and spot lights share 4 shadow slots).</summary>
        public bool CastShadows { get => GetBool("Cast Shadows"); set => SetBool("Cast Shadows", value); }
        public float ShadowBias { get => GetFloat("Shadow Bias"); set => SetFloat("Shadow Bias", value); }
        /// <summary>A point or spot light's cookie: a texture that tints its light (none: plain).</summary>
        public Texture Cookie
        {
            get { ulong g = GetAssetGuid("Cookie"); return new Texture(g == ulong.MaxValue ? 0 : g); }
            set => SetAssetGuid("Cookie", value.Guid);
        }
    }

    public enum UIImageType { Simple = 0, Sliced = 1 }

    /// <summary>A UI picture or panel: a texture (or white without one) tinted by <see cref="Color"/>.</summary>
    public unsafe class UIImage : Component
    {
        internal override string NativeName => "UI Image";
        public Color Color
        {
            get { Vector4 v = GetVector4("Color"); return new Color(v.X, v.Y, v.Z, v.W); }
            set => SetVector4("Color", new Vector4(value.R, value.G, value.B, value.A));
        }
        /// <summary>The texture it shows (none: a plain colored rect).</summary>
        public Texture Sprite { get => new(GetAssetGuid("Image")); set => SetAssetGuid("Image", value.Guid); }
        /// <summary>Simple stretches the texture; Sliced keeps its <see cref="Border"/> unstretched (9-slice).</summary>
        public UIImageType Type { get => (UIImageType)GetInt("Image Type"); set => SetInt("Image Type", (int)value); }
        /// <summary>The 9-slice borders in texture pixels: left, top, right, bottom.</summary>
        public Vector4 Border { get => GetVector4("Border"); set => SetVector4("Border", value); }
    }

    public unsafe class UIText : Component
    {
        internal override string NativeName => "UI Text";
        public string Text
        {
            get => Native.FromUtf8(Native.Api.GetUIText(Entity.Scene, Entity.Id));
            set { var e = Entity; Native.WithUtf8(value, p => Native.Api.SetUIText(e.Scene, e.Id, (byte*)p)); }
        }
        /// <summary>
        /// A string table key: while set, the text shown is its text in the current language (and follows when the
        /// language changes). Setting <see cref="Text"/> clears it.
        /// </summary>
        public string LocalizedKey
        {
            get => Localization.Call(8, null, Entity) ?? "";
            set => Localization.Call(9, value, Entity);
        }
    }

    public class UIButton : Component
    {
        internal override string NativeName => "UI Button";
        /// <summary>Raised when the button is clicked (mouse, or Submit while it is selected). Any script can listen.</summary>
        public event Action Clicked
        {
            add => EngineEvents.Add(Entity, EngineEvents.Kind.Click, value);
            remove => EngineEvents.Remove(Entity, EngineEvents.Kind.Click, value);
        }
    }

    public unsafe class UISlider : Component
    {
        internal override string NativeName => "UI Slider";
        public float Value
        {
            get => Native.Api.GetUIValue(Entity.Scene, Entity.Id);
            set => Native.Api.SetUIValue(Entity.Scene, Entity.Id, value);
        }
        /// <summary>Raised with the new value when the user moves the slider.</summary>
        public event Action<float> ValueChanged
        {
            add => EngineEvents.Add(Entity, EngineEvents.Kind.ValueChanged, value);
            remove => EngineEvents.Remove(Entity, EngineEvents.Kind.ValueChanged, value);
        }
    }

    public unsafe class UIToggle : Component
    {
        internal override string NativeName => "UI Toggle";
        public bool IsOn
        {
            get => Native.Api.GetUIValue(Entity.Scene, Entity.Id) != 0.0f;
            set => Native.Api.SetUIValue(Entity.Scene, Entity.Id, value ? 1.0f : 0.0f);
        }
        /// <summary>Raised with the new state when the user flips the toggle.</summary>
        public event Action<bool> ValueChanged
        {
            add => EngineEvents.Add(Entity, EngineEvents.Kind.ValueChanged, value);
            remove => EngineEvents.Remove(Entity, EngineEvents.Kind.ValueChanged, value);
        }
    }

    public unsafe class UIInputField : Component
    {
        internal override string NativeName => "UI Input Field";
        public string Text
        {
            get => Native.FromUtf8(Native.Api.GetUIText(Entity.Scene, Entity.Id));
            set { var e = Entity; Native.WithUtf8(value, p => Native.Api.SetUIText(e.Scene, e.Id, (byte*)p)); }
        }
        /// <summary>Raised with the text after each edit.</summary>
        public event Action<string> TextChanged
        {
            add => EngineEvents.Add(Entity, EngineEvents.Kind.TextChanged, value);
            remove => EngineEvents.Remove(Entity, EngineEvents.Kind.TextChanged, value);
        }
        /// <summary>Raised with the text when Enter submits it.</summary>
        public event Action<string> Submitted
        {
            add => EngineEvents.Add(Entity, EngineEvents.Kind.Submit, value);
            remove => EngineEvents.Remove(Entity, EngineEvents.Kind.Submit, value);
        }
    }

    /// <summary>A Joint (3D) or Joint 2D on the entity: hinge motor control and break state.</summary>
    public enum JointType { Fixed = 0, Hinge = 1, Point = 2, Distance = 3, Slider = 4 }
    public enum Joint2DType { Fixed = 0, Hinge = 1, Distance = 2, Slider = 3 }

    /// <summary>
    /// A 3D joint between this object's Rigidbody and ConnectedBody's (none: the world). Changing what it connects, its
    /// type, anchor, axis or limits while playing makes the joint again from the object's pose then (so a Distance of 0
    /// takes the distance at that moment); the motor and break limits apply as they are. AddComponent&lt;Joint&gt;() while
    /// playing pins the object to the world where it is until it is connected.
    /// </summary>
    public unsafe class Joint : Component
    {
        internal override string NativeName => "Joint";
        private float State(int i) { float* s = stackalloc float[4]; Native.Api.GetJointState(Entity.Scene, Entity.Id, s); return s[i]; }
        private void Rebuild() => Native.Api.RebuildJoint(Entity.Scene, Entity.Id);
        private void Shape(string field, float v) { SetFloat(field, v); Rebuild(); }
        private void Shape(string field, Vector3 v) { SetVector3(field, v); Rebuild(); }

        public JointType Type { get => (JointType)GetInt("Type"); set { SetInt("Type", (int)value); Rebuild(); } }
        /// <summary>The body it holds this one to (default: the world).</summary>
        public Entity ConnectedBody { get => GetEntity("Connected Body"); set { SetEntity("Connected Body", value); Rebuild(); } }
        /// <summary>The pivot, in this object's space.</summary>
        public Vector3 Anchor { get => GetVector3("Anchor"); set => Shape("Anchor", value); }
        /// <summary>Hinge or slider axis, in this object's space.</summary>
        public Vector3 Axis { get => GetVector3("Axis"); set => Shape("Axis", value); }
        public Vector3 ConnectedAnchor { get => GetVector3("Connected Anchor"); set => Shape("Connected Anchor", value); }
        public bool UseLimits { get => GetBool("Use Limits"); set { SetBool("Use Limits", value); Rebuild(); } }
        /// <summary>Hinge limits in degrees.</summary>
        public float MinAngle { get => GetFloat("Min Angle"); set => Shape("Min Angle", value); }
        public float MaxAngle { get => GetFloat("Max Angle"); set => Shape("Max Angle", value); }
        public float MinDistance { get => GetFloat("Min Distance"); set => Shape("Min Distance", value); }
        public float MaxDistance { get => GetFloat("Max Distance"); set => Shape("Max Distance", value); }
        public float SliderMin { get => GetFloat("Slider Min"); set => Shape("Slider Min", value); }
        public float SliderMax { get => GetFloat("Slider Max"); set => Shape("Slider Max", value); }
        /// <summary>Distance joints: above 0, a spring of this many oscillations per second.</summary>
        public float SpringFrequency { get => GetFloat("Spring Frequency"); set => Shape("Spring Frequency", value); }
        public float SpringDamping { get => GetFloat("Spring Damping"); set => Shape("Spring Damping", value); }
        /// <summary>Whether the two connected bodies collide with each other.</summary>
        public bool EnableCollision { get => GetBool("Enable Collision"); set { SetBool("Enable Collision", value); Rebuild(); } }
        public float MotorMaxTorque { get => GetFloat("Motor Max Torque"); set => SetFloat("Motor Max Torque", value); }
        /// <summary>It breaks above this force (N) or torque (N*m); 0 never.</summary>
        public float BreakForce { get => GetFloat("Break Force"); set => SetFloat("Break Force", value); }
        public float BreakTorque { get => GetFloat("Break Torque"); set => SetFloat("Break Torque", value); }
        /// <summary>Makes the joint again as it is set now (mending a broken one), from the bodies' poses now.</summary>
        public void Reconnect() => Rebuild();
        public bool UseMotor
        {
            get => State(0) != 0.0f;
            set => Native.Api.SetJointMotor(Entity.Scene, Entity.Id, value ? 1 : 0, MotorSpeed);
        }
        /// <summary>Degrees per second; positive turns this entity counter-clockwise around the hinge axis.</summary>
        public float MotorSpeed
        {
            get => State(1);
            set => Native.Api.SetJointMotor(Entity.Scene, Entity.Id, UseMotor ? 1 : 0, value);
        }
        public bool IsBroken { get { float* s = stackalloc float[4]; return Native.Api.GetJointState(Entity.Scene, Entity.Id, s) == 1; } }
        /// <summary>Reaction force (N) and torque (N*m) during the last physics step.</summary>
        public float Force => State(2);
        public float Torque => State(3);
    }

    /// <summary>Limits, spring and breaking of a 3D joint (see Joint for the motor).</summary>
    public unsafe class Joint3D : Joint
    {
        public bool UseLimits { get => GetBool("Use Limits"); set => SetBool("Use Limits", value); }
        /// <summary>Hinge limits in degrees.</summary>
        public float MinAngle { get => GetFloat("Min Angle"); set => SetFloat("Min Angle", value); }
        public float MaxAngle { get => GetFloat("Max Angle"); set => SetFloat("Max Angle", value); }
        /// <summary>Distance joint limits (m).</summary>
        public float MinDistance { get => GetFloat("Min Distance"); set => SetFloat("Min Distance", value); }
        public float MaxDistance { get => GetFloat("Max Distance"); set => SetFloat("Max Distance", value); }
        public float SpringFrequency { get => GetFloat("Spring Frequency"); set => SetFloat("Spring Frequency", value); }
        public float SpringDamping { get => GetFloat("Spring Damping"); set => SetFloat("Spring Damping", value); }
        public float MotorMaxTorque { get => GetFloat("Motor Max Torque"); set => SetFloat("Motor Max Torque", value); }
        /// <summary>Force (N) and torque (N*m) at which the joint breaks; 0 = unbreakable.</summary>
        public float BreakForce { get => GetFloat("Break Force"); set => SetFloat("Break Force", value); }
        public float BreakTorque { get => GetFloat("Break Torque"); set => SetFloat("Break Torque", value); }
    }

    /// <summary>Drives a Vehicle (car, motorcycle or tank) and reads its state.</summary>
    public unsafe class Vehicle : Component
    {
        internal override string NativeName => "Vehicle";
        /// <summary>-1 (reverse) to 1 (full throttle).</summary>
        public float Throttle { get => GetFloat("Throttle"); set => SetFloat("Throttle", Math.Clamp(value, -1.0f, 1.0f)); }
        /// <summary>-1 (left) to 1 (right).</summary>
        public float Steering { get => GetFloat("Steering"); set => SetFloat("Steering", Math.Clamp(value, -1.0f, 1.0f)); }
        public float Brake { get => GetFloat("Brake"); set => SetFloat("Brake", Math.Clamp(value, 0.0f, 1.0f)); }
        public float HandBrake { get => GetFloat("Hand Brake"); set => SetFloat("Hand Brake", Math.Clamp(value, 0.0f, 1.0f)); }
        public float EngineTorque { get => GetFloat("Engine Torque"); set => SetFloat("Engine Torque", value); }
        public float MaxSteerAngle { get => GetFloat("Max Steer Angle"); set => SetFloat("Max Steer Angle", value); }
        /// <summary>Speed along the vehicle's forward axis (negative when reversing), from the last physics step.</summary>
        public float SpeedKmh => Native.Api.VehicleState(Entity.Scene, Entity.Id, 0);
        public float EngineRPM => Native.Api.VehicleState(Entity.Scene, Entity.Id, 1);
        /// <summary>0 = neutral, negative = reverse.</summary>
        public int Gear => (int)Native.Api.VehicleState(Entity.Scene, Entity.Id, 2);
        /// <summary>Whether a wheel touches the ground (0 front left, 1 front right, 2 rear left, 3 rear right).</summary>
        public bool IsWheelGrounded(int wheel) => Native.Api.VehicleState(Entity.Scene, Entity.Id, 3 + Math.Clamp(wheel, 0, 3)) != 0.0f;
    }

    /// <summary>A simulated cloth sheet.</summary>
    public unsafe class Cloth : Component
    {
        internal override string NativeName => "Cloth";
        /// <summary>World-space acceleration on the free vertices (m/s^2), applied live (Unity's externalAcceleration).</summary>
        public Vector3 Wind { get => GetVector3("Wind"); set => SetVector3("Wind", value); }
        public Vector3 ExternalAcceleration { get => Wind; set => Wind = value; }
        // These take effect when the cloth is created (play start, or the object turned on).
        public float StretchCompliance { get => GetFloat("Stretch Compliance"); set => SetFloat("Stretch Compliance", value); }
        public float BendCompliance { get => GetFloat("Bend Compliance"); set => SetFloat("Bend Compliance", value); }
        public float Mass { get => GetFloat("Mass"); set => SetFloat("Mass", value); }
        public float Damping { get => GetFloat("Damping"); set => SetFloat("Damping", value); }
        public float Friction { get => GetFloat("Friction"); set => SetFloat("Friction", value); }
        public int Iterations { get => GetInt("Iterations"); set => SetInt("Iterations", value); }
        public float Thickness { get => GetFloat("Thickness"); set => SetFloat("Thickness", value); }
        /// <summary>Edges stretched past (1 + this) times their length tear (0 = never).</summary>
        public float TearStretch { get => GetFloat("Tear Stretch"); set => SetFloat("Tear Stretch", value); }
        /// <summary>The object the pinned vertices hold on to (Null: the cloth's own).</summary>
        public Entity AttachTo { get => GetEntity("Attach To"); set => SetEntity("Attach To", value); }
    }

    /// <summary>
    /// A cube map of the surroundings for reflections (Unity's ReflectionProbe): surfaces inside its box reflect it.
    /// Baked probes capture when shown first and when moved; <see cref="RenderProbe"/> captures again (after the scene
    /// around it changed); Realtime ones every frame.
    /// </summary>
    public unsafe class ReflectionProbe : Component
    {
        internal override string NativeName => "Reflection Probe";
        /// <summary>The influence box (centered on the probe).</summary>
        public Vector3 Size { get => GetVector3("Size"); set => SetVector3("Size", value); }
        public float Intensity { get => GetFloat("Intensity"); set => SetFloat("Intensity", value); }
        public bool BoxProjection { get => GetBool("Box Projection"); set => SetBool("Box Projection", value); }
        public float BlendDistance { get => GetFloat("Blend Distance"); set => SetFloat("Blend Distance", value); }
        public bool Realtime { get => GetBool("Realtime"); set => SetBool("Realtime", value); }
        /// <summary>Pixels per cube face: 64, 128, 256 or 512 (others round to the nearest).</summary>
        public int Resolution
        {
            get => 64 << GetInt("Resolution");
            set => SetInt("Resolution", value <= 96 ? 0 : value <= 192 ? 1 : value <= 384 ? 2 : 3);
        }
        /// <summary>Captures it again this frame (Unity's RenderProbe).</summary>
        public void RenderProbe() => Native.Api.EnvironmentOp(Entity.Scene, Entity.Id, 0, null);
        /// <summary>Whether a world point is inside its box.</summary>
        public bool Contains(Vector3 point)
        {
            Vector3 d = Vector3.Abs(point - Entity.Transform.WorldPosition), half = Vector3.Abs(Size) * 0.5f;
            return d.X <= half.X && d.Y <= half.Y && d.Z <= half.Z;
        }
    }

    /// <summary>
    /// Post-processing for the cameras inside its box, or every camera when Global (Unity's Volume with its overrides),
    /// blended over the scene's settings by Weight and, outside the box, fading over Blend Distance; higher Priority
    /// volumes blend over lower ones, and a camera only sees volumes on the layers it draws. Set a value to override it.
    /// </summary>
    public unsafe class PostProcessVolume : Component
    {
        internal override string NativeName => "Post Process Volume";
        public bool IsGlobal { get => GetBool("Global"); set => SetBool("Global", value); }
        public Vector3 Size { get => GetVector3("Size"); set => SetVector3("Size", value); }
        public float BlendDistance { get => GetFloat("Blend Distance"); set => SetFloat("Blend Distance", value); }
        public float Weight { get => GetFloat("Weight"); set => SetFloat("Weight", value); }
        public float Priority { get => GetFloat("Priority"); set => SetFloat("Priority", value); }
        /// <summary>Overridden values (null: the volume leaves it as it is).</summary>
        public float? Exposure { get => Get("Exposure", "Exposure"); set => Set("Exposure", "Exposure", value); }
        public float? BloomIntensity { get => Get("Bloom", "Bloom Intensity"); set => Set("Bloom", "Bloom Intensity", value); }
        public float? Vignette { get => Get("Vignette", "Vignette"); set => Set("Vignette", "Vignette", value); }
        public float? Saturation { get => Get("Saturation", "Saturation"); set => Set("Saturation", "Saturation", value); }
        public float? Contrast { get => Get("Contrast", "Contrast"); set => Set("Contrast", "Contrast", value); }
        public Color? ColorFilter
        {
            get { if (!GetBool("Override Color Filter")) return null; Vector3 c = GetVector3("Color Filter"); return new Color(c.X, c.Y, c.Z); }
            set
            {
                SetBool("Override Color Filter", value.HasValue);
                if (value is { } c) SetVector3("Color Filter", new Vector3(c.R, c.G, c.B));
            }
        }
        private float? Get(string flag, string field) => GetBool("Override " + flag) ? GetFloat(field) : null;
        private void Set(string flag, string field, float? value)
        {
            SetBool("Override " + flag, value.HasValue);
            if (value is { } v) SetFloat(field, v);
        }
    }

    /// <summary>9 spherical-harmonic coefficients of light from every direction (Unity's SphericalHarmonicsL2), as light
    /// probes hold them.</summary>
    public unsafe struct SphericalHarmonicsL2
    {
        internal fixed float Data[27];
        /// <summary>Coefficient `i` (0-8) as RGB.</summary>
        public Vector3 this[int i]
        {
            get => new(Data[i * 3], Data[i * 3 + 1], Data[i * 3 + 2]);
            set { Data[i * 3] = value.X; Data[i * 3 + 1] = value.Y; Data[i * 3 + 2] = value.Z; }
        }
        /// <summary>The light reaching a surface facing `normal` (irradiance over pi: an even environment of color c
        /// gives c).</summary>
        public Color Evaluate(Vector3 normal)
        {
            float* v = stackalloc float[30];
            for (int i = 0; i < 27; ++i) v[i] = Data[i];
            v[27] = normal.X; v[28] = normal.Y; v[29] = normal.Z;
            Native.Api.EnvironmentOp(IntPtr.Zero, 0, 3, v);
            return new Color(v[0], v[1], v[2], 1.0f);
        }
    }

    /// <summary>The baked light probes of the scene's Light Probe Volumes (Unity's LightProbes).</summary>
    public static unsafe class LightProbes
    {
        /// <summary>The probe light at a world point (the smallest baked volume holding it, blended between its probes;
        /// Intensity applied). False outside every baked volume.</summary>
        public static bool GetInterpolatedProbe(Vector3 position, out SphericalHarmonicsL2 probe) => GetInterpolatedProbe(position, out probe, out _);
        /// <summary>As above, with the volume's weight there (it fades to 0 over its Blend Distance at the walls).</summary>
        public static bool GetInterpolatedProbe(Vector3 position, out SphericalHarmonicsL2 probe, out float weight)
        {
            float* v = stackalloc float[30];
            v[0] = position.X; v[1] = position.Y; v[2] = position.Z;
            bool inside = Native.Api.EnvironmentOp(Native.CurrentScene, 0, 1, v) != 0;
            probe = default;
            for (int i = 0; i < 27; ++i) probe.Data[i] = v[i];
            weight = v[27];
            return inside;
        }
        /// <summary>The probe light reaching a surface at a point facing `normal` (black outside the volumes).</summary>
        public static Color GetLight(Vector3 position, Vector3 normal) =>
            GetInterpolatedProbe(position, out var sh) ? sh.Evaluate(normal) : new Color(0, 0, 0, 1);
    }

    public enum WindZoneMode { Directional, Spherical }

    /// <summary>
    /// Wind (Unity's WindZone): Directional blows along the object's forward everywhere, Spherical out from it within
    /// Radius; Main is the speed, with Turbulence swirls and Pulse gusts. It moves particles, cloth, trees and grass,
    /// and bodies with wind drag. <see cref="GetWindAt"/> gives the wind of every zone at a point.
    /// </summary>
    public unsafe class WindZone : Component
    {
        internal override string NativeName => "Wind Zone";
        public WindZoneMode Mode { get => (WindZoneMode)GetInt("Mode"); set => SetInt("Mode", (int)value); }
        public float Radius { get => GetFloat("Radius"); set => SetFloat("Radius", value); }
        /// <summary>Wind speed, m/s (Unity's windMain).</summary>
        public float Main { get => GetFloat("Main"); set => SetFloat("Main", value); }
        public float Turbulence { get => GetFloat("Turbulence"); set => SetFloat("Turbulence", value); }
        public float PulseMagnitude { get => GetFloat("Pulse Magnitude"); set => SetFloat("Pulse Magnitude", value); }
        public float PulseFrequency { get => GetFloat("Pulse Frequency"); set => SetFloat("Pulse Frequency", value); }
        /// <summary>The wind velocity (m/s) at a world point now, from all the scene's zones.</summary>
        public static Vector3 GetWindAt(Vector3 point)
        {
            float* v = stackalloc float[3] { point.X, point.Y, point.Z };
            Native.Api.EnvironmentOp(Native.CurrentScene, 0, 2, v);
            return new Vector3(v[0], v[1], v[2]);
        }
    }

    /// <summary>What a Timeline does at its end without Loop (Unity's DirectorWrapMode): Hold the last frame, Loop, or
    /// None (stop and go back to the start).</summary>
    public enum TimelineWrapMode { Hold, Loop, None }

    /// <summary>
    /// Plays a timeline asset (Unity's PlayableDirector). Play starts from the beginning, Pause holds it, Resume goes
    /// on, Stop goes back to the start (handing Animators back); Played / Paused / Stopped tell listeners (Stopped also
    /// at its end). Tracks drive the objects their paths name under this one unless bound to others
    /// (<see cref="SetGenericBinding"/>). Timeline events call the method of that name on the object's scripts.
    /// </summary>
    public unsafe class Timeline : Component
    {
        internal override string NativeName => "Timeline";
        private float Control(int op, float value = 0.0f) => Native.Api.TimelineControl(Entity.Scene, Entity.Id, op, value);
        public void Play() => Control(0);
        /// <summary>Stops and goes back to the start (Animators the timeline drove go back to their own playback).</summary>
        public void Stop() => Control(1);
        /// <summary>Holds it where it is (Resume goes on from there).</summary>
        public void Pause() => Control(5);
        public void Resume() => Control(6);
        public bool IsPlaying => Control(4) != 0.0f;
        public bool IsPaused => Control(7) != 0.0f;
        /// <summary>The asset's length in seconds.</summary>
        public float Duration => Control(8);
        /// <summary>Writes every track's values at <see cref="Time"/> now (after setting Time while not playing).</summary>
        public void Evaluate() => Control(9);
        /// <summary>The asset it plays; setting another one starts it over at 0 (bindings cleared).</summary>
        public TimelineAsset Asset
        {
            get => new(GetAssetGuid("Timeline"));
            set
            {
                ulong before = GetAssetGuid("Timeline");
                if (before == value.Guid) return;
                SetAssetGuid("Timeline", value.Guid);
                Native.Api.TimelineAssetChanged(Entity.Scene, Entity.Id, before);
            }
        }
        public TimelineWrapMode WrapMode
        {
            get => Loop ? TimelineWrapMode.Loop : GetBool("Reset On End") ? TimelineWrapMode.None : TimelineWrapMode.Hold;
            set { Loop = value == TimelineWrapMode.Loop; SetBool("Reset On End", value == TimelineWrapMode.None); }
        }
        /// <summary>Tracks in its asset.</summary>
        public int TrackCount => (int)Control(10);
        /// <summary>A track's kind, path and (property tracks) component and field, e.g. "Property Door Transform.Position".</summary>
        public string GetTrackName(int track)
        {
            byte* text = null;
            Native.Api.TimelineTrack(Entity.Scene, Entity.Id, 2, track, 0, &text);
            return text != null ? Native.FromUtf8(text) : "";
        }
        /// <summary>Has a track drive `target` instead of the object its path names (Unity's SetGenericBinding: one
        /// cutscene for different actors). Entity.Null goes back to the path.</summary>
        public void SetGenericBinding(int track, Entity target) =>
            Native.Api.TimelineTrack(Entity.Scene, Entity.Id, 1, track, target.IsValid ? target.Id : ulong.MaxValue, null);
        /// <summary>The object a track is bound to (Null: it follows its path).</summary>
        public Entity GetGenericBinding(int track)
        {
            ulong id = Native.Api.TimelineTrack(Entity.Scene, Entity.Id, 0, track, 0, null);
            return id == ulong.MaxValue ? Entity.Null : new Entity(Entity.Scene, id);
        }
        public event Action Played
        {
            add => EngineEvents.Add(Entity, EngineEvents.Kind.TimelinePlayed, value);
            remove => EngineEvents.Remove(Entity, EngineEvents.Kind.TimelinePlayed, value);
        }
        public event Action Paused
        {
            add => EngineEvents.Add(Entity, EngineEvents.Kind.TimelinePaused, value);
            remove => EngineEvents.Remove(Entity, EngineEvents.Kind.TimelinePaused, value);
        }
        /// <summary>Stop() was called, or it reached its end (without Loop).</summary>
        public event Action Stopped
        {
            add => EngineEvents.Add(Entity, EngineEvents.Kind.TimelineStopped, value);
            remove => EngineEvents.Remove(Entity, EngineEvents.Kind.TimelineStopped, value);
        }
        /// <summary>Seconds into the timeline; setting it jumps there.</summary>
        public float Time
        {
            get => Native.Api.TimelineControl(Entity.Scene, Entity.Id, 2, 0.0f);
            set => Native.Api.TimelineControl(Entity.Scene, Entity.Id, 3, value);
        }
        public float Speed { get => GetFloat("Speed"); set => SetFloat("Speed", value); }
        public bool Loop { get => GetBool("Loop"); set => SetBool("Loop", value); }
    }

    /// <summary>Ragdoll: capsules per bone that follow the animation, or go limp under physics with Simulate.</summary>
    public unsafe class Ragdoll : Component
    {
        internal override string NativeName => "Ragdoll";
        /// <summary>On: limp under physics (the object follows it). Off: the capsules follow the animation.</summary>
        public bool Simulate { get => GetBool("Simulate"); set => SetBool("Simulate", value); }
        /// <summary>Knocks the capsule nearest `point` (world space), e.g. where a raycast hit it.</summary>
        public void AddImpulse(Vector3 impulse, Vector3 point) => Native.Api.RagdollImpulse(Entity.Scene, Entity.Id, (float*)&impulse, (float*)&point);
        /// <summary>Sets the bone that Simulate makes go limp, with the bones under it (null or "" = the whole body),
        /// e.g. the arm a shot hit. Takes effect when Simulate next turns on.</summary>
        public void SetPartialRoot(string bone) { var e = Entity; Native.WithUtf8(bone, p => Native.Api.RagdollSetPartial(e.Scene, e.Id, (byte*)p)); }
        /// <summary>Seconds the animation blends in from the limp pose when Simulate turns off.</summary>
        public float GetUpTime { get => GetFloat("Get Up Time"); set => SetFloat("Get Up Time", value); }
        /// <summary>Lying on its back (pick a get-up animation by it before turning Simulate off).</summary>
        public bool IsFaceUp => Native.Api.RagdollQuery(Entity.Scene, Entity.Id, 0) > 0.5f;
        /// <summary>How far the blend back into the animation is, 0-1 (1 when done or not getting up).</summary>
        public float GetUpProgress => Native.Api.RagdollQuery(Entity.Scene, Entity.Id, 1);
    }

    /// <summary>Tow Hitch: hangs this object (a trailer) on another by a ball joint.</summary>
    public class TowHitch : Component
    {
        internal override string NativeName => "Tow Hitch";
        /// <summary>Clear to unhitch; set to hitch again where it stands.</summary>
        public bool Attached { get => GetBool("Attached"); set => SetBool("Attached", value); }
        public float BreakForce { get => GetFloat("Break Force"); set => SetFloat("Break Force", value); }
    }

    /// <summary>Boat: an engine, rudder and keel for a body floating in a Water Volume.</summary>
    public class Boat : Component
    {
        internal override string NativeName => "Boat";
        /// <summary>-1 (reverse) to 1.</summary>
        public float Throttle { get => GetFloat("Throttle"); set => SetFloat("Throttle", value); }
        /// <summary>-1 (left) to 1 (right).</summary>
        public float Steering { get => GetFloat("Steering"); set => SetFloat("Steering", value); }
    }

    /// <summary>Weather: falling rain and snow around the camera, wet and snowy surfaces (fade them from scripts).</summary>
    public class Weather : Component
    {
        internal override string NativeName => "Weather";
        public float Rain { get => GetFloat("Rain"); set => SetFloat("Rain", value); }
        public float Snowfall { get => GetFloat("Snowfall"); set => SetFloat("Snowfall", value); }
        public float Wetness { get => GetFloat("Wetness"); set => SetFloat("Wetness", value); }
        public float SnowCover { get => GetFloat("Snow Cover"); set => SetFloat("Snow Cover", value); }
    }

    /// <summary>Breakable: breaks into pieces on a hard enough hit, or when Break is called.</summary>
    public unsafe class Breakable : Component
    {
        internal override string NativeName => "Breakable";
        /// <summary>Impact speed (m/s) that breaks it; 0 = only Break().</summary>
        public float BreakSpeed { get => GetFloat("Break Speed"); set => SetFloat("Break Speed", value); }
        public int Pieces { get => GetInt("Pieces"); set => SetInt("Pieces", value); }
        /// <summary>Breaks it now, the pieces flying apart from `point` (world space). Returns how many pieces.</summary>
        public int Break(Vector3 point) => Native.Api.BreakableBreak(Entity.Scene, Entity.Id, (float*)&point);
        /// <summary>Breaks it now from its center.</summary>
        public int Break() => Break(Entity.Transform.Position);
    }

    /// <summary>A pressurized soft body made from the object's mesh.</summary>
    public unsafe class SoftBody : Component
    {
        internal override string NativeName => "Soft Body";
        /// <summary>Inflation; applied live (0 = limp).</summary>
        public float Pressure { get => GetFloat("Pressure"); set => SetFloat("Pressure", value); }
    }

    public unsafe class UIDropdown : Component
    {
        internal override string NativeName => "UI Dropdown";
        /// <summary>Index of the selected option.</summary>
        public int Value
        {
            get => (int)Native.Api.GetUIValue(Entity.Scene, Entity.Id);
            set => Native.Api.SetUIValue(Entity.Scene, Entity.Id, value);
        }
        /// <summary>Label of the selected option.</summary>
        public string SelectedText => Native.FromUtf8(Native.Api.GetUIText(Entity.Scene, Entity.Id));
        /// <summary>Raised with the new option index when the user picks an option.</summary>
        public event Action<int> ValueChanged
        {
            add => EngineEvents.Add(Entity, EngineEvents.Kind.ValueChanged, value);
            remove => EngineEvents.Remove(Entity, EngineEvents.Kind.ValueChanged, value);
        }
        public string[] Options
        {
            get
            {
                string joined = Native.FromUtf8(Native.Api.DropdownGetOptions(Entity.Scene, Entity.Id));
                return string.IsNullOrEmpty(joined) ? Array.Empty<string>() : joined.Split('\n');
            }
            set { var e = Entity; Native.WithUtf8(string.Join("\n", value ?? Array.Empty<string>()), p => Native.Api.DropdownSetOptions(e.Scene, e.Id, (byte*)p)); }
        }
    }

    public unsafe class Animator : Component
    {
        internal override string NativeName => "Animator";
        public void CrossFade(string clip, float seconds = 0.25f)
        {
            var e = Entity;
            Native.WithUtf8(clip, p => Native.Api.AnimatorCrossFade(e.Scene, e.Id, (byte*)p, seconds));
        }
        public void Play(string clip) => CrossFade(clip, 0.0f);

        // State machine parameters (Animator Controller).
        private void SetParameter(string name, float value, int kind)
        {
            var e = Entity;
            Native.WithUtf8(name, p => Native.Api.AnimatorSetParameter(e.Scene, e.Id, (byte*)p, value, kind));
        }
        // (These take the controller's parameters; Component's GetFloat / SetFloat of Inspector fields are hidden here.)
        public new void SetFloat(string name, float value) => SetParameter(name, value, 0);
        public void SetInteger(string name, int value) => SetParameter(name, value, 1);
        public new void SetBool(string name, bool value) => SetParameter(name, value ? 1 : 0, 2);
        public void SetTrigger(string name) => SetParameter(name, 1, 3);
        /// <summary>Clears a trigger that was set but has not fired a transition yet.</summary>
        public void ResetTrigger(string name) => Query(1, 0, name);

        // Ops: 0 parameter value, 1 reset trigger, 2 normalized time, 3 transition progress (-1 none), 4 speed, 5 set it,
        // 6 layer count, 7 layer weight, 8 set it, 9 has parameter, 10 layer index by name (-1 none).
        private float Query(int op, int layer, string name = null, float value = 0.0f)
        {
            var e = Entity;
            return Native.WithUtf8<float>(name ?? "", p => Native.Api.AnimatorQuery(e.Scene, e.Id, op, layer, (byte*)p, value));
        }
        private string StateName(int layer, int which) => Native.FromUtf8(Native.Api.AnimatorStateName(Entity.Scene, Entity.Id, layer, which));

        /// <summary>A parameter's value: what was set, else the controller's default (a trigger: true until it fires).</summary>
        public new float GetFloat(string name) => Query(0, 0, name);
        public int GetInteger(string name) => (int)MathF.Round(Query(0, 0, name));
        public new bool GetBool(string name) => Query(0, 0, name) != 0.0f;
        public bool HasParameter(string name) => Query(9, 0, name) != 0.0f;

        /// <summary>Name of the state machine's current state ("" without a controller).</summary>
        public string CurrentState => Native.FromUtf8(Native.Api.AnimatorGetState(Entity.Scene, Entity.Id));
        /// <summary>A layer's current state (0 = the base layer).</summary>
        public string GetCurrentState(int layer) => StateName(layer, 0);
        /// <summary>How far the current state has played: 0.5 halfway through, 2.3 in its third loop (as Unity's
        /// normalizedTime). Without a controller, the clip's position (0-1).</summary>
        public float NormalizedTime => Query(2, 0);
        public float GetNormalizedTime(int layer) => Query(2, layer);
        /// <summary>Whether the layer is crossfading into its current state.</summary>
        public bool IsInTransition(int layer = 0) => Query(3, layer) >= 0.0f;
        /// <summary>The crossfade's progress (0-1), or -1 when not in one.</summary>
        public float TransitionProgress(int layer = 0) => Query(3, layer);
        /// <summary>The state fading out while in a transition ("" otherwise).</summary>
        public string PreviousState(int layer = 0) => StateName(layer, 1);

        /// <summary>Playback speed of every layer (1 = normal, 0 = frozen).</summary>
        public float Speed { get => Query(4, 0); set => Query(5, 0, null, value); }
        /// <summary>Layers in the controller, the base layer included.</summary>
        public int LayerCount => (int)Query(6, 0);
        public string GetLayerName(int layer) => StateName(layer, 2);
        /// <summary>A layer's index by name ("Base Layer" is 0), or -1.</summary>
        public int GetLayerIndex(string name) => (int)Query(10, 0, name);
        public float GetLayerWeight(int layer) => Query(7, layer);
        /// <summary>Sets an extra layer's weight (0-1; the base layer's is always 1), at once.</summary>
        public void SetLayerWeight(int layer, float weight) => Query(8, layer, null, weight);

        private int Op(int op, int layer, string name, float* v)
        {
            var e = Entity;
            return Native.WithUtf8<int>(name ?? "", p => Native.Api.AnimatorOp(e.Scene, e.Id, op, layer, (byte*)p, v));
        }
        private bool PlayState(string state, int layer, float fadeSeconds, float offset, bool fixedTime)
        {
            float* v = stackalloc float[3] { fadeSeconds, offset, fixedTime ? 1 : 0 };
            return Op(0, layer, state, v) != 0;
        }
        /// <summary>
        /// Plays a state of the controller on a layer (without a controller: a clip of the model) at once, from
        /// `normalizedTime` into it (0.5: halfway). The state machine goes on from there (its transitions still apply).
        /// False when there is no such state.
        /// </summary>
        public bool Play(string state, int layer, float normalizedTime = 0.0f) => PlayState(state, layer, 0.0f, normalizedTime, false);
        /// <summary>
        /// Fades into a state of the controller (or a clip without one) over `seconds` (this engine's CrossFade takes
        /// seconds, as CrossFadeInFixedTime does, not Unity's fraction of the state), starting `normalizedTimeOffset`
        /// into it. False when there is no such state.
        /// </summary>
        public bool CrossFade(string state, float seconds, int layer, float normalizedTimeOffset = 0.0f) => PlayState(state, layer, seconds, normalizedTimeOffset, false);
        /// <summary>As CrossFade, with the start given in seconds into the state.</summary>
        public bool CrossFadeInFixedTime(string state, float seconds, int layer = 0, float fixedTimeOffset = 0.0f) => PlayState(state, layer, seconds, fixedTimeOffset, true);
        /// <summary>As Play, with the start given in seconds into the state.</summary>
        public bool PlayInFixedTime(string state, int layer = 0, float fixedTime = 0.0f) => PlayState(state, layer, 0.0f, fixedTime, true);

        private AnimatorStateInfo Info(int layer, int which)
        {
            float* v = stackalloc float[4] { which, 0, 0, 0 };
            if (Op(1, layer, null, v) == 0) return default;
            bool fading = IsInTransition(layer);
            string name = which == 0 ? (fading ? PreviousState(layer) : (layer == 0 ? CurrentState : GetCurrentState(layer))) : (layer == 0 ? CurrentState : GetCurrentState(layer));
            string tag = Native.FromUtf8(Native.Api.AnimatorStateName(Entity.Scene, Entity.Id, layer, which == 0 ? 3 : 4));
            return new AnimatorStateInfo(name, v[0], v[1], v[2], v[3] != 0.0f, tag);
        }
        /// <summary>The layer's state as of this frame's update (Unity's view: while fading, the state fading out; the
        /// one coming in is <see cref="GetNextAnimatorStateInfo"/>).</summary>
        public AnimatorStateInfo GetCurrentAnimatorStateInfo(int layer = 0) => Info(layer, 0);
        /// <summary>The state being faded into (only while in a transition; otherwise IsValid is false).</summary>
        public AnimatorStateInfo GetNextAnimatorStateInfo(int layer = 0) => Info(layer, 1);

        /// <summary>The root joint's movement moves the object (or its Character Controller).</summary>
        public bool ApplyRootMotion { get => GetBool("Apply Root Motion"); set => SetBool("Apply Root Motion", value); }
        /// <summary>This frame's root motion (world space), in OnAnimatorMove or after the update.</summary>
        public Vector3 DeltaPosition { get { float* v = stackalloc float[7]; Op(2, 0, null, v); return new Vector3(v[0], v[1], v[2]); } }
        /// <summary>This frame's root turn (world space; with Apply Root Rotation, about the object's up axis).</summary>
        public Quaternion DeltaRotation { get { float* v = stackalloc float[7]; Op(2, 0, null, v); return new Quaternion(v[3], v[4], v[5], v[6]); } }
        /// <summary>The root's turning (about the vertical axis) turns the object, with root motion.</summary>
        public bool ApplyRootRotation { get => GetBool("Apply Root Rotation"); set => SetBool("Apply Root Rotation", value); }
        /// <summary>Moves and turns the object by this frame's root motion as the Animator would (for OnAnimatorMove).</summary>
        public void ApplyBuiltinRootMotion() => Move(DeltaPosition, DeltaRotation);
        /// <summary>Moves the object by a world-space step the way root motion does (through its Character Controller),
        /// turning it by `worldTurn` first.</summary>
        public void Move(Vector3 worldDelta, Quaternion? worldTurn = null)
        {
            Quaternion q = worldTurn ?? Quaternion.Identity;
            float* v = stackalloc float[7] { worldDelta.X, worldDelta.Y, worldDelta.Z, q.X, q.Y, q.Z, q.W };
            Op(3, 0, null, v);
        }
        /// <summary>When it advances: with game time, real time (on while paused), the physics steps, or only by
        /// <see cref="Update"/> (Manual).</summary>
        public AnimatorUpdateMode UpdateMode { get => (AnimatorUpdateMode)GetInt("Update Mode"); set => SetInt("Update Mode", (int)value); }
        /// <summary>Advances this Animator by `deltaTime` seconds now (its state machine, pose, events and behaviours),
        /// whatever its update mode: with Manual, a script drives its clock.</summary>
        public void Update(float deltaTime) { float* v = stackalloc float[1] { deltaTime }; Op(5, 0, null, v); }

        /// <summary>Whether it animates off screen: always, or not at all (Cull Completely).</summary>
        public AnimatorCullingMode CullingMode { get => (AnimatorCullingMode)GetInt("Culling Mode"); set => SetInt("Culling Mode", (int)value); }
        /// <summary>Off screen and not animated this frame (Cull Completely).</summary>
        public bool IsCulled { get { float* v = stackalloc float[1]; return Op(10, 0, null, v) != 0; } }
        /// <summary>
        /// Brings a joint (by name: a hand or foot) to `position` at `targetNormalizedTime` of the base layer's current
        /// state, moving the object a little each frame from `startNormalizedTime` (Unity's MatchTarget, for jumps and
        /// climbs that must land on a ledge). `positionWeight` per axis: 0 leaves that axis to the animation. It ends
        /// at the target time or when the state changes.
        /// </summary>
        public void MatchTarget(Vector3 position, string joint, float startNormalizedTime, float targetNormalizedTime, Vector3? positionWeight = null)
        {
            Vector3 w = positionWeight ?? Vector3.One;
            float* v = stackalloc float[8] { position.X, position.Y, position.Z, w.X, w.Y, w.Z, startNormalizedTime, targetNormalizedTime };
            Op(6, 0, joint, v);
        }
        /// <summary>Stops matching; with `completeMatch`, the object is moved the rest of the way at the next update.</summary>
        public void InterruptMatchTarget(bool completeMatch = true) { float* v = stackalloc float[1] { completeMatch ? 1 : 0 }; Op(7, 0, null, v); }
        public bool IsMatchingTarget { get { float* v = stackalloc float[1]; return Op(8, 0, null, v) != 0; } }
        /// <summary>Back to the start: the default state from its beginning, parameters at their defaults.</summary>
        public void Rebind() { float* v = stackalloc float[1]; Op(9, 0, null, v); }

        /// <summary>The controller it runs (an Animator Controller or an Override Controller). Setting another one starts
        /// it over (Rebind) with that one's state machine and clips; an override of the same controller keeps the state
        /// and its time, swapping only the clips.</summary>
        public AnimatorController RuntimeAnimatorController
        {
            get => new(GetAssetGuid("Controller"));
            set
            {
                ulong before = GetAssetGuid("Controller");
                if (before == value.Guid) return;
                SetAssetGuid("Controller", value.Guid);
                float* v = stackalloc float[2];
                *(ulong*)v = before;
                if (Op(11, 0, null, v) == 0) Rebind();
            }
        }

        /// <summary>A joint of the model's skeleton (by name, e.g. "Hand_R") in the world as last posed, or false when
        /// there is no such joint (or nothing posed yet). Unity's GetBoneTransform, for any joint by name.</summary>
        public bool GetBoneMatrix(string joint, out Matrix4x4 world)
        {
            float* v = stackalloc float[16];
            world = Matrix4x4.Identity;
            if (Op(4, 0, joint, v) == 0) return false;
            // glm stores columns; System.Numerics' rows are those columns (row vectors), so the floats map one to one.
            world = new Matrix4x4(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
            return true;
        }
        public Vector3 GetBonePosition(string joint) => GetBoneMatrix(joint, out Matrix4x4 m) ? m.Translation : Vector3.Zero;
        public Quaternion GetBoneRotation(string joint)
        {
            if (!GetBoneMatrix(joint, out Matrix4x4 m) || !Matrix4x4.Decompose(m, out _, out Quaternion r, out _)) return Quaternion.Identity;
            return r;
        }
    }

    /// <summary>
    /// A script on Animator states (Unity's StateMachineBehaviour): add its class to a state's Behaviours in the
    /// Animator window. Each object with that Animator gets its own copy per state and layer. OnStateEnter runs when the
    /// state is entered, OnStateUpdate every later frame it plays (also while fading out), OnStateExit when its fade out
    /// ends, all after the frame's animation update.
    /// </summary>
    public abstract class StateMachineBehaviour
    {
        public virtual void OnStateEnter(Animator animator, AnimatorStateInfo stateInfo, int layerIndex) { }
        public virtual void OnStateUpdate(Animator animator, AnimatorStateInfo stateInfo, int layerIndex) { }
        public virtual void OnStateExit(Animator animator, AnimatorStateInfo stateInfo, int layerIndex) { }
    }

    public enum AnimatorUpdateMode { Normal, UnscaledTime, AnimatePhysics, Manual }
    public enum AnimatorCullingMode { AlwaysAnimate, CullCompletely }

    /// <summary>An Animator layer's state as of this frame (Unity's AnimatorStateInfo).</summary>
    public readonly record struct AnimatorStateInfo(string Name, float NormalizedTime, float Length, float SpeedMultiplier, bool Loop, string Tag = "")
    {
        /// <summary>Whether the state's Tag (set in the Animator window) is `tag`.</summary>
        public bool IsTag(string tag) => (Tag ?? "") == tag;
        /// <summary>False when there is no such state (no controller or clip; GetNextAnimatorStateInfo outside a transition).</summary>
        public bool IsValid => Name != null;
        public bool IsName(string name) => Name == name;
        /// <summary>Seconds into the current loop.</summary>
        public float Time => Length * (Loop ? NormalizedTime - MathF.Floor(NormalizedTime) : MathF.Min(NormalizedTime, 1.0f));
    }

    public unsafe class ParticleSystem : Component
    {
        internal override string NativeName => "Particle System";
        public void Emit(int count) => Native.Api.ParticlesEmit(Entity.Scene, Entity.Id, count);
        /// <summary>Emits `count` particles with some start values of their own (the rest are the system's), this frame.
        /// GPU systems take only the position.</summary>
        public void Emit(EmitParams p, int count = 1)
        {
            float* v = stackalloc float[12];
            int flags = 0;
            if (p.Position is Vector3 at) { v[0] = at.X; v[1] = at.Y; v[2] = at.Z; flags |= 1; }
            if (p.Velocity is Vector3 vel) { v[3] = vel.X; v[4] = vel.Y; v[5] = vel.Z; flags |= 2; }
            if (p.Color is Color c) { v[6] = c.R; v[7] = c.G; v[8] = c.B; v[9] = c.A; flags |= 4; }
            if (p.Size is float size) { v[10] = size; flags |= 8; }
            if (p.Lifetime is float life) { v[11] = life; flags |= 16; }
            Native.Api.ParticlesEmitWith(Entity.Scene, Entity.Id, v, flags, count);
        }
        /// <summary>Starts over, or goes on after <see cref="Pause"/>.</summary>
        public void Play() => Native.Api.ParticlesPlay(Entity.Scene, Entity.Id, 1);
        /// <summary>Stops emitting; the particles already out finish their lives.</summary>
        public void Stop() => Native.Api.ParticlesPlay(Entity.Scene, Entity.Id, 0);
        /// <summary>Stops emitting and removes every particle at once.</summary>
        public void Stop(bool clear) { Stop(); if (clear) Clear(); }

        // Ops: 0 pause, 1 is paused, 2 is playing, 3 is alive, 4 particle count, 5 clear, 6 time.
        private float Control(int op) => Native.Api.ParticlesControl(Entity.Scene, Entity.Id, op);
        /// <summary>Freezes it: its particles hang still and it emits nothing until Play.</summary>
        public void Pause() => Control(0);
        /// <summary>Removes every particle (it keeps playing, if it was).</summary>
        public void Clear() => Control(5);
        public bool IsPaused => Control(1) != 0.0f;
        /// <summary>Emitting (or paused while it was).</summary>
        public bool IsPlaying => Control(2) != 0.0f;
        /// <summary>Emitting, or still has particles out.</summary>
        public bool IsAlive => Control(3) != 0.0f;
        /// <summary>Particles out now (CPU systems; a GPU system's live on the GPU, so it reads 0).</summary>
        public int ParticleCount => (int)Control(4);
        /// <summary>Seconds into the current loop of Duration.</summary>
        public float Time => Control(6);

        public float Duration { get => GetFloat("Duration"); set => SetFloat("Duration", value); }
        public bool Looping { get => GetBool("Looping"); set => SetBool("Looping", value); }
        /// <summary>Particles per second while playing.</summary>
        public float EmissionRate { get => GetFloat("Rate Over Time"); set => SetFloat("Rate Over Time", value); }
        /// <summary>Emitted at once when it starts (and each loop).</summary>
        public int BurstCount { get => GetInt("Burst Count"); set => SetInt("Burst Count", value); }
        public int MaxParticles { get => GetInt("Max Particles"); set => SetInt("Max Particles", value); }
        /// <summary>Each particle picks a start lifetime (seconds) between X and Y; the same in both for a fixed one.</summary>
        public Vector2 StartLifetime { get => GetVector2("Start Lifetime"); set => SetVector2("Start Lifetime", value); }
        /// <summary>Start speed range (units per second).</summary>
        public Vector2 StartSpeed { get => GetVector2("Start Speed"); set => SetVector2("Start Speed", value); }
        /// <summary>Start size range (world units).</summary>
        public Vector2 StartSize { get => GetVector2("Start Size"); set => SetVector2("Start Size", value); }
        /// <summary>Each particle starts with a color between StartColorA and StartColorB; setting StartColor sets both.</summary>
        public Color StartColor { get => StartColorA; set { StartColorA = value; StartColorB = value; } }
        public Color StartColorA { get => ToColor(GetVector4("Start Color A")); set => SetVector4("Start Color A", ToVector(value)); }
        public Color StartColorB { get => ToColor(GetVector4("Start Color B")); set => SetVector4("Start Color B", ToVector(value)); }
        /// <summary>Multiples of gravity (9.81 m/s^2) pulling the particles down.</summary>
        public float GravityModifier { get => GetFloat("Gravity Modifier"); set => SetFloat("Gravity Modifier", value); }
        public bool SimulateOnGpu => GetBool("Simulate On GPU");
        private static Color ToColor(Vector4 v) => new(v.X, v.Y, v.Z, v.W);
        private static Vector4 ToVector(Color c) => new(c.R, c.G, c.B, c.A);
    }

    /// <summary>Start values for <see cref="ParticleSystem.Emit(EmitParams, int)"/>; the ones left null are the system's.</summary>
    public struct EmitParams
    {
        /// <summary>Where the particle starts, in world space (null: from the system's shape).</summary>
        public Vector3? Position;
        /// <summary>World-space velocity (null: the shape's direction at the start speed).</summary>
        public Vector3? Velocity;
        public Color? Color;
        public float? Size;
        /// <summary>Seconds.</summary>
        public float? Lifetime;
    }

    public unsafe class LineRenderer : Component
    {
        internal override string NativeName => "Line Renderer";

        public int PositionCount => Native.Api.LineGetPositions(Entity.Scene, Entity.Id, null, 0);

        public void SetPositions(Vector3[] positions)
        {
            positions ??= Array.Empty<Vector3>();
            fixed (Vector3* p = positions) Native.Api.LineSetPositions(Entity.Scene, Entity.Id, (float*)p, positions.Length);
        }

        public Vector3[] GetPositions()
        {
            var result = new Vector3[PositionCount];
            fixed (Vector3* p = result) Native.Api.LineGetPositions(Entity.Scene, Entity.Id, (float*)p, result.Length);
            return result;
        }

        public void SetPosition(int index, Vector3 position)
        {
            var positions = GetPositions();
            if (index < 0 || index >= positions.Length) throw new ArgumentOutOfRangeException(nameof(index));
            positions[index] = position;
            SetPositions(positions);
        }

        public Vector3 GetPosition(int index) => GetPositions()[index];
    }

    public unsafe class TrailRenderer : Component
    {
        internal override string NativeName => "Trail Renderer";
        public void Clear() => Native.Api.TrailClear(Entity.Scene, Entity.Id);
        public bool Emitting
        {
            get => Native.Api.TrailIsEmitting(Entity.Scene, Entity.Id) != 0;
            set => Native.Api.TrailSetEmitting(Entity.Scene, Entity.Id, value ? 1 : 0);
        }
    }

    public unsafe class AudioSource : Component
    {
        internal override string NativeName => "Audio Source";
        public void Play() => Native.Api.AudioPlay(Entity.Scene, Entity.Id, 1);
        public void Stop() => Native.Api.AudioPlay(Entity.Scene, Entity.Id, 0);
        public bool IsPlaying => Native.Api.AudioIsPlaying(Entity.Scene, Entity.Id) != 0;

        // Ops: 0 pause, 1 unpause, 2 is paused, 3 time, 4 seek, 5 length, 6 set loop.
        private float Control(int op, float value = 0.0f) => Native.Api.AudioControl(Entity.Scene, Entity.Id, op, value);
        /// <summary>Stops where it is: UnPause goes on from there (Play starts over, Stop forgets the position).</summary>
        public void Pause() => Control(0);
        public void UnPause() => Control(1);
        public bool IsPaused => Control(2) != 0.0f;
        /// <summary>The playing position in seconds; setting it seeks (also while paused or stopped, for the next UnPause).</summary>
        public float Time { get => Control(3); set => Control(4, value); }
        /// <summary>The clip's length in seconds (0 without a clip or an audio device).</summary>
        public float Length => Control(5);
        /// <summary>The clip it plays; setting another stops it (Play plays the new one).</summary>
        public AudioClip Clip
        {
            get => new(Native.Api.AudioClip(Entity.Scene, Entity.Id, 0, 0));
            set => Native.Api.AudioClip(Entity.Scene, Entity.Id, 1, value.Guid);
        }
        public float Volume { get => GetFloat("Volume"); set => SetFloat("Volume", Math.Clamp(value, 0.0f, 1.0f)); }
        public float Pitch { get => GetFloat("Pitch"); set => SetFloat("Pitch", Math.Clamp(value, 0.1f, 3.0f)); }
        public bool Mute { get => GetBool("Mute"); set => SetBool("Mute", value); }
        public bool Loop { get => GetBool("Loop"); set => Control(6, value ? 1.0f : 0.0f); }
        /// <summary>A fire-and-forget 3D sound at a world point, without an Audio Source (it goes when it ends).</summary>
        public static void PlayClipAtPoint(AudioClip clip, Vector3 position, float volume = 1.0f) =>
            Native.Api.PlayClipAtPoint(Native.CurrentScene, clip.Guid, (float*)&position, volume);
        /// <summary>Plays a clip (asset GUID; 0 = this source's clip) once, overlapping other sounds, through this source's mixer group.</summary>
        public void PlayOneShot(ulong clipGuid = 0, float volumeScale = 1.0f) => Native.Api.AudioPlayOneShot(Entity.Scene, Entity.Id, clipGuid, volumeScale);

        /// <summary>Plays from the start at an exact time on the audio clock (AudioSettings.DspTime): sample-accurate, for
        /// music and beats. Play cancels it.</summary>
        public void PlayScheduled(double dspTime) => Native.Api.AudioTiming(Entity.Scene, Entity.Id, 6, dspTime);
        /// <summary>Plays from the start after `seconds`.</summary>
        public void PlayDelayed(float seconds) => PlayScheduled(AudioSettings.DspTime + Math.Max(seconds, 0.0f));
        /// <summary>Stops it at a time on the audio clock (one already past: at once).</summary>
        public void SetScheduledEndTime(double dspTime) => Native.Api.AudioTiming(Entity.Scene, Entity.Id, 7, dspTime);

        /// <summary>0: plain stereo (2D); 1: placed at the object (3D: panned and fading with distance). This engine
        /// has no in-between: below 0.5 is 2D.</summary>
        public float SpatialBlend { get => GetBool("Spatial") ? 1.0f : 0.0f; set => SetBool("Spatial", value >= 0.5f); }
        /// <summary>Full volume inside it.</summary>
        public float MinDistance { get => GetFloat("Min Distance"); set => SetFloat("Min Distance", Math.Max(value, 0.01f)); }
        /// <summary>Logarithmic: where it stops fading. Linear: where it is silent.</summary>
        public float MaxDistance { get => GetFloat("Max Distance"); set => SetFloat("Max Distance", Math.Max(value, 0.01f)); }
        public AudioRolloffMode RolloffMode { get => (AudioRolloffMode)GetInt("Rolloff"); set => SetInt("Rolloff", (int)value); }
        /// <summary>How much moving toward or away from the listener shifts its pitch (0 none, 1 natural, up to 5).</summary>
        public float DopplerLevel { get => GetFloat("Doppler Level"); set => SetFloat("Doppler Level", Math.Clamp(value, 0.0f, 5.0f)); }
        /// <summary>The Audio Mixer group it plays through ("" Master); changing it while playing moves its sound there.</summary>
        public string OutputAudioMixerGroup { get => GetString("Output") ?? ""; set => SetString("Output", value ?? ""); }
        /// <summary>Spatial sources behind walls: quieter and muffled.</summary>
        public bool Occlusion { get => GetBool("Occlusion"); set => SetBool("Occlusion", value); }
    }

    /// <summary>How a 3D Audio Source fades with distance.</summary>
    public enum AudioRolloffMode { Logarithmic = 0, Linear = 1 }

    /// <summary>The audio device's clock and rate.</summary>
    public static unsafe class AudioSettings
    {
        /// <summary>Seconds of sound the device has played: the clock PlayScheduled times are on (it stops while
        /// AudioListener.Pause is on). 0 without a device.</summary>
        public static double DspTime => Native.Api.AudioTiming(IntPtr.Zero, 0, 0, 0);
        /// <summary>Samples per second the device plays.</summary>
        public static int OutputSampleRate => (int)Native.Api.AudioTiming(IntPtr.Zero, 0, 1, 0);
    }

    /// <summary>What the player hears (the main camera is the listener).</summary>
    public static unsafe class AudioListener
    {
        /// <summary>Every sound's volume together, 0 silent to 1 (back to what it was when play stops).</summary>
        public static float Volume
        {
            get => (float)Native.Api.AudioTiming(IntPtr.Zero, 0, 2, 0);
            set => Native.Api.AudioTiming(IntPtr.Zero, 0, 3, Math.Max(value, 0.0f));
        }
        /// <summary>Holds every sound where it is (a pause menu); off again, they go on. Play stopping turns it off.</summary>
        public static bool Pause
        {
            get => Native.Api.AudioTiming(IntPtr.Zero, 0, 4, 0) != 0.0;
            set => Native.Api.AudioTiming(IntPtr.Zero, 0, 5, value ? 1 : 0);
        }
    }

    /// <summary>Which parts of a Character Controller touched something in its last move.</summary>
    [Flags]
    public enum CollisionFlags { None = 0, Sides = 1, Above = 2, Below = 4 }

    /// <summary>What a Character Controller ran into while moving (given to OnControllerColliderHit).</summary>
    public struct ControllerColliderHit
    {
        /// <summary>The object it touched.</summary>
        public Entity Entity;
        public CharacterController Controller;
        /// <summary>Where they touch, and the surface there (pointing toward the character).</summary>
        public Vector3 Point, Normal;
        /// <summary>The move's direction (normalized) and how far it asked to go.</summary>
        public Vector3 MoveDirection;
        public float MoveLength;
        /// <summary>The object's Rigidbody (null when it has none, or a static one).</summary>
        public Rigidbody Rigidbody => Entity.GetComponent<Rigidbody>() is { IsDynamic: true } rb ? rb : null;
        public Collider Collider => Entity.GetComponent<Collider>();
    }

    /// <summary>
    /// Walks a capsule through the world without a Rigidbody: Move / SimpleMove slide along walls and step up stairs.
    /// Its shape and limits apply while playing too (the capsule is made again where it stands).
    /// </summary>
    public unsafe class CharacterController : Component
    {
        internal override string NativeName => "Character Controller";
        private void Changed() => Native.Api.RefreshColliders(Entity.Scene, Entity.Id);
        private void Set(string field, float v) { SetFloat(field, v); Changed(); }

        /// <summary>Moves by `motion` this frame (no gravity), sliding along and stepping over geometry; returns which
        /// parts of it touched something.</summary>
        public CollisionFlags Move(Vector3 motion) => (CollisionFlags)Native.Api.CharacterMove(Entity.Scene, Entity.Id, (float*)&motion, 0);
        /// <summary>Moves at `speed` units/s horizontally with gravity applied; returns whether it is grounded.</summary>
        public bool SimpleMove(Vector3 speed) => Native.Api.CharacterMove(Entity.Scene, Entity.Id, (float*)&speed, 1) != 0;
        /// <summary>Which parts of it touched something in its last move.</summary>
        public CollisionFlags CollisionFlags => (CollisionFlags)Native.Api.CharacterFlags(Entity.Scene, Entity.Id);
        /// <summary>From the bottom of the capsule to its top.</summary>
        public float Height { get => GetFloat("Height"); set => Set("Height", value); }
        public float Radius { get => GetFloat("Radius"); set => Set("Radius", value); }
        /// <summary>The capsule's middle, from the object's position.</summary>
        public Vector3 Center { get => GetVector3("Center"); set { SetVector3("Center", value); Changed(); } }
        /// <summary>The steepest slope it walks up, in degrees.</summary>
        public float SlopeLimit { get => GetFloat("Slope Limit"); set => Set("Slope Limit", value); }
        /// <summary>The tallest step it walks up (and down without falling).</summary>
        public float StepOffset { get => GetFloat("Step Offset"); set => Set("Step Offset", value); }
        public float SkinWidth { get => GetFloat("Skin Width"); set => Set("Skin Width", value); }
        /// <summary>The most force it pushes rigidbodies with.</summary>
        public float PushStrength { get => GetFloat("Push Strength"); set => Set("Push Strength", value); }
        /// <summary>Starts a jump for SimpleMove (vertical speed in units/s).</summary>
        public void Jump(float speed) => Native.Api.CharacterSetVerticalSpeed(Entity.Scene, Entity.Id, speed);
        public bool IsGrounded { get { Vector3 v; float vs; return Native.Api.CharacterState(Entity.Scene, Entity.Id, (float*)&v, &vs) != 0; } }
        public Vector3 Velocity { get { Vector3 v; float vs; Native.Api.CharacterState(Entity.Scene, Entity.Id, (float*)&v, &vs); return v; } }
    }

    public enum NavMeshPathStatus { Complete = 0, Partial = 1, Invalid = 2 }

    /// <summary>A way across the navigation mesh: its corners (from the start to the end) and whether it gets there.</summary>
    public class NavMeshPath
    {
        public Vector3[] Corners { get; internal set; } = Array.Empty<Vector3>();
        public NavMeshPathStatus Status { get; internal set; } = NavMeshPathStatus.Invalid;
        public void ClearCorners() { Corners = Array.Empty<Vector3>(); Status = NavMeshPathStatus.Invalid; }
        /// <summary>Copies the corners into `results`; returns how many there are (as Unity's, for no garbage).</summary>
        public int GetCornersNonAlloc(Vector3[] results) { Array.Copy(Corners, results, Math.Min(Corners.Length, results.Length)); return Corners.Length; }
        /// <summary>Its length, corner to corner.</summary>
        public float Length { get { float l = 0; for (int i = 1; i < Corners.Length; ++i) l += Vector3.Distance(Corners[i - 1], Corners[i]); return l; } }
        internal unsafe void Set(Vector3* buffer, int n)
        {
            Status = n == 0 ? NavMeshPathStatus.Invalid : n < 0 ? NavMeshPathStatus.Partial : NavMeshPathStatus.Complete;
            var corners = new Vector3[Math.Abs(n)];
            for (int i = 0; i < corners.Length; ++i) corners[i] = buffer[i];
            Corners = corners;
        }
    }

    /// <summary>A spot found on the navigation mesh (NavMesh.Raycast, FindClosestEdge).</summary>
    public struct NavMeshHit
    {
        public Vector3 Position;
        /// <summary>The way off the edge there (horizontal).</summary>
        public Vector3 Normal;
        public float Distance;
        /// <summary>Whether something was hit (Raycast: an edge was in the way).</summary>
        public bool Hit;
    }

    /// <summary>Walks the baked navigation mesh (NavMesh Surface) to a destination, steering around other agents.</summary>
    public unsafe class NavMeshAgent : Component
    {
        internal override string NativeName => "NavMesh Agent";
        private float Op(int op, Vector3 v = default) => Native.Api.NavAgent(Entity.Scene, Entity.Id, op, (float*)&v);
        /// <summary>Finds a path there and starts walking. False when there is no way at all.</summary>
        public bool SetDestination(Vector3 target) => Op(0, target) != 0.0f;
        public Vector3 Destination { get { Vector3 v; Native.Api.NavAgent(Entity.Scene, Entity.Id, 9, (float*)&v); return v; } set => SetDestination(value); }
        public Vector3 Velocity { get { Vector3 v; Native.Api.NavAgent(Entity.Scene, Entity.Id, 1, (float*)&v); return v; } }
        /// <summary>The velocity it steers for (with Root Motion, what it asks its animation for).</summary>
        public Vector3 DesiredVelocity { get { Vector3 v; Native.Api.NavAgent(Entity.Scene, Entity.Id, 12, (float*)&v); return v; } }
        /// <summary>Distance left along the path.</summary>
        public float RemainingDistance => Op(2);
        public bool HasPath => Op(3) != 0.0f;
        public NavMeshPathStatus PathStatus => (NavMeshPathStatus)(int)Op(4);
        /// <summary>Holds it where it is (keeping its path) while true.</summary>
        public bool IsStopped { get => Op(5) != 0.0f; set => Op(6, new Vector3(value ? 1 : 0, 0, 0)); }
        /// <summary>True once it has a path and is within Stopping Distance of its end.</summary>
        public bool HasArrived => HasPath && RemainingDistance <= GetFloat("Stopping Distance") + 0.05f;
        public void ResetPath() => Op(7);
        /// <summary>True while it crosses an off-mesh link (a jump, a ladder).</summary>
        public bool IsOnOffMeshLink => Op(10) != 0.0f;
        /// <summary>True while it waits at a closed door (an Off Mesh Link with Agents Wait that is not Activated).</summary>
        public bool IsWaiting => Op(11) != 0.0f;
        /// <summary>Moves it straight to the nearest point on the mesh to `position`.</summary>
        public bool Warp(Vector3 position) => Op(8, position) != 0.0f;
        public float Speed { get => GetFloat("Speed"); set => SetFloat("Speed", value); }
        public float StoppingDistance { get => GetFloat("Stopping Distance"); set => SetFloat("Stopping Distance", value); }
        /// <summary>A bit per area it may walk (1 Walkable, 4 Road, 8 Grass, 16 Mud, 32 Water, ...); -1 = all. Takes
        /// effect on its next path.</summary>
        public int AreaMask { get => GetInt("Area Mask"); set => SetInt("Area Mask", value); }
        /// <summary>The room it takes among other agents, and its height.</summary>
        public float Radius { get => GetFloat("Radius"); set => SetFloat("Radius", value); }
        public float Height { get => GetFloat("Height"); set => SetFloat("Height", value); }
        /// <summary>How fast it speeds up and slows down (m/s^2), and turns (degrees per second).</summary>
        public float Acceleration { get => GetFloat("Acceleration"); set => SetFloat("Acceleration", value); }
        public float AngularSpeed { get => GetFloat("Angular Speed"); set => SetFloat("Angular Speed", value); }
        /// <summary>Slows down to arrive instead of running past the end of its path.</summary>
        public bool AutoBraking { get => GetBool("Auto Braking"); set => SetBool("Auto Braking", value); }
        /// <summary>How far its object stands above the ground.</summary>
        public float BaseOffset { get => GetFloat("Base Offset"); set => SetFloat("Base Offset", value); }
        public bool ObstacleAvoidance { get => GetBool("Obstacle Avoidance"); set => SetBool("Obstacle Avoidance", value); }
        /// <summary>0 most important to 99: it gives way to agents with a lower number.</summary>
        public int AvoidancePriority { get => GetInt("Avoidance Priority"); set => SetInt("Avoidance Priority", value); }

        /// <summary>Whether the agent moves its object (true), or only its NextPosition, for a script (or root motion) to
        /// move the object itself. Turned on again, the object jumps to NextPosition.</summary>
        public bool UpdatePosition { get => Op(13) != 0.0f; set => Op(14, new Vector3(value ? 1 : 0, 0, 0)); }
        /// <summary>Whether the agent turns its object to face the way it goes.</summary>
        public bool UpdateRotation { get => Op(15) != 0.0f; set => Op(16, new Vector3(value ? 1 : 0, 0, 0)); }
        /// <summary>Where the agent's own walking has it (its object's position while UpdatePosition is on); setting it
        /// moves its walking there (and its object too, with UpdatePosition on).</summary>
        public Vector3 NextPosition
        {
            get { Vector3 v; Native.Api.NavAgent(Entity.Scene, Entity.Id, 17, (float*)&v); return v; }
            set => Op(18, value);
        }

        private const int MaxCorners = 256;
        /// <summary>The path it is walking (a copy).</summary>
        public NavMeshPath Path
        {
            get
            {
                var path = new NavMeshPath();
                Vector3* buffer = stackalloc Vector3[MaxCorners];
                int n = Native.Api.NavAgentPath(Entity.Scene, Entity.Id, 0, (float*)buffer, MaxCorners, 0);
                path.Set(buffer, n);
                if (n != 0) path.Status = PathStatus;
                return path;
            }
            set => SetPath(value);
        }
        /// <summary>Finds the way from where it stands to `target` for its Area Mask, without walking it (SetPath does).
        /// False when there is none.</summary>
        public bool CalculatePath(Vector3 target, NavMeshPath path)
        {
            Vector3* buffer = stackalloc Vector3[MaxCorners];
            buffer[0] = target;
            int n = Native.Api.NavAgentPath(Entity.Scene, Entity.Id, 2, (float*)buffer, MaxCorners, 0);
            path.Set(buffer, n);
            return n != 0;
        }
        /// <summary>Walks a path found before (CalculatePath, or corners of your own) from where it is.</summary>
        public bool SetPath(NavMeshPath path)
        {
            if (path == null || path.Corners.Length == 0 || path.Status == NavMeshPathStatus.Invalid) return false;
            fixed (Vector3* p = path.Corners)
                return Native.Api.NavAgentPath(Entity.Scene, Entity.Id, 1, (float*)p, path.Corners.Length, (int)path.Status) != 0;
        }
    }

    /// <summary>Queries on the baked navigation meshes.</summary>
    public static unsafe class NavMesh
    {
        /// <summary>The nearest point on a navigation mesh within maxDistance.</summary>
        public static bool SamplePosition(Vector3 position, out Vector3 hit, float maxDistance)
        {
            Vector3 h;
            int ok = Native.Api.NavQuery(Native.CurrentScene, 0, (float*)&position, null, maxDistance, (float*)&h, 1);
            hit = h;
            return ok != 0;
        }
        /// <summary>The corners of the shortest path (from a point on the mesh). Empty when there is none; `partial`
        /// when it ends as near as it gets to an unreachable `to`.</summary>
        public static Vector3[] CalculatePath(Vector3 from, Vector3 to, out bool partial)
        {
            Vector3* buffer = stackalloc Vector3[256];
            int n = Native.Api.NavQuery(Native.CurrentScene, 1, (float*)&from, (float*)&to, 0.0f, (float*)buffer, 256);
            partial = n < 0;
            var corners = new Vector3[Math.Abs(n)];
            for (int i = 0; i < corners.Length; ++i) corners[i] = buffer[i];
            return corners;
        }
        public static Vector3[] CalculatePath(Vector3 from, Vector3 to) => CalculatePath(from, to, out _);
        /// <summary>Every area (an Area Mask).</summary>
        public const int AllAreas = -1;
        /// <summary>The shortest path over the areas in `areaMask` (as Unity's): true when one was found (it may be
        /// Partial).</summary>
        public static bool CalculatePath(Vector3 from, Vector3 to, int areaMask, NavMeshPath path)
        {
            Vector3* buffer = stackalloc Vector3[256];
            float mask = BitConverter.Int32BitsToSingle(areaMask);
            int n = Native.Api.NavQuery(Native.CurrentScene, 6, (float*)&from, (float*)&to, mask, (float*)buffer, 256);
            path.Set(buffer, n);
            return n != 0;
        }
        /// <summary>Walks straight across the mesh from `source` toward `target`: true when an edge or wall is in the way
        /// (hit: where it stops), false when the way is clear (as Unity's).</summary>
        public static bool Raycast(Vector3 source, Vector3 target, out NavMeshHit hit, int areaMask = AllAreas)
        {
            float* d = stackalloc float[7];
            int blocked = Native.Api.NavQuery(Native.CurrentScene, 4, (float*)&source, (float*)&target, 0.0f, d, areaMask);
            hit = new NavMeshHit { Position = new Vector3(d[0], d[1], d[2]), Normal = new Vector3(d[3], d[4], d[5]), Distance = d[6], Hit = blocked != 0 };
            return blocked != 0;
        }
        /// <summary>The nearest edge of the mesh (the foot of a wall, a drop) to a point on it: where, the way from it to
        /// the point, and how far. False off the mesh.</summary>
        public static bool FindClosestEdge(Vector3 source, out NavMeshHit hit, int areaMask = AllAreas)
        {
            float* d = stackalloc float[7];
            int found = Native.Api.NavQuery(Native.CurrentScene, 5, (float*)&source, null, 0.0f, d, areaMask);
            hit = new NavMeshHit { Position = new Vector3(d[0], d[1], d[2]), Normal = new Vector3(d[3], d[4], d[5]), Distance = d[6], Hit = found != 0 };
            return found != 0;
        }
        /// <summary>Bakes every NavMesh Surface again from the colliders as they are now (after the level changed).</summary>
        public static void Build() => Native.Api.NavQuery(Native.CurrentScene, 2, null, null, 0.0f, null, 0);
        /// <summary>After the level changed inside a box (a wall knocked down, a bridge built): the meshes over it sample
        /// the ground there again, keeping the rest. Returns how many surfaces changed.</summary>
        public static int UpdateArea(Vector3 center, Vector3 size) =>
            Native.Api.NavQuery(Native.CurrentScene, 3, (float*)&center, (float*)&size, 0.0f, null, 0);
    }

    /// <summary>The project's audio mixer groups (Window > Audio Mixer).</summary>
    public static unsafe class AudioMixer
    {
        public static void SetVolume(string group, float decibels) => Native.WithUtf8(group, p => Native.Api.MixerSetVolume((byte*)p, decibels));
        public static float GetVolume(string group) => Native.WithUtf8(group, p => Native.Api.MixerGetVolume((byte*)p));
        /// <summary>Blends the mixer to a snapshot over `seconds`. False if there is no snapshot with that name.</summary>
        public static bool TransitionToSnapshot(string snapshot, float seconds) => Native.WithUtf8(snapshot, p => Native.Api.MixerTransition((byte*)p, seconds)) != 0;
        /// <summary>Linear 0-1 to decibels (0 = -80 dB).</summary>
        public static float LinearToDecibels(float v) => v <= 1e-4f ? -80.0f : System.MathF.Max(-80.0f, 20.0f * System.MathF.Log10(v));
    }

    /// <summary>Microphone input: record, read the level (voice activation), save the recording as a clip.</summary>
    public static unsafe class Microphone
    {
        public static string[] Devices =>
            (Native.FromUtf8(Native.Api.MicrophoneDevices()) ?? "").Split('\n', System.StringSplitOptions.RemoveEmptyEntries);
        /// <summary>Starts recording `seconds` (device: an index into Devices, -1 = the default). False without one.</summary>
        public static bool Start(int device = -1, float seconds = 10.0f, bool loop = true) => Native.Api.MicrophoneStart(device, seconds, loop ? 1 : 0) != 0;
        public static void Stop() => Native.Api.MicrophoneState(0);
        public static bool IsRecording => Native.Api.MicrophoneState(1) != 0.0f;
        /// <summary>Loudness of the last ~50 ms (RMS, 0-1).</summary>
        public static float Level => Native.Api.MicrophoneState(2);
        public static float Peak => Native.Api.MicrophoneState(3);
        /// <summary>Samples recorded so far (wraps when looping).</summary>
        public static long Position => (long)Native.Api.MicrophoneState(4);
        public static int SampleRate => (int)Native.Api.MicrophoneState(5);
        /// <summary>Saves the recording as a WAV file (relative paths are in Assets) and imports it as an audio clip.</summary>
        public static bool SaveWav(string path) => Native.WithUtf8(path, p => Native.Api.MicrophoneSave((byte*)p)) != 0;
    }
}
