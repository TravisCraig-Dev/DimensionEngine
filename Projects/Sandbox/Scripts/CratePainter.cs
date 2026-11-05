using Dimension;

namespace Game
{
    /// <summary>On a camera: P paints the object under the mouse a color of its own (its MeshRenderer.Material: a copy
    /// for it alone, glowing faintly), leaving the others sharing its material as they are; O turns the shared material
    /// of what is under the mouse metallic and back (every object using it changes).</summary>
    public class CratePainter : Behaviour
    {
        [Range(0.0f, 4.0f)] public float Glow = 0.6f;

        public override void OnUpdate(float dt)
        {
            bool paint = Input.GetKeyDown(KeyCode.P), shine = Input.GetKeyDown(KeyCode.O);
            if ((!paint && !shine) || Camera.Main is not { } cam) return;
            if (!Physics.Raycast(cam.ScreenPointToRay(Input.MousePosition), out RaycastHit hit) ||
                hit.Entity.GetComponent<MeshRenderer>() is not { } renderer) return;
            if (paint)
            {
                Material own = renderer.Material; // its own copy from now on
                var color = Color.FromHSV(Dimension.Random.Range(0.0f, 1.0f), 0.7f, 1.0f);
                own.Color = color;
                own.EmissionColor = color;
                own.EmissionStrength = Glow;
                Debug.Log($"Painted {hit.Entity.Name} (its own material now)");
            }
            else if (renderer.SharedMaterial is { IsValid: true } shared)
            {
                bool metal = shared.Metallic < 0.5f;
                shared.Metallic = metal ? 0.9f : 0.0f;
                shared.Smoothness = metal ? 0.85f : 0.4f;
                Debug.Log($"Every object using {hit.Entity.Name}'s material is {(metal ? "metal" : "wood")} now");
            }
        }
    }
}
