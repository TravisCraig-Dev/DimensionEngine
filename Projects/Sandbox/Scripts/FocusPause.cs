using Dimension;

namespace Game
{
    /// <summary>When the game's window loses focus (another window clicked, Alt+Tab) or is minimized, the game pauses:
    /// time stops (Time.TimeScale 0), the frame rate drops to IdleFrameRate to save power, and Label shows; it all comes
    /// back with the focus. Logs the game's name and version when it starts and when it closes.</summary>
    public class FocusPause : Behaviour
    {
        [Tooltip("Shown while paused")] public Entity Label;
        [Tooltip("Frames per second while paused")] public int IdleFrameRate = 15;

        private bool m_Paused;
        private float m_Scale = 1.0f;
        private int m_FrameRate = -1;

        public override void OnCreate()
        {
            if (Label.IsValid) Label.SetActive(false);
            Debug.Log($"{Application.ProductName} {Application.Version} on {Application.Platform}, " +
                      $"screen {Screen.CurrentResolution} at {Screen.Dpi:0} dpi");
        }

        public override void OnApplicationFocus(bool hasFocus) => SetPaused(!hasFocus);
        public override void OnApplicationPause(bool paused) { if (paused) SetPaused(true); }
        public override void OnApplicationQuit() =>
            Debug.Log($"Closing {Application.ProductName} after {Time.RealtimeSinceStartup:0.0} s");

        private void SetPaused(bool paused)
        {
            if (paused == m_Paused) return;
            m_Paused = paused;
            if (paused)
            {
                m_Scale = Time.TimeScale;
                m_FrameRate = Application.TargetFrameRate;
                Time.TimeScale = 0.0f;
                Application.TargetFrameRate = IdleFrameRate;
            }
            else
            {
                Time.TimeScale = m_Scale;
                Application.TargetFrameRate = m_FrameRate;
            }
            if (Label.IsValid) Label.SetActive(paused);
        }
    }
}
