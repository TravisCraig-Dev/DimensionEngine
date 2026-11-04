using System.Collections.Generic;
using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Press C to drop a copy of Template (with its children and scripts) somewhere in Area, turned at random;
    /// past MaxCopies the oldest copy goes.</summary>
    public class Cloner : Behaviour
    {
        public Entity Template;
        [Tooltip("Half size of the area copies fall into (around this height above the origin)")] public Vector3 Area = new(14, 4, 14);
        [Range(1, 50)] public int MaxCopies = 8;

        private readonly Queue<Entity> m_Copies = new();

        public override void OnUpdate(float dt)
        {
            if (!Input.GetKeyDown(KeyCode.C) || !Template.IsValid) return;
            Vector3 at = new(Dimension.Random.Range(-Area.X, Area.X), Area.Y, Dimension.Random.Range(-Area.Z, Area.Z));
            Quaternion turn = Quaternion.CreateFromAxisAngle(Vector3.UnitY, Dimension.Random.Range(0.0f, System.MathF.PI * 2.0f));
            Entity copy = Instantiate(Template, at, turn);
            copy.Name = $"{Template.Name} copy";
            m_Copies.Enqueue(copy);
            while (m_Copies.Count > MaxCopies) Destroy(m_Copies.Dequeue());
        }
    }
}
