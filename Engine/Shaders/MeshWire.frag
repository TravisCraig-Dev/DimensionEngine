#version 460
// Scene view Wireframe: every mesh's edges in a light gray (drawn without depth testing, like Unity's).
// The mesh vertex shader's outputs (only the entity is used; declaring them all keeps the interfaces matched).
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) flat in uint vEntity;
layout(location = 4) in vec4 vTangent;
layout(location = 5) in float vShell;
layout(location = 6) in float vFoam;

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntity;
layout(location = 2) out uvec4 outSurface;

void main()
{
    outColor = vec4(0.75, 0.78, 0.82, 1.0);
    outEntity = vEntity;
    outSurface = uvec4(0u);
}
