using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Gusty wind for a Cloth: a steady breeze plus smooth noise that strengthens and turns it.</summary>
    [AddComponentMenu("Physics/Wind Gust"), RequireComponent(typeof(Cloth))]
    public class WindGust : Behaviour
    {
        [Tooltip("The steady part (m/s^2, world space)")] public Vector3 BaseWind = new(6.0f, 0.0f, 1.5f);
        [Range(0.0f, 4.0f), Tooltip("Gusts up to this many times the base wind")] public float Gustiness = 1.2f;
        [Range(0.01f, 5.0f), Tooltip("How quickly gusts come and go")] public float Speed = 0.6f;

        public override void OnUpdate(float dt)
        {
            if (GetComponent<Cloth>() is not { } cloth) return;
            float t = Time.time * Speed;
            float gust = Mathf.PerlinNoise(t, 0.37f) * Gustiness;
            float swirl = (Mathf.PerlinNoise(0.71f, t) - 0.5f) * 2.0f;
            cloth.Wind = BaseWind * (0.4f + gust) + new Vector3(0.0f, 0.0f, swirl * BaseWind.Length() * 0.5f);
        }
    }
}
