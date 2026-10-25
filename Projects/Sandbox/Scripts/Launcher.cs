using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Press L to launch this Rigidbody upwards; logs what it lands on.</summary>
    [AddComponentMenu("Physics/Launcher"), RequireComponent(typeof(Rigidbody), typeof(SphereCollider))]
    public class Launcher : Behaviour
    {
        [Range(0.0f, 50.0f)] public float Impulse = 6.0f;
        [SerializeField, Tooltip("Launches so far (private, saved and shown through [SerializeField])")] private int m_Launches;
        public KeyCodeField Key = new();

        public override void OnUpdate(float dt)
        {
            if (Input.GetKeyDown(KeyCode.L) && GetComponent<Rigidbody>() is { } body)
            {
                body.AddImpulse(new Vector3(0, Impulse, 0));
                ++m_Launches;
            }
        }

        public override void OnCollisionEnter(Entity other) => Debug.Log($"{Name} hit {other.Name}");
    }

    /// <summary>Placeholder type to show that unsupported field types are simply not exposed.</summary>
    public class KeyCodeField { }
}
