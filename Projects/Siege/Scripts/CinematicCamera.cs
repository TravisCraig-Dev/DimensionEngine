using System;
using System.Collections.Generic;
using System.Numerics;
using Dimension;

namespace Siege
{
    /// <summary>Films the battle as a sequence of shots that follow what is happening: a crane up toward the castle,
    /// a dolly along the front line, a tracking shot beside a charging knight, a low shot behind the archers as they
    /// loose, an orbit around the thickest of the fighting, a low shot with the castle above the armies, the view from
    /// the ramparts, and a catapult's stone followed from the throw to where it lands (which interrupts the others
    /// when a catapult is about to throw). Each shot starts with a cut; within one the camera glides after its subject
    /// with a little hand-held sway, and shakes when a stone lands near it.
    /// Keys: Right arrow the next shot, Left the one before, Space holds the current shot (or lets them run again).</summary>
    [AddComponentMenu("Siege/Cinematic Camera")]
    public class CinematicCamera : Behaviour
    {
        [Tooltip("Go on to the next shot by itself when one has run its length")] public bool AutoPlay = true;
        [Tooltip("The shot to start with (0: the establishing crane; see Sequence)")] public int FirstShot = 0;
        [Tooltip("Scales every shot's length")] public float Pace = 1.0f;
        [Tooltip("Hand-held sway, in degrees")] public float Handheld = 0.3f;
        [Tooltip("A catapult about to throw cuts to it (at most every Catapult Every seconds)")] public bool CatapultInterrupts = true;
        public float CatapultEvery = 45.0f;

        private enum ShotKind { Establishing, FrontLine, Cavalry, Volley, Melee, HeroLow, Ramparts, Catapult }

        private static readonly (ShotKind Kind, float Seconds)[] Sequence =
        {
            (ShotKind.Establishing, 14.0f), (ShotKind.FrontLine, 11.0f), (ShotKind.Cavalry, 9.0f), (ShotKind.Volley, 9.0f),
            (ShotKind.Melee, 12.0f), (ShotKind.HeroLow, 9.0f), (ShotKind.Catapult, 16.0f), (ShotKind.Ramparts, 10.0f),
        };

        // The castle's south wall (Tools/castle_layout.json): the middle of its wall walk (at 247.5 m), along it, and out.
        private static readonly Vector3 WallMiddle = new(0.62f, 247.5f, -359.25f);
        private static readonly Vector3 WallAlong = new(0.994f, 0.0f, 0.106f), WallOut = new(-0.106f, 0.0f, 0.994f);
        private static readonly Vector3 CastleView = new(0.0f, 205.0f, -430.0f); // the castle on its crag, to look at

        private Battle m_Battle;
        private Camera m_Camera;
        private int m_Index;
        private ShotKind m_Kind;
        private float m_Length, m_Time, m_Clock;
        private bool m_Hold, m_Fresh = true, m_Started;
        private float m_LastCatapult = -1e9f;

        // The camera as it moves (springs toward each frame's wish, so it eases in and out of a change of subject rather
        // than jumping to full speed) and what it looks at, with their velocities.
        private Vector3 m_Pos, m_Look, m_PosV, m_LookV;
        private float m_Fov = 34.0f, m_Roll;
        private float m_Shake;

        // Per-shot choices made at the cut.
        private Vector2 m_Side;                  // which side of the subject the camera keeps to
        private float m_Orbit;                   // the orbit's angle (Melee)
        private Regiment m_Regiment;
        private SiegeEngines.Catapult m_Catapult;
        private SiegeEngines.Shot m_Stone;
        private float m_Landed = -1.0f;          // seconds since the followed stone landed (-1: not yet)
        private bool m_SawLoaded;                // the catapult has been seen loaded (its next stone is the one to follow)
        private readonly List<Soldier> m_Fighters = new();
        private Soldier m_Lead;                   // the lead rider filmed (kept until clearly beaten)

        // What the shots aim at, kept steady as the battle wears on (soldiers falling, fights scattering, reinforcements
        // marching in): the thickest knot of fighting, chosen at each cut and kept for the shot (when its soldier
        // falls, one fighting beside him takes over; when the knot is gone the camera stays where it was until the
        // next cut, rather than sweeping off across the field to another), and the front between the armies, followed slowly.
        private Soldier m_HotAnchor;
        private bool m_HotRepick = true;
        private Vector2 m_Hot, m_FrontS, m_TowardS;
        private bool m_HotValid, m_FrontValid;
        private const float HotRadius = 14.0f;
        private float m_ArrowMix;                 // how far the volley shot's look has turned to the arrows (0..1)
        private Vector3 m_ArrowLook;              // where it last saw them

        public override void OnCreate()
        {
            m_Battle = FindObjectOfType<Battle>();
            m_Camera = GetComponent<Camera>();
            m_Pos = Transform.Position;
            m_Look = m_Pos + Transform.Forward * 100.0f;
        }

        public override void OnLateUpdate(float dt)
        {
            if (m_Battle == null || m_Battle.Engines == null || dt <= 0.0f) return;
            // The first shot once the battle is deployed (its OnCreate may run after this one's).
            if (!m_Started)
            {
                m_Started = true;
                Begin(Math.Clamp(FirstShot, 0, Sequence.Length - 1));
            }
            m_Clock += dt;
            m_Time += dt;
            if (Input.GetKeyDown(KeyCode.RightArrow)) Begin(m_Index + 1);
            else if (Input.GetKeyDown(KeyCode.LeftArrow)) Begin(m_Index - 1 + Sequence.Length);
            if (Input.GetKeyDown(KeyCode.Space)) m_Hold = !m_Hold;
            if (AutoPlay && !m_Hold && m_Time >= m_Length * Pace) Begin(m_Index + 1);
            else if (AutoPlay && !m_Hold && CatapultInterrupts && m_Kind != ShotKind.Catapult && m_Time > 3.0f &&
                     m_Clock - m_LastCatapult > CatapultEvery && AboutToThrow() != null)
                Begin(Array.FindIndex(Sequence, s => s.Kind == ShotKind.Catapult));

            // A subject lost mid-shot (the lead rider's whole regiment down, the archers gone): a cut to the next shot
            // rather than a glide off to somewhere else.
            if (AutoPlay && !m_Hold && SubjectLost()) Begin(m_Index + 1);
            Track(dt);

            Want(out Vector3 pos, out Vector3 look, out float fov, out float follow);
            // Never under the ground (or the ramparts' stone).
            float ground = m_Battle.GroundAt(pos.X, pos.Z) + 1.0f;
            if (pos.Y < ground) pos.Y = ground;
            if (m_Fresh)
            {
                m_Pos = pos;
                m_Look = look;
                m_Fov = fov;
                m_PosV = m_LookV = Vector3.Zero;
                m_Fresh = false;
                m_Camera?.Cut();
            }
            else
            {
                // (twice the follow rate: the same lag behind a steadily moving subject as a plain exponential follow)
                m_Pos = Spring(m_Pos, ref m_PosV, pos, 2.0f * follow, dt);
                m_Look = Spring(m_Look, ref m_LookV, look, 2.0f * follow, dt);
                m_Fov += (fov - m_Fov) * (1.0f - MathF.Exp(-1.5f * dt));
            }
            Shake(dt);
            Aim();
        }

        // --- the shots ------------------------------------------------------------------------------------------

        private void Begin(int index, int skipped = 0)
        {
            m_Index = index % Sequence.Length;
            (m_Kind, m_Length) = Sequence[m_Index];
            m_Time = 0.0f;
            m_Fresh = true;
            var rng = m_Battle?.Rng;
            float coin = rng != null && rng.NextDouble() < 0.5 ? -1.0f : 1.0f;
            m_Side = new Vector2(coin, 0.0f);
            m_Orbit = rng != null ? (float)rng.NextDouble() * MathF.Tau : 0.0f;
            m_Regiment = null;
            m_Catapult = null;
            m_Stone = null;
            m_Lead = null;
            m_HotRepick = true;
            m_ArrowMix = 0.0f;
            m_Landed = -1.0f;
            m_SawLoaded = false;
            if (m_Battle == null) return;
            // (a shot with nothing to film, the cavalry or archers all down, gives way to the next)
            bool skip = skipped < Sequence.Length && AutoPlay && !m_Hold;
            switch (m_Kind)
            {
                case ShotKind.Cavalry:
                    m_Regiment = Charging();
                    if (m_Regiment == null && skip) { Begin(m_Index + 1, skipped + 1); return; }
                    break;
                case ShotKind.Volley:
                    m_Regiment = NextVolley();
                    if (m_Regiment == null && skip) { Begin(m_Index + 1, skipped + 1); return; }
                    break;
                case ShotKind.Catapult:
                    m_Catapult = Soonest(out float wait);
                    // In the rotation with no throw coming soon: on to the next shot (the throws cut in by themselves).
                    if (skip && wait > 4.0f) { Begin(m_Index + 1, skipped + 1); return; }
                    m_LastCatapult = m_Clock;
                    break;
            }
        }

        private void Want(out Vector3 pos, out Vector3 look, out float fov, out float follow)
        {
            float t = m_Time, u = Math.Clamp(m_Time / (m_Length * Pace), 0.0f, 1.0f);
            float e = u * u * (3.0f - 2.0f * u); // eased
            Vector2 front = m_FrontS, toward = m_TowardS, across = new(-toward.Y, toward.X);
            switch (m_Kind)
            {
                case ShotKind.Establishing:
                {
                    // From high behind the attackers' engines, craning down and in over the field toward the castle.
                    pos = Vector3.Lerp(new Vector3(-170.0f, 120.0f, 560.0f), new Vector3(-70.0f, 45.0f, 390.0f), e);
                    look = Vector3.Lerp(new Vector3(0.0f, 60.0f, 150.0f), CastleView, e);
                    fov = 38.0f;
                    follow = 6.0f;
                    return;
                }
                case ShotKind.FrontLine:
                {
                    // A slow dolly past the fighting (or the front, before it starts), a little on the defenders' side,
                    // looking along the line.
                    Vector2 c = Fighting(front);
                    float along = -35.0f + 4.5f * t;
                    Vector2 p = c + across * along * m_Side.X - toward * 14.0f;
                    Vector2 l = c + across * (along + 28.0f) * m_Side.X + toward * 3.0f;
                    pos = At(p, 5.0f);
                    look = At(l, 1.4f);
                    fov = 32.0f;
                    follow = 2.5f;
                    return;
                }
                case ShotKind.Cavalry:
                {
                    // Riding beside the knight at the head of a charge, keeping pace.
                    Soldier lead = m_Lead = Lead(m_Regiment, m_Lead);
                    if (lead == null) { Wide(front, toward, out pos, out look, out fov, out follow); return; }
                    Vector2 f = BattleMath.Facing(lead.Yaw), right = new(-f.Y, f.X);
                    Vector2 p = lead.Pos + right * 9.0f * m_Side.X - f * 3.0f;
                    pos = At(p, 2.4f);
                    look = At(lead.Pos + f * 9.0f, 1.6f);
                    fov = 40.0f;
                    follow = 5.0f;
                    return;
                }
                case ShotKind.Volley:
                {
                    // Low behind the archers, looking up over them as the arrows climb; then after the arrows.
                    if (m_Regiment == null || m_Regiment.AliveCount == 0) { Wide(front, toward, out pos, out look, out fov, out follow); return; }
                    Vector2 c = m_Regiment.Centroid, f = m_Regiment.Forward, right = m_Regiment.Right;
                    pos = At(c - f * 14.0f + right * 7.0f * m_Side.X, 2.2f);
                    look = At(c + f * 70.0f, 0.0f) + new Vector3(0.0f, 16.0f, 0.0f);
                    // (turning to the arrows and back over a second or so as volleys go up and land)
                    bool flying = m_Battle.Arrows.FlyingCenter(out Vector3 arrows, out Vector3 v) && t > 1.0f;
                    if (flying) m_ArrowLook = arrows + v * 0.4f;
                    m_ArrowMix = Math.Clamp(m_ArrowMix + (flying ? 1.0f : -1.0f) * Time.DeltaTime, 0.0f, 1.0f);
                    if (m_ArrowMix > 0.0f) look = Vector3.Lerp(look, m_ArrowLook, 0.6f * m_ArrowMix * m_ArrowMix * (3.0f - 2.0f * m_ArrowMix));
                    fov = 42.0f;
                    follow = 2.0f;
                    return;
                }
                case ShotKind.Melee:
                {
                    // A slow orbit around the thickest of the fighting.
                    Vector2 c = Fighting(front);
                    float a = m_Orbit + t * 0.09f * m_Side.X;
                    Vector2 p = c + new Vector2(MathF.Cos(a), MathF.Sin(a)) * 17.0f;
                    pos = At(p, 3.2f);
                    look = At(c, 1.1f);
                    fov = 36.0f;
                    follow = 1.5f;
                    return;
                }
                case ShotKind.HeroLow:
                {
                    // Low on the attackers' side of the fighting, pushing slowly in, the castle on its crag above it.
                    Vector2 c = Fighting(front);
                    Vector2 toCastle = Vector2.Normalize(new Vector2(CastleView.X, CastleView.Z) - c);
                    Vector2 p = c - toCastle * (17.0f - 1.0f * t) + new Vector2(-toCastle.Y, toCastle.X) * 5.0f * m_Side.X;
                    pos = At(p, 1.4f);
                    // Level, tilted up a little: the fighting low in the frame, the castle high in it.
                    Vector3 flat = new(toCastle.X, 0.0f, toCastle.Y);
                    look = pos + flat * 100.0f + new Vector3(0.0f, 11.0f, 0.0f);
                    fov = 42.0f;
                    follow = 2.0f;
                    return;
                }
                case ShotKind.Ramparts:
                {
                    // Gliding along the outside of the castle's south wall at the height of its battlements: it opens on
                    // the tower and merlons, then pans down and out to the fighting on the plains far below.
                    float sign = -1.0f; // (this side: the pan ends over the plains, not the east ridge)
                    pos = WallMiddle - WallAlong * sign * (22.0f - 1.2f * t) + WallOut * 7.0f + new Vector3(0.0f, 3.5f, 0.0f);
                    Vector3 alongWall = WallMiddle + WallAlong * sign * 30.0f + WallOut * 10.0f + new Vector3(0.0f, -6.0f, 0.0f);
                    look = Vector3.Lerp(alongWall, At(Fighting(front), 0.0f), e);
                    fov = 44.0f;
                    follow = 1.5f;
                    return;
                }
                default: // Catapult
                    CatapultShot(t, front, toward, out pos, out look, out fov, out follow);
                    return;
            }
        }

        private void CatapultShot(float t, Vector2 front, Vector2 toward, out Vector3 pos, out Vector3 look, out float fov, out float follow)
        {
            var engines = m_Battle.Engines;
            SiegeEngines.Catapult c = m_Catapult;
            if (c == null) { Wide(front, toward, out pos, out look, out fov, out follow); return; }
            // The stone this catapult threw: seen in its cup, then the newest in the air once it has let go.
            if (c.Shot != null) m_SawLoaded = true;
            if (m_Stone == null && m_SawLoaded && c.Shot == null && engines.Flying.Count > 0 && m_Landed < 0.0f)
                m_Stone = engines.Flying[engines.Flying.Count - 1];
            if (m_Stone != null && m_Landed < 0.0f && !Contains(engines.Flying, m_Stone)) m_Landed = 0.0f;
            if (m_Landed >= 0.0f)
            {
                // Landed: watch the blast from where the chase ended, settling back a little.
                m_Landed += Time.DeltaTime;
                pos = m_Pos + new Vector3(0.0f, 0.03f, 0.0f);
                look = engines.LastImpact + new Vector3(0.0f, 2.0f, 0.0f);
                fov = 38.0f;
                follow = 2.0f;
                if (m_Landed > 3.5f && AutoPlay && !m_Hold) m_Time = m_Length * Pace; // on to the next shot
                return;
            }
            if (m_Stone != null)
            {
                // The chase: behind and above the burning stone, keeping it ahead.
                Vector3 v = m_Stone.V;
                Vector3 back = Vector3.Normalize(new Vector3(v.X, 0.0f, v.Z));
                pos = m_Stone.P - back * 26.0f + new Vector3(0.0f, 7.0f, 0.0f) + Side3(back) * 6.0f * m_Side.X;
                look = m_Stone.P + v * 0.35f;
                fov = 44.0f;
                follow = 4.0f;
                return;
            }
            // Before the throw: beside and behind the engine, looking where it throws.
            Vector3 f = new(-MathF.Sin(c.Yaw), 0.0f, -MathF.Cos(c.Yaw));
            pos = c.Pos - f * 11.0f + Side3(f) * 7.0f * m_Side.X + new Vector3(0.0f, 3.0f, 0.0f);
            look = c.Pos + f * 14.0f + new Vector3(0.0f, 4.0f, 0.0f);
            fov = 40.0f;
            follow = 3.0f;
            // Waiting too long for this one: give the shot its full length, not forever.
            if (t > 10.0f && c.State < 2) m_Time = m_Length * Pace;
        }

        // A wide view of the field when a shot's subject is missing.
        private void Wide(Vector2 front, Vector2 toward, out Vector3 pos, out Vector3 look, out float fov, out float follow)
        {
            pos = At(front - toward * 90.0f + new Vector2(60.0f * m_Side.X, 0.0f), 28.0f);
            look = At(front, 0.0f);
            fov = 36.0f;
            follow = 2.0f;
        }

        // --- what to film -----------------------------------------------------------------------------------------

        /// <summary>The middle of the front between the armies, the line along it, and the direction from the
        /// defenders toward the attackers.</summary>
        private Vector2 Front(out Vector2 across, out Vector2 toward)
        {
            Vector2 d = Centroid(Side.Defenders, infantry: true) ?? new Vector2(0.0f, 100.0f);
            Vector2 a = Centroid(Side.Attackers, infantry: true) ?? new Vector2(0.0f, 240.0f);
            Vector2 between = a - d;
            toward = between.LengthSquared() > 1.0f ? Vector2.Normalize(between) : new Vector2(0.0f, 1.0f);
            across = new Vector2(-toward.Y, toward.X);
            return (d + a) * 0.5f;
        }

        private Vector2? Centroid(Side side, bool infantry)
        {
            Vector2 sum = Vector2.Zero;
            int n = 0;
            foreach (var s in m_Battle.Soldiers)
                if (!s.Dead && !s.Gone && s.Side == side && (!infantry || !s.Mounted)) { sum += s.Pos; ++n; }
            return n > 0 ? sum / n : null;
        }

        /// <summary>The thickest fighting (the front when nobody fights yet), followed slowly.</summary>
        private Vector2 Fighting(Vector2 front) => m_HotValid ? m_Hot : front;

        private bool SubjectLost() => (m_Kind == ShotKind.Cavalry || m_Kind == ShotKind.Volley) && m_Time > 0.5f &&
                                      (m_Regiment == null || m_Regiment.AliveCount == 0);

        /// <summary>The front and the thickest fighting this frame, eased toward from where they were.</summary>
        private void Track(float dt)
        {
            Vector2 front = Front(out _, out Vector2 toward);
            if (!m_FrontValid) { m_FrontS = front; m_TowardS = toward; m_FrontValid = true; }
            float slow = 1.0f - MathF.Exp(-dt / 3.0f);
            m_FrontS += (front - m_FrontS) * slow;
            Vector2 t = m_TowardS + (toward - m_TowardS) * slow;
            m_TowardS = t.LengthSquared() > 1e-6f ? Vector2.Normalize(t) : toward;

            // Fighting: a foe of his own, or his regiment in the fight (foes are dropped and picked up again all the time).
            m_Fighters.Clear();
            foreach (var s in m_Battle.Soldiers)
                if (!s.Dead && !s.Gone && !s.Mounted && (s.Foe != null || s.Regiment != null && s.Regiment.Engaged)) m_Fighters.Add(s);
            bool anchorOk = m_HotAnchor != null && !m_HotAnchor.Dead && !m_HotAnchor.Gone;
            if (m_HotRepick)
            {
                // At the cut: the soldier with the most fighters around him (the camera cuts there, no glide); with no
                // fighting yet, the front for this shot.
                m_HotRepick = false;
                if (m_Fighters.Count < 4) { m_HotAnchor = null; m_HotValid = false; return; }
                Soldier best = null;
                int bestCount = 0;
                foreach (var a in m_Fighters)
                {
                    int n = Around(a.Pos);
                    if (n > bestCount) { bestCount = n; best = a; }
                }
                m_HotAnchor = best;
                anchorOk = true;
                m_HotValid = false; // (taken as it is below)
            }
            else if (!anchorOk && m_HotAnchor != null)
            {
                // Its soldier fell: the nearest fighter beside him takes over (the same knot), if any.
                Soldier next = null;
                float nearest = HotRadius * HotRadius;
                foreach (var a in m_Fighters)
                {
                    float d2 = Vector2.DistanceSquared(a.Pos, m_HotAnchor.Pos);
                    if (d2 < nearest) { nearest = d2; next = a; }
                }
                m_HotAnchor = next;
                anchorOk = next != null;
            }
            if (!anchorOk) return; // (the knot is gone: the camera stays on where it was)
            // Its middle: the fighters around its soldier.
            Vector2 sum = Vector2.Zero;
            int count = 0;
            foreach (var s in m_Fighters)
                if (Vector2.DistanceSquared(s.Pos, m_HotAnchor.Pos) < HotRadius * HotRadius) { sum += s.Pos; ++count; }
            Vector2 middle = count > 0 ? sum / count : m_HotAnchor.Pos;
            if (!m_HotValid) { m_Hot = middle; m_HotValid = true; }
            else m_Hot += (middle - m_Hot) * (1.0f - MathF.Exp(-dt / 1.5f));
        }

        private int Around(Vector2 p)
        {
            int n = 0;
            foreach (var s in m_Fighters) if (Vector2.DistanceSquared(s.Pos, p) < HotRadius * HotRadius) ++n;
            return n;
        }

        private Regiment Charging()
        {
            Regiment best = null;
            float bestScore = -1.0f;
            foreach (var r in m_Battle.Regiments)
            {
                if (!r.Mounted || r.AliveCount == 0) continue;
                float score = r.Speed + (r.Delay > 0.0f ? 0.0f : 1.0f);
                if (score > bestScore) { bestScore = score; best = r; }
            }
            return best;
        }

        /// <summary>The rider farthest ahead, or `current` while it lives and is within a few meters of that one.</summary>
        private static Soldier Lead(Regiment r, Soldier current)
        {
            if (r == null) return null;
            Soldier best = null;
            float most = float.MinValue;
            foreach (var s in r.Members)
            {
                if (s.Dead) continue;
                float ahead = Vector2.Dot(s.Pos - r.Anchor, r.Forward);
                if (ahead > most) { most = ahead; best = s; }
            }
            if (current != null && !current.Dead && r.Members.Contains(current) &&
                Vector2.Dot(current.Pos - r.Anchor, r.Forward) > most - 6.0f)
                return current;
            return best;
        }

        private Regiment NextVolley()
        {
            Regiment best = null;
            foreach (var r in m_Battle.Regiments)
            {
                if (r.Kind != Kind.Archer || r.AliveCount == 0) continue;
                if (best == null || (r.Engaged && !best.Engaged) || (r.Engaged == best.Engaged && r.NextVolley < best.NextVolley)) best = r;
            }
            return best;
        }

        private SiegeEngines.Catapult AboutToThrow()
        {
            foreach (var c in m_Battle.Engines.Catapults)
                if (c.State == 1 && c.Timer < 1.8f) return c;
            return null;
        }

        /// <summary>The catapult that will throw next, and about how long until it does.</summary>
        private SiegeEngines.Catapult Soonest(out float wait)
        {
            SiegeEngines.Catapult best = null;
            wait = float.MaxValue;
            foreach (var c in m_Battle.Engines.Catapults)
            {
                float t = SiegeEngines.SecondsToThrow(c);
                if (t < wait) { wait = t; best = c; }
            }
            return best;
        }

        private static bool Contains(IReadOnlyList<SiegeEngines.Shot> list, SiegeEngines.Shot s)
        {
            foreach (var x in list) if (x == s) return true;
            return false;
        }

        // --- the camera itself ------------------------------------------------------------------------------------

        /// <summary>A critically damped spring's exact step toward `target` (velocity `v` carried between frames).</summary>
        private static Vector3 Spring(Vector3 x, ref Vector3 v, Vector3 target, float omega, float dt)
        {
            float decay = MathF.Exp(-omega * dt);
            Vector3 d = x - target;
            Vector3 temp = (v + omega * d) * dt;
            v = (v - omega * temp) * decay;
            return target + (d + temp) * decay;
        }

        private Vector3 At(Vector2 p, float height) => new(p.X, m_Battle.GroundAt(p.X, p.Y) + height, p.Y);

        private static Vector3 Side3(Vector3 forward) => Vector3.Normalize(new Vector3(-forward.Z, 0.0f, forward.X));

        private void Shake(float dt)
        {
            // A stone landing near the camera jolts it (more the nearer), dying away over a second or so.
            var engines = m_Battle.Engines;
            if (engines.LastImpactTime > m_Battle.Now - dt * 1.5f && engines.LastImpactTime <= m_Battle.Now)
            {
                float d = Vector3.Distance(engines.LastImpact, m_Pos);
                m_Shake = MathF.Max(m_Shake, Math.Clamp(1.0f - d / 120.0f, 0.0f, 1.0f) * 2.2f);
            }
            m_Shake *= MathF.Exp(-2.8f * dt);
        }

        private void Aim()
        {
            Vector3 d = m_Look - m_Pos;
            float flat = MathF.Sqrt(d.X * d.X + d.Z * d.Z);
            float yaw = MathF.Atan2(-d.X, -d.Z) * (180.0f / MathF.PI);
            float pitch = MathF.Atan2(d.Y, MathF.Max(flat, 1e-4f)) * (180.0f / MathF.PI);
            // Hand-held: a slow wander of a few tenths of a degree (sums of unrelated sines), plus any shake.
            float c = m_Clock;
            float sway = Handheld, jolt = m_Shake;
            yaw += sway * (MathF.Sin(c * 0.71f) + 0.5f * MathF.Sin(c * 1.93f + 1.3f)) + jolt * MathF.Sin(c * 31.0f);
            pitch += sway * (MathF.Sin(c * 0.53f + 2.1f) + 0.5f * MathF.Sin(c * 1.37f + 0.4f)) + jolt * MathF.Sin(c * 27.0f + 1.7f);
            m_Roll = sway * 0.6f * MathF.Sin(c * 0.37f + 0.8f) + jolt * 0.5f * MathF.Sin(c * 23.0f);
            Transform.SetLocal(m_Pos, new Vector3(pitch, yaw, m_Roll), Vector3.One);
            if (m_Camera != null && MathF.Abs(m_Camera.FieldOfView - m_Fov) > 0.01f) m_Camera.FieldOfView = m_Fov;
        }
    }
}
