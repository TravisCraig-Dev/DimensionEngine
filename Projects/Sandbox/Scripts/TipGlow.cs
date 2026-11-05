using Dimension;

namespace Game
{
    /// <summary>
    /// A state machine behaviour (StateMachineBehaviour) on the tentacle controller's Curl state: while the tentacle
    /// curls, the Tip Bead turns orange and pulses brighter as the curl plays through (OnStateUpdate's normalized
    /// time); leaving the state puts it back.
    /// </summary>
    public class TipGlow : StateMachineBehaviour
    {
        private static readonly Color Curling = new(1.0f, 0.5f, 0.15f, 1.0f);
        private Color m_Before;
        private MeshRenderer m_Bead;

        public override void OnStateEnter(Animator animator, AnimatorStateInfo stateInfo, int layerIndex)
        {
            m_Bead = Entity.Find("Tip Bead").GetComponent<MeshRenderer>();
            if (m_Bead == null) return;
            m_Before = m_Bead.Color;
            m_Bead.Color = Curling;
            Debug.Log($"{animator.Entity.Name}: {stateInfo.Name} (tag {stateInfo.Tag}) entered, {stateInfo.Length:0.0} s");
        }

        public override void OnStateUpdate(Animator animator, AnimatorStateInfo stateInfo, int layerIndex)
        {
            if (m_Bead != null) m_Bead.Emission = 3.0f + 6.0f * Mathf.Clamp01(stateInfo.NormalizedTime);
        }

        public override void OnStateExit(Animator animator, AnimatorStateInfo stateInfo, int layerIndex)
        {
            if (m_Bead == null) return;
            m_Bead.Color = m_Before;
            m_Bead.Emission = 3.0f;
            Debug.Log($"{animator.Entity.Name}: {stateInfo.Name} left");
        }
    }
}
