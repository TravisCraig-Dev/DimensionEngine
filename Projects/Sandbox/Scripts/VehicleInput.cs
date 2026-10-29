using System;
using Dimension;

namespace Game
{
    /// <summary>Drives a Vehicle from the Vertical and Horizontal input actions (WASD, arrows or the left stick) with Jump
    /// (Space, A) as the hand brake: the C# twin of the C++ CarDriver. Optionally shows the speed in a UI Text.</summary>
    [AddComponentMenu("Physics/Vehicle Input (C#)"), RequireComponent(typeof(Vehicle))]
    public class VehicleInput : Behaviour
    {
        [Tooltip("Throttle held when there is no input (demos and tests)"), Range(-1.0f, 1.0f)] public float CruiseThrottle = 0.0f;
        [Tooltip("Optional UI Text that shows the speed")] public Entity SpeedText;
        [Tooltip("Replaces the Vehicle's Max Steer Angle on start (degrees; 0 = keep it)")] public float SteerAngle = 0.0f;

        public override void OnCreate()
        {
            // Any Inspector field can be set by name, also on components without a C# class of their own.
            if (SteerAngle > 0.0f) Entity.GetComponent("Vehicle")?.SetFloat("Max Steer Angle", SteerAngle);
        }

        public override void OnUpdate(float dt)
        {
            if (GetComponent<Vehicle>() is not { } vehicle) return;
            // Input actions (Project Settings > Input) rather than keys: rebinding them needs no code change.
            float throttle = Input.GetAxis("Vertical");
            vehicle.Throttle = throttle != 0.0f ? throttle : CruiseThrottle;
            vehicle.Steering = Input.GetAxis("Horizontal");
            vehicle.HandBrake = Input.GetButton("Jump") ? 1.0f : 0.0f;
            if (SpeedText.IsValid && SpeedText.GetComponent<UIText>() is { } text)
                text.Text = $"{Math.Abs(vehicle.SpeedKmh):0} km/h  gear {vehicle.Gear}";
        }
    }
}
