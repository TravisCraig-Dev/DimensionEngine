using System.Numerics;

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

    public readonly record struct Material(ulong Guid) : IAssetReference
    {
        public bool IsValid => Guid != 0;
        public static implicit operator ulong(Material a) => a.Guid;
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
