using Dimension;

namespace Game
{
    /// <summary>Attach next to a UI Input Field: greets the typed name in the Target text when Enter is pressed.</summary>
    public class Greeter : Behaviour
    {
        public string Target = "Greeting";

        public override void OnTextChanged(string text)
        {
            if (Find(Target).GetComponent<UIText>() is { } label)
                label.Text = text.Length == 0 ? "" : "...";
        }

        public override void OnSubmit(string text)
        {
            string name = text.Trim();
            if (Find(Target).GetComponent<UIText>() is { } label)
                label.Text = name.Length == 0 ? "Hello, stranger!" : $"Hello, {name}!";
            Debug.Log($"Greeter: submitted '{text}'");
        }
    }
}
