#version 460
// Scene view Shaded Wireframe: the edges over the shaded meshes, dark. Their depth is pulled toward the camera by a
// tiny fraction (about 1 cm at 10 m, 25 cm at 50 m with a 0.1 m near plane) so they win against their own surface
// without the slope-scaled bias that would pull grazing edges through whatever is in front.
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
    outColor = vec4(0.0, 0.0, 0.0, 1.0);
    outEntity = vEntity;
    outSurface = uvec4(0u);
    gl_FragDepth = gl_FragCoord.z * 0.99999;
}
