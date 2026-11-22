using System;
using System.Collections.Generic;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using Dimension;

namespace Siege
{
    /// <summary>The battle's sounds (clips from Tools/build_audio.py): blades meeting, bows loosing, arrows passing,
    /// catapults creaking and their stones landing, the roar of the armies and the cavalry's hooves.
    ///
    /// A battle makes far more sounds than are worth playing, and most of them far from the camera. Each frame's
    /// requests are weighed by how loud they would be where the camera is (by their own distance rolloff, reaching a
    /// few hundred meters rather than the engine's 50), and only the loudest few play: each is placed a meter from the
    /// camera in its direction (panned as it should be) at the volume its distance gives it.</summary>
    internal sealed class BattleAudio
    {
        private readonly Battle m_Battle;
        private readonly Dictionary<string, AudioClip[]> m_Clips = new();
        private readonly List<(float gain, AudioClip clip, Vector3 dir)> m_Requests = new();
        private readonly AudioSource m_Crowd, m_Hooves, m_Music;
        private float m_Duck; // seconds the music stays lowered after a stone lands
        private Vector3 m_Camera;
        private float m_PassbyCooldown;

        /// <summary>False while the battle is simulated ahead (Prewarm): nothing sounds.</summary>
        public bool Live;
        /// <summary>One-shots played (for the battle's log).</summary>
        public int Played;
        private const int PerFrame = 5;

        public BattleAudio(Battle battle)
        {
            m_Battle = battle;
            foreach (var (name, count) in new[] { ("Clash_Sword", 4), ("Clash_Sword2", 3), ("Clash_Spear", 3), ("Clash_Haft", 3),
                                                  ("Swing", 3), ("Bow_Shoot", 3), ("Arrow_Passby", 3), ("Arrow_Approach", 3) })
            {
                var clips = new AudioClip[count];
                for (int i = 0; i < count; ++i) clips[i] = Clip($"Audio/SFX/{name}_{i + 1}.wav");
                m_Clips[name] = clips;
            }
            m_Clips["Impact"] = new[] { Clip("Audio/SFX/Impact_1.ogg"), Clip("Audio/SFX/Impact_2.ogg") };
            m_Clips["Creak"] = new[] { Clip("Audio/SFX/Catapult_Creak.flac") };
            m_Clips["Thud"] = new[] { Clip("Audio/SFX/Catapult_Thud.flac") };
            m_Crowd = Entity.Find("Battle Ambience").GetComponent<AudioSource>();
            m_Hooves = Entity.Find("Cavalry Hooves").GetComponent<AudioSource>();
            m_Music = Entity.Find("Music").GetComponent<AudioSource>();
        }

        /// <summary>An asset's GUID from its path under Assets/ (as Tools/siege.py makes them: the first 15 hex digits of
        /// the MD5 of the lower-case path, plus a million).</summary>
        private static AudioClip Clip(string path)
        {
            byte[] hash = MD5.HashData(Encoding.UTF8.GetBytes(path.ToLowerInvariant()));
            string hex = Convert.ToHexString(hash).Substring(0, 15);
            return new AudioClip(Convert.ToUInt64(hex, 16) + 1_000_000UL);
        }

        // --- the events -------------------------------------------------------------------------------------------

        /// <summary>A blow landing (on a blade, a shield or a body), or a swing that misses.</summary>
        public void Blow(Vector3 at, Kind attacker, bool blocked, bool missed)
        {
            if (missed) Request("Swing", at, 0.5f, 18.0f);
            else if (blocked) Request(attacker == Kind.Spearman ? "Clash_Haft" : "Clash_Sword", at, 0.9f, 30.0f);
            else Request(attacker == Kind.Spearman ? "Clash_Spear" : "Clash_Sword2", at, 0.7f, 25.0f);
        }

        public void BowLoosed(Vector3 at) => Request("Bow_Shoot", at, 0.55f, 22.0f);

        public void CatapultCreak(Vector3 at) => Request("Creak", at, 1.0f, 60.0f);
        public void CatapultThrow(Vector3 at) => Request("Thud", at, 1.0f, 70.0f);
        public void Impact(Vector3 at)
        {
            Request("Impact", at, 1.0f, 160.0f, important: true);
            if (Live && Vector3.Distance(at, m_Camera) < 200.0f) m_Duck = 1.6f; // the music steps back for the blast
        }

        /// <summary>An arrow in flight: heard as it passes close to the camera (one at a time).</summary>
        public void ArrowNear(Vector3 at, Vector3 velocity)
        {
            if (m_PassbyCooldown > 0 || !Live) return;
            Vector3 to = m_Camera - at;
            float d = to.Length();
            if (d > 9.0f || Vector3.Dot(to, velocity) < 0) return; // only ones still coming
            m_PassbyCooldown = 0.25f;
            Request(d < 4.0f ? "Arrow_Passby" : "Arrow_Approach", at, 0.8f, 6.0f, important: true);
        }

        private readonly System.Random m_Pick = new(11);

        private void Request(string name, Vector3 at, float volume, float halfDistance, bool important = false)
        {
            if (!Live) return;
            Vector3 to = at - m_Camera;
            float d = to.Length();
            // Loudness by distance: full within a few meters, halved at `halfDistance`, falling off beyond.
            float gain = volume / (1.0f + MathF.Pow(d / halfDistance, 1.6f));
            if (gain < 0.02f) return;
            var clips = m_Clips[name];
            m_Requests.Add((important ? gain + 10.0f : gain, clips[m_Pick.Next(clips.Length)], d > 1e-3f ? to / d : Vector3.UnitZ));
        }

        // --- each frame ---------------------------------------------------------------------------------------------

        /// <summary>Before the battle steps: where the camera is now.</summary>
        public void Begin(float dt)
        {
            m_Requests.Clear();
            m_PassbyCooldown -= dt;
            var camera = Camera.Main;
            if (camera != null) m_Camera = camera.Transform.Position;
        }

        /// <summary>After it: the loudest requests play; the ambience follows the fighting and the cavalry.</summary>
        public void End(float dt)
        {
            if (!Live) return;
            m_Requests.Sort((a, b) => b.gain.CompareTo(a.gain));
            for (int i = 0; i < m_Requests.Count && i < PerFrame; ++i)
            {
                var (gain, clip, dir) = m_Requests[i];
                AudioSource.PlayClipAtPoint(clip, m_Camera + dir, MathF.Min(gain >= 10.0f ? gain - 10.0f : gain, 1.0f));
                ++Played;
            }

            // The armies' roar: louder the nearer the camera is to where they fight (and the more of them there are).
            float nearest = float.MaxValue, fighting = 0;
            Regiment charging = null;
            float chargeDistance = float.MaxValue;
            foreach (var r in m_Battle.Regiments)
            {
                if (r.AliveCount == 0) continue;
                Vector2 c = r.Centroid;
                float d = Vector2.Distance(c, new Vector2(m_Camera.X, m_Camera.Z));
                if (r.Engaged)
                {
                    nearest = MathF.Min(nearest, d);
                    fighting += r.AliveCount;
                }
                if (r.Kind == Kind.Knight && r.Delay <= 0 && d < chargeDistance) // (riding: on the move or among the foe)
                {
                    charging = r;
                    chargeDistance = d;
                }
            }
            float roar = fighting > 0 ? MathF.Min(1.0f, fighting / 200.0f) / (1.0f + MathF.Pow(nearest / 120.0f, 1.5f)) : 0.0f;
            Ease(m_Crowd, 0.12f + 0.75f * roar, dt);
            // Hooves: the nearest charging regiment's, from where it is.
            if (charging != null)
            {
                Vector2 c = charging.Centroid;
                Vector3 at = new(c.X, m_Battle.GroundAt(c.X, c.Y), c.Y);
                Vector3 to = at - m_Camera;
                float d = to.Length();
                m_Hooves.Transform.Position = m_Camera + (d > 1e-3f ? to / d : Vector3.UnitZ);
                Ease(m_Hooves, MathF.Min(1.0f, charging.AliveCount / 8.0f) / (1.0f + MathF.Pow(d / 45.0f, 1.6f)), dt);
            }
            else Ease(m_Hooves, 0.0f, dt);
            // The music under it all, lowered a moment while a near stone's blast rings out.
            m_Duck -= dt;
            if (m_Music != null) Ease(m_Music, m_Duck > 0 ? 0.14f : 0.32f, dt * (m_Duck > 0 ? 4.0f : 1.0f));
        }

        private static void Ease(AudioSource source, float volume, float dt) =>
            source.Volume += (volume - source.Volume) * (1.0f - MathF.Exp(-2.0f * dt));
    }
}
