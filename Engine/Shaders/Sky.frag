#version 460
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

layout(location = 0) in vec3 vDirection;
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntity; // write-masked

// Procedural gradient sky with a sun disk following the directional light.
void main()
{
    vec3 dir = normalize(vDirection);
    vec3 sky = SkyColor(dir);
    if (uScene.LightDirection.w > 0.5)
    {
        vec3 toSun = normalize(-uScene.LightDirection.xyz);
        float c = max(dot(dir, toSun), 0.0);
        sky += uScene.LightColor.rgb * (pow(c, 900.0) * 6.0 + pow(c, 12.0) * 0.08);
    }
    outColor = vec4(sky, 1.0);
    outEntity = 0u;
}
