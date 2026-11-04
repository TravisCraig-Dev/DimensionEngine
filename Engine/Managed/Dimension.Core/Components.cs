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
        /// <summary>Spin in degrees per second, counterclockwise.</summary>
        public float AngularVelocity
        {
            get => Native.Api.Body2D(Entity.Scene, Entity.Id, 0, 0.0f) * 180.0f / MathF.PI;
            set => Native.Api.Body2D(Entity.Scene, Entity.Id, 1, value * MathF.PI / 180.0f);
        }
        /// <summary>Turns the body (N*m, counterclockwise) for this step, like AddForce for rotation.</summary>
        public void AddTorque(float torque) => Native.Api.Body2D(Entity.Scene, Entity.Id, 2, torque);
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
        /// <summary>Moved by physics (Body Type Dynamic). Colliders without a Rigidbody get a static one while playing.</summary>
        public bool IsDynamic => GetInt("Body Type") == 1;
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

    public unsafe class MeshRenderer : Component
    {
        internal override string NativeName => "Mesh Renderer";
        public Color Color
        {
            get { Color c; Native.Api.GetColor(Entity.Scene, Entity.Id, (float*)&c); return c; }
            set => Native.Api.SetColor(Entity.Scene, Entity.Id, (float*)&value);
        }
    }

    /// <summary>
    /// A camera; <see cref="Main"/> is the one the scene renders with. Screen points are in pixels of the view the game
    /// is shown in (<see cref="Screen.Width"/> x <see cref="Screen.Height"/>), with the origin at the top-left like
    /// <see cref="Input.MousePosition"/>; their z is the distance in front of the camera in world units.
    /// </summary>
    public unsafe class Camera : Component
    {
        internal override string NativeName => "Camera";

        /// <summary>The camera the running scene renders with (its primary one), or null.</summary>
        public static Camera Main
        {
            get
            {
                IntPtr scene = Native.CurrentScene;
                ulong id = scene == IntPtr.Zero ? ulong.MaxValue : Native.Api.MainCamera(scene);
                return id == ulong.MaxValue ? null : new Camera { Entity = new Entity(scene, id) };
            }
        }

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
    }

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

    public enum NavMeshPathStatus { Complete = 0, Partial = 1, Invalid = 2 }

    /// <summary>Walks the baked navigation mesh (NavMesh Surface) to a destination, steering around other agents.
    /// Its other settings (Acceleration, Angular Speed, ...) are read and set with GetFloat/SetFloat.</summary>
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
