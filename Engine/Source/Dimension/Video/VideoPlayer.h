#pragma once
#include "Dimension/Core/Guid.h"
#include "Dimension/ECS/Entity.h"
#include <glm/glm.hpp>

namespace Dimension
{
    class Scene;

    // Video Players while a scene plays (like Unity's VideoPlayer): each decodes its clip on a thread of its own, shows
    // the picture due now in a texture (on its object's UI Image, Sprite Renderer or Mesh Renderer material, or only for
    // scripts) and plays the clip's sound through the audio engine; the pictures follow the sound's clock, so they stay
    // in step with it (without sound, real time).
    namespace Video
    {
        // Called by Scene: players start (Play On Awake) as their objects first appear, keep up every frame and go with
        // the scene.
        void Update(Scene& scene, float unscaledDt);
        void OnRuntimeStop(Scene& scene);

        bool Play(Scene& scene, Entity e);      // from where it paused, else from the start
        void Pause(Scene& scene, Entity e);
        void Stop(Scene& scene, Entity e);      // back to the start (the last picture stays shown)
        bool IsPlaying(Scene& scene, Entity e); // playing and not paused
        bool IsPaused(Scene& scene, Entity e);
        bool IsPrepared(Scene& scene, Entity e); // the clip is open (size and length known)
        double GetTime(Scene& scene, Entity e);   // seconds into the clip (from 0 again each loop)
        double GetLength(Scene& scene, Entity e);
        glm::uvec2 GetSize(Scene& scene, Entity e);
        AssetHandle GetTexture(Scene& scene, Entity e); // the picture (a runtime texture), None until prepared
        uint64_t GetFrameCount(Scene& scene, Entity e); // pictures shown so far (tests)
        double GetFrameRate(Scene& scene, Entity e);
        // Jumps to a time (seconds, clamped to the clip): the picture there shows (paused too) and playing goes on from
        // it. A player not started opens paused there. False without a clip.
        bool SetTime(Scene& scene, Entity e, double seconds);
        // Pauses and shows the next picture.
        void StepForward(Scene& scene, Entity e);
    }
}
