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
};

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
