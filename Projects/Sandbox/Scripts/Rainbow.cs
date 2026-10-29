using Dimension;

namespace Game
{
    /// <summary>Cycles the Mesh Renderer (or Sprite Renderer) color through the hue wheel.</summary>
    public class Rainbow : Behaviour
    {
        public float CyclesPerSecond = 0.2f;
        public float Saturation = 0.8f;
        public float Brightness = 1.0f;

        private float m_Hue;

        public override void OnUpdate(float dt)
        {
            // Shows in the Profiler as "Rainbow" (with "Rainbow Color" inside it) and a "Rainbow Hue" counter.
            using (Profiler.Sample("Rainbow"))
            {
                m_Hue += CyclesPerSecond * dt;
                Profiler.BeginSample("Rainbow Color");
                var color = Color.FromHSV(m_Hue, Saturation, Brightness);
                if (HasComponent<MeshRenderer>()) GetComponent<MeshRenderer>().Color = color;
                else if (HasComponent<SpriteRenderer>()) GetComponent<SpriteRenderer>().Color = color;
                Profiler.EndSample();
                Profiler.SetCounter("Rainbow Hue", m_Hue % 1.0f);
            }
        }
    }
}
