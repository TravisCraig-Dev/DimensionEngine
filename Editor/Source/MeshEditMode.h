#pragma once
#include "EditorContext.h"
#include <Dimension/Modeling/EditableMesh.h>
#include <Dimension/Modeling/MeshOps.h>
#include <Dimension/Modeling/MeshRaycast.h>
#include <imgui.h>
#include <deque>

namespace Editor
{
    // Blender-style Edit Mode in the Scene view: Tab on an object with a Mesh Renderer edits its model's vertices,
    // edges and faces. Models made in the editor are .dmodel assets (editable meshes); other meshes are copied into one
    // first. Edits show live on the object and are written to the model file once a change is finished.
    class MeshEditMode
    {
    public:
        explicit MeshEditMode(EditorContext& ctx) : m_Ctx(ctx) {}

        bool IsActive() const { return m_Active; }
        Entity GetEntity() const { return m_Entity; }
        AssetHandle GetAsset() const { return m_Asset; }
        Modeling::EditableMesh& GetMesh() { return m_Mesh; }
        Modeling::SelectMode GetSelectMode() const { return m_Mode; }
        void SetSelectMode(Modeling::SelectMode mode);

        // Tab: enters on the selected object or leaves. Objects whose mesh is not a model ask to make an editable copy.
        void Toggle();
        bool Enter(Entity e);
        void Exit();
        // Writes the model file now (it is also written once each change is finished).
        void Save();

        // Each frame before the Scene view: leaves edit mode when the object goes away or play starts; saves.
        void Update();
        // Draws the mesh over the Scene view and handles its mouse and keys (call while the Scene window is current).
        // `blocked`: the mouse is over something else (the view cube). Returns true while it owns the mouse.
        bool UpdateSceneView(const ImVec2& origin, const ImVec2& size, const glm::mat4& view, const glm::mat4& proj, bool hovered,
                             bool focused, bool blocked);
        // The question asked before editing an object whose mesh is not a model (call each frame).
        void DrawPopups();
        // A move, rotate or scale following the mouse (G, R, S): the camera stays still meanwhile.
        bool IsModal() const { return m_Modal.Kind != ModalKind::None; }

        bool CanUndo() const { return !m_Undo.empty(); }
        bool CanRedo() const { return !m_Redo.empty(); }
        void Undo();
        void Redo();
        // The selection's world-space center and size (F frames it); false when nothing is selected.
        bool GetFrame(glm::vec3& center, float& radius) const;

        // GameObject > 3D Object > Model: a new model file (Assets/Models) of a primitive on a new object.
        enum class Primitive { Cube, Plane, Circle, Grid, UVSphere, IcoSphere, Cylinder, Cone, Torus, Count };
        static const char* PrimitiveName(Primitive p);
        static Modeling::EditableMesh MakePrimitive(Primitive p);
        static Entity CreateModelObject(EditorContext& ctx, Primitive p, Entity parent = Entity::Null());
        // A new model file (unique name in `folder`) holding `mesh`; returns its handle.
        static AssetHandle CreateModelAsset(const Modeling::EditableMesh& mesh, const fs::path& folder, const std::string& name);

    private:
        enum class ModalKind { None, Move, Rotate, Scale };
        struct Modal
        {
            ModalKind Kind = ModalKind::None;
            int StartFrame = 0;
            std::vector<glm::vec3> Start;  // positions before it (model space)
            std::vector<float> Weights;    // per vertex (selection 1, proportional falloff around it)
            glm::vec3 Pivot{ 0.0f };       // world space
            ImVec2 MouseStart, PivotScreen;
            glm::vec3 PlaneStart{ 0.0f };  // where the mouse ray first met the move plane
            float Angle = 0.0f, LastScreenAngle = 0.0f;
            int Axis = -1;                 // constrained axis (0 X, 1 Y, 2 Z), -1 none
            bool Plane = false;            // Shift+axis: the other two axes
            bool LocalAxes = false;        // the object's axes (axis key pressed twice)
            std::string Typed;             // numeric input
        };

        struct Snapshot
        {
            Modeling::EditableMesh Mesh;
            Modeling::SelectMode Mode;
            std::string Name;
        };

        // Records an undo step (before a change) and marks the model changed afterwards (Commit).
        void Record(const char* name);
        void Commit();         // the mesh changed: the object shows it, the file is written later
        void SyncRenderMesh(); // pushes the mesh to the object's renderer
        void SelectionChanged();

        void UpdateProjection(const ImVec2& origin, const ImVec2& size, const glm::mat4& view, const glm::mat4& proj);
        void DrawOverlay(ImDrawList* dl);
        void DrawToolbar(const ImVec2& origin);
        void DrawMenus();
        void HandleKeys();
        void HandleMouse(bool hovered, bool blocked);
        void ClickSelect(const ImVec2& mouse, bool extend);
        void BoxSelect(const ImVec2& a, const ImVec2& b, int op); // op: 0 replace, 1 add, 2 subtract
        int64_t NearestVertex(const ImVec2& mouse, float maxPixels) const;
        int64_t NearestEdge(const ImVec2& mouse, float maxPixels) const;
        int64_t FaceUnderMouse(const ImVec2& mouse) const;

        void BeginModal(ModalKind kind);
        void UpdateModal();
        void EndModal(bool confirm);
        glm::vec3 AxisVector(int axis) const;
        bool GizmoManipulate(const ImVec2& origin, const ImVec2& size, const glm::mat4& view, const glm::mat4& proj);

        // Operations from keys and menus.
        void DoDelete(Modeling::Ops::DeleteKind kind);
        void DoAdd(Primitive p);

        EditorContext& m_Ctx;
        bool m_Active = false;
        Entity m_Entity = Entity::Null();
        AssetHandle m_Asset = AssetHandle::None();
        fs::path m_File;
        Modeling::EditableMesh m_Mesh;
        Modeling::SelectMode m_Mode = Modeling::SelectMode::Vertex;
        bool m_Dirty = false;   // changed since written
        bool m_XRay = false;    // see and select through the surface
        bool m_Proportional = false;
        bool m_ProportionalConnected = false;
        Modeling::Ops::Falloff m_Falloff = Modeling::Ops::Falloff::Smooth;
        float m_ProportionalSize = 1.0f; // world units
        int m_Gizmo = 0;                  // 0 none, else ImGuizmo operation (the toolbar's handles)
        std::deque<Snapshot> m_Undo, m_Redo;
        Modal m_Modal;
        Entity m_ConvertEntity = Entity::Null(); // asking whether to make an editable copy
        bool m_OpenConvert = false;

        // This frame's view.
        ImVec2 m_Origin, m_Size;
        glm::mat4 m_World{ 1.0f }, m_ViewProj{ 1.0f }, m_View{ 1.0f }, m_Proj{ 1.0f };
        glm::vec3 m_CameraLocal{ 0.0f }, m_CameraWorld{ 0.0f };
        std::vector<ImVec2> m_Screen;     // per vertex
        std::vector<uint8_t> m_VertVisible, m_FaceVisible;
        std::vector<uint8_t> m_EdgeVisible;
        Modeling::MeshRaycaster m_Rays;
        uint64_t m_GeometryVersion = 1, m_RaysVersion = 0, m_VisibleVersion = 0;
        glm::mat4 m_VisibleViewProj{ 0.0f };
        bool m_VisibleXRay = false;

        // Mouse gestures.
        bool m_Pressing = false;   // left button went down in the view
        bool m_PressAlt = false;
        ImVec2 m_PressPos;
        bool m_BoxSelecting = false;
        bool m_OverUI = false;     // the toolbar or a menu has the mouse
        ImVec2 m_ToolbarMin, m_ToolbarMax;
        int m_OpenMenu = 0;        // a popup to open at the mouse this frame (1 delete, 2 merge, 3 normals, 4 add, 5 edges)
        float m_MergeDistance = 0.0001f;
    };
}
