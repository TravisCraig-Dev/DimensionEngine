using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Press L to launch this Rigidbody upwards; logs what it lands on.</summary>
    public class Launcher : Behaviour
    {
        public float Impulse = 6.0f;
        public KeyCodeField Key = new();

        public override void OnUpdate(float dt)
        {
            if (Input.GetKeyDown(KeyCode.L) && GetComponent<Rigidbody>() is { } body)
                body.AddImpulse(new Vector3(0, Impulse, 0));
        }

        public override void OnCollisionEnter(Entity other) => Debug.Log($"{Name} hit {other.Name}");
    }

    /// <summary>Placeholder type to show that unsupported field types are simply not exposed.</summary>
    public class KeyCodeField { }
}
