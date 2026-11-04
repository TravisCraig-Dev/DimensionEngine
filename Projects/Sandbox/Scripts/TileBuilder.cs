using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>Put on a Tilemap: right-click paints Tile in the cell under the mouse, Shift+right-click empties it. It
    /// leaves cells where something else is (the player, a box), and the collider follows at once.</summary>
    public class TileBuilder : Behaviour
    {
        [Tooltip("The tileset's tile painted (counted row by row)")] public int Tile = 1;

        private Tilemap m_Map;

        public override void OnCreate()
        {
            m_Map = GetComponent<Tilemap>();
            if (m_Map != null && m_Map.GetCellBounds(out Vector2Int min, out Vector2Int max))
                Debug.Log($"{Name}: {m_Map.TileCount} tiles from {min} to {max}; right-click builds, Shift+right-click digs");
        }

        public override void OnUpdate(float dt)
        {
            if (m_Map == null || !Input.GetMouseButtonDown(1) || Camera.Main == null) return;
            Vector3 world = Camera.Main.ScreenToWorldPoint(new Vector3(Input.MousePosition, 1.0f));
            Vector2Int cell = m_Map.WorldToCell(world);
            bool dig = Input.GetKey(KeyCode.LeftShift) || Input.GetKey(KeyCode.RightShift);
            if (dig)
            {
                m_Map.ClearTile(cell);
                return;
            }
            if (m_Map.HasTile(cell)) return;
            // Not on top of a body: the cell's middle must be free of anything but this map.
            Vector3 center = m_Map.CellToWorld(cell);
            foreach (Entity other in Physics2D.OverlapBox(new Vector2(center.X, center.Y), new Vector2(0.9f, 0.9f)))
                if (other != Entity) return;
            m_Map.SetTile(cell, Tile);
        }
    }
}
