using System.Collections;
using Dimension;

namespace Game
{
    /// <summary>
    /// A corner readout of frames and physics steps per second, from coroutines: one counts frames (WaitForEndOfFrame),
    /// one every physics step however many a frame takes (WaitForFixedUpdate), and one reports each real second
    /// (WaitForSecondsRealtime, so it goes on while the game is paused). R starts the totals over (a CustomYieldInstruction
    /// waiting for the key). The routines are started by name.
    /// </summary>
    public class FrameWatcher : Behaviour
    {
        public int Frames, Steps;
        private int m_FramesThen, m_StepsThen;
        private string m_Rates = "";

        /// <summary>Waits until a key goes down.</summary>
        private sealed class WaitForKey : CustomYieldInstruction
        {
            private readonly KeyCode m_Key;
            public WaitForKey(KeyCode key) { m_Key = key; }
            public override bool KeepWaiting => !Input.GetKeyDown(m_Key);
        }

        public override void OnCreate()
        {
            StartCoroutine(nameof(CountFrames));
            StartCoroutine(nameof(CountSteps));
            StartCoroutine(nameof(Report), 1.0f);
            StartCoroutine(nameof(ResetOnKey));
        }

        private IEnumerator CountFrames()
        {
            while (true)
            {
                yield return new WaitForEndOfFrame();
                ++Frames;
                Show();
            }
        }

        private IEnumerator CountSteps()
        {
            var step = new WaitForFixedUpdate();
            while (true)
            {
                yield return step;
                ++Steps;
            }
        }

        private IEnumerator Report(float seconds)
        {
            var wait = new WaitForSecondsRealtime(seconds);
            while (true)
            {
                yield return wait;
                m_Rates = $"{(Frames - m_FramesThen) / seconds:0} fps, {(Steps - m_StepsThen) / seconds:0} physics steps/s";
                m_FramesThen = Frames;
                m_StepsThen = Steps;
            }
        }

        private IEnumerator ResetOnKey()
        {
            while (true)
            {
                yield return new WaitForKey(KeyCode.R);
                Frames = Steps = m_FramesThen = m_StepsThen = 0;
            }
        }

        private void Show()
        {
            if (GetComponent<UIText>() is { } text) text.Text = $"{m_Rates}{(m_Rates.Length > 0 ? "; " : "")}{Frames} frames, {Steps} steps (R resets)";
        }
    }
}
