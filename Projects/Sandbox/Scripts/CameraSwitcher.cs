using System;
using Dimension;

namespace Game
{
    /// <summary>V cuts to the scene's next camera (Camera.AllCameras, in turn), so the game shows the view from there;
    /// the one shown also clears to its own background.</summary>
    public class CameraSwitcher : Behaviour
    {
        public override void OnUpdate(float dt)
        {
            if (!Input.GetKeyDown(KeyCode.V)) return;
            Camera[] cameras = Camera.AllCameras;
            if (cameras.Length < 2) return;
            int current = Array.FindIndex(cameras, c => c.IsMain);
            Camera next = cameras[(current + 1) % cameras.Length];
            Camera.Main = next;
            Debug.Log($"Showing {next.Entity.Name}");
        }
    }
}
