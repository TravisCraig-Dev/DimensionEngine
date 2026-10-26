using System;
using Dimension;

namespace Game
{
    /// <summary>Drives a Vehicle from WASD/arrows or the left stick (Space = hand brake): the C# twin of the C++
    /// CarDriver. Optionally shows the speed in a UI Text.</summary>
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
            var move = Input.GetMoveInput();
            vehicle.Throttle = move.Y != 0.0f ? move.Y : CruiseThrottle;
            vehicle.Steering = move.X;
            vehicle.HandBrake = Input.GetKey(KeyCode.Space) ? 1.0f : 0.0f;
            if (SpeedText.IsValid && SpeedText.GetComponent<UIText>() is { } text)
                text.Text = $"{Math.Abs(vehicle.SpeedKmh):0} km/h  gear {vehicle.Gear}";
        }
    }
}
