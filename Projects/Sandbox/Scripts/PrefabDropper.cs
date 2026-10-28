using System;
using System.Numerics;
using Dimension;

namespace Game
{
    // Shows a 0-1 float as a percentage (drawn by PercentDrawer).
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class PercentAttribute : Attribute { }

    // A property drawer: every [Percent] field becomes a 0-100 slider (the field keeps 0-1).
    [CustomPropertyDrawer(typeof(PercentAttribute))]
    public class PercentDrawer : PropertyDrawer
    {
        public override object OnGUI(string label, object value) => EditorGUI.Slider(label + " %", (float)value * 100.0f, 0.0f, 100.0f) / 100.0f;
    }

    /// <summary>Drops a prefab above the object every Interval seconds, each time with the given chance. The prefab is
    /// an asset field: pick it in the Inspector or drop one from the Project window.</summary>
    [AddComponentMenu("Gameplay/Prefab Dropper")]
    public class PrefabDropper : Behaviour
    {
        [Tooltip("What to drop")] public Prefab Prefab;
        public float Interval = 2.0f;
        [Percent] public float Chance = 0.75f;
        public Vector3 Offset = new(0, 2, 0);

        [SerializeField, HideInInspector] private float m_Timer;

        public override void OnUpdate(float dt)
        {
            if (!Prefab.IsValid || Interval <= 0.0f) return;
            m_Timer += dt;
            if (m_Timer < Interval) return;
            m_Timer -= Interval;
            if (Dimension.Random.Value <= Chance) Prefab.Instantiate(Transform.Position + Offset);
        }
    }
}
