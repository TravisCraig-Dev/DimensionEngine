using System;
using System.Collections.Generic;
using System.Numerics;
using Dimension;
using Random = System.Random;

namespace Siege
{
    public enum Side { Defenders = 0, Attackers = 1 }
    public enum Kind { Swordsman = 0, Spearman = 1, Archer = 2, Knight = 3 }

    /// <summary>One soldier (and his horse, for cavalry): where he is on the plains (x, z), what he is doing, and the
    /// entities that show him.</summary>
    internal sealed class Soldier
    {
        public int Index;
        public Side Side;
        public Kind Kind;
        public bool Mounted;
        public Regiment Regiment;
        public Vector2 Slot;          // place in the regiment's formation (right, forward) from its anchor
        public Entity Body;
        public Transform Xf;
        public Animator Anim;
        public Entity Horse;
        public Transform HorseXf;
        public Animator HorseAnim;

        public Vector2 Pos, Vel;
        public float Yaw;             // radians; 0 faces +Z
        public float Ground;
        public float Speed;           // how fast he moves when he does (m/s)
        public int Health;
        public bool Dead;
        public float DeadFor;
        public bool Gone;             // a body cleared from the field (its entities can be reused)

        public Soldier Foe;
        public float Retarget;
        public float Cooldown;        // until he can strike again
        public float StrikeAt = -1;   // seconds into the swing when it lands
        public float SwingTime;
        public string Clip = "";
        public bool OneShot;          // playing a clip that must finish (a swing, a hit, a shot)
        public float OneShotLeft;
        public float ChargeHit;       // cavalry: until a rider can trample again

        public bool Alive => !Dead;
        public Vector3 Position3 => new(Pos.X, Ground, Pos.Y);
    }

    /// <summary>A block of soldiers of one kind marching and fighting together: an anchor (the middle of its front rank)
    /// that advances along Forward, each member keeping to his slot until the fighting breaks the ranks.</summary>
    internal sealed class Regiment
    {
        public Side Side;
        public Kind Kind;
        public bool Mounted;
        public string Name;
        public Vector2 Anchor, Forward;
        public float Speed;
        public float Delay;           // seconds before it moves off
        public bool Engaged;
        public float NextVolley;
        public Regiment Target;       // cavalry: the regiment it is charging; archers: the one they shoot at
        public float ChargeEnd;       // cavalry: how far past the target the charge carries on
        public readonly List<Soldier> Members = new();

        public Vector2 Right => new(-Forward.Y, Forward.X);
        public Vector2 SlotPosition(Vector2 slot) => Anchor + Right * slot.X + Forward * slot.Y;

        public int AliveCount
        {
            get { int n = 0; foreach (var s in Members) if (!s.Dead) ++n; return n; }
        }

        public Vector2 Centroid
        {
            get
            {
                Vector2 sum = Vector2.Zero;
                int n = 0;
                foreach (var s in Members) if (!s.Dead) { sum += s.Pos; ++n; }
                return n > 0 ? sum / n : Anchor;
            }
        }

        public Vector2 Velocity => Engaged || Delay > 0 ? Vector2.Zero : Forward * Speed;
    }

    /// <summary>The soldiers alive in cells of a few meters over the plains, rebuilt every frame: finding the nearest
    /// foe, the neighbours to keep apart from and who an arrow or a stone lands on.</summary>
    internal sealed class SpatialGrid
    {
        public const float Cell = 5.0f;
        public readonly float MinX, MinZ;
        public readonly int Width, Height;
        private readonly int[] m_Head;
        private int[] m_Next = Array.Empty<int>();
        private List<Soldier> m_Soldiers;

        public SpatialGrid(float minX, float minZ, float maxX, float maxZ)
        {
            MinX = minX;
            MinZ = minZ;
            Width = (int)MathF.Ceiling((maxX - minX) / Cell);
            Height = (int)MathF.Ceiling((maxZ - minZ) / Cell);
            m_Head = new int[Width * Height];
        }

        public void Build(List<Soldier> soldiers)
        {
            m_Soldiers = soldiers;
            Array.Fill(m_Head, -1);
            if (m_Next.Length < soldiers.Count) m_Next = new int[soldiers.Count * 2];
            for (int i = 0; i < soldiers.Count; ++i)
            {
                Soldier s = soldiers[i];
                if (s.Dead) continue;
                int c = CellOf(s.Pos);
                if (c < 0) continue;
                m_Next[i] = m_Head[c];
                m_Head[c] = i;
            }
        }

        private int CellOf(Vector2 p)
        {
            int x = (int)((p.X - MinX) / Cell), z = (int)((p.Y - MinZ) / Cell);
            return x < 0 || z < 0 || x >= Width || z >= Height ? -1 : z * Width + x;
        }

        /// <summary>Fills `into` with every living soldier within `radius` of `p`.</summary>
        public void Collect(Vector2 p, float radius, List<Soldier> into)
        {
            into.Clear();
            int x0 = Math.Max(0, (int)((p.X - radius - MinX) / Cell)), x1 = Math.Min(Width - 1, (int)((p.X + radius - MinX) / Cell));
            int z0 = Math.Max(0, (int)((p.Y - radius - MinZ) / Cell)), z1 = Math.Min(Height - 1, (int)((p.Y + radius - MinZ) / Cell));
            float r2 = radius * radius;
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                    for (int i = m_Head[z * Width + x]; i >= 0; i = m_Next[i])
                    {
                        Soldier s = m_Soldiers[i];
                        if (Vector2.DistanceSquared(s.Pos, p) <= r2) into.Add(s);
                    }
        }

        /// <summary>The nearest living soldier of `side` within `radius` (null if none).</summary>
        public Soldier Nearest(Vector2 p, float radius, Side side)
        {
            Soldier best = null;
            float bestD = radius * radius;
            int x0 = Math.Max(0, (int)((p.X - radius - MinX) / Cell)), x1 = Math.Min(Width - 1, (int)((p.X + radius - MinX) / Cell));
            int z0 = Math.Max(0, (int)((p.Y - radius - MinZ) / Cell)), z1 = Math.Min(Height - 1, (int)((p.Y + radius - MinZ) / Cell));
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                    for (int i = m_Head[z * Width + x]; i >= 0; i = m_Next[i])
                    {
                        Soldier s = m_Soldiers[i];
                        if (s.Side != side) continue;
                        float d = Vector2.DistanceSquared(s.Pos, p);
                        if (d < bestD) { bestD = d; best = s; }
                    }
            return best;
        }
    }

    /// <summary>Where a rider sits on his horse. The riding clips (Tools/anim_author.py: RIDE_PELVIS) put the pelvis 1 m
    /// over the rider's root; the saddle's seat is 0.4 m above the horse's Torso2 joint (at Battle.HorseScale), the hips a
    /// hand over it and a little behind the joint.</summary>
    public static class Mount
    {
        public const float PelvisHeight = 1.0f, SeatAboveTorso = 0.5f, SeatBack = -0.06f;

        /// <summary>The rider's root for the horse's Torso2 joint (world) and the yaw both face (radians).</summary>
        public static Vector3 RiderRoot(Vector3 torso2, float yaw)
        {
            Vector2 back = BattleMath.Facing(yaw) * SeatBack;
            return torso2 + new Vector3(back.X, SeatAboveTorso - PelvisHeight, back.Y);
        }
    }

    internal static class BattleMath
    {
        public static float YawOf(Vector2 dir) => MathF.Atan2(dir.X, dir.Y);
        public static Vector2 Facing(float yaw) => new(MathF.Sin(yaw), MathF.Cos(yaw));

        public static float TurnTowards(float yaw, float target, float maxStep)
        {
            float d = target - yaw;
            while (d > MathF.PI) d -= MathF.Tau;
            while (d < -MathF.PI) d += MathF.Tau;
            return yaw + Math.Clamp(d, -maxStep, maxStep);
        }

        public static Vector2 RandomInDisc(Random rng, float radius)
        {
            float a = (float)rng.NextDouble() * MathF.Tau, r = MathF.Sqrt((float)rng.NextDouble()) * radius;
            return new Vector2(MathF.Cos(a) * r, MathF.Sin(a) * r);
        }

        public static float Range(Random rng, float a, float b) => a + (float)rng.NextDouble() * (b - a);
    }
}
