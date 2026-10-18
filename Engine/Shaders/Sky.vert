#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

layout(location = 0) out vec3 vDirection;

// Fullscreen triangle at the far plane; reconstructs the world-space view ray.
void main()
{
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    gl_Position = vec4(pos, 1.0, 1.0);
    // The scene viewport is flipped, so clip-space y here already matches the camera's NDC y.
    vec4 far = uScene.InvViewProj * vec4(pos, 1.0, 1.0);
    vec4 near = uScene.InvViewProj * vec4(pos, 0.0, 1.0);
    vDirection = far.xyz / far.w - near.xyz / near.w;
}
