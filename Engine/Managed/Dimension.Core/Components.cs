using System;
using System.Numerics;
using Dimension.Interop;

namespace Dimension
{
    /// <summary>Thin wrapper over an engine component on an entity.</summary>
    public abstract unsafe class Component
    {
        public Entity Entity { get; internal set; }
        /// <summary>Component name in the engine registry (as shown in the Inspector).</summary>
        internal abstract string NativeName { get; }

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
        public Vector3 GetVector3(string field) { float* v = stackalloc float[4]; ReadField(field, v); return new Vector3(v[0], v[1], v[2]); }
        public bool SetVector3(string field, Vector3 value) => WriteField(field, (float*)&value, 3);
        public Vector4 GetVector4(string field) { float* v = stackalloc float[4]; ReadField(field, v); return new Vector4(v[0], v[1], v[2], v[3]); }
        public bool SetVector4(string field, Vector4 value) => WriteField(field, (float*)&value, 4);
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

    public unsafe class Rigidbody2D : Component
    {
        internal override string NativeName => "Rigidbody 2D";
        public Vector2 Velocity
        {
            get { Vector2 v; Native.Api.GetVelocity2D(Entity.Scene, Entity.Id, (float*)&v); return v; }
            set => Native.Api.SetVelocity2D(Entity.Scene, Entity.Id, (float*)&value);
        }
        public void AddForce(Vector2 force) => Native.Api.ApplyForce2D(Entity.Scene, Entity.Id, (float*)&force);
        public void AddImpulse(Vector2 impulse) => Native.Api.ApplyImpulse2D(Entity.Scene, Entity.Id, (float*)&impulse);
    }

    /// <summary>3D colliders, for HasComponent/AddComponent and [RequireComponent] (their settings are edited in the Inspector).</summary>
    public class BoxCollider : Component { internal override string NativeName => "Box Collider"; }
    public class SphereCollider : Component { internal override string NativeName => "Sphere Collider"; }
    public class CapsuleCollider : Component { internal override string NativeName => "Capsule Collider"; }
    public class MeshCollider : Component { internal override string NativeName => "Mesh Collider"; }

    public unsafe class Rigidbody : Component
    {
        internal override string NativeName => "Rigidbody";
        public Vector3 Velocity
        {
            get { Vector3 v; Native.Api.GetVelocity3D(Entity.Scene, Entity.Id, (float*)&v); return v; }
            set => Native.Api.SetVelocity3D(Entity.Scene, Entity.Id, (float*)&value);
        }
        public void AddForce(Vector3 force) => Native.Api.AddForce3D(Entity.Scene, Entity.Id, (float*)&force);
        public void AddImpulse(Vector3 impulse) => Native.Api.AddImpulse3D(Entity.Scene, Entity.Id, (float*)&impulse);
    }

    public unsafe class MeshRenderer : Component
    {
        internal override string NativeName => "Mesh Renderer";
        public Color Color
        {
            get { Color c; Native.Api.GetColor(Entity.Scene, Entity.Id, (float*)&c); return c; }
            set => Native.Api.SetColor(Entity.Scene, Entity.Id, (float*)&value);
        }
    }

    public unsafe class SpriteRenderer : Component
    {
        internal override string NativeName => "Sprite Renderer";
        public Color Color
        {
            get { Color c; Native.Api.GetColor(Entity.Scene, Entity.Id, (float*)&c); return c; }
            set => Native.Api.SetColor(Entity.Scene, Entity.Id, (float*)&value);
        }
    }

    public unsafe class UIText : Component
    {
        internal override string NativeName => "UI Text";
        public string Text
        {
            get => Native.FromUtf8(Native.Api.GetUIText(Entity.Scene, Entity.Id));
            set { var e = Entity; Native.WithUtf8(value, p => Native.Api.SetUIText(e.Scene, e.Id, (byte*)p)); }
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
    public unsafe class Joint : Component
    {
        internal override string NativeName => "Joint";
        private float State(int i) { float* s = stackalloc float[4]; Native.Api.GetJointState(Entity.Scene, Entity.Id, s); return s[i]; }
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
        /// <summary>World-space acceleration on the free vertices (m/s^2), applied live.</summary>
        public Vector3 Wind { get => GetVector3("Wind"); set => SetVector3("Wind", value); }
        /// <summary>Takes effect when the cloth is created (play start).</summary>
        public float StretchCompliance { get => GetFloat("Stretch Compliance"); set => SetFloat("Stretch Compliance", value); }
        public float BendCompliance { get => GetFloat("Bend Compliance"); set => SetFloat("Bend Compliance", value); }
    }

    /// <summary>A pressurized soft body made from the object's mesh.</summary>
    /// <summary>Plays a Timeline asset on this object and the objects under it.</summary>
    public unsafe class Timeline : Component
    {
        internal override string NativeName => "Timeline";
        public void Play() => Native.Api.TimelineControl(Entity.Scene, Entity.Id, 0, 0.0f);
        public void Stop() => Native.Api.TimelineControl(Entity.Scene, Entity.Id, 1, 0.0f);
        public bool IsPlaying => Native.Api.TimelineControl(Entity.Scene, Entity.Id, 4, 0.0f) != 0.0f;
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
        /// <summary>Seconds the animation blends in from the limp pose when Simulate turns off.</summary>
        public float GetUpTime { get => GetFloat("Get Up Time"); set => SetFloat("Get Up Time", value); }
        /// <summary>Lying on its back (pick a get-up animation by it before turning Simulate off).</summary>
        public bool IsFaceUp => Native.Api.RagdollQuery(Entity.Scene, Entity.Id, 0) > 0.5f;
        /// <summary>How far the blend back into the animation is, 0-1 (1 when done or not getting up).</summary>
        public float GetUpProgress => Native.Api.RagdollQuery(Entity.Scene, Entity.Id, 1);
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

    public unsafe class SoftBody : Component
    {
        internal override string NativeName => "Soft Body";
        /// <summary>Inflation; applied live (0 = limp).</summary>
        public float Pressure { get => GetFloat("Pressure"); set => SetFloat("Pressure", value); }
    }

    public unsafe class Joint2D : Joint
    {
        internal override string NativeName => "Joint 2D";
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
        public void SetFloat(string name, float value) => SetParameter(name, value, 0);
        public void SetInteger(string name, int value) => SetParameter(name, value, 1);
        public void SetBool(string name, bool value) => SetParameter(name, value ? 1 : 0, 2);
        public void SetTrigger(string name) => SetParameter(name, 1, 3);
        /// <summary>Name of the state machine's current state ("" without a controller).</summary>
        public string CurrentState => Native.FromUtf8(Native.Api.AnimatorGetState(Entity.Scene, Entity.Id));
    }

    public unsafe class ParticleSystem : Component
    {
        internal override string NativeName => "Particle System";
        public void Emit(int count) => Native.Api.ParticlesEmit(Entity.Scene, Entity.Id, count);
        public void Play() => Native.Api.ParticlesPlay(Entity.Scene, Entity.Id, 1);
        public void Stop() => Native.Api.ParticlesPlay(Entity.Scene, Entity.Id, 0);
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
        /// <summary>Plays a clip (asset GUID; 0 = this source's clip) once, overlapping other sounds, through this source's mixer group.</summary>
        public void PlayOneShot(ulong clipGuid = 0, float volumeScale = 1.0f) => Native.Api.AudioPlayOneShot(Entity.Scene, Entity.Id, clipGuid, volumeScale);
    }

    /// <summary>Capsule character moved by scripts (like Unity's CharacterController).</summary>
    public unsafe class CharacterController : Component
    {
        internal override string NativeName => "Character Controller";
        /// <summary>Moves by `motion` this frame (no gravity), sliding along and stepping over geometry.</summary>
        public void Move(Vector3 motion) => Native.Api.CharacterMove(Entity.Scene, Entity.Id, (float*)&motion, 0);
        /// <summary>Moves at `speed` units/s horizontally with gravity applied.</summary>
        public void SimpleMove(Vector3 speed) => Native.Api.CharacterMove(Entity.Scene, Entity.Id, (float*)&speed, 1);
        /// <summary>Starts a jump for SimpleMove (vertical speed in units/s).</summary>
        public void Jump(float speed) => Native.Api.CharacterSetVerticalSpeed(Entity.Scene, Entity.Id, speed);
        public bool IsGrounded { get { Vector3 v; float vs; return Native.Api.CharacterState(Entity.Scene, Entity.Id, (float*)&v, &vs) != 0; } }
        public Vector3 Velocity { get { Vector3 v; float vs; Native.Api.CharacterState(Entity.Scene, Entity.Id, (float*)&v, &vs); return v; } }
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
