using Dimension;

namespace Game
{
    /// <summary>Drives a tentacle's Animator Controller: "Energy" rises and falls, and it gets poked now and then (only
    /// once its current state has played through and no crossfade is running). J plays its Curl state from halfway
    /// (Animator.Play with a normalized time; GetCurrentAnimatorStateInfo says how long it is), and Tip rides the tip
    /// joint (GetBoneMatrix, in OnLateUpdate: after this frame's animation).</summary>
    public class TentacleBrain : Behaviour
    {
        public float EnergyPeriod = 6.0f;
        public float PokeInterval = 7.0f;
        [Tooltip("Optional: kept on the tip joint")] public Entity Tip;
        public string TipJoint = "Bone5";
        [Tooltip("Where on the joint Tip sits, in the joint's space")] public System.Numerics.Vector3 TipOffset = new(0, 0.6f, 0);

        private float m_Time;
        private float m_NextPoke;
        private string m_LastState = "";

        public override void OnCreate() => m_NextPoke = PokeInterval;

        public override void OnLateUpdate(float dt)
        {
            if (Tip.IsValid && GetComponent<Animator>() is { } animator && animator.GetBoneMatrix(TipJoint, out var bone))
                Tip.Transform.Position = System.Numerics.Vector3.Transform(TipOffset, bone); // Tip has no parent
        }

        public override void OnUpdate(float dt)
        {
            var animator = GetComponent<Animator>();
            if (animator == null) return;
            m_Time += dt;
            animator.SetFloat("Energy", 0.5f + 0.5f * System.MathF.Sin(m_Time * 2 * System.MathF.PI / EnergyPeriod));
            if (m_Time >= m_NextPoke && animator.HasParameter("Poke") && !animator.IsInTransition() && animator.NormalizedTime >= 1.0f)
            {
                m_NextPoke = m_Time + PokeInterval;
                animator.SetTrigger("Poke");
            }
            if (Input.GetKeyDown(KeyCode.J) && animator.Play("Curl", 0, 0.5f))
                Debug.Log($"{Name}: Curl from halfway");
            if (animator.CurrentState != m_LastState)
            {
                string from = animator.IsInTransition() ? animator.PreviousState() : m_LastState;
                m_LastState = animator.CurrentState;
                AnimatorStateInfo next = animator.GetNextAnimatorStateInfo();
                Debug.Log($"{Name}: {from} -> {m_LastState} (Energy {animator.GetFloat("Energy"):0.00}" +
                          (next.IsValid ? $", {next.Length:0.0} s per loop)" : ")"));
            }
        }
    }
}
