using Dimension;

namespace Game
{
    /// <summary>Clicking the ground moves Marker to the clicked point (a NavMesh Agent whose Target is the marker walks
    /// there): a ray from the camera through the mouse, cast into the colliders. T toggles slow motion.</summary>
    public class ClickToMove : Behaviour
    {
        [Tooltip("Moved to the clicked point")] public Entity Marker;
        [Range(0.05f, 1.0f), Tooltip("Time scale while slowed down (T)")] public float SlowMotion = 0.25f;

        public override void OnUpdate(float dt)
        {
            if (Input.GetKeyDown(KeyCode.T)) Time.TimeScale = Time.TimeScale < 1.0f ? 1.0f : SlowMotion;
            if (!Input.GetMouseButtonDown(0) || !Marker.IsValid || Camera.Main == null) return;

            Ray ray = Camera.Main.ScreenPointToRay(Input.MousePosition);
            if (!Physics.Raycast(ray, out RaycastHit hit, 500.0f)) return;
            Marker.Transform.Position = hit.Point;
            Debug.DrawRay(ray.Origin, ray.Direction * hit.Distance, new Color(1.0f, 0.85f, 0.2f), 1.0f);
            Debug.Log($"Sent to {hit.Point.X:0.0}, {hit.Point.Z:0.0} ({hit.Entity.Name})");
        }
    }
}
