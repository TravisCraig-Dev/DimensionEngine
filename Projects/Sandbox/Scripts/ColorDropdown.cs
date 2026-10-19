using Dimension;

namespace Game
{
    /// <summary>Applies the color picked in a UI Dropdown (options named Green, Red, Blue, Gold, ...) to another entity's Mesh Renderer.</summary>
    public class ColorDropdown : Behaviour
    {
        public string Target = "Player";

        public override void OnCreate() => Apply();
        public override void OnValueChanged(float index) => Apply();

        private void Apply()
        {
            var dropdown = GetComponent<UIDropdown>();
            var target = Entity.Find(Target);
            if (dropdown == null || !target.IsValid) return;
            var renderer = target.GetComponent<MeshRenderer>();
            if (renderer == null) return;
            renderer.Color = dropdown.SelectedText switch
            {
                "Red" => new Color(0.9f, 0.2f, 0.2f),
                "Blue" => new Color(0.25f, 0.45f, 1.0f),
                "Gold" => new Color(1.0f, 0.75f, 0.2f),
                "White" => new Color(0.95f, 0.95f, 0.95f),
                _ => new Color(0.2f, 0.85f, 0.4f), // Green
            };
        }
    }
}
