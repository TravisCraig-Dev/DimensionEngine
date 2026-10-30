using Dimension;

namespace Game
{
    /// <summary>Put on a UI Button: each click switches to the next language of the project's string tables (kept for
    /// the next run). UI Texts with a Localized Key, like the button's own label, follow.</summary>
    public class LanguageButton : Behaviour
    {
        public override void OnCreate() => Listen();

        // After a recompile while playing, event listeners start over.
        public override void OnReloaded() => Listen();

        private void Listen()
        {
            if (GetComponent<UIButton>() is { } button) button.Clicked += Next;
        }

        private void Next()
        {
            string[] languages = Localization.Languages;
            if (languages.Length == 0) return;
            int current = System.Array.IndexOf(languages, Localization.Language);
            Localization.Language = languages[(current + 1) % languages.Length];
            Debug.Log($"Language: {Localization.LanguageName(Localization.Language)}");
        }
    }
}
