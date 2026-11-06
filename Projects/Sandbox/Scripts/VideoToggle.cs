using Dimension;

namespace Game
{
    /// <summary>Clicking the object pauses its Video Player, and clicking again plays on; while paused, Shift+click steps
    /// to the next picture and Ctrl+click jumps back to the start (needs a collider for the click). Logs where it is.</summary>
    public class VideoToggle : Behaviour
    {
        private VideoPlayer m_Player;

        public override void OnCreate() => m_Player = GetComponent<VideoPlayer>();

        public override void OnMouseUpAsButton()
        {
            if (m_Player == null || EventSystem.IsPointerOverUI) return;
            bool shift = Input.GetKey(KeyCode.LeftShift) || Input.GetKey(KeyCode.RightShift);
            bool ctrl = Input.GetKey(KeyCode.LeftControl) || Input.GetKey(KeyCode.RightControl);
            if (m_Player.IsPaused && (shift || ctrl))
            {
                if (shift) m_Player.StepForward();
                else m_Player.Time = 0.0;
                Debug.Log($"Video at {m_Player.Time:0.000} s");
            }
            else if (m_Player.IsPlaying)
            {
                m_Player.Pause();
                Debug.Log($"Video paused at {m_Player.Time:0.00} s of {m_Player.Length:0.00} ({m_Player.Width}x{m_Player.Height}, " +
                          $"{m_Player.FrameRate:0} fps, {m_Player.FrameCount} pictures shown)");
            }
            else
            {
                m_Player.Play();
                Debug.Log("Video playing");
            }
        }
    }
}
