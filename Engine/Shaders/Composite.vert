#version 460

layout(location = 0) out vec2 vUV;

// Fullscreen triangle, no vertex buffer.
void main()
{
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
