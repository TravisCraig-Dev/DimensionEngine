using System;
using System.Numerics;
using Dimension.Interop;

namespace Dimension
{
    /// <summary>Thin wrapper over an engine component on an entity.</summary>
    public abstract class Component
    {
        public Entity Entity { get; internal set; }
        /// <summary>Component name in the engine registry (as shown in the Inspector).</summary>
        internal abstract string NativeName { get; }
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
}
