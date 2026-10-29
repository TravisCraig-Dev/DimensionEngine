using Dimension;

namespace Game
{
    /// <summary>Attach next to a UI Input Field: greets the typed name in the Target text when Enter is pressed. The
    /// name is remembered between runs (PlayerPrefs), with how often it was given (a save file).</summary>
    public class Greeter : Behaviour
    {
        public string Target = "Greeting";

        // Saved with SaveFile: fields (and lists, dictionaries...) as JSON.
        public class Visits
        {
            public string LastName = "";
            public int Count;
        }

        public override void OnCreate()
        {
            string last = PlayerPrefs.GetString("Greeter Name");
            if (last.Length > 0 && Find(Target).GetComponent<UIText>() is { } label)
                label.Text = $"Welcome back, {last}! ({SaveFile.Read("greeter", new Visits()).Count} visits)";
        }

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
            if (name.Length == 0) return;
            PlayerPrefs.SetString("Greeter Name", name);
            var visits = SaveFile.Read("greeter", new Visits());
            visits.LastName = name;
            visits.Count++;
            SaveFile.Write("greeter", visits);
        }
    }
}
