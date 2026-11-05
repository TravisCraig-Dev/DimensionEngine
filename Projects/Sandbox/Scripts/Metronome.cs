using Dimension;

namespace Game
{
    /// <summary>
    /// K starts and stops a steady beat from this object's Audio Source: each tick is scheduled on the audio clock
    /// (PlayScheduled) a little ahead, so the beat stays exact whatever the frame rate. Every Accent-th tick plays
    /// higher. L pauses all sound and lets it go on (AudioListener.Pause).
    /// </summary>
    public class Metronome : Behaviour
    {
        [Tooltip("Beats per minute")] public float Tempo = 120.0f;
        [Tooltip("Every this many beats one plays higher (0: none)")] public int Accent = 4;
        [Tooltip("How far ahead each tick is scheduled, in seconds")] public float Lookahead = 0.1f;

        private AudioSource m_Source;
        private bool m_On;
        private double m_Next;
        private int m_Beat;

        public override void OnCreate() => m_Source = GetComponent<AudioSource>();

        public override void OnUpdate(float dt)
        {
            if (Input.GetKeyDown(KeyCode.L))
            {
                AudioListener.Pause = !AudioListener.Pause;
                Debug.Log(AudioListener.Pause ? "All sound paused" : "Sound goes on");
            }
            if (m_Source == null) return;
            if (Input.GetKeyDown(KeyCode.K))
            {
                m_On = !m_On;
                m_Next = AudioSettings.DspTime + Lookahead;
                m_Beat = 0;
                if (!m_On) m_Source.Stop();
                Debug.Log(m_On ? $"Metronome at {Tempo} bpm" : "Metronome stopped");
            }
            // The next tick, once the one before it is under way (one source: a tick at a time).
            if (!m_On || AudioSettings.DspTime < m_Next - Lookahead) return;
            m_Source.Pitch = Accent > 0 && m_Beat % Accent == 0 ? 1.5f : 1.0f;
            m_Source.PlayScheduled(m_Next);
            m_Source.SetScheduledEndTime(m_Next + 60.0 / System.Math.Max(Tempo, 1.0f) * 0.5);
            m_Next += 60.0 / System.Math.Max(Tempo, 1.0f);
            ++m_Beat;
        }
    }
}
