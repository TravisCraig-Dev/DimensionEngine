#version 460
// Screen-space UI: positions are pixels with a top-left origin.
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;
layout(location = 3) in uint aTexture;
layout(location = 4) in uint aEntity;

layout(push_constant) uniform Push { vec2 ScreenSize; } pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) flat out uint vTexture;

void main()
{
    vColor = aColor;
    vUV = aUV;
    vTexture = aTexture;
    gl_Position = vec4(aPosition.xy / pc.ScreenSize * 2.0 - 1.0, 0.0, 1.0);
}
