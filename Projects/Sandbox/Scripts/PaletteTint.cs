using Dimension;

namespace Game
{
    /// <summary>Tints the object's sprite (or mesh) with a Palette data asset's colors over time; several objects can
    /// share one palette, each a little later (Offset).</summary>
    public class PaletteTint : Behaviour
    {
        public Palette Palette;
        [Tooltip("Seconds ahead of the palette's time")] public float Offset;

        public override void OnUpdate(float dt)
        {
            if (Palette == null) return;
            Color c = Palette.At(Time.time + Offset);
            if (GetComponent<SpriteRenderer>() is { } sprite) sprite.Color = c;
            else if (GetComponent<MeshRenderer>() is { } mesh) mesh.Color = c;
        }
    }
}
