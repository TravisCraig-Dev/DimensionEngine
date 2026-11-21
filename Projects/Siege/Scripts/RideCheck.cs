using System;
using System.Numerics;
using Dimension;

namespace Siege
{
    /// <summary>Keeps a rider in a horse's saddle as Battle does (Mount), for scenes that show riding without a battle
    /// (Tools/build_ridecheck.py).</summary>
    public class RideCheck : Behaviour
    {
        [Tooltip("The horse's name")] public string Horse = "";
        [Tooltip("The way horse and rider face, in degrees")] public float Yaw = 0.0f;
        private Animator m_Horse;

        public override void OnCreate()
        {
            Entity horse = Entity.Find(Horse);
            if (horse.IsValid) m_Horse = horse.GetComponent<Animator>();
        }

        public override void OnLateUpdate(float dt)
        {
            if (m_Horse == null) return;
            float yaw = Yaw * MathF.PI / 180.0f;
            Transform.SetLocal(Mount.RiderRoot(m_Horse.GetBonePosition("Torso2"), yaw), new Vector3(0, Yaw, 0), Vector3.One);
        }
    }
}
