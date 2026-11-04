using Dimension;

namespace Game
{
    /// <summary>Put on a UI Text: counts what other scripts report (HitFlash tells it about hard hits) and shows the
    /// count with Format.</summary>
    public class Scoreboard : Behaviour
    {
        public string Format = "Hard hits: {0} (last: {1})";

        private UIText m_Text;
        private int m_Count;
        private string m_Last = "none";

        public override void OnCreate()
        {
            m_Text = GetComponent<UIText>();
            Show();
        }

        public int Count => m_Count;

        public void Add(string what)
        {
            ++m_Count;
            m_Last = what;
            Show();
        }

        private void Show()
        {
            if (m_Text != null) m_Text.Text = string.Format(Format, m_Count, m_Last);
        }
    }
}
