#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

// The longest motion in each tile of the velocity buffer (Tile x Tile pixels), for the motion blur to know how far
// moving things can reach into a pixel from around it.

layout(push_constant) uniform Push
{
    uint Velocity; // bindless
    uint Tile;     // pixels per side
} pc;

layout(location = 0) out vec2 outVelocity;

void main()
{
    ivec2 size = textureSize(uTextures[nonuniformEXT(pc.Velocity)], 0);
    ivec2 start = ivec2(gl_FragCoord.xy) * int(pc.Tile);
    ivec2 end = min(start + int(pc.Tile), size);
    vec2 longest = vec2(0.0);
    float best = 0.0;
    for (int y = start.y; y < end.y; ++y)
        for (int x = start.x; x < end.x; ++x)
        {
            vec2 v = texelFetch(uTextures[nonuniformEXT(pc.Velocity)], ivec2(x, y), 0).xy;
            float l = dot(v, v);
            if (l > best)
            {
                best = l;
                longest = v;
            }
        }
    outVelocity = longest;
}
