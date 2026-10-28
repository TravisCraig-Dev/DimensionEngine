using System;
using System.Collections.Generic;
using System.Numerics;
using Dimension;

namespace Game
{
    // Where a wave comes from: any mix of the four sides ([Flags]: a multi-select dropdown in the Inspector).
    [Flags]
    public enum SpawnSides { None = 0, North = 1, East = 2, South = 4, West = 8 }

    // [Serializable] types are script fields too: their fields open under the field, and lists of them work.
    [Serializable]
    public class SpawnArea
    {
        public Vector3 Center;
        [Tooltip("How far out the sides are")] public float Radius = 5.0f;
    }

    [Serializable]
    public struct Wave
    {
        public Prefab Prefab;
        [Range(1, 20)] public int Count;
        [Tooltip("Seconds after the previous wave")] public float Delay;
        public SpawnSides Sides;
    }

    /// <summary>Sends its Waves one after another: each, Delay seconds after the last, puts Count copies of its prefab
    /// at each of its Sides of the Area (around the object).</summary>
    [AddComponentMenu("Gameplay/Wave Spawner")]
    public class WaveSpawner : Behaviour
    {
        public SpawnArea Area = new();
        public List<Wave> Waves = new() { new Wave { Count = 3, Delay = 1.0f, Sides = SpawnSides.North | SpawnSides.South } };

        private int m_Next;
        private float m_Timer;

        public override void OnUpdate(float dt)
        {
            if (m_Next >= Waves.Count) return;
            m_Timer += dt;
            Wave wave = Waves[m_Next];
            if (m_Timer < wave.Delay) return;
            m_Timer -= wave.Delay;
            ++m_Next;
            if (!wave.Prefab.IsValid) return;
            Vector3 center = Transform.Position + Area.Center;
            (SpawnSides side, Vector3 direction)[] sides =
            {
                (SpawnSides.North, -Vector3.UnitZ), (SpawnSides.East, Vector3.UnitX), (SpawnSides.South, Vector3.UnitZ), (SpawnSides.West, -Vector3.UnitX),
            };
            foreach (var (side, direction) in sides)
            {
                if ((wave.Sides & side) == 0) continue;
                for (int i = 0; i < wave.Count; ++i)
                    wave.Prefab.Instantiate(center + direction * Area.Radius + new Vector3(0, 1 + i, 0));
            }
            Debug.Log($"{Name}: wave {m_Next} ({wave.Sides})");
        }
    }
}
