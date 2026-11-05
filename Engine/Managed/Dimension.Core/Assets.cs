using System.Numerics;
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

    public readonly record struct Texture(ulong Guid) : IAssetReference
    {
        public bool IsValid => Guid != 0;
        public static implicit operator ulong(Texture a) => a.Guid;
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
}
