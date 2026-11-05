using Dimension;

namespace Game
{
    /// <summary>
    /// Flips a UI Style class on its object each time its UI Button is clicked (UIStyle.ToggleClass): the sheet's
    /// values for that class come and go with it, like a USS class in UI Toolkit.
    /// </summary>
    public class StyleToggle : Behaviour
    {
        public string Class = "lit";

        public override void OnCreate()
        {
            if (GetComponent<UIButton>() is { } button) button.Clicked += Flip;
        }

        private void Flip()
        {
            if (GetComponent<UIStyle>() is { } style) style.ToggleClass(Class, !style.HasClass(Class));
        }
    }
}
