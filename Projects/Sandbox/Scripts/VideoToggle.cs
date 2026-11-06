using Dimension;

namespace Game
{
    /// <summary>Clicking the object pauses its Video Player, and clicking again plays on (needs a collider for the
    /// click). Logs where it stopped.</summary>
    public class VideoToggle : Behaviour
    {
        private VideoPlayer m_Player;

        public override void OnCreate() => m_Player = GetComponent<VideoPlayer>();

        public override void OnMouseUpAsButton()
        {
            if (m_Player == null || EventSystem.IsPointerOverUI) return;
            if (m_Player.IsPlaying)
            {
                m_Player.Pause();
                Debug.Log($"Video paused at {m_Player.Time:0.00} s of {m_Player.Length:0.00} ({m_Player.Width}x{m_Player.Height}, {m_Player.FrameCount} pictures shown)");
            }
            else
            {
                m_Player.Play();
                Debug.Log("Video playing");
            }
        }
    }
}
