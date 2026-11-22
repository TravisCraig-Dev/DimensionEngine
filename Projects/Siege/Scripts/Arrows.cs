using System;
using System.Collections.Generic;
using System.Numerics;
using Dimension;
using Random = System.Random;

namespace Siege
{
    /// <summary>Every arrow in the battle: a pool of arrow entities flying ballistic arcs (gravity only; they turn
    /// along their flight), striking the soldiers they come down on or sticking in the ground where they land. All
    /// share one mesh, so the engine draws hundreds in a few instanced draws.</summary>
    internal sealed class ArrowSystem
    {
        private struct Arrow
        {
            public Entity E;
            public Transform X;
            public Vector3 P, V;
            public float Age;
            public byte State; // 0 free, 1 flying, 2 stuck
            public Side From;
        }

        private const float Gravity = 9.81f;
        private const float StuckSeconds = 35.0f;
        private readonly Battle m_Battle;
        private readonly Arrow[] m_Arrows;
        private readonly Stack<int> m_Free = new();
        private readonly List<int> m_Live = new();
        private readonly List<Soldier> m_Near = new();

        public int InFlight { get; private set; }

        /// <summary>The middle of the arrows in the air and their average velocity (false when none fly).</summary>
        public bool FlyingCenter(out Vector3 center, out Vector3 velocity)
        {
            center = velocity = Vector3.Zero;
            int n = 0;
            foreach (int i in m_Live)
                if (m_Arrows[i].State == 1) { center += m_Arrows[i].P; velocity += m_Arrows[i].V; ++n; }
            if (n == 0) return false;
            center /= n;
            velocity /= n;
            return true;
        }

        public ArrowSystem(Battle battle, Entity template, int capacity)
        {
            m_Battle = battle;
            m_Arrows = new Arrow[capacity];
            for (int i = capacity - 1; i >= 0; --i)
            {
                Entity e = Entity.Instantiate(template, new Vector3(0, -500, 0), Quaternion.Identity);
                e.SetActive(false);
                m_Arrows[i] = new Arrow { E = e, X = e.Transform };
                m_Free.Push(i);
            }
        }

        /// <summary>Looses an arrow from `from` to land on `target` after `flight` seconds.</summary>
        public void Fire(Vector3 from, Vector3 target, float flight, Side side)
        {
            int i;
            if (m_Free.Count > 0) i = m_Free.Pop();
            else
            {
                // out of arrows: take back the one stuck longest
                int oldest = -1;
                float age = -1;
                foreach (int k in m_Live)
                    if (m_Arrows[k].State == 2 && m_Arrows[k].Age > age) { age = m_Arrows[k].Age; oldest = k; }
                if (oldest < 0) return;
                m_Live.Remove(oldest);
                i = oldest;
            }
            Vector3 d = target - from;
            Vector3 v = new(d.X / flight, (d.Y + 0.5f * Gravity * flight * flight) / flight, d.Z / flight);
            ref Arrow a = ref m_Arrows[i];
            a.P = from;
            a.V = v;
            a.Age = 0;
            a.State = 1;
            a.From = side;
            a.E.SetActive(true);
            Orient(ref a);
            m_Live.Add(i);
        }

        public void Update(float dt)
        {
            int flying = 0;
            for (int n = m_Live.Count - 1; n >= 0; --n)
            {
                int i = m_Live[n];
                ref Arrow a = ref m_Arrows[i];
                a.Age += dt;
                if (a.State == 2)
                {
                    if (a.Age > StuckSeconds) Release(n, i);
                    continue;
                }
                ++flying;
                a.V.Y -= Gravity * dt;
                a.P += a.V * dt;
                m_Battle.Audio.ArrowNear(a.P, a.V);
                if (a.P.Y < 80.0f) // near the plains: check what it comes down on
                {
                    float ground = m_Battle.GroundAt(a.P.X, a.P.Z);
                    float above = a.P.Y - ground;
                    if (above < 1.9f && a.V.Y < 0)
                    {
                        Soldier hit = null;
                        m_Battle.Grid.Collect(new Vector2(a.P.X, a.P.Z), 0.55f, m_Near);
                        foreach (var s in m_Near)
                            if (s.Side != a.From && !s.Dead && (above > 0.15f || s.Mounted)) { hit = s; break; }
                        if (hit != null)
                        {
                            if (m_Battle.Rng.NextDouble() < 0.45) m_Battle.Kill(hit, new Vector2(a.P.X, a.P.Z) - new Vector2(a.V.X, a.V.Z));
                            else m_Battle.Wound(hit, new Vector2(a.P.X - a.V.X, a.P.Z - a.V.Z), 1);
                            Release(n, i);
                            continue;
                        }
                    }
                    if (above <= 0)
                    {
                        // stuck in the ground, a third of the shaft in
                        a.P -= Vector3.Normalize(a.V) * 0.25f;
                        a.State = 2;
                        a.Age = 0;
                    }
                }
                Orient(ref a);
            }
            InFlight = flying;
        }

        private static void Orient(ref Arrow a)
        {
            // Arrow.gltf points along -Z: pitch up by the climb, then turn to the heading (as a camera looks).
            float h = MathF.Sqrt(a.V.X * a.V.X + a.V.Z * a.V.Z);
            float yaw = MathF.Atan2(-a.V.X, -a.V.Z);
            float pitch = MathF.Atan2(a.V.Y, h);
            a.X.SetLocal(a.P, new Vector3(pitch, yaw, 0) * (180.0f / MathF.PI), Vector3.One);
        }

        private void Release(int liveIndex, int i)
        {
            m_Arrows[i].State = 0;
            m_Arrows[i].E.SetActive(false);
            m_Live.RemoveAt(liveIndex);
            m_Free.Push(i);
        }
    }
}
