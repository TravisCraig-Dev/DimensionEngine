// GPU particles: shared by ParticleSim.comp (simulation) and ParticleGpu.vert (drawing).
// Must match GpuParticle and GpuParticleParams in SceneRenderer.cpp.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

struct GpuParticle
{
    vec4 PosAge;  // xyz position (simulation space), w age (seconds)
    vec4 VelLife; // xyz velocity, w lifetime (0 = never born)
    vec4 Color;   // start color
    vec4 Misc;    // x size, y rotation (radians), z angular velocity, w flipbook start (0-1)
};

layout(buffer_reference, std430, buffer_reference_align = 16) buffer GpuParticlesRef { GpuParticle Items[]; };

#define PARTICLE_LUT 32
#define PARTICLE_SEGMENTS 16
#define TRAIL_POINTS 8u
#define PARTICLE_COLLIDERS 32

// Trails: TRAIL_POINTS recorded positions per slot, newest first (xyz simulation space, w age when recorded; < 0 = none).
layout(buffer_reference, std430, buffer_reference_align = 16) buffer GpuTrailRef { vec4 Points[]; };

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GpuParticleParamsRef
{
    mat4 Emitter;     // emitter local -> world
    mat4 SimToWorld;  // simulation space -> world (the emitter's transform for local space, else identity)
    mat4 WorldToSim;
    vec4 Lifetime;    // xy lifetime range, zw speed range
    vec4 SizeRot;     // xy size range, zw start rotation range (radians)
    vec4 Spin;        // xy angular velocity range (radians / s), z drag, w dt
    vec4 ColorA;
    vec4 ColorB;
    vec4 Shape;       // x shape (0 cone, 1 sphere, 2 box, 3 point), y cone angle (radians), z radius, w 1 = world space
    vec4 Box;         // xyz box size, w noise scroll offset (z of the noise domain)
    vec4 Gravity;     // xyz gravity (simulation space), w 1 = force over lifetime
    vec4 Force;       // xyz force (simulation space), scaled by the force curve
    vec4 Noise;       // x strength, y frequency, z 1 = enabled
    vec4 Collision;   // x 1 = plane, y plane height + particle radius (world), z bounce, w dampen
    vec4 Misc;        // x lifetime loss per collision, y world scale (drawing), z 1 = random flipbook start, w flipbook cycles
    uvec4 Counts;     // x capacity, y ring head (first new particle), z new particles, w random seed
    uvec4 Sheet;      // x columns, y rows, z frames (0 = all), w 1 = flipbook on
    uvec4 SegmentCounts[PARTICLE_SEGMENTS / 4]; // new particles per emission segment (bursts at points)
    vec4 SegmentOrigins[PARTICLE_SEGMENTS];     // xyz world origin, w 1 = at the origin (else at the emitter)
    vec4 ColorLut[PARTICLE_LUT];                // color over lifetime
    vec4 CurveLut[PARTICLE_LUT];                // x size, y speed, z force over lifetime
    // Depth-buffer collision (Collision.x == 2): the last frame's depth and the view it was rendered with.
    mat4 DepthViewProj;
    mat4 DepthInvViewProj;
    uvec4 DepthInfo; // x bindless depth, y 1 = valid
    vec4 DepthParams; // x thickness (m), y particle radius
    vec4 Trail;        // x 1 = trails, y seconds between recorded points, z width (times the particle size)
    uvec4 TrailBuffer; // xy: GpuTrailRef address
    // 3D colliders to bounce off (world space), 4 vec4 each: (center, type 0 sphere / 1 box / 2 capsule),
    // (box x axis, half x; sphere and capsule: w radius), (box y axis or capsule axis, half y or half segment),
    // (box z axis, half z).
    uvec4 ColliderCount; // x count
    vec4 Colliders[PARTICLE_COLLIDERS * 4];
    uvec4 MeshSamples;   // Mesh shape: xy address of (position, normal) vec4 pairs, z count
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MeshSamplesRef { vec4 Items[]; };

vec4 SampleColorLut(GpuParticleParamsRef p, float t)
{
    float x = clamp(t, 0.0, 1.0) * float(PARTICLE_LUT - 1);
    int i = min(int(x), PARTICLE_LUT - 2);
    return mix(p.ColorLut[i], p.ColorLut[i + 1], x - float(i));
}

vec4 SampleCurveLut(GpuParticleParamsRef p, float t)
{
    float x = clamp(t, 0.0, 1.0) * float(PARTICLE_LUT - 1);
    int i = min(int(x), PARTICLE_LUT - 2);
    return mix(p.CurveLut[i], p.CurveLut[i + 1], x - float(i));
}

bool ParticleAlive(GpuParticle p) { return p.PosAge.w < p.VelLife.w; }
