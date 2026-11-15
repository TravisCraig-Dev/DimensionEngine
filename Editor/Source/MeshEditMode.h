#pragma once
#include "EditorContext.h"
#include <Dimension/Modeling/EditableMesh.h>
#include <Dimension/Modeling/MeshOps.h>
#include <Dimension/Modeling/MeshTools.h>
#include <Dimension/Modeling/Modifiers.h>
#include <Dimension/Modeling/MeshRaycast.h>
#include <Dimension/Modeling/UVTools.h>
#include <Dimension/Modeling/Sculpt.h>
#include <Dimension/Modeling/Paint.h>
#include <Dimension/Modeling/Bake.h>
#include <Dimension/Modeling/Rig.h>
#include <Dimension/Modeling/Curves.h>
#include <Dimension/Modeling/Retopo.h>
#include <functional>
#include <memory>
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
        // Writes the model file now (it is also written once each change is finished); models whose Boolean or
        // Shrinkwrap modifiers use it are worked out again.
        void Save();
        // Models (loaded ones) whose modifiers use `changed` (directly or through others) made again.
        void RefreshDependents(AssetHandle changed);
        // A model file put back by undo or redo (outside Edit Mode): the object shows it again.
        void OnFileRestored(const fs::path& file);

        // Each frame before the Scene view: leaves edit mode when the object goes away or play starts; saves.
        void Update();
        // Draws the mesh over the Scene view and handles its mouse and keys (call while the Scene window is current).
        // `blocked`: the mouse is over something else (the view cube). Returns true while it owns the mouse.
        bool UpdateSceneView(const ImVec2& origin, const ImVec2& size, const glm::mat4& view, const glm::mat4& proj, bool hovered,
                             bool focused, bool blocked);
        // The Inspector's Modifiers section (under the Mesh Renderer) for an object whose mesh is a model made in the
        // editor: the model's modifier stack, edited live (in Edit Mode on that object, as undo steps of it).
        void DrawModifierPanel(Entity e);
        // The question asked before editing an object whose mesh is not a model (call each frame).
        void DrawPopups();
        // The UV Editor window (Window > UV Editor): the selected faces' UVs over the object's texture, selected, moved,
        // unwrapped and packed there (MeshEditUV.cpp). Call each frame while it is open.
        void DrawUVEditor(bool* open);
        // Automation: the UV Editor's view of 0..1 (zoom in pixels per UV unit, pan to the 0,0 corner).
        void SetUVView(float zoom, ImVec2 pan) { m_UVZoom = zoom; m_UVPan = pan; m_UVFramed = true; }
        // A move, rotate or scale following the mouse (G, R, S), or another tool that owns the keys (inset, bevel, loop
        // cut, knife...).
        bool IsModal() const
        {
            return m_Modal.Kind != ModalKind::None || m_Tool != Tool::None || m_UVModal.Kind != UVModalKind::None || m_Stroking || m_SculptAdjust != 0 ||
                   m_Painting || m_PaintAdjust != 0 || m_BoneModal.Kind != 0 || m_WeightPainting || m_CurveModal.Kind != 0;
        }
        // Whether the camera should leave the mouse alone (the knife lets it orbit between cuts).
        bool BlocksCamera() const
        {
            return m_Modal.Kind != ModalKind::None || m_Tool == Tool::LoopCut || m_Stroking || m_SculptAdjust != 0 || m_Painting || m_PaintAdjust != 0 ||
                   m_BoneModal.Kind != 0 || m_WeightPainting || m_CurveModal.Kind != 0;
        }
        // Vertex Paint (1) and Texture Paint (2): the model colored with brushes (MeshEditPaint.cpp); 0 off.
        int GetPaintMode() const { return m_Active ? m_PaintMode : 0; }
        void SetPaintMode(int mode);
        // Rigging (MeshEditRig.cpp): 1 the armature's bones, 2 posing them, 3 weight painting; 0 off.
        int GetRigMode() const { return m_Active ? m_RigMode : 0; }
        void SetRigMode(int mode);
        // Curve editing (MeshEditCurve.cpp): the model's curves' control points instead of its vertices.
        bool IsCurveEditing() const { return m_Active && m_CurveMode; }
        void SetCurveMode(bool on);
        // Sculpt Mode: the same model shaped with brushes instead of selected and moved (MeshEditSculpt.cpp).
        bool IsSculpting() const { return m_Active && m_Sculpting; }
        void SetSculpting(bool on);

        bool CanUndo() const { return !m_Undo.empty(); }
        bool CanRedo() const { return !m_Redo.empty(); }
        void Undo();
        void Redo();
        // The selection's world-space center and size (F frames it); false when nothing is selected.
        bool GetFrame(glm::vec3& center, float& radius) const;

        // GameObject > 3D Object > Model: a new model file (Assets/Models) of a primitive on a new object.
        enum class Primitive { Cube, Plane, Circle, Grid, UVSphere, IcoSphere, Cylinder, Cone, Torus, BezierCurve, CircleCurve, Text, Count };
        static const char* PrimitiveName(Primitive p);
        static Modeling::EditableMesh MakePrimitive(Primitive p);
        static Entity CreateModelObject(EditorContext& ctx, Primitive p, Entity parent = Entity::Null());
        // A new model file (unique name in `folder`) holding `mesh`; returns its handle.
        static AssetHandle CreateModelAsset(const Modeling::EditableMesh& mesh, const fs::path& folder, const std::string& name);
        // Object > Join (Ctrl+J outside Edit Mode): the other selected objects' meshes join the primary one's model
        // (which must be a model made in the editor); the other objects go.
        static bool JoinSelected(EditorContext& ctx);

    private:
        // A move, rotate or scale of UVs in the UV Editor (G, R, S there).
        enum class UVModalKind { None, Move, Rotate, Scale };
        struct UVModal
        {
            UVModalKind Kind = UVModalKind::None;
            std::vector<std::pair<Modeling::UV::Corner, glm::vec2>> Start;
            glm::vec2 Pivot{ 0.0f };
            ImVec2 MouseStart{ 0, 0 };
            int Axis = -1; // 0 u, 1 v
            float Angle = 0.0f, LastAngle = 0.0f;
            int StartFrame = 0;
        };
        enum class ModalKind { None, Move, Rotate, Scale, ShrinkFatten, Inset, Bevel, Slide };
        enum class Tool { None, LoopCut, Knife };
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
            int Axis = -1;                 // constrained axis (0 X, 1 Y, 2 Z, 3 the custom axis), -1 none
            glm::vec3 CustomAxis{ 0, 1, 0 }; // world space (an extrusion's normal)
            bool Plane = false;            // Shift+axis: the other two axes
            bool LocalAxes = false;        // the object's axes (axis key pressed twice)
            std::string Typed;             // numeric input
            // Tools that rebuild the mesh from where it started (inset, bevel) or move along precomputed paths.
            Modeling::EditableMesh Base;
            std::vector<glm::vec3> Dirs;   // shrink/fatten: per vertex (model space)
            Modeling::Ops::SlideRails Rails;
            ImVec2 SlideScreen{ 0, 0 };     // edge slide: the screen direction of +1
            float WorldPerPixel = 0.01f;   // at the pivot
            float Value = 0.0f, Applied = -1e30f;
            int Segments = 1, AppliedSegments = 0;
            bool Option = false, AppliedOption = false; // inset: each face on its own; bevel: vertices
        };

        // An object Separate made (kept to take it away on undo and make it again on redo).
        struct SeparatedPart
        {
            uint64_t Guid = 0, ParentGuid = 0;
            std::string Name;
            TransformComponent Transform;
            MeshRendererComponent Renderer;
        };
        void RemoveParts(std::vector<SeparatedPart>& parts);
        void RestoreParts(std::vector<SeparatedPart>& parts);
        // Texture Paint's undo: the changed tiles of the image (swapped with the image's on undo and redo).
        struct PaintTile
        {
            int X, Y, W, H;
            std::vector<uint32_t> Pixels;
        };
        struct PaintUndo
        {
            AssetHandle Texture = AssetHandle::None();
            std::vector<PaintTile> Tiles;
        };
        struct Snapshot
        {
            Modeling::EditableMesh Mesh;
            Modeling::SelectMode Mode;
            std::string Name;
            std::shared_ptr<PaintUndo> Paint; // a texture paint stroke
            std::shared_ptr<std::vector<SeparatedPart>> Parts; // objects a Separate made (undo removes them, redo makes them again)
        };

        // Records an undo step (before a change) and marks the model changed afterwards (Commit).
        void Record(const char* name);
        void Commit();         // the mesh changed: the object shows it, the file is written later
        void SyncRenderMesh(); // pushes the mesh to the object's renderer
        void SelectionChanged();

        void UpdateProjection(const ImVec2& origin, const ImVec2& size, const glm::mat4& view, const glm::mat4& proj);
        void DrawOverlay(ImDrawList* dl);
        void DrawToolbar(const ImVec2& origin);
        // Toolbars: the next item on this line when it fits before the view cube, else on a new one.
        void ToolbarNext(const char* label, float width = 0.0f);
        int m_OpenModelPopup = 0;
        void DrawMenus();
        void HandleKeys();
        void HandleMouse(bool hovered, bool blocked);
        void ClickSelect(const ImVec2& mouse, bool extend);
        void BoxSelect(const ImVec2& a, const ImVec2& b, int op); // op: 0 replace, 1 add, 2 subtract
        int64_t NearestVertex(const ImVec2& mouse, float maxPixels) const;
        int64_t NearestEdge(const ImVec2& mouse, float maxPixels) const;
        int64_t FaceUnderMouse(const ImVec2& mouse) const;

        // `recorded`: the operation that starts it already recorded the undo step (cancelling undoes the operation too).
        void BeginModal(ModalKind kind, bool recorded = false);
        void ApplyParametric();
        void UpdateToolModal(bool first, bool confirm); // shrink/fatten, inset, bevel, edge slide
        bool StartSlide();       // turns the modal into an edge slide (false: nothing to slide)
        bool ReadTyped(float& value); // numeric input while modal
        float WorldScale() const;
        // Loop cut (Ctrl+R) and knife (K).
        void StartTool(Tool tool);
        void UpdateTool(bool hovered);
        void UpdateLoopCut(bool hovered);
        void UpdateKnife(bool hovered);
        bool KnifeSnap(const ImVec2& mouse, Modeling::Ops::CutPoint& cut, glm::vec3& pos) const;
        void FinishKnife();
        void DrawToolOverlay(ImDrawList* dl);
        void UpdateModal();
        void EndModal(bool confirm);
        glm::vec3 AxisVector(int axis) const;
        bool GizmoManipulate(const ImVec2& origin, const ImVec2& size, const glm::mat4& view, const glm::mat4& proj);

        // Operations from keys and menus.
        void DoDelete(Modeling::Ops::DeleteKind kind);
        void DoAdd(Primitive p);
        void DoExtrude(bool individual);
        void DoDuplicate();
        void DoDissolve();
        void DoSeparate(bool looseParts);
        void DoSpin();
        // Runs an operation as one undo step and shows the result.
        template <class F> void Operate(const char* name, F&& fn)
        {
            Record(name);
            fn();
            Modeling::Ops::FlushSelection(m_Mesh, m_Mode);
            Commit();
        }

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
        int m_OpenMenu = 0;        // a popup to open at the mouse this frame (1 delete, 2 merge, 3 normals, 4 add, 5 edges, 6 faces, 7 extrude, 8 separate)
        float m_MergeDistance = 0.0001f;

        // Tools.
        Tool m_Tool = Tool::None;
        int m_LoopCuts = 1;
        int64_t m_LoopEdge = -1;
        uint64_t m_LoopPreviewKey = 0;
        std::vector<std::pair<glm::vec3, glm::vec3>> m_LoopPreview; // model space
        struct KnifePoint
        {
            Modeling::Ops::CutPoint Cut;
            glm::vec3 Pos; // model space
        };
        std::vector<KnifePoint> m_KnifePoints;
        int m_ToolStartFrame = 0;
        // The Modifiers section outside Edit Mode: the model as read from its file.
        Modeling::EditableMesh m_PanelMesh;
        AssetHandle m_PanelAsset = AssetHandle::None();
        fs::file_time_type m_PanelStamp{};
        bool m_PanelEditing = false; // a change is under way (one undo step)
        bool m_PanelUnsaved = false;
        // UV editing.
        void UVMenuItems(bool inUVEditor); // the UV menu (U in either view)
        template <class F> void UVOperate(const char* name, F&& fn)
        {
            Record(name);
            fn();
            Commit();
        }
        ImVec2 UVToScreen(const glm::vec2& uv) const;
        glm::vec2 ScreenToUV(const ImVec2& p) const;
        void FrameUVs(bool selectedOnly);
        void HandleUVKeys(const std::vector<uint32_t>& faces, bool hovered);
        void HandleUVMouse(const std::vector<uint32_t>& faces, bool hovered);
        void UVClickSelect(const std::vector<uint32_t>& faces, const ImVec2& mouse, bool extend);
        void UVBoxSelect(const std::vector<uint32_t>& faces, const ImVec2& a, const ImVec2& b, int op);
        void BeginUVModal(UVModalKind kind);
        void UpdateUVModal(const std::vector<uint32_t>& faces);
        void EndUVModal(bool confirm);
        AssetHandle UVBackground() const; // the texture drawn under the UVs
        glm::ivec2 UVTextureSize() const; // its size in pixels (64 x 64 without one)
        std::vector<uint32_t> UVFaces() const; // the faces the UV Editor shows (all visible ones with sync selection)
        UVModal m_UVModal;
        bool m_UVSync = false;            // the UV selection is the mesh's (UV Sync Selection)
        bool m_UVPixelSnap = false;       // moved UVs land on pixel corners
        bool m_FollowEven = false;        // Follow Active Quads: even steps (else edge lengths)
        bool m_StitchSnap = true;         // Stitch: islands move onto each other
        int m_UVMode = 0;                 // 0 vertex, 1 edge, 2 face, 3 island
        Modeling::UV::Sticky m_UVSticky = Modeling::UV::Sticky::Location;
        int m_UVStretch = -1;             // -1 off, else Modeling::UV::StretchMode
        bool m_UVShowTexture = true;
        ImVec2 m_UVCanvasMin{ 0, 0 }, m_UVCanvasSize{ 1, 1 }, m_UVPan{ 0, 0 };
        float m_UVZoom = 300.0f;
        bool m_UVFramed = false;
        bool m_UVPressing = false, m_UVBoxSelecting = false, m_UVPanning = false;
        ImVec2 m_UVPressPos{ 0, 0 };
        int m_UVOpenMenu = 0;             // 1 the UV menu at the mouse, 2 merge
        Modeling::UV::UnwrapSettings m_Unwrap;
        float m_SmartAngle = 66.0f;
        float m_CubeSize = 1.0f;
        int m_ProjectAxis = 1;
        float m_UVMergeDistance = 0.001f;
        // Sculpting.
        bool UpdateSculptView(bool hovered, bool focused, bool blocked);
        void DrawSculptToolbar(const ImVec2& origin);
        void HandleSculptKeys();
        void UpdateStroke(bool hovered, bool blocked);
        void UpdateSculptAdjust();
        void SculptRay(const ImVec2& mouse, glm::vec3& from, glm::vec3& dir) const; // model space
        float ModelPerPixel(const glm::vec3& at) const;                               // at a model-space point
        Modeling::Sculpt::BrushSettings StrokeBrush(const glm::vec3& at, const glm::vec3& viewDir) const;
        void EnsureSculptMesh();
        void SculptPreview(); // the object shows the sculpt mesh as it is (quick, during a stroke)
        void SculptCommit();  // the sculpt mesh back into the model (an undo step was recorded before)
        template <class F> void SculptOperate(const char* name, F&& fn)
        {
            EnsureSculptMesh();
            Record(name);
            fn();
            SculptCommit();
        }
        void DoVoxelRemesh();
        bool m_Sculpting = false, m_SculptValid = false;
        Modeling::Sculpt::SculptMesh m_Sculpt;
        Modeling::Sculpt::Brush m_BrushType = Modeling::Sculpt::Brush::Draw;
        float m_BrushRadiusPx = 60.0f;
        float m_BrushStrength[(size_t)Modeling::Sculpt::Brush::Count] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f, 0.5f };
        AssetHandle m_BrushTexAsset = AssetHandle::None(); // varies the brushes' strength
        Modeling::Sculpt::BrushTexture m_BrushTex;
        void LoadBrushTexture();
        bool m_LiveModifiers = true;      // strokes show the modifiers as they go (while that stays quick)
        float m_LiveModifierMs = 0.0f;
        bool m_BrushInvert = false, m_FrontFacesOnly = true;
        uint8_t m_Symmetry = 1;
        bool m_DynTopo = false, m_ConstantDetail = false;
        float m_DetailPx = 12.0f, m_DetailSize = 0.05f;
        float m_VoxelSize = 0.05f;
        bool m_RemeshSharp = false;
        bool m_Stroking = false;
        Modeling::Sculpt::Brush m_StrokeType = Modeling::Sculpt::Brush::Draw;
        bool m_StrokeInvert = false;
        glm::vec3 m_StrokeStart{ 0.0f }, m_StrokeLast{ 0.0f }, m_StrokeView{ 0.0f };
        ImVec2 m_StrokeMouse{ 0, 0 };
        bool m_StrokeMoved = false;
        int m_SculptAdjust = 0; // F: 1 radius, 2 strength (Shift+F)
        ImVec2 m_AdjustCenter{ 0, 0 };
        float m_AdjustStart = 0.0f;
        int m_AdjustFrame = 0;
        // Painting.
        bool UpdatePaintView(bool hovered, bool focused, bool blocked);
        void DrawPaintToolbar(const ImVec2& origin);
        void HandlePaintKeys();
        void UpdatePaintStroke(bool hovered, bool blocked);
        bool PaintHit(const ImVec2& mouse, glm::vec3& point, uint32_t& face, glm::vec3& dir); // model space
        Modeling::Paint::Brush PaintBrush(const glm::vec3& at, const glm::vec3& viewDir, bool blur) const;
        void PaintDab(const glm::vec3& at, const glm::vec3& viewDir, bool blur);
        AssetHandle PaintTarget() const; // the object's base color texture
        void AssignBaseColorTexture(AssetHandle texture);
        bool LoadPaintTexture();
        void NewPaintTexture();
        void SavePaintTexture();
        void SwapPaintUndo(PaintUndo& undo);
        void DrawBakePopup();
        void RunBake();
        int m_PaintMode = 0;
        bool m_Painting = false, m_PaintBlurStroke = false;
        int m_PaintAdjust = 0; // F: 1 radius, 2 strength
        ImVec2 m_PaintAdjustCenter{ 0, 0 };
        float m_PaintAdjustStart = 0.0f;
        int m_PaintAdjustFrame = 0;
        ImVec2 m_PaintMouse{ 0, 0 };
        glm::vec4 m_PaintColor{ 0.85f, 0.2f, 0.15f, 1.0f };
        Modeling::Paint::Blend m_PaintBlend = Modeling::Paint::Blend::Mix;
        float m_PaintRadiusPx = 40.0f, m_PaintStrength = 1.0f, m_PaintHardness = 0.0f;
        bool m_PaintFrontFaces = true;
        bool m_PaintLinear = false;
        uint8_t m_PaintSymmetry = 0;
        Modeling::MeshRaycaster m_PaintRays;
        uint64_t m_PaintRaysVersion = ~0ull;
        Modeling::Paint::Image m_PaintImage;
        AssetHandle m_PaintTexture = AssetHandle::None();
        bool m_PaintUnsaved = false, m_PaintGpuDirty = false;
        std::vector<uint32_t> m_PaintBefore;
        Modeling::Paint::Rect m_PaintStrokeRect;
        int m_NewTextureSize = 1024;
        glm::vec4 m_NewTextureColor{ 1.0f };
        Modeling::Bake::Settings m_Bake;
        Entity m_BakeSource = Entity::Null();
        bool m_BakeAssign = true, m_OpenBake = false;
        // Rigging.
        struct BoneModal
        {
            int Kind = 0; // 1 move, 2 rotate, 3 scale
            std::vector<Modeling::Bone> Start;
            ImVec2 MouseStart{ 0, 0 };
            glm::vec3 Pivot{ 0.0f };   // model space
            glm::vec3 PlaneStart{ 0.0f };
            float LastAngle = 0.0f, Angle = 0.0f;
            int Axis = -1;
            int StartFrame = 0;
            bool Recorded = false; // the operation before it recorded the undo step
        };
        bool UpdateRigView(bool hovered, bool focused, bool blocked);
        void DrawRigToolbar(const ImVec2& origin);
        void DrawBoneOverlay(ImDrawList* dl);
        void DrawShapeKeysPopup();
        void HandleRigKeys(bool hovered);
        void HandleRigMouse(bool hovered, bool blocked);
        int PickBone(const ImVec2& mouse, int* joint) const; // joint: BoneHead/BoneTail when near one, else BoneBody
        void BeginBoneModal(int kind, bool recorded = false);
        void UpdateBoneModal();
        void EndBoneModal(bool confirm);
        void UpdateWeightStroke(bool hovered, bool blocked);
        void SyncRigPreview(); // the object shows the posed mesh (Pose) or the active bone's weights (Weights)
        void RigOperate(const char* name, const std::function<void()>& fn);
        bool BoneScreen(int bone, ImVec2& head, ImVec2& tail) const;
        int m_RigMode = 0;
        int m_ActiveBone = -1;
        BoneModal m_BoneModal;
        bool m_WeightPainting = false;
        ImVec2 m_WeightMouse{ 0, 0 };
        Modeling::Rig::WeightMode m_WeightMode = Modeling::Rig::WeightMode::Mix;
        float m_WeightValue = 1.0f, m_WeightStrength = 0.5f, m_WeightRadiusPx = 40.0f;
        bool m_WeightFrontFaces = true;
        uint8_t m_WeightSymmetry = 1;
        int m_Action = -1;
        float m_ActionTime = 0.0f;
        bool m_ActionPlaying = false;
        bool m_OpenShapeKeys = false;
        bool m_KeySelectedOnly = false, m_KeyShapes = false, m_AutoIK = false, m_ShowDopeSheet = true;
        int m_AutoIKChain = 2;
        float m_SelectedKey = -1.0f;   // the dope sheet's selected key (its time), -1 none
        float m_DragKeyFrom = -1.0f;   // a key being dragged (its time when the drag began)
        void DrawDopeSheet(ImDrawList* dl);
        // Curves, text and retopology.
        struct CurveModal
        {
            int Kind = 0; // 1 move, 2 radius, 3 a handle
            std::vector<Modeling::Curve> Start;
            size_t HandleCurve = 0, HandlePoint = 0;
            int HandleSide = 0; // 1 in, 2 out
            glm::vec3 HandleStart{ 0.0f };
            glm::vec3 Pivot{ 0.0f }, PlaneStart{ 0.0f };
            ImVec2 MouseStart{ 0, 0 };
            int Axis = -1;
            int StartFrame = 0;
            bool Recorded = false;
        };
        bool UpdateCurveView(bool hovered, bool focused, bool blocked);
        void DrawCurveOverlay(ImDrawList* dl);
        void DrawCurveToolbar(const ImVec2& origin);
        void DrawTextPopup();
        void HandleCurveKeys();
        void HandleCurveMouse(bool hovered, bool blocked);
        bool CurvePointScreen(size_t curve, size_t point, ImVec2& out) const;
        void BeginCurveModal(int kind, bool recorded = false);
        void UpdateCurveModal();
        void EndCurveModal(bool confirm);
        void AddCurvePointAtMouse(const ImVec2& mouse);
        void ConvertGeneratedToMesh();
        // Snapping to other objects' surfaces (retopology).
        void BuildSnapTree();
        bool SnapRay(const glm::vec3& worldFrom, const glm::vec3& worldDir, glm::vec3& worldHit, glm::vec3& worldNormal);
        void SnapModalVertices();
        void AddVertexAtMouse(const ImVec2& mouse);
        bool m_CurveMode = false;
        CurveModal m_CurveModal;
        bool m_SnapSurface = false;
        int m_SnapMode = 0; // 0 from the view, 1 the nearest surface point
        float m_SnapOffset = 0.0f;
        Modeling::TriangleTree m_SnapTree;
        uint64_t m_SnapSignature = 0;
        // Operation settings.
        int m_SubdivideCuts = 1;
        float m_DissolveAngle = 5.0f;
        int m_SpinSteps = 12;
        float m_SpinAngle = 360.0f;
        int m_SpinAxis = 1;               // 0 X, 1 Y, 2 Z (the object's axes)
        bool m_SpinAboutSelection = false; // else about the object's origin
    };
}
