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

    public readonly record struct Prefab(ulong Guid) : IAssetReference
    {
        public bool IsValid => Guid != 0;
        // A new instance in the scene of the running script.
        public Entity Instantiate(Vector3 position) => Entity.Instantiate(Guid, position);
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
