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
            m_Hue += CyclesPerSecond * dt;
            var color = Color.FromHSV(m_Hue, Saturation, Brightness);
            if (HasComponent<MeshRenderer>()) GetComponent<MeshRenderer>().Color = color;
            else if (HasComponent<SpriteRenderer>()) GetComponent<SpriteRenderer>().Color = color;
        }
    }
}
