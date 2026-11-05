using Dimension;

namespace Game
{
    /// <summary>
    /// Shows what its object's Timeline does in a UI Text on the same object: it listens to Played / Paused / Stopped,
    /// and a timeline event named "Opened" calls <see cref="Opened"/> directly (events call the method of their name).
    /// P pauses or resumes the timeline, Backspace stops it.
    /// </summary>
    public class TimelineStatus : Behaviour
    {
        private int m_Played, m_Paused, m_Stopped, m_Opened;
        private Timeline m_Timeline;
        private UIText m_Text;

        public override void OnCreate()
        {
            m_Timeline = GetComponent<Timeline>();
            m_Text = GetComponent<UIText>();
            if (m_Timeline == null) return;
            m_Timeline.Played += () => { ++m_Played; Show(); };
            m_Timeline.Paused += () => { ++m_Paused; Show(); };
            m_Timeline.Stopped += () => { ++m_Stopped; Show(); };
        }

        // Called by a timeline event named "Opened".
        private void Opened()
        {
            ++m_Opened;
            Show();
        }

        public override void OnUpdate(float dt)
        {
            if (m_Timeline == null) return;
            if (Input.GetKeyDown(KeyCode.P))
            {
                if (m_Timeline.IsPlaying) m_Timeline.Pause();
                else m_Timeline.Resume();
            }
            if (Input.GetKeyDown(KeyCode.Backspace)) m_Timeline.Stop();
        }

        private void Show()
        {
            if (m_Text != null)
                m_Text.Text = $"played {m_Played}, paused {m_Paused}, stopped {m_Stopped}, opened {m_Opened} ({m_Timeline.Time:0.0} / {m_Timeline.Duration:0.0} s)";
        }
    }
}
