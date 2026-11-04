using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Watches Target along a line of sight (Physics2D.Linecast that ignores triggers, so the coins never block
    /// it): its sprite turns green while it sees Target and red when something solid is in between, and its Line Renderer
    /// draws the sight line up to the target or to whatever blocks it.</summary>
    public class Lookout : Behaviour
    {
        public Entity Target;
        public Color Sees = new(0.3f, 1.0f, 0.4f, 1.0f);
        public Color Blocked = new(1.0f, 0.3f, 0.25f, 1.0f);

        private bool m_Saw = true;

        public override void OnUpdate(float dt)
        {
            if (!Target.IsValid) return;
            Vector3 from3 = Transform.WorldPosition, to3 = Target.Transform.WorldPosition;
            Vector2 from = new(from3.X, from3.Y), to = new(to3.X, to3.Y);
            // Start just outside its own collider (a line starting inside one hits it at once).
            Vector2 start = from + Vector2.Normalize(to - from) * 0.3f;
            bool blocked = Physics2D.Linecast(start, to, out RaycastHit2D hit, Physics2D.DefaultLayers, QueryTriggerInteraction.Ignore) &&
                           hit.Entity != Target;
            Vector2 end = blocked ? hit.Point : to;
            if (GetComponent<LineRenderer>() is { } line) line.SetPositions(new[] { new Vector3(start, 0.1f), new Vector3(end, 0.1f) });
            if (GetComponent<SpriteRenderer>() is { } sprite) sprite.Color = blocked ? Blocked : Sees;
            if (blocked == m_Saw)
            {
                m_Saw = !blocked;
                Debug.Log(blocked ? $"Lookout lost sight ({hit.Entity.Name} in the way)" : "Lookout sees the player");
            }
        }
    }
}
