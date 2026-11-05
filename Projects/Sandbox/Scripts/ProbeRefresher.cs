using Dimension;

namespace Game
{
    /// <summary>
    /// Keeps a baked Reflection Probe up to date cheaply: it captures it again every Interval seconds
    /// (ReflectionProbe.RenderProbe) instead of every frame as a Realtime one would, so moving objects show up in the
    /// reflections a little later at a fraction of the cost.
    /// </summary>
    public class ProbeRefresher : Behaviour
    {
        [Tooltip("Seconds between captures")] public float Interval = 2.0f;

        private float m_Wait;

        public override void OnUpdate(float dt)
        {
            m_Wait -= dt;
            if (m_Wait > 0.0f || GetComponent<ReflectionProbe>() is not { } probe) return;
            m_Wait = Interval;
            probe.RenderProbe();
        }
    }
}
