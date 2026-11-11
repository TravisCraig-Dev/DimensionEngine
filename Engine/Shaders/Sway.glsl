// Grass and fur in the wind, shared by Mesh.vert and Velocity.vert (needs Common.glsl for uScene).

// Grass tufts: the wind sways them (stronger higher up, in gusts that travel across the field, with a flutter) and
// bodies standing in them push them away. `root` is the tuft's base, `params` its sway and bending. Each point
// moves sideways by an amount growing with its height above the base and sinks so the blade keeps its length.
// `time`: the wind's clock (uScene.Wind.w now; the motion vectors also ask where the blade was a frame ago).
vec3 GrassMotion(vec3 world, vec3 root, vec2 params, float time)
{
    float h = max(world.y - root.y, 0.0);
    if (h <= 0.0) return world;
    vec2 offset = vec2(0.0);
    vec2 wind = uScene.Wind.xz;
    float speed = length(wind);
    if (speed > 1e-3 && params.x > 0.0)
    {
        vec2 dir = wind / speed;
        float wave = dot(root.xz, dir) * 0.25 - time * (1.0 + speed * 0.3);
        float gust = 0.6 + 0.4 * sin(wave) + uScene.WindParams.x * 0.5 * max(sin(wave * 0.31 + 1.7), 0.0);
        float flutter = sin(time * 7.0 + root.x * 3.1 + root.z * 2.3) * 0.12;
        offset += (wind * gust + vec2(-dir.y, dir.x) * speed * flutter) * (0.12 * params.x * h * h);
    }
    int benders = int(uScene.WindParams.y);
    for (int i = 0; i < benders && params.y > 0.0; ++i)
    {
        vec4 b = uScene.GrassBenders[i];
        vec2 away = root.xz - b.xz;
        float d = length(away);
        // Nearest point of the blade's axis to the sphere: the push fades with that distance.
        float axisY = clamp(b.y, root.y, root.y + h);
        float gap = length(vec3(d, axisY - b.y, 0.0)) - b.w;
        float push = 1.0 - smoothstep(0.0, b.w * 0.8 + 0.1, gap);
        offset += (d > 1e-4 ? away / d : vec2(1.0, 0.0)) * push * params.y * h * 1.4;
    }
    float len = length(offset);
    if (len > h * 0.9) offset *= h * 0.9 / len;
    len = min(len, h * 0.9);
    return vec3(world.x + offset.x, world.y - (h - sqrt(h * h - len * len)), world.z + offset.y);
}

// A fur shell's point: out along the (world) normal to the shell's height `h` (0-1) of the fur's length `len`, the
// tips hanging down by `droop` and swaying in the wind by `wind` (the Fur's Wind Response).
vec3 FurMotion(vec3 world, vec3 normal, float len, float h, float droop, float wind, float time)
{
    world += normal * len * h;
    world.y -= droop * len * h * h;
    float sway = 0.75 + 0.25 * sin(time * 3.0 + world.x * 1.7 + world.z * 1.3);
    world.xz += uScene.Wind.xz * (wind * 0.1 * len * h * h * sway);
    return world;
}
