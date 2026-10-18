using System.Collections;
using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Traffic-light lamp driven by a coroutine (red, yellow, green), with an InvokeRepeating tick.
    /// Press T to flash it white three times (a coroutine started from OnUpdate, waiting on a nested routine).</summary>
    public class CoroutineLamp : Behaviour
    {
        public float RedSeconds = 1.0f;
        public float YellowSeconds = 0.5f;
        public float GreenSeconds = 1.0f;
        public float TickStep = 0.0f; // moves the lamp along +X on every tick (0 = stay put)
        public int Ticks;

        private MeshRenderer m_Renderer;
        private Coroutine m_Cycle;
        private Color m_Current = Color.White;

        public override void OnCreate()
        {
            m_Renderer = GetComponent<MeshRenderer>();
            m_Cycle = StartCoroutine(Cycle());
            InvokeRepeating(nameof(Tick), 0.25f, 0.25f);
        }

        public override void OnUpdate(float deltaTime)
        {
            if (Input.GetKeyDown(KeyCode.T)) StartCoroutine(Flash());
        }

        private IEnumerator Cycle()
        {
            while (true)
            {
                yield return Show(Color.Red, RedSeconds);
                yield return Show(new Color(1.0f, 0.8f, 0.1f), YellowSeconds);
                yield return Show(Color.Green, GreenSeconds);
            }
        }

        private IEnumerator Show(Color color, float seconds)
        {
            m_Current = color;
            if (m_Renderer != null) m_Renderer.Color = color;
            yield return new WaitForSeconds(seconds);
        }

        private IEnumerator Flash()
        {
            StopCoroutine(m_Cycle);
            for (int i = 0; i < 3; ++i)
            {
                m_Renderer.Color = Color.White;
                yield return new WaitForSeconds(0.1f);
                m_Renderer.Color = m_Current;
                yield return new WaitForSeconds(0.1f);
            }
            Debug.Log($"{Entity.Name}: flashed at t={Time.time:0.00}s, resuming the cycle");
            m_Cycle = StartCoroutine(Cycle());
        }

        private void Tick()
        {
            ++Ticks;
            if (TickStep != 0.0f) GetComponent<Transform>().Translate(new Vector3(TickStep, 0, 0));
        }
    }
}
