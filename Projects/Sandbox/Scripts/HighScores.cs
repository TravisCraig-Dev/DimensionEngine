using System;
using System.Collections.Generic;
using Dimension;

namespace Game
{
    [Serializable]
    public class Score
    {
        public string Name = "";
        public int Points;
    }

    /// <summary>
    /// A leaderboard for a UI List View: the list view's Path names Scores, and each row's UI Bindings show
    /// item.Name, item.Points and index. A new score comes in every few seconds and takes its place in the order;
    /// only the rows in view exist, however long the list grows.
    /// </summary>
    public class HighScores : Behaviour
    {
        public List<Score> Scores = new();
        [Tooltip("Seconds between new scores")] public float Interval = 2.0f;

        private static readonly string[] s_Names = { "Ada", "Grace", "Alan", "Edsger", "Barbara", "Ken", "Dennis", "Margaret", "Linus", "Bjarne" };
        private float m_Timer;

        public override void OnCreate()
        {
            for (int i = 0; i < 40; ++i) Add();
        }

        public override void OnUpdate(float dt)
        {
            m_Timer += dt;
            if (m_Timer < Interval) return;
            m_Timer = 0.0f;
            Add();
        }

        private void Add()
        {
            var score = new Score { Name = s_Names[Dimension.Random.Range(0, s_Names.Length)], Points = Dimension.Random.Range(100, 10000) };
            int at = Scores.FindIndex(s => s.Points < score.Points);
            Scores.Insert(at < 0 ? Scores.Count : at, score);
        }
    }
}
