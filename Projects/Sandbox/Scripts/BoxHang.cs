using Dimension;

namespace Game
{
    /// <summary>Q hangs the loose boxes still in the air (their Rigidbody 2D made Static) and drops them again (Dynamic);
    /// H makes them Heavy, or not (their Mass, so they tip the seesaw and shrug off kicks).</summary>
    public class BoxHang : Behaviour
    {
        [Tooltip("Times their mass when heavy")] public float Heavy = 6.0f;

        private Rigidbody2D[] m_Boxes;
        private bool m_Hung, m_Heavy;

        // The loose boxes when first asked (not the player, nor the jointed pendulum and seesaw).
        private Rigidbody2D[] Boxes => m_Boxes ??= System.Array.FindAll(FindObjectsOfType<Rigidbody2D>(), b =>
            b.BodyType == RigidbodyType.Dynamic && !b.Entity.HasComponent("Joint 2D") && b.Entity.GetComponent<Tether>() == null);

        public override void OnUpdate(float dt)
        {
            if (Input.GetKeyDown(KeyCode.Q))
            {
                m_Hung = !m_Hung;
                foreach (Rigidbody2D box in Boxes)
                    if (box.Entity.IsValid) box.BodyType = m_Hung ? RigidbodyType.Static : RigidbodyType.Dynamic;
                Debug.Log(m_Hung ? $"{Boxes.Length} boxes hang" : "Boxes drop");
            }
            if (Input.GetKeyDown(KeyCode.H))
            {
                m_Heavy = !m_Heavy;
                foreach (Rigidbody2D box in Boxes)
                    if (box.Entity.IsValid) box.Mass *= m_Heavy ? Heavy : 1.0f / Heavy;
                Debug.Log(m_Heavy ? "Boxes heavy" : "Boxes light");
            }
        }
    }
}
