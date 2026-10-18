#pragma once
#include "EditorContext.h"

namespace Editor
{
    enum class ObjectKind
    {
        Empty, Sprite, SpriteSquareBody, Cube, Sphere, Plane, Quad, Cylinder,
        Camera, DirectionalLight, PointLight, AudioSource, PhysicsCube, PhysicsSphere,
        Text3D, Canvas, UIImage, UIText, UIButton, ParticleSystem,
    };

    // Creates a preconfigured GameObject (records undo, selects it).
    Entity CreateObject(EditorContext& ctx, ObjectKind kind, Entity parent = Entity::Null(), const glm::vec3* position = nullptr);
    void DrawCreateMenuItems(EditorContext& ctx, Entity parent);

    void DeleteSelected(EditorContext& ctx);
    void DuplicateSelected(EditorContext& ctx);
    // Copies the selected GameObject (with children) to the OS clipboard as JSON.
    void CopySelected(EditorContext& ctx);
    // Pastes GameObjects from the clipboard next to the selection (edit mode only).
    void PasteClipboard(EditorContext& ctx);
    // Saves the entity subtree as a .prefab in the given folder.
    void CreatePrefab(EditorContext& ctx, Entity e, const fs::path& folder);
}
