#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// Motion blur from the velocity buffer (how each pixel moved on screen since the last frame: the camera's motion,
// and moving objects' own), as long as the shutter was open (Intensity), up to MaxLength of the screen. A
// reconstruction filter (McGuire et al. 2012) samples along the longest motion in the 3x3 tiles around the pixel, so
// a fast object smears out over the still background behind it too, not only within its own outline, while sharp
// things in front of a moving background stay sharp.

layout(push_constant) uniform Push
{
    uint Hdr;        // bindless: this frame's HDR color
    uint Depth;      // bindless: this frame's depth
    uint Velocity;   // bindless: the velocity buffer (UV units)
    uint Tiles;      // bindless: its tiles' longest motions
    float Intensity; // fraction of the motion blurred (the shutter)
    float MaxLength; // longest blur, as a fraction of the screen width
    uint Frame;      // for the sample jitter
    uint Pad;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec2 g_Size; // the screen in pixels

// A velocity as the blur's length in pixels (shutter and cap applied).
vec2 Blur(vec2 velocity)
{
    vec2 v = velocity * pc.Intensity * g_Size;
    float len = length(v), cap = pc.MaxLength * g_Size.x;
    return len > cap ? v * (cap / len) : v;
}

float ViewDepth(vec2 uv)
{
    float d = textureLod(uTextures[nonuniformEXT(pc.Depth)], uv, 0.0).r;
    vec4 p = uScene.InvProj * vec4(0.0, 0.0, d, 1.0);
    return -p.z / p.w;
}

// 1 where depth a is in front of (or level with) depth b, fading to 0 over a small distance behind it.
float InFront(float a, float b) { return clamp(1.0 - (a - b) / max(0.05, 0.02 * b), 0.0, 1.0); }
float Cone(float distance, float len) { return clamp(1.0 - distance / max(len, 0.5), 0.0, 1.0); }
float Cylinder(float distance, float len) { return 1.0 - smoothstep(0.95 * len, 1.05 * len + 0.01, distance); }

void main()
{
    g_Size = vec2(textureSize(uTextures[nonuniformEXT(pc.Hdr)], 0));
    vec4 center = textureLod(uTextures[nonuniformEXT(pc.Hdr)], vUV, 0.0);

    // The longest motion that can reach this pixel: from its tile and the eight around it.
    ivec2 tiles = textureSize(uTextures[nonuniformEXT(pc.Tiles)], 0);
    ivec2 tile = ivec2(vUV * vec2(tiles));
    vec2 longest = vec2(0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 t = clamp(tile + ivec2(x, y), ivec2(0), tiles - 1);
            vec2 v = Blur(texelFetch(uTextures[nonuniformEXT(pc.Tiles)], t, 0).xy);
            if (dot(v, v) > dot(longest, longest)) longest = v;
        }
    float longestLen = length(longest);
    if (longestLen < 0.5)
    {
        outColor = center;
        return;
    }

    vec2 own = Blur(textureLod(uTextures[nonuniformEXT(pc.Velocity)], vUV, 0.0).xy);
    // Radii: the smears reach half their length each way.
    float ownLen = max(length(own) * 0.5, 0.5);
    float depth = ViewDepth(vUV);
    // Samples along the longest motion (centered on the pixel), jittered per pixel and frame so the steps blend.
    const int kSamples = 15;
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy + float(pc.Frame % 64u) * 7.0, vec2(0.06711056, 0.00583715)))) - 0.5;
    float weight = 1.0 / ownLen;
    vec3 sum = center.rgb * weight;
    for (int i = 0; i < kSamples; ++i)
    {
        float t = mix(-1.0, 1.0, (float(i) + jitter + 1.0) / float(kSamples + 1)) * 0.5;
        vec2 offset = longest * t;
        vec2 uv = clamp(vUV + offset / g_Size, vec2(0.0), vec2(1.0));
        float distance = length(offset);
        float sampleDepth = ViewDepth(uv);
        float sampleLen = 0.5 * length(Blur(textureLod(uTextures[nonuniformEXT(pc.Velocity)], uv, 0.0).xy));
        // In front and smeared over this pixel; behind it and seen through this pixel's own smear; or both moving
        // together.
        float front = InFront(sampleDepth, depth), back = InFront(depth, sampleDepth);
        float w = front * Cone(distance, sampleLen) + back * Cone(distance, ownLen) +
                  Cylinder(distance, sampleLen) * Cylinder(distance, ownLen) * 2.0;
        weight += w;
        sum += textureLod(uTextures[nonuniformEXT(pc.Hdr)], uv, 0.0).rgb * w;
    }
    outColor = vec4(sum / weight, center.a);
}
