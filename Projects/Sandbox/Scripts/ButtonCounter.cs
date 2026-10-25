using Dimension;

namespace Game
{
    /// <summary>Put on a UI Text: counts clicks of another entity's UI Button through its C# event (no script needed
    /// on the button), and stops listening after Limit clicks.</summary>
    public class ButtonCounter : Behaviour
    {
        public string Button = "Spawn Button";
        public int Limit = 0; // 0 = keep counting

        private UIButton m_Button;
        [SerializeField, HideInInspector] private int m_Clicks; // kept when the scripts reload while playing

        public override void OnCreate() => Listen();

        // After a recompile while playing: m_Clicks is kept, but event listeners start over.
        public override void OnReloaded() => Listen();

        private void Listen()
        {
            m_Button = Find(Button).GetComponent<UIButton>();
            if (m_Button != null && (Limit <= 0 || m_Clicks < Limit)) m_Button.Clicked += OnButtonClicked;
            Show();
        }

        private void OnButtonClicked()
        {
            ++m_Clicks;
            if (Limit > 0 && m_Clicks >= Limit) m_Button.Clicked -= OnButtonClicked;
            Show();
        }

        private void Show()
        {
            if (GetComponent<UIText>() is { } text) text.Text = $"Clicks: {m_Clicks}";
        }
    }
}
