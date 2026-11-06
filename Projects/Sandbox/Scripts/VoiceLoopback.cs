using Dimension;

namespace Game
{
    /// <summary>M starts voice chat with this very machine (its own port as the peer), so you hear your microphone as
    /// the other player would: Opus-coded, over UDP, through the jitter buffer. M again stops it. Logs the packets now
    /// and then. Two machines: point Host at the other one and give each the other's port.</summary>
    public class VoiceLoopback : Behaviour
    {
        public string Host = "127.0.0.1";
        public int LocalPort = 47900;
        [Tooltip("The other side's port (the same as LocalPort: talking to yourself)")] public int RemotePort = 47900;
        [Range(0.0f, 0.2f), Tooltip("Frames quieter than this are not sent (0: always)")] public float ActivationThreshold = 0.0f;

        private float m_NextLog;

        public override void OnUpdate(float dt)
        {
            if (Input.GetKeyDown(KeyCode.M))
            {
                if (VoiceChat.IsRunning)
                {
                    VoiceChat.Stop();
                    Debug.Log("Voice loopback off");
                }
                else if (VoiceChat.Start(LocalPort, Host, RemotePort))
                {
                    VoiceChat.ActivationThreshold = ActivationThreshold;
                    m_NextLog = Time.RealtimeSinceStartup + 2.0f;
                    Debug.Log("Voice loopback on: say something");
                }
            }
            if (VoiceChat.IsRunning && Time.RealtimeSinceStartup >= m_NextLog)
            {
                m_NextLog = Time.RealtimeSinceStartup + 2.0f;
                Debug.Log($"Voice loopback: {VoiceChat.PacketsSent} packets sent, {VoiceChat.PacketsReceived} received, {VoiceChat.PacketsLost} lost; " +
                          $"level {VoiceChat.LocalLevel:0.000} out, {VoiceChat.RemoteLevel:0.000} in");
            }
        }

        public override void OnDestroy()
        {
            if (VoiceChat.IsRunning) VoiceChat.Stop();
        }
    }
}
