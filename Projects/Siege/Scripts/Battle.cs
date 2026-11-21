using System;
using System.Collections.Generic;
using System.Numerics;
using Dimension;
using Random = System.Random;

namespace Siege
{
    /// <summary>The battle on the plains below the castle. Two armies of regiments (spearmen, swordsmen, archers and
    /// mounted knights) march at each other: archers loose volleys over the lines, the attackers' catapults throw
    /// burning stones, the cavalry charges through the ranks, and the infantry closes and fights hand to hand until the
    /// survivors are reinforced from the rear. Soldiers are entities copied from the scene's inactive "Template ..."
    /// objects; the simulation itself (formations, foes, swings, hits) runs here on plain C# data.</summary>
    [AddComponentMenu("Siege/Battle")]
    public class Battle : Behaviour
    {
        [Tooltip("Infantry and archers per army")] public int ArmySize = 320;
        [Tooltip("Mounted knights per army")] public int CavalryPerSide = 24;
        [Tooltip("The attackers' catapults")] public int Catapults = 4;
        [Tooltip("Fresh regiments march in when an army falls below this share of its size")] public float ReinforceBelow = 0.6f;
        [Tooltip("Seconds of battle simulated before the first frame (0 = from the march)")] public float Prewarm = 0.0f;
        public int Seed = 7;

        internal Random Rng;
        internal readonly List<Soldier> Soldiers = new();
        internal readonly List<Regiment> Regiments = new();
        internal SpatialGrid Grid;
        internal ArrowSystem Arrows;
        internal SiegeEngines Engines;
        internal float Now;

        private Terrain m_Terrain;
        private readonly Dictionary<string, Entity> m_Templates = new();
        private readonly List<Soldier> m_Near = new();
        private readonly List<(float time, Soldier archer, Vector3 target, bool release)> m_Shots = new();
        private readonly int[] m_Initial = new int[2];
        private float m_ReinforceCheck;

        internal static readonly Vector3 HorseScale = new(0.55f, 0.55f, 0.55f);
        private static readonly string[] HorseCoats = { "Bay", "Chestnut", "Black", "Grey" }; // Tools/build_horse.py
        private const float RiderSeat = 0.62f; // the rider's root below the horse's saddle joint

        public override void OnCreate()
        {
            Rng = new Random(Seed);
            m_Terrain = Entity.Find("Valley").GetComponent<Terrain>();
            foreach (string side in new[] { "Defenders", "Attackers" })
                foreach (string kind in new[] { "Swordsman", "Spearman", "Archer", "Knight" })
                    m_Templates[$"{side}_{kind}"] = Entity.Find($"Template {side}_{kind}");
            foreach (string name in new[] { "Arrow", "Catapult", "Catapult Arm", "Shot", "Impact", "Flash" })
                m_Templates[name] = Entity.Find($"Template {name}");
            foreach (string side in new[] { "Defenders", "Attackers" })
                foreach (string coat in HorseCoats)
                    m_Templates[$"Horse {side} {coat}"] = Entity.Find($"Template Horse {side} {coat}");
            Grid = new SpatialGrid(-700, -350, 700, 700);
            Arrows = new ArrowSystem(this, m_Templates["Arrow"], 1400);
            Engines = new SiegeEngines(this, m_Templates["Catapult"], m_Templates["Catapult Arm"], m_Templates["Shot"],
                                       m_Templates["Impact"], m_Templates["Flash"]);
            Deploy();
            for (float t = 0; t < Prewarm; t += 1.0f / 30.0f) Step(1.0f / 30.0f, false);
            Debug.Log($"Battle: {Soldiers.Count} soldiers in {Regiments.Count} regiments, {Catapults} catapults");
        }

        private readonly System.Diagnostics.Stopwatch m_Watch = new(), m_Wall = System.Diagnostics.Stopwatch.StartNew();
        private int m_Frames;
        private double m_ScriptMs, m_FrameMs;

        public override void OnUpdate(float dt)
        {
            m_Watch.Restart();
            Step(Math.Min(dt, 1.0f / 20.0f), true);
            m_ScriptMs += m_Watch.Elapsed.TotalMilliseconds;
            if (++m_Frames == 30)
            {
                m_FrameMs = m_Wall.Elapsed.TotalMilliseconds;
                m_Wall.Restart();
                Debug.Log($"Battle: frame {m_FrameMs / 30:0.0} ms, battle script {m_ScriptMs / 30:0.0} ms, {Arrows.InFlight} arrows in flight");
                m_Frames = 0;
                m_ScriptMs = m_FrameMs = 0;
            }
        }

        // --- the armies -------------------------------------------------------------------------------------------

        private void Deploy()
        {
            // Defenders hold the north of the plains (toward the castle), facing south (+Z); the attackers come from the
            // south. Lines start ~110 m apart and close at a walk.
            int n = ArmySize;
            int spears = n * 32 / 100, swords = n * 38 / 100, archers = n - spears - swords;
            Vector2 south = new(0, 1), north = new(0, -1);
            // Defenders
            AddRegiment(Side.Defenders, Kind.Spearman, new(-62, 118), south, spears / 2, 14, 1.35f, 1.15f, 0);
            AddRegiment(Side.Defenders, Kind.Spearman, new(62, 118), south, spears - spears / 2, 14, 1.35f, 1.15f, 0);
            AddRegiment(Side.Defenders, Kind.Swordsman, new(-150, 112), south, swords / 3, 10, 1.5f, 1.2f, 2);
            AddRegiment(Side.Defenders, Kind.Swordsman, new(0, 104), south, swords / 3, 12, 1.5f, 1.15f, 1);
            AddRegiment(Side.Defenders, Kind.Swordsman, new(150, 112), south, swords - 2 * (swords / 3), 10, 1.5f, 1.2f, 2);
            AddRegiment(Side.Defenders, Kind.Archer, new(-70, 70), south, archers / 2, 16, 1.6f, 1.1f, 0);
            AddRegiment(Side.Defenders, Kind.Archer, new(70, 70), south, archers - archers / 2, 16, 1.6f, 1.1f, 0);
            AddRegiment(Side.Defenders, Kind.Knight, new(-255, 95), south, CavalryPerSide / 2, 6, 3.2f, 1.6f, 14, mounted: true);
            AddRegiment(Side.Defenders, Kind.Knight, new(255, 95), south, CavalryPerSide - CavalryPerSide / 2, 6, 3.2f, 1.6f, 18, mounted: true);
            // Attackers
            AddRegiment(Side.Attackers, Kind.Swordsman, new(-115, 232), north, swords / 3, 12, 1.5f, 1.3f, 0);
            AddRegiment(Side.Attackers, Kind.Swordsman, new(0, 226), north, swords / 3, 14, 1.5f, 1.3f, 0);
            AddRegiment(Side.Attackers, Kind.Swordsman, new(115, 232), north, swords - 2 * (swords / 3), 12, 1.5f, 1.3f, 0);
            AddRegiment(Side.Attackers, Kind.Spearman, new(-58, 250), north, spears / 2, 14, 1.35f, 1.25f, 1);
            AddRegiment(Side.Attackers, Kind.Spearman, new(58, 250), north, spears - spears / 2, 14, 1.35f, 1.25f, 1);
            AddRegiment(Side.Attackers, Kind.Archer, new(-75, 290), north, archers / 2, 16, 1.6f, 1.2f, 0);
            AddRegiment(Side.Attackers, Kind.Archer, new(75, 290), north, archers - archers / 2, 16, 1.6f, 1.2f, 0);
            AddRegiment(Side.Attackers, Kind.Knight, new(-240, 250), north, CavalryPerSide / 2, 6, 3.2f, 1.6f, 10, mounted: true);
            AddRegiment(Side.Attackers, Kind.Knight, new(240, 250), north, CavalryPerSide - CavalryPerSide / 2, 6, 3.2f, 1.6f, 12, mounted: true);
            foreach (var s in Soldiers) ++m_Initial[(int)s.Side];
            Engines.Deploy(Catapults, new Vector2(0, 345), north, 78.0f);
        }

        private Regiment AddRegiment(Side side, Kind kind, Vector2 anchor, Vector2 forward, int count, int columns, float spacing,
                                     float speed, float delay, bool mounted = false)
        {
            var r = new Regiment
            {
                Side = side, Kind = kind, Mounted = mounted, Anchor = anchor, Forward = Vector2.Normalize(forward), Speed = speed,
                Delay = delay, Name = $"{side} {kind}s {Regiments.Count}", NextVolley = BattleMath.Range(Rng, 1.0f, 4.0f),
            };
            Regiments.Add(r);
            for (int i = 0; i < count; ++i)
            {
                int row = i / columns, col = i % columns;
                int inRow = Math.Min(columns, count - row * columns);
                Vector2 slot = new((col - (inRow - 1) * 0.5f) * spacing, -row * spacing);
                slot += BattleMath.RandomInDisc(Rng, spacing * 0.12f);
                Spawn(r, slot, r.SlotPosition(slot));
            }
            return r;
        }

        private Soldier Spawn(Regiment r, Vector2 slot, Vector2 pos)
        {
            Soldier s = null;
            foreach (var old in Soldiers)
                if (old.Gone && old.Side == r.Side && old.Kind == r.Kind && old.Mounted == r.Mounted) { s = old; break; }
            if (s == null)
            {
                s = new Soldier { Index = Soldiers.Count, Side = r.Side, Kind = r.Kind, Mounted = r.Mounted };
                Entity body = Entity.Instantiate(m_Templates[$"{r.Side}_{r.Kind}"], new Vector3(pos.X, 0, pos.Y), Quaternion.Identity);
                s.Body = body;
                s.Xf = body.Transform;
                s.Anim = body.GetComponent<Animator>();
                if (r.Mounted)
                {
                    // A horse of the rider's side (its saddle cloth) in any coat.
                    string coat = HorseCoats[Rng.Next(HorseCoats.Length)];
                    s.Horse = Entity.Instantiate(m_Templates[$"Horse {r.Side} {coat}"], new Vector3(pos.X, 0, pos.Y), Quaternion.Identity);
                    s.HorseXf = s.Horse.Transform;
                    s.HorseAnim = s.Horse.GetComponent<Animator>();
                }
                Soldiers.Add(s);
            }
            s.Body.SetActive(true);
            if (s.Horse.IsValid) s.Horse.SetActive(true);
            s.Regiment = r;
            s.Slot = slot;
            s.Pos = pos;
            s.Vel = Vector2.Zero;
            s.Yaw = BattleMath.YawOf(r.Forward);
            s.Health = r.Kind switch { Kind.Archer => 2, Kind.Knight => 4, _ => 3 };
            s.Speed = BattleMath.Range(Rng, 0.95f, 1.08f);
            s.Dead = s.Gone = false;
            s.DeadFor = 0;
            s.Foe = null;
            s.OneShot = false;
            s.StrikeAt = -1;
            s.Cooldown = BattleMath.Range(Rng, 0.0f, 1.5f);
            s.Retarget = BattleMath.Range(Rng, 0.0f, 0.5f);
            s.Clip = "";
            s.Ground = m_Terrain.SampleHeight(new Vector3(pos.X, 0, pos.Y));
            r.Members.Add(s);
            if (s.Mounted)
            {
                Loop(s, "Sitting_Idle_Loop", 1.0f);
                s.HorseAnim.CrossFade("Idle", 0.2f);
            }
            else Loop(s, IdleClip(s), 1.0f);
            Place(s);
            return s;
        }

        // --- one step of the battle -------------------------------------------------------------------------------

        private void Step(float dt, bool visible)
        {
            Now += dt;
            Grid.Build(Soldiers);
            foreach (var r in Regiments) UpdateRegiment(r, dt);
            foreach (var s in Soldiers)
            {
                if (s.Gone) continue;
                if (s.Dead) { UpdateBody(s, dt); continue; }
                UpdateSoldier(s, dt);
            }
            Separate();
            UpdateShots(dt);
            Arrows.Update(dt);
            Engines.Update(dt);
            foreach (var s in Soldiers)
                if (!s.Dead && !s.Gone) Place(s);
            m_ReinforceCheck -= dt;
            if (m_ReinforceCheck <= 0)
            {
                m_ReinforceCheck = 6.0f;
                Reinforce(Side.Defenders);
                Reinforce(Side.Attackers);
            }
        }

        private void UpdateRegiment(Regiment r, float dt)
        {
            if (r.Delay > 0) { r.Delay -= dt; return; }
            if (r.AliveCount == 0) return;
            Regiment nearest = NearestEnemyRegiment(r, out float distance);
            if (r.Mounted)
            {
                UpdateCharge(r, dt);
                return;
            }
            if (r.Kind == Kind.Archer)
            {
                // Archers advance into range, then loose volleys at the enemy regiment they see best.
                r.Engaged = nearest != null && distance < 150.0f;
                if (!r.Engaged) r.Anchor += r.Forward * r.Speed * dt;
                r.NextVolley -= dt;
                if (r.Engaged && r.NextVolley <= 0)
                {
                    r.NextVolley = BattleMath.Range(Rng, 6.5f, 9.0f);
                    Regiment target = VolleyTarget(r);
                    if (target != null) Volley(r, target);
                }
                return;
            }
            // Infantry: march until the enemy is close, then the ranks break into the fight.
            if (!r.Engaged && nearest != null && distance < 26.0f) r.Engaged = true;
            if (!r.Engaged) r.Anchor += r.Forward * r.Speed * dt;
            else if (nearest != null) r.Anchor = Vector2.Lerp(r.Anchor, nearest.Centroid, dt * 0.05f);
        }

        private Regiment NearestEnemyRegiment(Regiment r, out float distance)
        {
            Regiment best = null;
            distance = float.MaxValue;
            Vector2 c = r.Centroid;
            foreach (var o in Regiments)
            {
                if (o.Side == r.Side || o.AliveCount == 0) continue;
                float d = Vector2.Distance(c, o.Centroid);
                if (d < distance) { distance = d; best = o; }
            }
            return best;
        }

        private Regiment VolleyTarget(Regiment archers)
        {
            // The biggest enemy regiment in range that isn't locked in melee with friends (arrows would hit them too).
            Regiment best = null;
            float score = 0;
            Vector2 c = archers.Centroid;
            foreach (var o in Regiments)
            {
                if (o.Side == archers.Side) continue;
                int alive = o.AliveCount;
                if (alive == 0) continue;
                float d = Vector2.Distance(c, o.Centroid);
                if (d > 230.0f || d < 25.0f) continue;
                float s = alive * (o.Engaged && o.Kind != Kind.Archer ? 0.15f : 1.0f) * (o.Mounted ? 0.6f : 1.0f);
                if (s > score) { score = s; best = o; }
            }
            return best;
        }

        private void Volley(Regiment archers, Regiment target)
        {
            Vector2 aim = target.Centroid;
            foreach (var a in archers.Members)
            {
                if (a.Dead || a.Foe != null) continue;
                float when = Now + BattleMath.Range(Rng, 0.0f, 1.3f);
                // aim where the target will be when the arrow lands (it flies ~3-5 s)
                float flight = Math.Clamp(Vector2.Distance(a.Pos, aim) / 36.0f, 1.6f, 5.0f);
                Vector2 p = aim + target.Velocity * flight + BattleMath.RandomInDisc(Rng, 11.0f);
                m_Shots.Add((when, a, new Vector3(p.X, 0, p.Y), false));
            }
        }

        private void UpdateShots(float dt)
        {
            for (int i = m_Shots.Count - 1; i >= 0; --i)
            {
                var (time, a, target, release) = m_Shots[i];
                if (Now < time) continue;
                m_Shots.RemoveAt(i);
                if (a.Dead || a.Foe != null) continue;
                Vector2 to = new Vector2(target.X, target.Z) - a.Pos;
                a.Yaw = BattleMath.YawOf(to);
                if (!release)
                {
                    Once(a, "Bow_Shoot", 2.0f);
                    m_Shots.Add((Now + BowRelease, a, target, true));
                    continue;
                }
                // From the bow at full draw: out in front of the archer at about head height.
                Vector2 f = BattleMath.Facing(a.Yaw);
                Vector3 from = new(a.Pos.X + f.X * 0.55f, a.Ground + 1.7f, a.Pos.Y + f.Y * 0.55f);
                target.Y = m_Terrain.SampleHeight(target);
                float flight = Math.Clamp(to.Length() / 36.0f, 1.6f, 5.0f);
                Arrows.Fire(from, target, flight, a.Side);
            }
        }

        // --- cavalry --------------------------------------------------------------------------------------------

        private void UpdateCharge(Regiment r, float dt)
        {
            // Wheel toward a target regiment, gallop through it and on for a while, then turn and go again.
            if (r.Target == null || r.Target.AliveCount == 0 || r.ChargeEnd <= 0)
            {
                Regiment best = null;
                float bestD = float.MaxValue;
                Vector2 c = r.Centroid;
                foreach (var o in Regiments)
                {
                    if (o.Side == r.Side || o.AliveCount == 0 || o.Mounted) continue;
                    float d = Vector2.Distance(c, o.Centroid) * (o.Kind == Kind.Archer ? 0.7f : 1.0f) * (o.Kind == Kind.Spearman ? 1.6f : 1.0f);
                    if (d < bestD) { bestD = d; best = o; }
                }
                if (best == null) return;
                r.Target = best;
                Vector2 to = best.Centroid - r.Anchor;
                r.ChargeEnd = to.Length() + 60.0f;
                r.Forward = Vector2.Normalize(to);
                r.Speed = 0.0f;
                r.Delay = 3.0f; // re-forming before the charge
                return;
            }
            r.Speed = Math.Min(r.Speed + dt * 2.2f, 8.5f);
            float step = r.Speed * dt;
            r.Anchor += r.Forward * step;
            r.ChargeEnd -= step;
        }

        // --- a soldier ------------------------------------------------------------------------------------------

        private static float Reach(Soldier s, Soldier foe)
        {
            float r = s.Kind == Kind.Spearman ? 2.3f : s.Kind == Kind.Archer ? 1.1f : 1.45f;
            if (foe != null && foe.Mounted) r += 0.9f;
            return r;
        }

        private const float BowRelease = 1.2f; // Bow_Shoot looses its arrow this far in (Tools/anim_author.py)

        private static string IdleClip(Soldier s) => s.Kind switch
        {
            Kind.Archer => "Idle_Loop",
            Kind.Spearman => "Spear_Rest",
            _ => "Idle_Shield_Loop",
        };

        private void UpdateSoldier(Soldier s, float dt)
        {
            Regiment r = s.Regiment;
            s.Cooldown -= dt;
            if (s.OneShot)
            {
                s.OneShotLeft -= dt;
                if (s.OneShotLeft <= 0 || s.Anim.NormalizedTime >= 0.97f) s.OneShot = false;
            }
            if (s.Mounted)
            {
                UpdateRider(s, dt);
                return;
            }
            // A swing in progress: it lands part way through.
            if (s.StrikeAt >= 0)
            {
                s.SwingTime += dt;
                if (s.SwingTime >= s.StrikeAt)
                {
                    s.StrikeAt = -1;
                    if (s.Foe != null && !s.Foe.Dead && Vector2.Distance(s.Pos, s.Foe.Pos) < Reach(s, s.Foe) + 0.9f) Strike(s, s.Foe);
                }
            }
            // Who to fight: the nearest foe close by (or anywhere near once the regiment is engaged).
            s.Retarget -= dt;
            if (s.Foe != null && s.Foe.Dead) s.Foe = null;
            if (s.Retarget <= 0)
            {
                s.Retarget = BattleMath.Range(Rng, 0.4f, 0.8f);
                float radius = r.Kind == Kind.Archer ? 6.0f : r.Engaged ? 22.0f : 10.0f;
                Soldier foe = Grid.Nearest(s.Pos, radius, s.Side == Side.Defenders ? Side.Attackers : Side.Defenders);
                if (foe != null || s.Foe == null || Vector2.Distance(s.Pos, s.Foe.Pos) > radius * 1.5f) s.Foe = foe;
            }
            Vector2 desired = Vector2.Zero;
            float faceYaw = s.Yaw;
            if (s.Foe != null)
            {
                Vector2 to = s.Foe.Pos - s.Pos;
                float d = to.Length();
                float reach = Reach(s, s.Foe);
                faceYaw = BattleMath.YawOf(to);
                if (d > reach)
                {
                    float run = d > 6 ? 3.3f : 1.8f;
                    desired = to / Math.Max(d, 0.001f) * run * s.Speed;
                }
                else if (s.Cooldown <= 0 && !s.OneShot)
                {
                    Swing(s);
                }
            }
            else
            {
                Vector2 slot = r.SlotPosition(s.Slot);
                Vector2 to = slot - s.Pos;
                float d = to.Length();
                Vector2 regimentVel = r.Velocity;
                if (d > 0.3f)
                {
                    float catchUp = Math.Min(regimentVel.Length() + d * 0.6f, 3.5f);
                    desired = to / d * catchUp;
                }
                desired += regimentVel * 0.6f;
                faceYaw = desired.LengthSquared() > 0.04f ? BattleMath.YawOf(desired) : BattleMath.YawOf(r.Forward);
            }
            if (s.OneShot) desired *= 0.15f; // swinging, struck: barely moving
            s.Vel = Vector2.Lerp(s.Vel, desired, Math.Min(1, dt * 4.0f));
            s.Pos += s.Vel * dt;
            s.Yaw = BattleMath.TurnTowards(s.Yaw, faceYaw, dt * 5.0f);
            Animate(s);
        }

        private void Animate(Soldier s)
        {
            if (s.OneShot) return;
            float v = s.Vel.Length();
            bool spear = s.Kind == Kind.Spearman;
            string ready = spear ? "Spear_Idle" : "Sword_Idle";
            if (v < 0.25f) Loop(s, s.Foe != null && s.Kind != Kind.Archer ? ready : IdleClip(s), 1.0f);
            else if (v < 2.3f) Loop(s, spear ? "Spear_Walk" : "Walk_Loop", Math.Clamp(v / 1.35f, 0.6f, 1.6f));
            else if (v < 4.8f || spear) Loop(s, spear ? "Spear_Jog" : "Jog_Fwd_Loop", Math.Clamp(v / 3.4f, 0.7f, spear ? 1.7f : 1.4f));
            else Loop(s, "Sprint_Loop", Math.Clamp(v / 6.0f, 0.7f, 1.4f));
        }

        private static readonly string[] s_SwordSwings = { "Sword_Regular_A", "Sword_Regular_B", "Sword_Regular_C", "Sword_Attack" };
        private static readonly string[] s_SpearThrusts = { "Spear_Thrust", "Spear_Thrust_Low" };

        private void Swing(Soldier s)
        {
            string clip = s.Kind switch
            {
                Kind.Spearman => s_SpearThrusts[Rng.Next(s_SpearThrusts.Length)],
                Kind.Archer => "Punch_Cross",
                _ => s_SwordSwings[Rng.Next(s_SwordSwings.Length)],
            };
            Once(s, clip, 1.3f);
            s.StrikeAt = 0.42f;
            s.SwingTime = 0;
            s.Cooldown = BattleMath.Range(Rng, 1.4f, 2.6f);
        }

        private void Strike(Soldier attacker, Soldier foe)
        {
            bool shield = foe.Kind != Kind.Archer && !foe.Mounted;
            bool facing = Vector2.Dot(BattleMath.Facing(foe.Yaw), attacker.Pos - foe.Pos) > 0;
            if (shield && facing && Rng.NextDouble() < 0.4)
            {
                if (!foe.OneShot) Once(foe, Rng.Next(2) == 0 ? "Sword_Block" : "Idle_Shield_Break", 0.8f);
                return;
            }
            if (Rng.NextDouble() > 0.6) return; // a miss
            Wound(foe, attacker.Pos, 1);
        }

        internal void Wound(Soldier s, Vector2 from, int damage, bool knockback = false)
        {
            if (s.Dead) return;
            s.Health -= damage;
            if (s.Health <= 0) { Kill(s, from); return; }
            if (knockback)
            {
                Vector2 away = s.Pos - from;
                if (away.LengthSquared() > 1e-4f) s.Pos += Vector2.Normalize(away) * 0.8f;
                Once(s, "Hit_Knockback", 1.2f);
            }
            else if (!s.OneShot || s.StrikeAt < 0) Once(s, Rng.Next(2) == 0 ? "Hit_Chest" : "Hit_Head", 0.7f);
            s.StrikeAt = -1;
        }

        internal void Kill(Soldier s, Vector2 from)
        {
            if (s.Dead) return;
            s.Dead = true;
            s.DeadFor = 0;
            s.Foe = null;
            s.StrikeAt = -1;
            s.Vel = Vector2.Zero;
            // fall away from the blow
            Vector2 away = s.Pos - from;
            if (away.LengthSquared() > 1e-4f) s.Yaw = BattleMath.YawOf(-away);
            if (s.Mounted)
            {
                s.HorseAnim.CrossFade("Death", 0.15f);
                SetLoop(s.HorseAnim, false);
            }
            SetLoop(s.Anim, false);
            s.Anim.Speed = BattleMath.Range(Rng, 0.85f, 1.15f);
            s.Anim.CrossFade("Death01", 0.12f);
            s.Clip = "Death01";
            s.OneShot = true;
            Place(s);
        }

        private void UpdateRider(Soldier s, float dt)
        {
            Regiment r = s.Regiment;
            Vector2 slot = r.SlotPosition(s.Slot);
            Vector2 to = slot - s.Pos;
            float d = to.Length();
            float speed = r.Delay > 0 ? 0.0f : r.Speed;
            Vector2 desired = r.Forward * speed;
            if (d > 0.5f) desired += to / d * Math.Min(d * 0.8f, 4.0f);
            s.Vel = Vector2.Lerp(s.Vel, desired, Math.Min(1, dt * 2.0f));
            s.Pos += s.Vel * dt;
            float v = s.Vel.Length();
            if (v > 0.3f) s.Yaw = BattleMath.TurnTowards(s.Yaw, BattleMath.YawOf(s.Vel), dt * 2.5f);
            // Riding down the infantry in the way.
            s.ChargeHit -= dt;
            if (v > 4.0f && s.ChargeHit <= 0)
            {
                Grid.Collect(s.Pos + BattleMath.Facing(s.Yaw) * 1.2f, 1.6f, m_Near);
                foreach (var o in m_Near)
                {
                    if (o.Side == s.Side || o.Mounted || o.Dead) continue;
                    if (Rng.NextDouble() < 0.55) Kill(o, s.Pos);
                    else Wound(o, s.Pos, 1, knockback: true);
                    s.ChargeHit = 0.35f;
                    break;
                }
            }
            string horseClip = v > 5.0f ? "Gallop" : v > 0.8f ? "Walk" : "Idle";
            if (s.HorseAnim.CurrentState != horseClip)
            {
                SetLoop(s.HorseAnim, true);
                s.HorseAnim.CrossFade(horseClip, 0.3f);
            }
            s.HorseAnim.Speed = horseClip == "Gallop" ? Math.Clamp(v / 8.0f, 0.7f, 1.3f) : horseClip == "Walk" ? Math.Clamp(v / 1.8f, 0.6f, 1.6f) : 1.0f;
        }

        // --- keeping apart --------------------------------------------------------------------------------------

        private void Separate()
        {
            foreach (var s in Soldiers)
            {
                if (s.Dead || s.Gone) continue;
                float mine = s.Mounted ? 1.3f : 0.42f;
                Grid.Collect(s.Pos, 3.0f, m_Near);
                foreach (var o in m_Near)
                {
                    if (o == s || o.Index < s.Index) continue;
                    float min = mine + (o.Mounted ? 1.3f : 0.42f);
                    Vector2 d = s.Pos - o.Pos;
                    float len2 = d.LengthSquared();
                    if (len2 >= min * min) continue;
                    float len = MathF.Sqrt(len2);
                    Vector2 n = len > 1e-4f ? d / len : BattleMath.RandomInDisc(Rng, 1.0f);
                    float push = (min - len) * 0.5f;
                    // horses barge infantry aside rather than the other way around
                    float ws = s.Mounted && !o.Mounted ? 0.15f : !s.Mounted && o.Mounted ? 0.85f : 0.5f;
                    s.Pos += n * push * 2 * ws;
                    o.Pos -= n * push * 2 * (1 - ws);
                }
            }
        }

        // --- the fallen -------------------------------------------------------------------------------------------

        private void UpdateBody(Soldier s, float dt)
        {
            s.DeadFor += dt;
            // The fallen lie where they fell for a while, then sink out of sight to make room for the living.
            if (s.DeadFor < 60.0f) return;
            float sink = (s.DeadFor - 60.0f) * 0.25f;
            if (sink > 2.2f)
            {
                s.Gone = true;
                s.Body.SetActive(false);
                if (s.Horse.IsValid) s.Horse.SetActive(false);
                s.Regiment?.Members.Remove(s);
                return;
            }
            Vector3 p = new(s.Pos.X, s.Ground - sink, s.Pos.Y);
            s.Xf.SetLocal(p, new Vector3(0, s.Yaw * 180.0f / MathF.PI, 0), Vector3.One);
            if (s.Horse.IsValid) s.HorseXf.SetLocal(p, new Vector3(0, s.Yaw * 180.0f / MathF.PI, 0), HorseScale);
        }

        private void Reinforce(Side side)
        {
            int alive = 0;
            foreach (var s in Soldiers) if (s.Side == side && !s.Dead && !s.Gone) ++alive;
            if (alive >= m_Initial[(int)side] * ReinforceBelow) return;
            // The weakest kind of infantry marches in from the rear in a fresh regiment.
            Kind kind = Rng.Next(3) switch { 0 => Kind.Spearman, 1 => Kind.Archer, _ => Kind.Swordsman };
            bool defenders = side == Side.Defenders;
            Vector2 forward = defenders ? new Vector2(0, 1) : new Vector2(0, -1);
            Vector2 at = defenders ? new Vector2(BattleMath.Range(Rng, -160, 160), -20) : new Vector2(BattleMath.Range(Rng, -160, 160), 400);
            int count = kind == Kind.Archer ? 32 : 48;
            AddRegiment(side, kind, at, forward, count, kind == Kind.Archer ? 16 : 12, kind == Kind.Archer ? 1.6f : 1.4f, 1.4f, 0);
        }

        // --- animation and placement -----------------------------------------------------------------------------

        internal static void SetLoop(Animator a, bool loop) => ((Component)a).SetBool("Loop", loop);

        private static void Loop(Soldier s, string clip, float speed)
        {
            if (s.Clip != clip)
            {
                SetLoop(s.Anim, true);
                s.Anim.CrossFade(clip, 0.3f);
                s.Clip = clip;
                s.Anim.Speed = speed;
            }
            else if (MathF.Abs(s.Anim.Speed - speed) > 0.08f) s.Anim.Speed = speed;
        }

        internal static void Once(Soldier s, string clip, float maxSeconds)
        {
            SetLoop(s.Anim, false);
            s.Anim.CrossFade(clip, 0.12f);
            s.Anim.Speed = 1.0f;
            s.Clip = clip;
            s.OneShot = true;
            s.OneShotLeft = maxSeconds;
        }

        private void Place(Soldier s)
        {
            s.Ground = m_Terrain.SampleHeight(new Vector3(s.Pos.X, 0, s.Pos.Y));
            float yawDeg = s.Yaw * 180.0f / MathF.PI;
            if (s.Mounted && !s.Dead)
            {
                Vector3 horse = new(s.Pos.X, s.Ground, s.Pos.Y);
                // The saddle rides on the horse's back as it moves (from the pose last drawn, relative to the horse).
                Vector3 seat = s.HorseAnim.GetBonePosition("Torso2") - s.HorseXf.Position;
                if (seat.LengthSquared() > 25.0f || seat == Vector3.Zero) seat = new Vector3(0, 1.5f, 0);
                s.HorseXf.SetLocal(horse, new Vector3(0, yawDeg, 0), HorseScale);
                Vector2 back = BattleMath.Facing(s.Yaw) * -0.12f;
                s.Xf.SetLocal(horse + seat + new Vector3(back.X, -RiderSeat, back.Y), new Vector3(0, yawDeg, 0), Vector3.One);
                return;
            }
            s.Xf.SetLocal(new Vector3(s.Pos.X, s.Ground, s.Pos.Y), new Vector3(0, yawDeg, 0), Vector3.One);
            if (s.Mounted) s.HorseXf.SetLocal(new Vector3(s.Pos.X, s.Ground, s.Pos.Y), new Vector3(0, yawDeg, 0), HorseScale);
        }

        internal float GroundAt(float x, float z) => m_Terrain.SampleHeight(new Vector3(x, 0, z));
    }
}
