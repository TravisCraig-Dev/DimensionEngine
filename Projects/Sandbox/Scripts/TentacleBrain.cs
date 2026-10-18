using Dimension;

namespace Game
{
    /// <summary>Drives a tentacle's Animator Controller: "Energy" rises and falls, and it gets poked now and then.</summary>
    public class TentacleBrain : Behaviour
    {
        public float EnergyPeriod = 6.0f;
        public float PokeInterval = 7.0f;

        private float m_Time;
        private float m_NextPoke;
        private string m_LastState = "";

        public override void OnCreate() => m_NextPoke = PokeInterval;

        public override void OnUpdate(float dt)
        {
            var animator = GetComponent<Animator>();
            if (animator == null) return;
            m_Time += dt;
            animator.SetFloat("Energy", 0.5f + 0.5f * System.MathF.Sin(m_Time * 2 * System.MathF.PI / EnergyPeriod));
            if (m_Time >= m_NextPoke)
            {
                m_NextPoke += PokeInterval;
                animator.SetTrigger("Poke");
            }
            if (animator.CurrentState != m_LastState)
            {
                m_LastState = animator.CurrentState;
                Debug.Log($"{Name}: state -> {m_LastState}");
            }
        }
    }
}
