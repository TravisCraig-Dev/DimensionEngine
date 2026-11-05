#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Temporal anti-aliasing: the scene is rendered with a sub-pixel jitter that changes every frame, and each pixel is
// blended with its own history (reprojected from the previous frame through the depth buffer), so edges and fine
// detail converge to a smooth, supersampled result. The history is clipped to the current neighborhood's color range
// (YCoCg) so moving objects and disocclusions do not leave ghosts.

layout(push_constant) uniform Push
{
    mat4 PrevViewProj; // last frame's (unjittered) view-projection
    uint Current;      // bindless: this frame's HDR color
    uint History;      // bindless: the accumulated result so far
    uint Depth;        // bindless: this frame's depth
    uint HasHistory;
    vec2 Texel;
    float Feedback;    // weight of the current frame (lower = smoother, slower to respond)
    uint Velocity;     // bindless velocity buffer + 1 (0 = reproject through the depth with PrevViewProj)
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 ToYCoCg(vec3 c) { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 FromYCoCg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Colors are blended in a tonemapped space so a few very bright pixels do not dominate (and flicker).
vec3 Compress(vec3 c) { return c / (1.0 + max(max(c.r, c.g), c.b)); }
vec3 Expand(vec3 c) { return c / max(1.0 - max(max(c.r, c.g), c.b), 1e-4); }

vec3 Fetch(vec2 uv) { return Compress(max(textureLod(uTextures[nonuniformEXT(pc.Current)], uv, 0.0).rgb, vec3(0.0))); }

void main()
{
    // Neighborhood statistics, and the nearest depth around the pixel (so edges reproject with the foreground).
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    vec3 center = Fetch(vUV);
    float nearest = 1.0;
    vec2 nearestUV = vUV;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            vec2 uv = vUV + vec2(x, y) * pc.Texel;
            vec3 c = ToYCoCg(x == 0 && y == 0 ? center : Fetch(uv));
            m1 += c;
            m2 += c * c;
            float d = textureLod(uTextures[nonuniformEXT(pc.Depth)], uv, 0.0).r;
            if (d < nearest)
            {
                nearest = d;
                nearestUV = uv;
            }
        }
    vec3 mean = m1 / 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    vec3 lo = mean - sigma * 1.25, hi = mean + sigma * 1.25;

    // Where this pixel was last frame: by the velocity buffer (moving objects too), else as the camera moved.
    vec2 prevUV;
    if (pc.Velocity != 0u)
        prevUV = vUV - textureLod(uTextures[nonuniformEXT(pc.Velocity - 1u)], nearestUV, 0.0).xy;
    else
    {
        vec4 world = uScene.InvViewProj * vec4(nearestUV.x * 2.0 - 1.0, 1.0 - nearestUV.y * 2.0, nearest, 1.0);
        world /= world.w;
        vec4 prev = pc.PrevViewProj * vec4(world.xyz, 1.0);
        prevUV = vec2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5) + (vUV - nearestUV);
    }
    if (pc.HasHistory == 0u || any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0))))
    {
        outColor = vec4(Expand(center), 1.0);
        return;
    }
    vec3 history = ToYCoCg(Compress(max(textureLod(uTextures[nonuniformEXT(pc.History)], prevUV, 0.0).rgb, vec3(0.0))));
    // Clip toward the neighborhood's mean until inside its box.
    vec3 toHistory = history - mean;
    vec3 extent = max(hi - mean, vec3(1e-5));
    vec3 ratio = abs(toHistory) / extent;
    float scale = max(ratio.x, max(ratio.y, ratio.z));
    if (scale > 1.0) history = mean + toHistory / scale;
    vec3 result = mix(FromYCoCg(history), center, pc.Feedback);
    outColor = vec4(Expand(result), 1.0);
}
