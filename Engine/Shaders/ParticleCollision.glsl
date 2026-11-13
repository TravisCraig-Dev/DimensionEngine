// GPU particle collisions, shared by ParticleSim.comp and VfxSim.comp (which declare uTextures, the bindless
// textures, and GL_EXT_nonuniform_qualifier before including it): the last frame's depth buffer, the terrain's own
// shape and the nearest colliders. Any bounce sets g_ParticleHit (Visual Effects send their collision events by it).
bool g_ParticleHit = false;

// The visible surface at a point of the last frame's depth buffer, in world space.
vec3 DepthWorld(GpuParticleParamsRef P, vec2 uv)
{
    float d = textureLod(uTextures[nonuniformEXT(P.DepthInfo.x)], uv, 0.0).r;
    vec4 w = P.DepthInvViewProj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    return w.xyz / w.w;
}

// Collision decals: a particle striking a surface (moving into it) records the spot once; the flipbook start (Misc.w,
// 0-1) carries the mark as +2, which the flipbook's fract ignores.
void RecordHit(GpuParticleParamsRef P, inout GpuParticle p, vec3 point, vec3 n, float vn)
{
    g_ParticleHit = true;
    if (P.DecalBuffer.w == 0u || vn > -0.3 || p.Misc.w >= 2.0) return;
    p.Misc.w += 2.0;
    GpuHitsRef hits = GpuHitsRef(P.DecalBuffer.xy);
    uint i = atomicAdd(hits.Count, 1u);
    if (i >= P.DecalBuffer.z) return;
    float life = clamp(p.PosAge.w / max(p.VelLife.w, 1e-4), 0.0, 1.0);
    vec4 color = p.Color * SampleColorLut(P, life);
    color.a = p.Color.a;
    hits.Items[i * 3u] = vec4(point, 0.0);
    hits.Items[i * 3u + 1u] = vec4(n, 0.0);
    hits.Items[i * 3u + 2u] = color;
}

// Collision with what the camera saw last frame: a particle just behind the visible surface bounces off it (along
// the surface normal rebuilt from the depth around it). Off screen, particles pass freely.
void DepthCollide(GpuParticleParamsRef P, inout GpuParticle p)
{
    if (P.DepthInfo.y == 0u) return;
    vec3 wp = (P.SimToWorld * vec4(p.PosAge.xyz, 1.0)).xyz;
    vec4 clip = P.DepthViewProj * vec4(wp, 1.0);
    if (clip.w <= 0.01) return;
    vec2 uv = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    if (any(lessThan(uv, vec2(0.002))) || any(greaterThan(uv, vec2(0.998)))) return;
    float d = textureLod(uTextures[nonuniformEXT(P.DepthInfo.x)], uv, 0.0).r;
    if (d >= 1.0) return; // sky
    vec3 surface = DepthWorld(P, uv);
    float surfaceW = (P.DepthViewProj * vec4(surface, 1.0)).w;
    float behind = clip.w - surfaceW + P.DepthParams.y;
    if (behind <= 0.0 || behind > P.DepthParams.x) return;
    vec2 texel = 1.0 / vec2(textureSize(uTextures[nonuniformEXT(P.DepthInfo.x)], 0));
    vec3 n = normalize(cross(DepthWorld(P, uv + vec2(texel.x, 0.0)) - surface, DepthWorld(P, uv + vec2(0.0, texel.y)) - surface));
    // Face the camera: against the view ray through this pixel.
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    vec4 nearP = P.DepthInvViewProj * vec4(ndc, 0.0, 1.0), farP = P.DepthInvViewProj * vec4(ndc, 1.0, 1.0);
    if (dot(n, farP.xyz / farP.w - nearP.xyz / nearP.w) > 0.0) n = -n;
    // Only particles moving into the visible side bounce: one coming from behind (under a slab whose top the camera
    // sees) carries on, rather than being pushed through to the far side.
    vec3 v = mat3(P.SimToWorld) * p.VelLife.xyz;
    float vn = dot(v, n);
    if (vn >= 0.0) return;
    RecordHit(P, p, surface, n, vn);
    vec3 normalPart = n * vn, tangent = v - normalPart;
    v = tangent * (1.0 - P.Collision.w) - normalPart * P.Collision.z;
    p.VelLife.w -= p.VelLife.w * clamp(P.Misc.x, 0.0, 1.0);
    p.VelLife.xyz = mat3(P.WorldToSim) * v;
    p.PosAge.xyz = (P.WorldToSim * vec4(surface + n * P.DepthParams.y, 1.0)).xyz;
}

// The terrain's height at a point of its space, on the same triangles as its mesh (see Terrain::HeightAt).
float TerrainHeightAt(GpuParticleParamsRef P, vec2 xz)
{
    int n = int(P.TerrainInfo.z);
    TerrainHeightsRef h = TerrainHeightsRef(P.TerrainInfo.xy);
    vec2 size = max(P.TerrainSize.xy, vec2(0.01));
    vec2 f = clamp((xz / size + 0.5) * float(n - 1), vec2(0.0), vec2(float(n - 1)));
    int c0 = min(int(f.x), n - 2), r0 = min(int(f.y), n - 2);
    float tx = f.x - float(c0), tz = f.y - float(r0);
    float a = h.Heights[r0 * n + c0], b = h.Heights[r0 * n + c0 + 1], c = h.Heights[(r0 + 1) * n + c0], d = h.Heights[(r0 + 1) * n + c0 + 1];
    if (tx >= tz) return a + (b - a) * tx + (d - b) * tz;
    return a + (c - a) * tz + (d - c) * tx;
}

// Collision with the terrain's own shape, anywhere on it (not only where the camera sees): a particle below its
// surface (by its collision radius) is put back on it, and its velocity into it bounces and its sliding slows.
void TerrainCollide(GpuParticleParamsRef P, inout GpuParticle p)
{
    if (P.TerrainInfo.w == 0u || P.TerrainInfo.z < 2u) return;
    vec3 wp = (P.SimToWorld * vec4(p.PosAge.xyz, 1.0)).xyz;
    vec3 local = (P.TerrainToLocal * vec4(wp, 1.0)).xyz;
    vec2 halfSize = max(P.TerrainSize.xy, vec2(0.01)) * 0.5;
    if (abs(local.x) > halfSize.x || abs(local.z) > halfSize.y) return;
    float radius = P.DepthParams.y;
    float ground = TerrainHeightAt(P, local.xz);
    if (local.y >= ground + radius) return;
    float step = max(P.TerrainSize.x, P.TerrainSize.y) / float(max(int(P.TerrainInfo.z) - 1, 1)) * 0.5;
    float dx = TerrainHeightAt(P, local.xz + vec2(step, 0.0)) - TerrainHeightAt(P, local.xz - vec2(step, 0.0));
    float dz = TerrainHeightAt(P, local.xz + vec2(0.0, step)) - TerrainHeightAt(P, local.xz - vec2(0.0, step));
    vec3 n = normalize(transpose(mat3(P.TerrainToLocal)) * vec3(-dx, 2.0 * step, -dz));
    vec3 surface = (P.TerrainToWorld * vec4(local.x, ground, local.z, 1.0)).xyz;
    vec3 v = mat3(P.SimToWorld) * p.VelLife.xyz;
    float vn = dot(v, n);
    if (vn < 0.0)
    {
        RecordHit(P, p, surface, n, vn);
        vec3 normalPart = n * vn, tangent = v - normalPart;
        v = tangent * (1.0 - P.Collision.w) - normalPart * P.Collision.z;
        p.VelLife.w -= p.VelLife.w * clamp(P.Misc.x, 0.0, 1.0);
    }
    p.VelLife.xyz = mat3(P.WorldToSim) * v;
    p.PosAge.xyz = (P.WorldToSim * vec4(surface + n * radius, 1.0)).xyz;
}

// Collision with the 3D colliders listed in the parameters: a particle inside one (by its collision radius) moves out
// along the nearest surface's normal, and its velocity into it bounces (Bounce) and its sliding slows (Dampen).
void ColliderCollide(GpuParticleParamsRef P, inout GpuParticle p)
{
    uint count = min(P.ColliderCount.x, uint(PARTICLE_COLLIDERS));
    if (count == 0u) return;
    vec3 wp = (P.SimToWorld * vec4(p.PosAge.xyz, 1.0)).xyz;
    vec3 v = mat3(P.SimToWorld) * p.VelLife.xyz;
    float radius = P.DepthParams.y;
    bool hit = false;
    for (uint i = 0u; i < count; ++i)
    {
        vec4 a = P.Colliders[i * 4u], b = P.Colliders[i * 4u + 1u], c = P.Colliders[i * 4u + 2u], d = P.Colliders[i * 4u + 3u];
        int type = int(a.w + 0.5);
        vec3 n = vec3(0.0, 1.0, 0.0);
        float depth = -1.0;
        if (type == 1)
        {
            vec3 rel = wp - a.xyz;
            vec3 local = vec3(dot(rel, b.xyz), dot(rel, c.xyz), dot(rel, d.xyz));
            vec3 extent = vec3(b.w, c.w, d.w) + radius;
            vec3 room = extent - abs(local);
            if (all(greaterThan(room, vec3(0.0))))
            {
                // Out through the nearest face.
                if (room.x <= room.y && room.x <= room.z) { depth = room.x; n = b.xyz * sign(local.x); }
                else if (room.y <= room.z) { depth = room.y; n = c.xyz * sign(local.y); }
                else { depth = room.z; n = d.xyz * sign(local.z); }
            }
        }
        else
        {
            vec3 center = a.xyz;
            if (type == 2) center += c.xyz * clamp(dot(wp - a.xyz, c.xyz), -c.w, c.w);
            vec3 rel = wp - center;
            float len = length(rel);
            depth = b.w + radius - len;
            n = len > 1e-6 ? rel / len : vec3(0.0, 1.0, 0.0);
        }
        if (depth <= 0.0) continue;
        wp += n * depth;
        float vn = dot(v, n);
        if (vn < 0.0)
        {
            RecordHit(P, p, wp - n * radius, n, vn);
            vec3 normalPart = n * vn;
            v = (v - normalPart) * (1.0 - P.Collision.w) - normalPart * P.Collision.z;
            hit = true;
        }
    }
    if (hit) p.VelLife.w -= p.VelLife.w * clamp(P.Misc.x, 0.0, 1.0);
    p.PosAge.xyz = (P.WorldToSim * vec4(wp, 1.0)).xyz;
    p.VelLife.xyz = mat3(P.WorldToSim) * v;
}

