#pragma once
#include "EditorContext.h"
#include <Dimension/Image/ImagePaint.h>
#include <Dimension/Image/ImageEdit.h>
#include <Dimension/Image/ImageFilters.h>
#include <Dimension/Image/ImageGame.h>
#include <array>
#include <deque>
#include <functional>
#include <imgui.h>
#include <memory>
#include <string>

namespace Editor
{
    // Image Mode: a full-screen image editor (like Photoshop's) in place of the editor's windows. An image is a .dimage
    // asset (its layers kept); each save also writes the flattened picture (a PNG by default) that the game uses like any
    // other texture. Opening a PNG (or JPG, TGA, BMP) edits it through a .dimage made next to it that exports back to it.
    class ImageMode
    {
    public:
        explicit ImageMode(EditorContext& ctx);
        ~ImageMode();

        bool IsActive() const { return m_Active; }
        bool IsShown() const { return m_Active && !m_Suspended; }
        // Put aside while another workspace tab is shown (saved, but kept open with its undo history).
        void Suspend();
        void Resume();
        // The .dimage being edited (or last edited); it may not exist until the first save.
        const fs::path& GetFile() const { return m_File; }
        std::string GetName() const { return m_File.stem().string(); }
        // Opens a .dimage, or an image file through its .dimage (made from the picture when there is none yet).
        bool Open(const fs::path& file);
        // Saves and goes back to the editor.
        void Close();
        bool Save();
        // The whole window (call each frame while shown, instead of the editor's windows).
        void OnImGui();
        // File > New Image...: a name, a size and a background; makes the files in Assets/Images and opens them.
        void DrawNewImageWindow(bool* open);

        static bool IsImageFile(const fs::path& path); // .png .jpg .jpeg .tga .bmp
        static fs::path DocumentFor(const fs::path& image); // Pic.png -> Pic.dimage

        // Automation and tests.
        Dimension::Imaging::Document& GetDocument() { return m_Doc; }

    private:
        enum class Tool
        {
            Move, RectMarquee, EllipseMarquee, Lasso, PolyLasso, MagicWand, Crop,
            Brush, Pencil, Eraser, CloneStamp, Healing, Gradient, Fill, Blur, Sharpen, Smudge, Dodge, Burn, Sponge, Eyedropper,
            Type, RectShape, RoundShape, EllipseShape, PolygonShape, StarShape, LineShape, Hand, Zoom,
            Count
        };
        // The toolbar's slots: tools that share one (Photoshop's groups), the slot's key; Shift+key goes through them.
        struct Slot
        {
            std::vector<Tool> Tools;
            ImGuiKey Key;
            const char* KeyName;
        };
        struct Snapshot
        {
            Dimension::Imaging::Document Doc; // (pixels shared with the document until painted)
            int Active = 0;
            bool EditMask = false;
            std::string Label; // the change between this state and the next
        };
        struct Preset
        {
            std::string Name;
            Dimension::Imaging::BrushSettings Brush;
            bool BuiltIn = false;
        };

        void DrawTopBar();
        void DrawToolOptions();
        void DrawBrushOptions(Dimension::Imaging::BrushSettings& b);
        void DrawToolbar();
        void DrawCanvas(const ImVec2& size);
        void DrawColors(float width);
        void DrawLayers();
        void DrawLayerRow(int index, int depth, int& moveFrom, int& moveTo, bool& into);
        void DrawLayerMenu(int index);
        void DrawHistory();
        void DrawStatusBar();
        void HandleKeys();
        void DrawToolIcon(Tool tool, const ImVec2& min, const ImVec2& max, ImU32 color);
        void DrawThumbnail(const ImVec2& at, float size, const Dimension::Imaging::Layer& layer, bool mask);

        void SetZoom(float zoom, const ImVec2& anchor); // keeps the canvas point under `anchor` (screen) still
        void Fit();
        void UpdateTexture();
        void MarkAll() { m_DirtyAll = true; }
        void Mark(const Dimension::Imaging::Rect& r) { m_Dirty.Add(r); }

        // Undo: Record before a change (named for the History panel), Changed after it.
        void Record(const std::string& label);
        void Changed(bool dirty = true); // (selection changes are not edits to save)
        void Undo();
        void Redo();
        void TrimUndo();
        void Note(const std::string& text);

        void SelectTool(Tool tool);
        int SlotOf(Tool tool) const;
        static bool IsBrushTool(Tool tool);
        Dimension::Imaging::BrushSettings* ToolBrush(Tool tool);
        Tool EffectiveTool() const; // (Space: the hand; Alt with a painting tool: the eyedropper)
        bool CanEditTarget(Tool tool); // notes why not
        void BeginPaint(const glm::vec2& p, bool line);
        void ContinuePaint(const glm::vec2& p);
        void EndPaint();
        void PickColor(const glm::vec2& p, bool background);
        void ApplyGradient(const glm::vec2& a, const glm::vec2& b);

        void NewLayer();
        void GroupLayer();
        void Ungroup();
        void DuplicateLayer();
        void DeleteLayer();
        void MergeDown();
        void StepLayer(int direction);
        void MoveLayerTo(int index, int target, bool into);
        void AddMask(bool revealAll);
        void SetActive(int index, bool mask = false);

        void LoadPresets();
        void SavePresets();

        // ---- Selections, moving, transforming, cropping, the clipboard, menus and dialogs (ImageModeSelect.cpp)
        struct LayerCopy
        {
            uint32_t Id = 0;
            std::shared_ptr<const Dimension::Imaging::Bitmap> Pixels;
            std::shared_ptr<const Dimension::Imaging::Mask> LayerMask;
            Dimension::Imaging::TextData Text;   // (text and shape layers move by their settings)
            Dimension::Imaging::ShapeData Shape;
        };
        // Free Transform (Ctrl+T): the layers' pixels from before it, carried from Source (their bounds) to Quad.
        struct TransformState
        {
            bool Active = false;
            std::vector<LayerCopy> Layers;
            std::shared_ptr<const Dimension::Imaging::Mask> Selection;
            glm::vec2 Source[4], Quad[4], StartQuad[4];
            int Drag = -1; // 0-3 corners, 4-7 sides (top, right, bottom, left), 8 move, 9 rotate
            glm::vec2 DragStart{ 0.0f };
            bool Dirty = false;
            Dimension::Imaging::Resample Method = Dimension::Imaging::Resample::Bilinear;
        };
        // The Move tool's drag (or a selection outline dragged with a selection tool).
        struct MoveState
        {
            bool Active = false, SelectionOnly = false;
            std::vector<LayerCopy> Layers;
            std::shared_ptr<const Dimension::Imaging::Mask> Selection;
            glm::vec2 Start{ 0.0f };
            glm::ivec2 Offset{ 0 };
        };
        static bool IsSelectTool(Tool tool);
        void DrawMenuBar();
        void DrawSelectOptions();
        void DrawTransformOptions();
        void DrawCropOptions();
        void DrawDialogs();
        void SelectionInput(const glm::vec2& p, bool hovered, Tool tool);
        void CropInput(const glm::vec2& p, bool hovered);
        void TransformInput(const glm::vec2& p, bool hovered);
        void DrawOverlay(ImDrawList* dl, const glm::vec2& mouse);
        Dimension::Imaging::SelectOp OpFromKeys() const;
        void SetSelection(Dimension::Imaging::Mask shape, Dimension::Imaging::SelectOp op, const char* label);
        void Deselect();
        void SelectAll();
        void Reselect();
        void InverseSelection();
        void ModifySelection(int kind, float amount);
        std::vector<int> AffectedLayers(); // the active pixel layer, or the pixel layers in the active group
        bool BeginMove(const glm::vec2& p, bool selectionOnly);
        void UpdateMove(glm::ivec2 offset);
        void EndMove();
        void Nudge(glm::ivec2 by);
        bool BeginTransform();
        void ApplyTransform();
        void CommitTransform();
        void CancelTransform();
        void CommitCrop();
        void Copy(bool merged);
        void Cut();
        void Paste(bool inPlace);
        void Clear();
        void FillWith(const glm::vec4& color, const char* label);
        void LayerVia(bool cut);
        void TransformLayer(int kind); // 0 flip horizontal, 1 flip vertical, 2 rotate 90 clockwise, 3 counterclockwise, 4 180
        void CanvasChange(const char* label, const std::function<void()>& change);

        Dimension::Imaging::SelectOp m_SelectOp = Dimension::Imaging::SelectOp::Replace;
        float m_SelFeather = 0.0f;
        bool m_SelAntialias = true;
        float m_WandTolerance = 32.0f;
        bool m_WandContiguous = true, m_WandSampleAll = false;
        bool m_SelDrag = false;
        glm::vec2 m_SelStart{ 0.0f }, m_SelEnd{ 0.0f };
        Dimension::Imaging::SelectOp m_DragOp = Dimension::Imaging::SelectOp::Replace;
        int m_DragMods = 0; // Shift (1) and Alt (2) held as the drag began (they chose the operation, not the shape)
        std::vector<glm::vec2> m_Lasso;
        bool m_LassoActive = false;
        std::shared_ptr<const Dimension::Imaging::Mask> m_LastSelection; // Reselect
        std::shared_ptr<const Dimension::Imaging::Mask> m_OutlineOf; // (held: a freed selection's address could come back)
        std::vector<Dimension::Imaging::Segment> m_Outline;
        MoveState m_Move;
        TransformState m_Transform;
        bool m_CropActive = false;
        glm::vec2 m_CropA{ 0.0f }, m_CropB{ 0.0f }, m_CropStartA{ 0.0f }, m_CropStartB{ 0.0f }, m_CropDragStart{ 0.0f };
        int m_CropDrag = -1; // 0-3 corners, 4-7 sides, 8 move, 9 new
        Dimension::Imaging::Bitmap m_Clipboard;
        glm::ivec2 m_ClipboardAt{ 0 };
        glm::uvec2 m_ClipboardCanvas{ 0 };
        std::shared_ptr<const Dimension::Imaging::Mask> m_StrokeSelection;
        // Dialogs: 1 Image Size, 2 Canvas Size, 3 Color Range, 4 Expand, 5 Contract, 6 Feather, 7 Border.
        int m_Dialog = 0;
        bool m_OpenDialog = false;
        int m_DialogSize[2] = { 0, 0 };
        bool m_DialogConstrain = true;
        int m_DialogResample = 1; // Nearest, Bilinear
        int m_DialogAnchor = 4;
        float m_DialogValue = 0.0f;

        // ---- Adjustments, filters and layer styles (ImageModeAdjust.cpp)
        // A change shown live while its window is open: Image > Adjustments, a filter, or the Layer Style; OK keeps it.
        struct PreviewState
        {
            int Kind = 0; // 0 none, 1 an adjustment, 2 a filter, 3 the layer style
            uint32_t Layer = 0;
            bool Mask = false; // (on the layer's mask)
            std::shared_ptr<const Dimension::Imaging::Bitmap> Pixels;
            std::shared_ptr<const Dimension::Imaging::Mask> LayerMask;
            Dimension::Imaging::LayerEffects Effects, OriginalEffects;
            Dimension::Imaging::Adjustment Adjust;
            Dimension::Imaging::FilterSettings Filter;
            bool Dirty = false, Show = true;
            int Section = 0; // (the layer style's effect being set)
        };
        void OpenAdjustment(Dimension::Imaging::AdjustmentType type);
        void OpenFilter(Dimension::Imaging::FilterType type);
        void OpenLayerStyle(int section = 0);
        void NewAdjustmentLayer(Dimension::Imaging::AdjustmentType type);
        void UpdatePreview();
        void FinishPreview(bool keep);
        void DrawPreviewWindow();
        void DrawProperties();
        void DrawAdjustmentMenu(bool asLayer);
        void DrawFilterMenu();
        bool DrawAdjustmentUI(Dimension::Imaging::Adjustment& a, bool& started);
        bool DrawFilterUI(Dimension::Imaging::FilterSettings& f, bool& started);
        bool DrawEffectsUI(Dimension::Imaging::LayerEffects& e, int& section);
        bool CurveEditor(std::vector<glm::vec2>& points, bool& started);
        void RefreshHistogram(int channel);

        PreviewState m_Preview;
        bool m_ShowProperties = false;
        bool m_PropsRecorded = false;
        int m_LevelsChannel = 0, m_CurvesChannel = 0, m_BalanceTone = 1;
        int m_CurveDrag = -1;
        std::array<uint32_t, 256> m_Histogram{};
        int m_HistogramChannel = -1, m_HistogramAge = 0;

        // ---- Text and shapes (ImageModeVector.cpp)
        static bool IsShapeTool(Tool tool);
        void VectorInput(const glm::vec2& p, bool hovered, Tool tool);
        void DrawVectorOverlay(ImDrawList* dl, const glm::vec2& mouse);
        void DrawTextOptions();
        void DrawShapeOptions();
        bool DrawTextSettings(Dimension::Imaging::TextData& t, bool full);  // (full: the text and its placement too)
        bool DrawShapeSettings(Dimension::Imaging::ShapeData& s, bool full);
        void DrawVectorProperties();
        void BeginTextEdit(int index, const glm::vec2& p);
        void CommitText();
        void TextKeys();
        // A layer's settings changed through `change` (one undo step per edit, as Properties does).
        void EditVector(const std::function<bool(Dimension::Imaging::Layer&)>& change, const char* label);
        void RasterizeLayer();

        Dimension::Imaging::TextData m_TextStyle;
        Dimension::Imaging::ShapeData m_ShapeStyle;
        bool m_TextEditing = false, m_TextNew = false;
        uint32_t m_TextLayer = 0;
        size_t m_Caret = 0;
        std::string m_TextBefore;
        std::string m_FontFilter;
        bool m_ShapeDrag = false;
        glm::vec2 m_ShapeStart{ 0.0f }, m_ShapeEnd{ 0.0f };
        int m_ShapeMods = 0;
        bool m_VectorRecorded = false;

        // ---- Game art (ImageModeGame.cpp): swatches and palettes, sprite sheets, export, tiling and grid views
        void DrawSwatches();
        void LoadPalettes();
        void DrawGameDialog(); // Sprite Sheet (m_Dialog 8), Export As (9)
        void DrawGameOverlay(ImDrawList* dl);
        void ComputeSpriteRects();
        void ExportImage();

        std::vector<Dimension::Imaging::Palette> m_Palettes;
        size_t m_BuiltInPalettes = 0;
        int m_PaletteIndex = 0;
        bool m_PalettesLoaded = false;
        std::string m_PaletteName;
        int m_Symmetry = 0;          // brush mirrors: 1 left-right, 2 top-bottom
        bool m_PixelPerfect = false; // the pencil
        bool m_TilePreview = false, m_ShowGrid = false;
        int m_GridSize[2] = { 16, 16 };
        int m_SpriteMode = 0; // 0 by count, 1 by cell size, 2 automatic
        int m_SpriteCount[2] = { 4, 4 }, m_SpriteCell[2] = { 32, 32 }, m_SpriteOffset[2] = { 0, 0 }, m_SpriteGap[2] = { 0, 0 };
        bool m_SpriteSkipEmpty = true;
        int m_SpriteAlpha = 8, m_SpriteMin = 2;
        std::string m_SpritePrefix;
        std::vector<glm::ivec4> m_SpriteRects;
        std::string m_ExportName;
        int m_ExportScale = 2, m_ExportMethod = 0; // (scale: an index into the list; method: nearest, bilinear)
        bool m_ExportDefault = false;
        bool m_ReduceUseSwatches = true;
        // The GPU (compute shaders) composites and blurs when there is one; the CPU otherwise (and when turned off).
        bool m_UseGpu = true;
        double m_ComposeMs = 0.0;
        bool m_ComposedOnGpu = false;

        EditorContext& m_Ctx;
        bool m_Active = false, m_Suspended = false, m_Unsaved = false;
        fs::path m_File;
        Dimension::Imaging::Document m_Doc;
        int m_ActiveLayer = 0;
        bool m_EditMask = false; // painting goes to the active layer's mask
        std::deque<Snapshot> m_Undo, m_Redo;
        std::string m_StartLabel = "Open";

        // The composite shown on the canvas, and what changed since it was last made.
        Dimension::Imaging::Bitmap m_Composite;
        Dimension::Imaging::Rect m_Dirty;
        bool m_DirtyAll = true;
        AssetHandle m_Texture = AssetHandle::None();
        bool m_TexturePoint = false;
        uint32_t m_TextureW = 0, m_TextureH = 0;

        // The view: zoom (screen pixels per image pixel) and the canvas's center offset from the view's center.
        float m_Zoom = 1.0f;
        ImVec2 m_Pan{ 0, 0 };
        bool m_FitPending = true;
        ImVec2 m_ViewMin{ 0, 0 }, m_ViewSize{ 1, 1 };
        ImVec2 m_CanvasMin{ 0, 0 }; // the image's top-left on screen

        Tool m_Tool = Tool::Brush;
        std::vector<Slot> m_Slots;
        std::vector<Tool> m_SlotTool; // the tool each slot shows (the last used of its group)
        std::array<Dimension::Imaging::BrushSettings, (size_t)Tool::Count> m_Brushes;
        float m_Smoothing = 0.0f; // 0..1: the brush trails the mouse on a string (steadier lines)
        Dimension::Imaging::FillSettings m_Fill;
        Dimension::Imaging::GradientSettings m_Gradient;
        int m_GradientColors = 0; // foreground to background, foreground to transparent, black to white
        int m_SampleSize = 0;      // eyedropper: 0 point, 1 3x3, 2 5x5, 3 11x11
        bool m_SampleAll = true;   // eyedropper: every layer (else the current one)
        glm::vec4 m_Foreground{ 0, 0, 0, 1 }, m_Background{ 1, 1, 1, 1 };
        bool m_EditBackground = false; // which swatch the picker edits
        std::vector<Preset> m_Presets;
        std::string m_PresetName;
        bool m_OpenSavePreset = false;

        // Clone Stamp and Healing Brush: where Alt+click said to take from; Aligned keeps the offset between strokes.
        glm::vec2 m_CloneSource{ 0.0f };
        bool m_HasCloneSource = false, m_CloneAligned = true, m_CloneSampleAll = false;
        glm::vec2 m_CloneOffset{ 0.0f };
        bool m_CloneOffsetSet = false;
        Dimension::Imaging::Bitmap m_StrokeSource;

        std::unique_ptr<Dimension::Imaging::Stroke> m_Stroke;
        glm::vec2 m_LastPaint{ 0.0f }, m_SmoothPos{ 0.0f }, m_MousePaint{ 0.0f };
        bool m_HasLastPaint = false;
        bool m_GradientDrag = false;
        glm::vec2 m_GradientStart{ 0.0f };
        bool m_Panning = false;
        glm::vec2 m_Hover{ -1.0f };
        bool m_HoverInside = false;

        int m_RenamingLayer = -1;
        std::string m_RenameBuffer;
        int m_PanelTab = 0; // 0 Layers, 1 History
        bool m_ShowNew = false;
        std::string m_NewName = "New Image";
        int m_NewSize[2] = { 1024, 1024 };
        int m_NewBackground = 0; // White, Black, Transparent
        std::string m_Status;
        float m_StatusTime = 0.0f;
    };
}
