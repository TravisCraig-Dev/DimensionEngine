#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Underwater murk: the part of each view ray inside the Water Volume (its box, up to the surface) dims what lies behind
// it toward the water's color, lit by the ambient light and the sun (less of it the deeper the water). Seen from below
// the surface everything is murky; from above, only what lies under the water. Output is premultiplied:
// result = scene * (1 - alpha) + rgb.

layout(push_constant) uniform Push
{
    uint Depth; // bindless depth
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
    float depth = textureLod(uTextures[nonuniformEXT(pc.Depth)], vUV, 0.0).r;
    vec4 far = uScene.InvViewProj * vec4(vUV.x * 2.0 - 1.0, 1.0 - vUV.y * 2.0, depth >= 1.0 ? 1.0 : depth, 1.0);
    vec3 start = uScene.CameraPosition.xyz;
    vec3 ray = far.xyz / far.w - start;
    float len = depth >= 1.0 ? 1e5 : length(ray);
    vec3 dir = ray / max(length(ray), 1e-4);

    // The ray's stretch inside the box.
    vec3 safe = mix(vec3(1e-6), dir, greaterThan(abs(dir), vec3(1e-6)));
    vec3 t0 = (uScene.WaterMin.xyz - start) / safe, t1 = (uScene.WaterMax.xyz - start) / safe;
    vec3 tNear = min(t0, t1), tFar = max(t0, t1);
    float enter = max(max(tNear.x, tNear.y), max(tNear.z, 0.0));
    float leave = min(min(tFar.x, tFar.y), min(tFar.z, len));
    float d = leave - enter;
    if (d <= 0.0)
    {
        outColor = vec4(0.0);
        return;
    }
    // Light in the water: the ambient light and the sun from above, fading with the depth of the stretch's middle.
    vec3 mid = start + dir * (0.5 * (enter + leave));
    float below = max(uScene.WaterMax.y - mid.y, 0.0);
    vec3 light = uScene.Ambient.rgb;
    if (uScene.LightDirection.w > 0.5) light += uScene.LightColor.rgb * max(-normalize(uScene.LightDirection.xyz).y, 0.0) * 0.5;
    light *= exp(-below * uScene.WaterFog.a * 0.5);
    float alpha = 1.0 - exp(-uScene.WaterFog.a * d);
    outColor = vec4(uScene.WaterFog.rgb * light * alpha, alpha);
}
