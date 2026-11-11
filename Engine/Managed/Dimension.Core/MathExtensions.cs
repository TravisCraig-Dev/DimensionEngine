using System;
using System.Numerics;

namespace Dimension
{
    /// <summary>
    /// Unity's vector and quaternion helpers on System.Numerics' types (C# extension members): <c>Vector3.MoveTowards</c>,
    /// <c>Quaternion.Euler</c>, <c>q.EulerAngles</c> and the rest. Angles are in degrees. Rotations follow this engine:
    /// Euler angles as the Transform's (X, Y and Z, applied as glm does), and an object's forward is its -Z.
    /// </summary>
    public static class VectorMath
    {
        private const float kEpsilon = 1e-5f;

        private static float Angle01(float cos) => MathF.Acos(Math.Clamp(cos, -1.0f, 1.0f)) * Mathf.Rad2Deg;

        extension(Vector3)
        {
            /// <summary>Up (+Y), down, right (+X), left, and this engine's forward (-Z, where a Transform faces) and back.</summary>
            public static Vector3 Up => Vector3.UnitY;
            public static Vector3 Down => -Vector3.UnitY;
            public static Vector3 Right => Vector3.UnitX;
            public static Vector3 Left => -Vector3.UnitX;
            public static Vector3 Forward => -Vector3.UnitZ;
            public static Vector3 Back => Vector3.UnitZ;

            /// <summary>Moves from `current` toward `target` by at most `maxDistanceDelta`, not past it.</summary>
            public static Vector3 MoveTowards(Vector3 current, Vector3 target, float maxDistanceDelta)
            {
                Vector3 d = target - current;
                float length = d.Length();
                return length <= maxDistanceDelta || length < kEpsilon ? target : current + d / length * maxDistanceDelta;
            }

            /// <summary>Eases toward `target` as a critically damped spring (smoothTime: about when it gets there), keeping
            /// its speed in `velocity` between calls; never faster than maxSpeed.</summary>
            public static Vector3 SmoothDamp(Vector3 current, Vector3 target, ref Vector3 velocity, float smoothTime, float deltaTime,
                                             float maxSpeed = float.PositiveInfinity)
            {
                smoothTime = MathF.Max(0.0001f, smoothTime);
                float omega = 2.0f / smoothTime, x = omega * deltaTime;
                float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
                Vector3 change = current - target;
                float maxChange = maxSpeed * smoothTime;
                if (change.LengthSquared() > maxChange * maxChange) change = Vector3.Normalize(change) * maxChange;
                Vector3 goal = current - change;
                Vector3 temp = (velocity + omega * change) * deltaTime;
                velocity = (velocity - omega * temp) * exp;
                Vector3 result = goal + (change + temp) * exp;
                // Not past the target.
                if (Vector3.Dot(target - current, result - target) > 0.0f)
                {
                    result = target;
                    velocity = (result - target) / MathF.Max(deltaTime, 1e-6f);
                }
                return result;
            }

            /// <summary>Turns `current` toward `target` by at most `maxRadiansDelta` (and changes its length by at most
            /// `maxMagnitudeDelta`), as Unity's.</summary>
            public static Vector3 RotateTowards(Vector3 current, Vector3 target, float maxRadiansDelta, float maxMagnitudeDelta)
            {
                float a = current.Length(), b = target.Length();
                if (a < kEpsilon || b < kEpsilon) return MoveTowards(current, target, maxMagnitudeDelta);
                Vector3 from = current / a, to = target / b;
                float angle = MathF.Acos(Math.Clamp(Vector3.Dot(from, to), -1.0f, 1.0f));
                float length = Mathf.MoveTowards(a, b, maxMagnitudeDelta);
                if (angle < 1e-6f) return to * length;
                float t = MathF.Min(1.0f, maxRadiansDelta / angle);
                return SlerpUnit(from, to, angle, t) * length;
            }

            /// <summary>Along the sphere between two directions (lengths blended too).</summary>
            public static Vector3 Slerp(Vector3 a, Vector3 b, float t)
            {
                t = Mathf.Clamp01(t);
                float la = a.Length(), lb = b.Length();
                if (la < kEpsilon || lb < kEpsilon) return Vector3.Lerp(a, b, t);
                Vector3 from = a / la, to = b / lb;
                float angle = MathF.Acos(Math.Clamp(Vector3.Dot(from, to), -1.0f, 1.0f));
                return SlerpUnit(from, to, angle, t) * Mathf.LerpUnclamped(la, lb, t);
            }

            /// <summary>The part of `vector` along `onNormal`, and the part left in the plane with that normal.</summary>
            public static Vector3 Project(Vector3 vector, Vector3 onNormal)
            {
                float n = onNormal.LengthSquared();
                return n < kEpsilon * kEpsilon ? Vector3.Zero : onNormal * (Vector3.Dot(vector, onNormal) / n);
            }
            public static Vector3 ProjectOnPlane(Vector3 vector, Vector3 planeNormal) => vector - Project(vector, planeNormal);

            /// <summary>The angle between two directions (0 to 180 degrees), and signed by which way round `axis` it goes.</summary>
            public static float Angle(Vector3 from, Vector3 to)
            {
                float d = MathF.Sqrt(from.LengthSquared() * to.LengthSquared());
                return d < 1e-15f ? 0.0f : Angle01(Vector3.Dot(from, to) / d);
            }
            public static float SignedAngle(Vector3 from, Vector3 to, Vector3 axis)
            {
                float angle = Angle(from, to);
                return Vector3.Dot(axis, Vector3.Cross(from, to)) < 0.0f ? -angle : angle;
            }

            /// <summary>The vector, shortened to at most `maxLength`.</summary>
            public static Vector3 ClampMagnitude(Vector3 vector, float maxLength)
            {
                float l = vector.LengthSquared();
                return l > maxLength * maxLength ? vector / MathF.Sqrt(l) * maxLength : vector;
            }
        }

        extension(Vector3 v)
        {
            /// <summary>Its length, and itself at length 1 (zero stays zero).</summary>
            public float Magnitude => v.Length();
            public Vector3 Normalized => v.LengthSquared() > 1e-12f ? Vector3.Normalize(v) : Vector3.Zero;
        }

        private static Vector3 SlerpUnit(Vector3 from, Vector3 to, float angle, float t)
        {
            float s = MathF.Sin(angle);
            if (s < 1e-5f)
            {
                // Opposite: around any axis at right angles.
                if (Vector3.Dot(from, to) > 0.0f) return Vector3.Normalize(Vector3.Lerp(from, to, t));
                Vector3 axis = Vector3.Cross(from, MathF.Abs(from.X) < 0.9f ? Vector3.UnitX : Vector3.UnitY);
                return Vector3.Transform(from, Quaternion.CreateFromAxisAngle(Vector3.Normalize(axis), angle * t));
            }
            return (from * MathF.Sin((1.0f - t) * angle) + to * MathF.Sin(t * angle)) / s;
        }

        extension(Vector2)
        {
            public static Vector2 MoveTowards(Vector2 current, Vector2 target, float maxDistanceDelta)
            {
                Vector2 d = target - current;
                float length = d.Length();
                return length <= maxDistanceDelta || length < kEpsilon ? target : current + d / length * maxDistanceDelta;
            }
            public static Vector2 SmoothDamp(Vector2 current, Vector2 target, ref Vector2 velocity, float smoothTime, float deltaTime,
                                             float maxSpeed = float.PositiveInfinity)
            {
                Vector3 v3 = new(velocity, 0);
                Vector3 r = Vector3.SmoothDamp(new Vector3(current, 0), new Vector3(target, 0), ref v3, smoothTime, deltaTime, maxSpeed);
                velocity = new Vector2(v3.X, v3.Y);
                return new Vector2(r.X, r.Y);
            }
            /// <summary>The angle between two directions (0 to 180 degrees), and signed (counterclockwise positive).</summary>
            public static float Angle(Vector2 from, Vector2 to)
            {
                float d = MathF.Sqrt(from.LengthSquared() * to.LengthSquared());
                return d < 1e-15f ? 0.0f : Angle01(Vector2.Dot(from, to) / d);
            }
            public static float SignedAngle(Vector2 from, Vector2 to) => from.X * to.Y - from.Y * to.X < 0.0f ? -Vector2.Angle(from, to) : Vector2.Angle(from, to);
            /// <summary>Turned a quarter counterclockwise.</summary>
            public static Vector2 Perpendicular(Vector2 v) => new(-v.Y, v.X);
            public static Vector2 ClampMagnitude(Vector2 vector, float maxLength)
            {
                float l = vector.LengthSquared();
                return l > maxLength * maxLength ? vector / MathF.Sqrt(l) * maxLength : vector;
            }
        }

        extension(Quaternion)
        {
            /// <summary>The rotation of Euler angles in degrees, as a Transform's EulerAngles are (so
            /// <c>Quaternion.Euler(t.EulerAngles)</c> is the object's rotation).</summary>
            public static Quaternion Euler(float x, float y, float z) => Quaternion.Euler(new Vector3(x, y, z));
            public static Quaternion Euler(Vector3 degrees)
            {
                // glm's quat(eulerAngles), which the engine uses for Transforms.
                Vector3 h = degrees * (Mathf.Deg2Rad * 0.5f);
                float cx = MathF.Cos(h.X), cy = MathF.Cos(h.Y), cz = MathF.Cos(h.Z);
                float sx = MathF.Sin(h.X), sy = MathF.Sin(h.Y), sz = MathF.Sin(h.Z);
                return new Quaternion(sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz);
            }

            /// <summary>A turn of `degrees` about `axis` (right-handed: counterclockwise looking down the axis).</summary>
            public static Quaternion AngleAxis(float degrees, Vector3 axis) =>
                axis.LengthSquared() < 1e-12f ? Quaternion.Identity : Quaternion.CreateFromAxisAngle(Vector3.Normalize(axis), degrees * Mathf.Deg2Rad);

            /// <summary>The rotation that makes an object face `forward` (its -Z along it) with its top toward `up`.</summary>
            public static Quaternion LookRotation(Vector3 forward, Vector3 up)
            {
                if (forward.LengthSquared() < 1e-12f) return Quaternion.Identity;
                Vector3 back = -Vector3.Normalize(forward); // the object's +Z
                Vector3 right = Vector3.Cross(up, back);
                if (right.LengthSquared() < 1e-12f) right = Vector3.Cross(MathF.Abs(back.Y) < 0.9f ? Vector3.UnitY : Vector3.UnitX, back);
                right = Vector3.Normalize(right);
                Vector3 top = Vector3.Cross(back, right);
                var m = new Matrix4x4(right.X, right.Y, right.Z, 0, top.X, top.Y, top.Z, 0, back.X, back.Y, back.Z, 0, 0, 0, 0, 1);
                return Quaternion.Normalize(Quaternion.CreateFromRotationMatrix(m));
            }
            public static Quaternion LookRotation(Vector3 forward) => Quaternion.LookRotation(forward, Vector3.UnitY);

            /// <summary>The shortest rotation turning direction `from` onto `to`.</summary>
            public static Quaternion FromToRotation(Vector3 from, Vector3 to)
            {
                if (from.LengthSquared() < 1e-12f || to.LengthSquared() < 1e-12f) return Quaternion.Identity;
                Vector3 a = Vector3.Normalize(from), b = Vector3.Normalize(to);
                float d = Vector3.Dot(a, b);
                if (d > 1.0f - 1e-6f) return Quaternion.Identity;
                if (d < -1.0f + 1e-6f)
                {
                    Vector3 axis = Vector3.Cross(a, MathF.Abs(a.X) < 0.9f ? Vector3.UnitX : Vector3.UnitY);
                    return Quaternion.CreateFromAxisAngle(Vector3.Normalize(axis), MathF.PI);
                }
                Vector3 c = Vector3.Cross(a, b);
                return Quaternion.Normalize(new Quaternion(c.X, c.Y, c.Z, 1.0f + d));
            }

            /// <summary>The angle between two rotations (0 to 180 degrees).</summary>
            public static float Angle(Quaternion a, Quaternion b)
            {
                float d = MathF.Min(MathF.Abs(Quaternion.Dot(Quaternion.Normalize(a), Quaternion.Normalize(b))), 1.0f);
                return d > 1.0f - 1e-6f ? 0.0f : 2.0f * MathF.Acos(d) * Mathf.Rad2Deg;
            }

            /// <summary>Turns `from` toward `to` by at most `maxDegreesDelta` (a negative one turns away).</summary>
            public static Quaternion RotateTowards(Quaternion from, Quaternion to, float maxDegreesDelta)
            {
                float angle = Quaternion.Angle(from, to);
                if (angle < 1e-4f) return to;
                return Quaternion.Slerp(from, to, MathF.Min(1.0f, maxDegreesDelta / angle));
            }
        }

        extension(Quaternion q)
        {
            /// <summary>Its Euler angles in degrees, as a Transform's EulerAngles are (glm's eulerAngles).</summary>
            public Vector3 EulerAngles
            {
                get
                {
                    Quaternion n = Quaternion.Normalize(q);
                    float x = n.X, y = n.Y, z = n.Z, w = n.W;
                    float py = 2.0f * (y * z + w * x), px = w * w - x * x - y * y + z * z;
                    float ry = 2.0f * (x * y + w * z), rx = w * w + x * x - y * y - z * z;
                    float yaw = MathF.Asin(Math.Clamp(-2.0f * (x * z - w * y), -1.0f, 1.0f));
                    // Gimbal lock (yaw +-90): roll's terms are rounding noise (glm's often gives 180, turning the
                    // object over); roll is 0 and pitch takes the whole turn (as the engine's EulerAngles).
                    if (MathF.Abs(ry) < 1e-6f && MathF.Abs(rx) < 1e-6f) return new Vector3(2.0f * MathF.Atan2(x, w), yaw, 0.0f) * Mathf.Rad2Deg;
                    float pitch = MathF.Abs(py) < 1e-6f && MathF.Abs(px) < 1e-6f ? 2.0f * MathF.Atan2(x, w) : MathF.Atan2(py, px);
                    float roll = MathF.Atan2(ry, rx);
                    return new Vector3(pitch, yaw, roll) * Mathf.Rad2Deg;
                }
            }
            /// <summary>The rotation's forward (-Z), up and right directions.</summary>
            public Vector3 Forward => Vector3.Transform(-Vector3.UnitZ, q);
            public Vector3 Up => Vector3.Transform(Vector3.UnitY, q);
            public Vector3 Right => Vector3.Transform(Vector3.UnitX, q);
        }
    }
}
