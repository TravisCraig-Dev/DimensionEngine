using System;
using System.Numerics;
using Dimension;

namespace Game
{
    /// <summary>
    /// A Character Controller walker: WASD / arrows walk (relative to the camera), Space jumps, X crouches (its capsule
    /// made half as tall while playing; it stands again only where there is room). Crates it walks into get shoved
    /// (OnControllerColliderHit), and bumping its head is logged (CollisionFlags.Above).
    /// </summary>
    public class Walker : Behaviour
    {
        public float Speed = 4.0f;
        public float JumpSpeed = 5.0f;
        [Tooltip("How hard it shoves the bodies it walks into")] public float Push = 2.5f;
        [Tooltip("The mesh drawn for it (squashed while crouching)")] public Entity Body;

        private CharacterController m_Controller;
        private float m_StandHeight;
        private bool m_Crouched;

        public override void OnCreate()
        {
            m_Controller = GetComponent<CharacterController>();
            if (m_Controller != null) m_StandHeight = m_Controller.Height;
        }

        public override void OnUpdate(float dt)
        {
            if (m_Controller == null) return;
            Vector2 input = Input.GetMoveInput();
            Vector3 forward = -Vector3.UnitZ, right = Vector3.UnitX;
            if (Camera.Main is { } cam)
            {
                Vector3 f = cam.Transform.Forward;
                f.Y = 0;
                if (f.LengthSquared() > 1e-6f) { forward = Vector3.Normalize(f); right = Vector3.Cross(forward, Vector3.UnitY); }
            }
            float speed = m_Crouched ? Speed * 0.5f : Speed;
            Vector3 move = (forward * input.Y + right * input.X) * speed;
            if (Input.GetKeyDown(KeyCode.Space) && m_Controller.IsGrounded && !m_Crouched) m_Controller.Jump(JumpSpeed);
            if (Input.GetKeyDown(KeyCode.X)) SetCrouched(!m_Crouched);
            m_Controller.SimpleMove(move);
            if ((m_Controller.CollisionFlags & CollisionFlags.Above) != 0) Debug.Log("Walker bumped its head");
        }

        public override void OnControllerColliderHit(ControllerColliderHit hit)
        {
            // Shove what it walks into (not what it stands on), along the ground.
            if (hit.Rigidbody is not { } body || hit.MoveDirection.Y < -0.5f) return;
            Vector3 push = new(hit.MoveDirection.X, 0, hit.MoveDirection.Z);
            if (push.LengthSquared() < 1e-4f) return;
            body.Velocity = new Vector3(0, body.Velocity.Y, 0) + Vector3.Normalize(push) * Push;
        }

        private void SetCrouched(bool crouch)
        {
            float height = crouch ? m_StandHeight * 0.5f : m_StandHeight;
            if (!crouch)
            {
                // Room to stand: nothing but itself in the capsule it would grow into.
                float radius = m_Controller.Radius * 0.9f;
                Vector3 feet = Transform.WorldPosition + m_Controller.Center - new Vector3(0, m_Controller.Height * 0.5f, 0);
                Entity[] inTheWay = Physics.OverlapCapsule(feet + new Vector3(0, radius + 0.05f, 0), feet + new Vector3(0, m_StandHeight - radius, 0), radius,
                                                           Physics.DefaultLayers, QueryTriggerInteraction.Ignore);
                if (Array.Exists(inTheWay, e => e != Entity)) { Debug.Log("No room to stand up"); return; }
            }
            m_Crouched = crouch;
            // Shorter or taller, its feet staying where they are (the capsule's middle moves down or back).
            m_Controller.Center = new Vector3(0, (height - m_StandHeight) * 0.5f, 0);
            m_Controller.Height = height;
            if (Body.IsValid)
            {
                Body.Transform.Position = m_Controller.Center;
                Body.Transform.Scale = new Vector3(1, height / m_StandHeight, 1);
            }
        }
    }
}
