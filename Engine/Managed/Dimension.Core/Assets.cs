using System;
using System.Numerics;
using System.Runtime.InteropServices;
using Dimension.Interop;

namespace Dimension
{
    // References to assets by GUID (as the Inspector shows them). Script fields of these types are picked in the
    // Inspector (or dropped there from the Project window) and saved with the scene; EditorGUI.AssetField picks them in
    // editor extensions. They convert to the GUID where an API takes one (AudioSource.PlayOneShot(clip)).
    public interface IAssetReference
    {
        ulong Guid { get; }
    }

    /// <summary>
    /// Data kept in an asset (like Unity's ScriptableObject): derive a class from this, give it public fields, then
    /// make <c>.asset</c> files of it in the Project window (Create > Data Asset) and set their values in the Inspector.
    /// Script fields of the class pick one of those assets; while playing, every field referring to an asset holds the
    /// same object (changes made to it last until play stops). Saving the asset in the Inspector while playing updates
    /// the object in place.
    /// </summary>
    public abstract class DataAsset
    {
        /// <summary>The asset's GUID.</summary>
        public ulong Guid { get; internal set; }
        /// <summary>After the values are read from the asset (first use while playing, and each save in the Inspector).</summary>
        public virtual void OnLoaded() { }
    }

    public readonly record struct Prefab(ulong Guid) : IAssetReference
    {
        public bool IsValid => Guid != 0;
        // A new instance in the scene of the running script.
        public Entity Instantiate(Vector3 position) => Entity.Instantiate(Guid, position);
        /// <summary>At a world position and rotation, under `parent` (default: a root object).</summary>
        public Entity Instantiate(Vector3 position, Quaternion rotation, Entity parent = default) => Entity.Instantiate(Guid, position, rotation, parent);
        public static implicit operator ulong(Prefab a) => a.Guid;
    }

    public readonly unsafe record struct Texture(ulong Guid) : IAssetReference
    {
        public bool IsValid => Guid != 0;
        public static implicit operator ulong(Texture a) => a.Guid;
        /// <summary>Its size in pixels (texture assets, render textures and textures made by scripts); 0 when unknown.</summary>
        public int Width => Size().X;
        public int Height => Size().Y;
        private (int X, int Y) Size()
        {
            ulong guid = Guid;
            int* s = stackalloc int[2];
            return Native.Api.TextureOp(Native.CurrentScene, 4, &guid, s, null, 0, 0) != 0 ? (s[0], s[1]) : (0, 0);
        }
    }

    /// <summary>How a texture made by a script is sampled between pixels (Unity's FilterMode).</summary>
    public enum FilterMode { Bilinear, Point }

    /// <summary>
    /// A texture made by a script (Unity's Texture2D): pixels set on the CPU with SetPixel / SetPixels, shown once
    /// <see cref="Apply"/> sends them to the GPU. Assign it (or its <see cref="Texture"/>) wherever a texture goes: a
    /// Mesh Renderer's Albedo, a Sprite Renderer, a UI Image, a material's maps. Pixels count from the top-left (as the
    /// screen and texture files do here; Unity's start at the bottom-left). It lasts until play stops or Destroy().
    /// </summary>
    public sealed unsafe class Texture2D
    {
        private readonly Color32[] m_Pixels;
        public int Width { get; }
        public int Height { get; }
        public ulong Guid { get; private set; }
        public Texture Texture => new(Guid);
        public static implicit operator Texture(Texture2D t) => t?.Texture ?? default;

        /// <param name="linear">Data rather than colors (normal maps, masks): not sRGB.</param>
        public Texture2D(int width, int height, FilterMode filter = FilterMode.Bilinear, bool linear = false)
        {
            Width = Math.Clamp(width, 1, 8192);
            Height = Math.Clamp(height, 1, 8192);
            int* size = stackalloc int[2] { Width, Height };
            ulong guid = 0;
            Native.Api.TextureOp(Native.CurrentScene, 0, &guid, size, null, 0, (linear ? 1 : 0) | (filter == FilterMode.Point ? 2 : 0));
            Guid = guid;
            m_Pixels = new Color32[Width * Height];
            Array.Fill(m_Pixels, new Color32(255, 255, 255, 255));
        }

        public Color32 GetPixel32(int x, int y) => m_Pixels[Index(x, y)];
        public Color GetPixel(int x, int y) => m_Pixels[Index(x, y)];
        public void SetPixel(int x, int y, Color color) => m_Pixels[Index(x, y)] = color;
        public void SetPixel32(int x, int y, Color32 color) => m_Pixels[Index(x, y)] = color;
        /// <summary>Every pixel, row by row from the top.</summary>
        public Color32[] GetPixels32() => (Color32[])m_Pixels.Clone();
        public void SetPixels32(Color32[] pixels) => Array.Copy(pixels, m_Pixels, Math.Min(pixels.Length, m_Pixels.Length));
        public Color[] GetPixels() => Array.ConvertAll(m_Pixels, c => (Color)c);
        public void SetPixels(Color[] pixels) { for (int i = 0; i < Math.Min(pixels.Length, m_Pixels.Length); ++i) m_Pixels[i] = pixels[i]; }
        /// <summary>Fills the whole texture with one color.</summary>
        public void Fill(Color color) => Array.Fill(m_Pixels, (Color32)color);
        private int Index(int x, int y) => Math.Clamp(y, 0, Height - 1) * Width + Math.Clamp(x, 0, Width - 1);

        /// <summary>Sends the pixels set since the last Apply to the GPU (where it is shown from the next frame).</summary>
        public void Apply()
        {
            ulong guid = Guid;
            fixed (Color32* p = m_Pixels) Native.Api.TextureOp(Native.CurrentScene, 1, &guid, null, (uint*)p, m_Pixels.Length, 0);
        }
        /// <summary>Frees it now (otherwise when play stops).</summary>
        public void Destroy()
        {
            ulong guid = Guid;
            Native.Api.TextureOp(Native.CurrentScene, 3, &guid, null, null, 0, 0);
            Guid = 0;
        }

        /// <summary>
        /// Copies another texture's pixels as the GPU holds them into this one (Unity's ReadPixels, from a texture rather
        /// than the active render target): a render texture a camera drew (as of the last finished frame: a camera's
        /// Render() shows here the frame after), or an RGBA8 image. The overlapping part is copied from the top-left;
        /// call Apply to show the result. False when it cannot be read (another format).
        /// </summary>
        public bool ReadPixels(Texture source)
        {
            int w = source.Width, h = source.Height;
            if (w <= 0 || h <= 0) return false;
            var read = new Color32[w * h];
            ulong guid = source.Guid;
            int* size = stackalloc int[2] { w, h };
            fixed (Color32* p = read)
                if (Native.Api.TextureOp(Native.CurrentScene, 6, &guid, size, (uint*)p, read.Length, 0) == 0) return false;
            for (int y = 0; y < Math.Min(h, Height); ++y)
                Array.Copy(read, y * w, m_Pixels, y * Width, Math.Min(w, Width));
            return true;
        }

        /// <summary>The pixels as a PNG file's bytes (File.WriteAllBytes to save it).</summary>
        public byte[] EncodeToPNG()
        {
            int* size = stackalloc int[2] { Width, Height };
            int n;
            fixed (Color32* p = m_Pixels) n = Native.Api.TextureOp(IntPtr.Zero, 7, null, size, (uint*)p, m_Pixels.Length, 0);
            if (n <= 0) return Array.Empty<byte>();
            var bytes = new byte[n];
            fixed (byte* b = bytes) Native.Api.TextureOp(IntPtr.Zero, 8, null, null, (uint*)b, n, 0);
            return bytes;
        }
    }

    /// <summary>
    /// A render texture made by a script (Unity's RenderTexture): set it as a Camera's TargetTexture and show it
    /// anywhere a texture goes. Black until a camera draws into it; it lasts until play stops or Destroy().
    /// </summary>
    public sealed unsafe class RenderTexture
    {
        public int Width { get; }
        public int Height { get; }
        public ulong Guid { get; private set; }
        public Texture Texture => new(Guid);
        public static implicit operator Texture(RenderTexture t) => t?.Texture ?? default;

        public RenderTexture(int width, int height)
        {
            Width = Math.Clamp(width, 1, 8192);
            Height = Math.Clamp(height, 1, 8192);
            int* size = stackalloc int[2] { Width, Height };
            ulong guid = 0;
            Native.Api.TextureOp(Native.CurrentScene, 5, &guid, size, null, 0, 0);
            Guid = guid;
        }

        public void Destroy()
        {
            ulong guid = Guid;
            Native.Api.TextureOp(Native.CurrentScene, 3, &guid, null, null, 0, 0);
            Guid = 0;
        }
    }

    /// <summary>
    /// Extra content loaded while the game runs (Unity's AssetBundle): a .pak the editor builds from a project folder
    /// (Project window > Build Asset Bundle), with its scenes, prefabs and other assets. Loading adds its assets to the
    /// game's (under Assets/Bundles/&lt;name&gt;, their GUIDs kept), so its scenes load by path and its prefabs place
    /// like any other; they stay for the rest of the run.
    /// </summary>
    public sealed unsafe class AssetBundle
    {
        /// <summary>Its folder among the game's assets ("Bundles/&lt;name&gt;").</summary>
        public string Folder { get; }
        public string Name { get; }
        private readonly string m_Path;
        private AssetBundle(string path, string folder) { m_Path = path; Folder = folder; Name = System.IO.Path.GetFileNameWithoutExtension(path); }

        /// <summary>Loads a bundle file (null when it is not one).</summary>
        public static AssetBundle LoadFromFile(string path)
        {
            string folder = Native.WithUtf8(path, p => Native.FromUtf8(Native.Api.BundleOp(0, (byte*)p, null)));
            return string.IsNullOrEmpty(folder) ? null : new AssetBundle(path, folder);
        }

        /// <summary>Its assets' paths (Assets-relative, e.g. "Bundles/Winter/Scenes/Lake.scene").</summary>
        public string[] GetAllAssetNames()
        {
            string joined = Native.WithUtf8(m_Path, p => Native.FromUtf8(Native.Api.BundleOp(1, (byte*)p, null)));
            return joined.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        }
        /// <summary>Its scenes' paths, for SceneManager.LoadScene.</summary>
        public string[] GetAllScenePaths() => Array.FindAll(GetAllAssetNames(), a => a.EndsWith(".scene", StringComparison.OrdinalIgnoreCase));

        /// <summary>A prefab of the bundle by its path inside it ("Prefabs/Sled.prefab"); IsValid false when missing.</summary>
        public Prefab LoadPrefab(string pathInBundle) => new(Guid(pathInBundle));
        public Texture LoadTexture(string pathInBundle) => new(Guid(pathInBundle));
        public Material LoadMaterial(string pathInBundle) => new(Guid(pathInBundle));
        /// <summary>Loads one of its scenes ("Scenes/Lake" or "Scenes/Lake.scene").</summary>
        public void LoadScene(string pathInBundle, LoadSceneMode mode = LoadSceneMode.Single) => SceneManager.LoadScene(Folder + "/" + pathInBundle, mode);

        private ulong Guid(string pathInBundle)
        {
            ulong* guid = stackalloc ulong[1];
            guid[0] = 0;
            IntPtr at = (IntPtr)guid;
            Native.WithUtf8(Folder + "/" + pathInBundle, p => { Native.Api.BundleOp(2, (byte*)p, (ulong*)at); return 0; });
            return guid[0];
        }
    }

    /// <summary>A color of four bytes (Unity's Color32), sRGB-encoded as texture pixels are.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Color32
    {
        public byte R, G, B, A;
        public Color32(byte r, byte g, byte b, byte a) { R = r; G = g; B = b; A = a; }
        public static implicit operator Color(Color32 c) => new(c.R / 255f, c.G / 255f, c.B / 255f, c.A / 255f);
        public static implicit operator Color32(Color c) =>
            new((byte)Math.Clamp((int)MathF.Round(c.R * 255f), 0, 255), (byte)Math.Clamp((int)MathF.Round(c.G * 255f), 0, 255),
                (byte)Math.Clamp((int)MathF.Round(c.B * 255f), 0, 255), (byte)Math.Clamp((int)MathF.Round(c.A * 255f), 0, 255));
        public override string ToString() => $"RGBA({R}, {G}, {B}, {A})";
    }

    /// <summary>A timeline asset (.timeline): what a <see cref="Dimension.Timeline"/> component plays (Unity's
    /// PlayableAsset / TimelineAsset).</summary>
    public readonly record struct TimelineAsset(ulong Guid) : IAssetReference
    {
        public bool IsValid => Guid != 0;
        public static implicit operator ulong(TimelineAsset a) => a.Guid;
    }

    /// <summary>An Animator Controller asset, or an Animator Override Controller (its controller with some clips
    /// swapped): what <see cref="Animator.RuntimeAnimatorController"/> runs (Unity's RuntimeAnimatorController).</summary>
    public readonly record struct AnimatorController(ulong Guid) : IAssetReference
    {
        public bool IsValid => Guid != 0;
        public static implicit operator ulong(AnimatorController a) => a.Guid;
    }

    /// <summary>
    /// A material asset (or one made while playing). Its properties are read and set by their Inspector names ("Base
    /// Color", "Metallic", "Base Map", ...) or Unity's ("_Color", "_MainTex", "_Glossiness", ...). Changing an asset's
    /// changes every object using it (and the asset, until it is reloaded); <c>new Material(source)</c> or a renderer's
    /// <see cref="MeshRenderer.Material"/> make copies that are only in memory, gone when play stops (or Destroy()).
    /// </summary>
    public readonly unsafe record struct Material(ulong Guid) : IAssetReference
    {
        public bool IsValid => Guid != 0;
        public static implicit operator ulong(Material a) => a.Guid;

        /// <summary>A new material: a copy of `source` (the defaults when it is none), for this play session.</summary>
        public Material(Material source) : this(Copy(source.Guid)) { }
        /// <summary>A new material with the default look (white, 0.5 smoothness).</summary>
        public static Material Create() => new(Copy(0));
        private static ulong Copy(ulong from)
        {
            ulong made = 0;
            Native.Api.MaterialOp(Native.CurrentScene, from, 4, null, null, &made);
            return made;
        }
        /// <summary>Frees a material made while playing (assets are left alone).</summary>
        public void Destroy() => Native.Api.MaterialOp(Native.CurrentScene, Guid, 5, null, null, null);
        /// <summary>Whether it was made while playing (a copy), not an asset.</summary>
        public bool IsCopy => Native.Api.MaterialOp(Native.CurrentScene, Guid, 6, null, null, null) != 0.0f;

        // Unity's property names, as this engine's fields.
        private static string Field(string name) => name switch
        {
            "_Color" or "_BaseColor" => "Base Color",
            "_MainTex" or "_BaseMap" => "Base Map",
            "_BumpMap" => "Normal Map",
            "_BumpScale" => "Normal Strength",
            "_Metallic" => "Metallic",
            "_EmissionColor" => "Emission Color",
            "_EmissionMap" => "Emission Map",
            "_Cutoff" => "Alpha Cutoff",
            "_MetallicGlossMap" => "Metallic Roughness Map",
            _ => name,
        };
        private static bool IsSmoothness(string name) => name is "_Glossiness" or "_Smoothness" or "Smoothness";
        private float Op(int op, string name, float* v, ulong* asset = null)
        {
            ulong guid = Guid; // (a struct's lambda cannot use `this`)
            return Native.WithUtf8<float>(Field(name), p => Native.Api.MaterialOp(Native.CurrentScene, guid, op, (byte*)p, v, asset));
        }

        /// <summary>Whether it has a property of that name.</summary>
        public bool HasProperty(string name)
        {
            float* v = stackalloc float[4];
            ulong a = 0;
            return IsSmoothness(name) || Op(0, name, v) != 0.0f || Op(2, name, v, &a) != 0.0f;
        }
        public Color GetColor(string name)
        {
            float* v = stackalloc float[4] { 0, 0, 0, 1 };
            Op(0, name, v);
            return new Color(v[0], v[1], v[2], v[3]);
        }
        /// <summary>Sets a color property (the alpha is kept for colors without one, as Emission Color).</summary>
        public void SetColor(string name, Color color)
        {
            float* v = stackalloc float[4] { color.R, color.G, color.B, color.A };
            Op(1, name, v);
        }
        public float GetFloat(string name)
        {
            float* v = stackalloc float[4];
            if (IsSmoothness(name)) { Op(0, "Roughness", v); return 1.0f - v[0]; }
            Op(0, name, v);
            return v[0];
        }
        public void SetFloat(string name, float value)
        {
            float* v = stackalloc float[4] { value, 0, 0, 0 };
            if (IsSmoothness(name)) { v[0] = 1.0f - value; Op(1, "Roughness", v); return; }
            Op(1, name, v);
        }
        public int GetInt(string name) => (int)GetFloat(name);
        public void SetInt(string name, int value) => SetFloat(name, value);
        public Vector2 GetVector(string name)
        {
            float* v = stackalloc float[4];
            Op(0, name, v);
            return new Vector2(v[0], v[1]);
        }
        public void SetVector(string name, Vector2 value)
        {
            float* v = stackalloc float[4] { value.X, value.Y, 0, 0 };
            Op(1, name, v);
        }
        public Texture GetTexture(string name)
        {
            ulong a = 0;
            float* v = stackalloc float[4];
            Op(2, name, v, &a);
            return new Texture(a);
        }
        public void SetTexture(string name, Texture texture)
        {
            ulong a = texture.Guid;
            float* v = stackalloc float[4];
            Op(3, name, v, &a);
        }

        /// <summary>The base color (Unity's Material.color).</summary>
        public Color Color { get => GetColor("Base Color"); set => SetColor("Base Color", value); }
        /// <summary>The base color map, and its tiling and offset.</summary>
        public Texture MainTexture { get => GetTexture("Base Map"); set => SetTexture("Base Map", value); }
        public Vector2 MainTextureScale { get => GetVector("Tiling"); set => SetVector("Tiling", value); }
        public Vector2 MainTextureOffset { get => GetVector("Offset"); set => SetVector("Offset", value); }
        public float Metallic { get => GetFloat("Metallic"); set => SetFloat("Metallic", value); }
        /// <summary>0 rough to 1 polished (1 - Roughness).</summary>
        public float Smoothness { get => GetFloat("Smoothness"); set => SetFloat("Smoothness", value); }
        /// <summary>The glow's color, and how strong it is (above 1 blooms).</summary>
        public Color EmissionColor { get => GetColor("Emission Color"); set => SetColor("Emission Color", value); }
        public float EmissionStrength { get => GetFloat("Emission Strength"); set => SetFloat("Emission Strength", value); }
        /// <summary>Copies every property from another material.</summary>
        public void CopyPropertiesFrom(Material other)
        {
            float* v = stackalloc float[4];
            foreach (string f in new[] { "Base Color", "Normal Strength", "Metallic", "Roughness", "Emission Color", "Emission Strength", "Tiling", "Offset",
                                         "Blend Mode", "Alpha Cutoff", "Double Sided", "Cast Shadows", "Refraction" })
                if (other.Op(0, f, v) != 0.0f) Op(1, f, v);
            foreach (string f in new[] { "Base Map", "Normal Map", "Metallic Roughness Map", "Emission Map" }) SetTexture(f, other.GetTexture(f));
        }
    }

    public readonly record struct Mesh(ulong Guid) : IAssetReference
    {
        public bool IsValid => Guid != 0;
        public static implicit operator ulong(Mesh a) => a.Guid;
    }

    public readonly record struct AudioClip(ulong Guid) : IAssetReference
    {
        public bool IsValid => Guid != 0;
        public static implicit operator ulong(AudioClip a) => a.Guid;
    }

    /// <summary>A video file asset (MP4, MOV, WMV...), as a Video Player plays it.</summary>
    public readonly record struct VideoClip(ulong Guid) : IAssetReference
    {
        public bool IsValid => Guid != 0;
        public static implicit operator ulong(VideoClip a) => a.Guid;
    }
}
