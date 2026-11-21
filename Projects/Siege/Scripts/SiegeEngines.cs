using System;
using System.Collections.Generic;
using System.Numerics;
using Dimension;
using Random = System.Random;

namespace Siege
{
    /// <summary>The attackers' catapults (mangonels). Each winds its arm back, takes a stone that is set alight, and
    /// throws it: the arm whips up against the crossbar, the burning stone flies its arc trailing fire and smoke and
    /// lighting the ground beneath, and where it lands it blasts a crater into the terrain, scatters the soldiers
    /// around and leaves the earth scorched.</summary>
    internal sealed class SiegeEngines
    {
        private sealed class Catapult
        {
            public Entity Frame, Arm;
            public Transform ArmXf;
            public Vector3 Pos;
            public float Yaw;           // radians; the frame throws toward -Z turned by this
            public float Angle = Loaded;
            public int State;           // 0 winding back, 1 loaded, 2 throwing, 3 settling
            public float Timer;
            public Shot Shot;
        }

        private sealed class Shot
        {
            public Entity E;
            public Transform X;
            public Vector3 P, V;
            public bool Flying;
            public float Spin;
        }

        // Arm angles about its axle (degrees): lying back loaded, and up against the crossbar after the throw.
        private const float Loaded = 6.0f, Thrown = -126.0f;
        private static readonly Vector3 Pivot = new(0.0f, 1.05f, 0.9f);   // build_props.ARM_PIVOT
        private static readonly Vector3 Cup = new(0.0f, 0.62f, 4.15f);     // build_props.ARM_CUP, the stone's middle
        private const float Gravity = 9.81f;

        private readonly Battle m_Battle;
        private readonly Entity m_Frame, m_Arm, m_Shot, m_Impact, m_Flash;
        private readonly List<Catapult> m_Catapults = new();
        private readonly List<Shot> m_Flying = new();
        private readonly List<Soldier> m_Near = new();

        public SiegeEngines(Battle battle, Entity frame, Entity arm, Entity shot, Entity impact, Entity flash)
        {
            m_Battle = battle;
            (m_Frame, m_Arm, m_Shot, m_Impact, m_Flash) = (frame, arm, shot, impact, flash);
        }

        public void Deploy(int count, Vector2 center, Vector2 facing, float spacing)
        {
            float yaw = MathF.Atan2(-facing.X, -facing.Y);
            Vector2 right = new(-facing.Y, facing.X);
            for (int i = 0; i < count; ++i)
            {
                Vector2 p = center + right * ((i - (count - 1) * 0.5f) * spacing) + BattleMath.RandomInDisc(m_Battle.Rng, 6.0f);
                var c = new Catapult { Yaw = yaw + BattleMath.Range(m_Battle.Rng, -0.08f, 0.08f) };
                c.Pos = new Vector3(p.X, m_Battle.GroundAt(p.X, p.Y), p.Y);
                c.Frame = Entity.Instantiate(m_Frame, c.Pos, Quaternion.Identity);
                c.Frame.SetActive(true);
                c.Frame.Transform.SetLocal(c.Pos, new Vector3(0, c.Yaw * 180.0f / MathF.PI, 0), Vector3.One);
                c.Arm = Entity.Instantiate(m_Arm, Vector3.Zero, Quaternion.Identity, c.Frame);
                c.Arm.SetActive(true);
                c.ArmXf = c.Arm.Transform;
                c.State = 0;
                c.Angle = Thrown;
                c.Timer = BattleMath.Range(m_Battle.Rng, 0.0f, 6.0f); // they don't all shoot together
                m_Catapults.Add(c);
                PoseArm(c);
            }
        }

        public void Update(float dt)
        {
            foreach (var c in m_Catapults) UpdateCatapult(c, dt);
            for (int i = m_Flying.Count - 1; i >= 0; --i)
            {
                Shot s = m_Flying[i];
                s.V.Y -= Gravity * dt;
                s.P += s.V * dt;
                s.Spin += dt * 400.0f;
                s.X.SetLocal(s.P, new Vector3(s.Spin, s.Spin * 0.6f, 0), new Vector3(5.5f));
                float ground = m_Battle.GroundAt(s.P.X, s.P.Z);
                if (s.P.Y <= ground + 0.2f && s.V.Y < 0)
                {
                    m_Flying.RemoveAt(i);
                    Impact(s, new Vector3(s.P.X, ground, s.P.Z));
                }
            }
        }

        private void UpdateCatapult(Catapult c, float dt)
        {
            c.Timer -= dt;
            switch (c.State)
            {
                case 0: // winding the arm back down (the skein creaks), over a few seconds
                    c.Angle = Math.Min(Loaded, c.Angle + dt * 34.0f);
                    if (c.Angle >= Loaded && c.Timer <= 0)
                    {
                        c.State = 1;
                        c.Timer = BattleMath.Range(m_Battle.Rng, 1.5f, 3.5f);
                        c.Shot = Load(c);
                    }
                    break;
                case 1: // loaded and burning, waiting for the order
                    c.Shot.X.SetLocal(CupPosition(c), Vector3.Zero, new Vector3(5.5f));
                    if (c.Timer <= 0) { c.State = 2; c.Timer = 0; }
                    break;
                case 2: // the throw: the arm accelerates up; the stone leaves the cup near the top of the swing
                {
                    c.Timer += dt;
                    float t = Math.Min(c.Timer / 0.5f, 1.0f);
                    c.Angle = Loaded + (Thrown - Loaded) * t * t;
                    if (c.Shot != null)
                    {
                        if (t >= 0.8f) Release(c);
                        else c.Shot.X.SetLocal(CupPosition(c), Vector3.Zero, new Vector3(5.5f));
                    }
                    if (t >= 1.0f) { c.State = 3; c.Timer = 1.2f; }
                    break;
                }
                case 3: // the arm rests against the crossbar, then the crew winds it back
                    if (c.Timer <= 0) { c.State = 0; c.Timer = BattleMath.Range(m_Battle.Rng, 3.0f, 6.0f); }
                    break;
            }
            PoseArm(c);
        }

        private void PoseArm(Catapult c) =>
            c.ArmXf.SetLocal(Pivot, new Vector3(c.Angle, 0, 0), Vector3.One);

        private Vector3 CupPosition(Catapult c)
        {
            // the cup about the axle by the arm's angle, then the frame's turn and place
            float a = c.Angle * MathF.PI / 180.0f;
            float y = Cup.Y * MathF.Cos(a) - Cup.Z * MathF.Sin(a);
            float z = Cup.Y * MathF.Sin(a) + Cup.Z * MathF.Cos(a);
            Vector3 local = new Vector3(Cup.X, y, z) + Pivot;
            float cs = MathF.Cos(c.Yaw), sn = MathF.Sin(c.Yaw);
            return c.Pos + new Vector3(local.X * cs + local.Z * sn, local.Y, -local.X * sn + local.Z * cs);
        }

        private Shot Load(Catapult c)
        {
            Entity e = Entity.Instantiate(m_Shot, CupPosition(c), Quaternion.Identity);
            e.SetActive(true);
            var s = new Shot { E = e, X = e.Transform };
            s.X.SetLocal(CupPosition(c), Vector3.Zero, new Vector3(5.5f));
            return s;
        }

        private void Release(Catapult c)
        {
            Shot s = c.Shot;
            c.Shot = null;
            s.P = CupPosition(c);
            // At the regiment of defenders with the most men, somewhere in its middle.
            Regiment target = null;
            int most = 0;
            foreach (var r in m_Battle.Regiments)
            {
                if (r.Side != Side.Defenders || r.Mounted) continue;
                int n = r.AliveCount;
                if (n > most) { most = n; target = r; }
            }
            Vector2 aim = target != null ? target.Centroid + BattleMath.RandomInDisc(m_Battle.Rng, 14.0f)
                                         : new Vector2(s.P.X, s.P.Z) + new Vector2(MathF.Sin(c.Yaw), MathF.Cos(c.Yaw)) * -250.0f;
            Vector3 to = new Vector3(aim.X, m_Battle.GroundAt(aim.X, aim.Y), aim.Y) - s.P;
            float flight = 4.0f + new Vector2(to.X, to.Z).Length() / 110.0f;
            s.V = new Vector3(to.X / flight, (to.Y + 0.5f * Gravity * flight * flight) / flight, to.Z / flight);
            s.Flying = true;
            m_Flying.Add(s);
        }

        private void Impact(Shot s, Vector3 at)
        {
            // The stone stops; its fire, smoke trail and light die away before it goes.
            s.X.SetLocal(at + new Vector3(0, 0.3f, 0), new Vector3(s.Spin, 0, 0), new Vector3(5.5f));
            s.E.GetComponent<ParticleSystem>()?.Stop();
            var trail = s.E.GetComponent<TrailRenderer>();
            if (trail != null) trail.Emitting = false;
            m_Battle.Destroy(s.E, 4.0f);
            // The blast: dirt and flame thrown up, a flash, the ground torn and scorched.
            Entity impact = Entity.Instantiate(m_Impact, at, Quaternion.Identity);
            impact.SetActive(true);
            m_Battle.Destroy(impact, 9.0f);
            Entity flash = Entity.Instantiate(m_Flash, at + new Vector3(0, 2.0f, 0), Quaternion.Identity);
            flash.SetActive(true);
            m_Battle.Destroy(flash, 0.35f);
            TerrainTools.DigCrater(at, 3.4f, 1.1f, 7);
            Vector2 p = new(at.X, at.Z);
            m_Battle.Grid.Collect(p, 7.0f, m_Near);
            foreach (var o in m_Near)
            {
                float d = Vector2.Distance(o.Pos, p);
                if (d < 2.6f) m_Battle.Kill(o, p);
                else if (m_Battle.Rng.NextDouble() < 0.6) m_Battle.Wound(o, p, d < 4.5f ? 2 : 1, knockback: true);
            }
        }
    }
}
