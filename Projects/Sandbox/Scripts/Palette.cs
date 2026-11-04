using System;
using System.Collections.Generic;
using Dimension;

namespace Game
{
    /// <summary>A data asset (Project window: New Data Asset > Game.Palette): colors to blend through, shared by every
    /// script that refers to the asset, so editing it in the Inspector changes them all.</summary>
    public class Palette : DataAsset
    {
        public List<Color> Colors = new() { new Color(1.0f, 0.55f, 0.45f), new Color(0.45f, 0.75f, 1.0f) };
        [Range(0.1f, 10.0f)] public float SecondsPerColor = 1.5f;

        /// <summary>The color at a time: each color in turn, blended into the next.</summary>
        public Color At(float time)
        {
            if (Colors.Count == 0) return new Color(1, 1, 1);
            float t = time / MathF.Max(SecondsPerColor, 0.01f);
            int i = (int)MathF.Floor(t);
            float f = t - i;
            Color a = Colors[((i % Colors.Count) + Colors.Count) % Colors.Count], b = Colors[(((i + 1) % Colors.Count) + Colors.Count) % Colors.Count];
            return new Color(a.R + (b.R - a.R) * f, a.G + (b.G - a.G) * f, a.B + (b.B - a.B) * f, a.A + (b.A - a.A) * f);
        }
    }
}
