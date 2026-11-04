using System.Collections;
using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>G turns 2D gravity upside down for a moment (everything loose floats up, then drops back); B makes the
    /// boxes bouncy, or not (their colliders' Bounciness, changed while playing).</summary>
    public class GravityPulse : Behaviour
    {
        [Tooltip("Seconds gravity stays reversed")] public float Seconds = 0.6f;
        [Range(0.0f, 1.0f)] public float Bouncy = 0.85f;

        private bool m_Pulsing, m_Bouncy;

        public override void OnUpdate(float dt)
        {
            if (Input.GetKeyDown(KeyCode.G) && !m_Pulsing) StartCoroutine(Pulse());
            if (Input.GetKeyDown(KeyCode.B))
            {
                m_Bouncy = !m_Bouncy;
                int n = 0;
                foreach (Collider2D c in FindObjectsOfType<Collider2D>())
                {
                    if (c.Entity.GetComponent<Rigidbody2D>() is not { } body || c.IsTrigger || body.GetInt("Body Type") != 1) continue;
                    c.Bounciness = m_Bouncy ? Bouncy : 0.0f;
                    ++n;
                }
                Debug.Log($"{n} bodies {(m_Bouncy ? "bouncy" : "not bouncy")}");
            }
        }

        private IEnumerator Pulse()
        {
            m_Pulsing = true;
            Vector2 gravity = Physics2D.Gravity;
            Physics2D.Gravity = -gravity;
            yield return new WaitForSeconds(Seconds);
            Physics2D.Gravity = gravity;
            m_Pulsing = false;
        }
    }
}
