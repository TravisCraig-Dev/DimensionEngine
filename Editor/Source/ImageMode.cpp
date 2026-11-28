#include "ImageMode.h"
#include "EditorUI.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Asset/AssetManager.h>
#include <Dimension/Project/Project.h>
#include <Dimension/Renderer/Texture.h>
#include <Dimension/Image/ImageGpu.h>
#include <chrono>
#include <nlohmann/json.hpp>
#include <glm/gtc/constants.hpp>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace Editor
{
    using namespace Dimension;
    using namespace Dimension::Imaging;

    namespace
    {
        constexpr size_t kUndoBudget = size_t(1) << 30; // bytes of pixels the history may hold
        constexpr size_t kUndoMax = 100;
        constexpr float kMinZoom = 0.01f, kMaxZoom = 64.0f;
        constexpr const char* kToolNames[] = { "Move", "Rectangular Marquee", "Elliptical Marquee", "Lasso", "Polygonal Lasso", "Magic Wand", "Crop",
                                               "Brush", "Pencil",  "Eraser", "Clone Stamp", "Healing Brush", "Gradient",   "Paint Bucket", "Blur",
                                               "Sharpen", "Smudge", "Dodge",  "Burn",        "Sponge",        "Eyedropper", "Type",
                                               "Rectangle", "Rounded Rectangle", "Ellipse", "Polygon", "Star", "Line", "Hand", "Zoom" };

        ImVec2 Add(const ImVec2& a, const ImVec2& b) { return ImVec2(a.x + b.x, a.y + b.y); }
        ImVec2 Sub(const ImVec2& a, const ImVec2& b) { return ImVec2(a.x - b.x, a.y - b.y); }
        ImVec2 Mul(const ImVec2& a, float s) { return ImVec2(a.x * s, a.y * s); }
        ImU32 ToU32(const glm::vec4& c) { return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a)); }
        std::string Hex(const glm::vec4& c)
        {
            char buf[16];
            uint32_t p = Pack(c);
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF, p >> 24);
            return buf;
        }

        // A free file name `name` + ext in `folder` (with " 2", " 3"... when taken), checking both extensions.
        fs::path UniqueImageFile(const fs::path& folder, const std::string& name)
        {
            std::string base = name.empty() ? "New Image" : name;
            for (int i = 1;; ++i)
            {
                std::string n = i == 1 ? base : base + " " + std::to_string(i);
                if (!fs::exists(folder / (n + ".dimage")) && !fs::exists(folder / (n + ".png"))) return folder / (n + ".dimage");
            }
        }

        bool Percent(const char* id, float* value, float width, const char* format = "%.0f%%")
        {
            float v = *value * 100.0f;
            ImGui::SetNextItemWidth(width);
            bool changed = ImGui::SliderFloat(id, &v, 0.0f, 100.0f, format, ImGuiSliderFlags_AlwaysClamp);
            if (changed) *value = v / 100.0f;
            return changed;
        }

        void Label(const char* text)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::SameLine();
        }
    }

    ImageMode::ImageMode(EditorContext& ctx) : m_Ctx(ctx)
    {
        m_Slots = { { { Tool::Move }, ImGuiKey_V, "V" },
                    { { Tool::RectMarquee, Tool::EllipseMarquee }, ImGuiKey_M, "M" },
                    { { Tool::Lasso, Tool::PolyLasso }, ImGuiKey_L, "L" },
                    { { Tool::MagicWand }, ImGuiKey_W, "W" },
                    { { Tool::Crop }, ImGuiKey_C, "C" },
                    { { Tool::Brush, Tool::Pencil }, ImGuiKey_B, "B" },
                    { { Tool::Eraser }, ImGuiKey_E, "E" },
                    { { Tool::CloneStamp }, ImGuiKey_S, "S" },
                    { { Tool::Healing }, ImGuiKey_J, "J" },
                    { { Tool::Gradient, Tool::Fill }, ImGuiKey_G, "G" },
                    { { Tool::Blur, Tool::Sharpen, Tool::Smudge }, ImGuiKey_R, "R" },
                    { { Tool::Dodge, Tool::Burn, Tool::Sponge }, ImGuiKey_O, "O" },
                    { { Tool::Eyedropper }, ImGuiKey_I, "I" },
                    { { Tool::Type }, ImGuiKey_T, "T" },
                    { { Tool::RectShape, Tool::RoundShape, Tool::EllipseShape, Tool::PolygonShape, Tool::StarShape, Tool::LineShape }, ImGuiKey_U, "U" },
                    { { Tool::Hand }, ImGuiKey_H, "H" },
                    { { Tool::Zoom }, ImGuiKey_Z, "Z" } };
        for (const Slot& s : m_Slots) m_SlotTool.push_back(s.Tools.front());
        // Each tool keeps its own brush (as Photoshop does).
        auto set = [&](Tool t, float size, float hardness, float opacity, float flow, BrushTool kind) {
            BrushSettings& b = m_Brushes[(size_t)t];
            b.Size = size;
            b.Hardness = hardness;
            b.Opacity = opacity;
            b.Flow = flow;
            b.Tool = kind;
        };
        set(Tool::Brush, 24, 0.8f, 1, 1, BrushTool::Paint);
        set(Tool::Pencil, 1, 1, 1, 1, BrushTool::Paint);
        m_Brushes[(size_t)Tool::Pencil].Aliased = true;
        set(Tool::Eraser, 40, 0.8f, 1, 1, BrushTool::Erase);
        set(Tool::CloneStamp, 40, 0.5f, 1, 1, BrushTool::Clone);
        set(Tool::Healing, 24, 0.8f, 1, 1, BrushTool::Heal);
        set(Tool::Blur, 30, 0.5f, 0.5f, 1, BrushTool::Blur);
        set(Tool::Sharpen, 30, 0.5f, 0.5f, 1, BrushTool::Sharpen);
        set(Tool::Smudge, 30, 0.5f, 0.5f, 1, BrushTool::Smudge);
        set(Tool::Dodge, 60, 0.0f, 0.5f, 1, BrushTool::Dodge);
        set(Tool::Burn, 60, 0.0f, 0.5f, 1, BrushTool::Burn);
        set(Tool::Sponge, 60, 0.0f, 0.5f, 1, BrushTool::Sponge);
    }

    ImageMode::~ImageMode()
    {
        if (m_Texture.IsValid() && AssetManager::IsInitialized()) AssetManager::DestroyRuntimeTexture(m_Texture);
    }

    bool ImageMode::IsImageFile(const fs::path& path)
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp";
    }

    fs::path ImageMode::DocumentFor(const fs::path& image)
    {
        fs::path p = image;
        return p.replace_extension(".dimage");
    }

    bool ImageMode::Open(const fs::path& path)
    {
        fs::path file = IsImageFile(path) ? DocumentFor(path) : path;
        if (m_Active && file == m_File)
        {
            Resume();
            return true;
        }
        Document doc;
        if (IsImageFile(path) && !fs::exists(file))
        {
            Bitmap picture;
            if (!Bitmap::Load(path, picture)) return false;
            doc = Document::FromBitmap(std::move(picture));
            doc.Export = path.filename().string(); // (saving writes the picture back where it was)
        }
        else if (!Document::Load(file, doc))
            return false;
        if (m_Active) Close();
        if (m_Presets.empty()) LoadPresets();
        m_Doc = std::move(doc);
        m_File = file;
        m_Active = true;
        m_Suspended = false;
        m_Unsaved = false;
        m_ActiveLayer = (int)m_Doc.Layers.size() - 1;
        m_EditMask = false;
        m_Undo.clear();
        m_Redo.clear();
        m_StartLabel = "Open";
        m_Stroke.reset();
        m_HasLastPaint = m_HasCloneSource = m_CloneOffsetSet = m_GradientDrag = false;
        m_Transform = TransformState();
        m_Move = MoveState();
        m_Preview = PreviewState();
        m_SelDrag = m_LassoActive = m_CropActive = m_TextEditing = m_ShapeDrag = false;
        m_LastSelection.reset();
        m_OutlineOf.reset();
        m_RenamingLayer = -1;
        m_FitPending = true;
        MarkAll();
        Log::Info("Opened image '{}' (Image Mode)", GetName());
        return true;
    }

    void ImageMode::Close()
    {
        if (!m_Active) return;
        CommitText();
        FinishPreview(true);
        CommitTransform();
        EndMove();
        EndPaint();
        if (m_Unsaved) Save();
        m_Active = false;
        m_Suspended = false;
        m_Undo.clear();
        m_Redo.clear();
        m_Doc = Document();
        m_Composite = Bitmap();
        m_StrokeSource = Bitmap();
        Imaging::Gpu::Release();
        SetBoxBlurHook(nullptr);
        if (m_Texture.IsValid()) AssetManager::DestroyRuntimeTexture(m_Texture);
        m_Texture = AssetHandle::None();
    }

    void ImageMode::Suspend()
    {
        if (!m_Active || m_Suspended) return;
        CommitText();
        FinishPreview(true);
        CommitTransform();
        EndMove();
        EndPaint();
        m_GradientDrag = false;
        if (m_Unsaved) Save();
        m_Suspended = true;
    }

    void ImageMode::Resume()
    {
        if (m_Active) m_Suspended = false;
    }

    bool ImageMode::Save()
    {
        if (!m_Active) return false;
        std::error_code ec;
        fs::create_directories(m_File.parent_path(), ec);
        if (!m_Doc.Save(m_File))
        {
            Log::Error("Could not write the image {}", m_File.string());
            Note("Could not save");
            return false;
        }
        // The flattened picture the game uses (shown at once by whatever uses the texture).
        fs::path out = m_Doc.ExportPath(m_File);
        Bitmap flat = m_Doc.Flatten();
        bool exported = Bitmap::CanSave(out) && flat.Save(out);
        if (!exported) Log::Error("Could not write the image's picture {}", out.string());
        AssetManager::Import(m_File);
        if (exported)
        {
            AssetHandle texture = AssetManager::Import(out);
            if (texture.IsValid() && AssetManager::IsLoaded(texture))
                AssetManager::SetTexturePixels(texture, flat.Pixels.data(), flat.Width, flat.Height);
        }
        m_Unsaved = false;
        Note("Saved " + m_File.filename().string() + (exported ? " and " + out.filename().string() : std::string()));
        return exported;
    }

    void ImageMode::Note(const std::string& text)
    {
        m_Status = text;
        m_StatusTime = 4.0f;
    }

    // ---- Undo

    void ImageMode::Record(const std::string& label)
    {
        m_Undo.push_back({ m_Doc, m_ActiveLayer, m_EditMask, label });
        m_Redo.clear();
        TrimUndo();
    }

    void ImageMode::Changed(bool dirty)
    {
        if (dirty) m_Unsaved = true;
        m_ActiveLayer = std::clamp(m_ActiveLayer, 0, std::max(0, (int)m_Doc.Layers.size() - 1));
        if (m_EditMask && (m_Doc.Layers.empty() || !m_Doc.Layers[(size_t)m_ActiveLayer].HasMask())) m_EditMask = false;
    }

    void ImageMode::TrimUndo()
    {
        auto drop = [&] {
            m_StartLabel = m_Undo.front().Label;
            m_Undo.pop_front();
        };
        while (m_Undo.size() > kUndoMax) drop();
        auto total = [&] {
            std::vector<const void*> seen;
            size_t bytes = m_Doc.MemoryBytes(&seen);
            for (const Snapshot& s : m_Undo) bytes += s.Doc.MemoryBytes(&seen);
            return bytes;
        };
        while (m_Undo.size() > 1 && total() > kUndoBudget) drop();
    }

    void ImageMode::Undo()
    {
        CommitText();
        if (m_Preview.Kind)
        {
            FinishPreview(false);
            return;
        }
        if (m_Transform.Active)
        {
            CancelTransform(); // (Ctrl+Z in a transform: the transform goes)
            return;
        }
        EndMove();
        if (m_Undo.empty()) return;
        EndPaint();
        Snapshot& s = m_Undo.back();
        m_Redo.push_back({ m_Doc, m_ActiveLayer, m_EditMask, s.Label });
        m_Doc = std::move(s.Doc);
        m_ActiveLayer = s.Active;
        m_EditMask = s.EditMask;
        m_Undo.pop_back();
        Changed();
        MarkAll();
    }

    void ImageMode::Redo()
    {
        CommitText();
        if (m_Transform.Active || m_Redo.empty()) return;
        EndMove();
        EndPaint();
        Snapshot& s = m_Redo.back();
        m_Undo.push_back({ m_Doc, m_ActiveLayer, m_EditMask, s.Label });
        m_Doc = std::move(s.Doc);
        m_ActiveLayer = s.Active;
        m_EditMask = s.EditMask;
        m_Redo.pop_back();
        Changed();
        MarkAll();
    }

    // ---- Layers

    void ImageMode::SetActive(int index, bool mask)
    {
        if (index < 0 || index >= (int)m_Doc.Layers.size()) return;
        m_ActiveLayer = index;
        m_EditMask = mask && m_Doc.Layers[(size_t)index].HasMask();
    }

    void ImageMode::NewLayer()
    {
        Record("New Layer");
        int at = m_ActiveLayer + 1;
        uint32_t parent = 0;
        if (m_ActiveLayer < (int)m_Doc.Layers.size())
        {
            const Layer& active = m_Doc.Layers[(size_t)m_ActiveLayer];
            parent = active.Parent;
            // On a group: at the top of the group.
            if (active.IsGroup())
            {
                parent = active.Id;
                std::vector<int> kids = m_Doc.Children(active.Id);
                at = kids.empty() ? m_ActiveLayer : kids.back() + 1;
            }
        }
        SetActive(m_Doc.AddLayer(m_Doc.UniqueLayerName("Layer 1"), at, parent));
        Changed();
        MarkAll();
    }

    void ImageMode::GroupLayer()
    {
        if (m_Doc.Layers.empty()) return;
        Record("Group Layers");
        SetActive(m_Doc.GroupLayer(m_ActiveLayer));
        Changed();
        MarkAll();
    }

    void ImageMode::Ungroup()
    {
        if (m_Doc.Layers.empty() || !m_Doc.Layers[(size_t)m_ActiveLayer].IsGroup()) return;
        Record("Ungroup Layers");
        int top = m_Doc.Ungroup(m_ActiveLayer);
        SetActive(top >= 0 ? top : std::max(0, m_ActiveLayer - 1));
        Changed();
        MarkAll();
    }

    void ImageMode::DuplicateLayer()
    {
        if (m_Doc.Layers.empty()) return;
        Record(m_Doc.Layers[(size_t)m_ActiveLayer].IsGroup() ? "Duplicate Group" : "Duplicate Layer");
        SetActive(m_Doc.DuplicateLayer(m_ActiveLayer));
        Changed();
        MarkAll();
    }

    void ImageMode::DeleteLayer()
    {
        if (m_Doc.Layers.empty()) return;
        const Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
        if (l.LockAll)
        {
            Note("The layer is locked");
            return;
        }
        size_t going = 1;
        for (size_t i = 0; i < m_Doc.Layers.size(); ++i) going += m_Doc.IsInside((int)i, l.Id) ? 1 : 0;
        if (going >= m_Doc.Layers.size())
        {
            Note("An image keeps at least one layer");
            return;
        }
        Record(l.IsGroup() ? "Delete Group" : "Delete Layer");
        int below = m_Doc.Below(m_ActiveLayer);
        uint32_t keep = below >= 0 ? m_Doc.Layers[(size_t)below].Id : l.Parent;
        m_Doc.RemoveLayer(m_ActiveLayer);
        int next = keep ? m_Doc.IndexOf(keep) : -1;
        SetActive(next >= 0 ? next : (int)m_Doc.Layers.size() - 1);
        Changed();
        MarkAll();
    }

    void ImageMode::MergeDown()
    {
        if (m_Doc.Layers.empty()) return;
        const Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
        if (l.IsGroup())
        {
            Record("Merge Group");
            SetActive(m_Doc.MergeGroup(m_ActiveLayer));
        }
        else
        {
            int below = m_Doc.Below(m_ActiveLayer);
            if (below < 0 || !m_Doc.Layers[(size_t)below].HasPixels())
            {
                Note("Merge Down needs a pixel layer (not a group or an adjustment) under this one");
                return;
            }
            if (m_Doc.Layers[(size_t)below].LockAll || m_Doc.Layers[(size_t)below].LockPixels)
            {
                Note("The layer under this one is locked");
                return;
            }
            Record("Merge Down");
            SetActive(m_Doc.MergeDown(m_ActiveLayer));
        }
        Changed();
        MarkAll();
    }

    void ImageMode::StepLayer(int direction)
    {
        if (m_Doc.Layers.empty()) return;
        Record("Move Layer");
        int moved = m_Doc.Step(m_ActiveLayer, direction);
        if (moved == m_ActiveLayer && m_Undo.size() > 0)
        {
            m_Undo.pop_back(); // (already at the end)
            return;
        }
        SetActive(moved, m_EditMask);
        Changed();
        MarkAll();
    }

    void ImageMode::MoveLayerTo(int index, int target, bool into)
    {
        if (index == target || index < 0 || target < 0) return;
        Record("Move Layer");
        const Layer& t = m_Doc.Layers[(size_t)target];
        int moved;
        if (into)
        {
            std::vector<int> kids = m_Doc.Children(t.Id);
            moved = m_Doc.MoveInto(index, t.Id, kids.empty() ? -1 : kids.back());
            if (moved >= 0) m_Doc.Layers[(size_t)m_Doc.IndexOf(t.Id)].Expanded = true;
        }
        else
            moved = m_Doc.MoveInto(index, t.Parent, target);
        if (moved < 0)
        {
            m_Undo.pop_back();
            Note("A group cannot go inside itself");
            return;
        }
        SetActive(moved);
        Changed();
        MarkAll();
    }

    void ImageMode::AddMask(bool revealAll)
    {
        if (m_Doc.Layers.empty()) return;
        if (m_Doc.Layers[(size_t)m_ActiveLayer].HasMask())
        {
            SetActive(m_ActiveLayer, true);
            return;
        }
        Record("Add Layer Mask");
        m_Doc.AddMask(m_ActiveLayer, revealAll);
        // With a selection: the mask shows (or hides) what is selected, and the selection goes.
        if (m_Doc.Selection)
        {
            Mask m = *m_Doc.Selection;
            if (!revealAll) InvertSelection(m);
            m_Doc.Layers[(size_t)m_ActiveLayer].LayerMask = std::make_shared<Mask>(std::move(m));
            m_LastSelection = m_Doc.Selection;
            m_Doc.Selection.reset();
        }
        SetActive(m_ActiveLayer, true);
        Changed();
        MarkAll();
    }

    // ---- Brush presets (the built-in ones, and the project's own in .dimension/ImageBrushes.json)

    void ImageMode::LoadPresets()
    {
        m_Presets.clear();
        auto add = [&](const char* name, float size, float hardness, float flow, float spacing, float roundness, float angle) {
            Preset p;
            p.Name = name;
            p.Brush.Size = size;
            p.Brush.Hardness = hardness;
            p.Brush.Flow = flow;
            p.Brush.Spacing = spacing;
            p.Brush.Roundness = roundness;
            p.Brush.Angle = angle;
            p.BuiltIn = true;
            m_Presets.push_back(p);
        };
        add("Hard Round", 24, 1.0f, 1.0f, 0.25f, 1.0f, 0.0f);
        add("Soft Round", 60, 0.0f, 1.0f, 0.25f, 1.0f, 0.0f);
        add("Airbrush", 90, 0.0f, 0.1f, 0.1f, 1.0f, 0.0f);
        add("Hard Small", 6, 1.0f, 1.0f, 0.2f, 1.0f, 0.0f);
        add("Flat Angled", 40, 0.9f, 1.0f, 0.1f, 0.25f, 45.0f);
        add("Calligraphy", 24, 1.0f, 1.0f, 0.05f, 0.1f, 30.0f);
        fs::path file = Project::GetDirectory() / ".dimension" / "ImageBrushes.json";
        std::ifstream in(file);
        if (!in) return;
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_array()) return;
        for (const auto& e : j)
        {
            Preset p;
            p.Name = e.value("name", std::string("Brush"));
            p.Brush.Size = e.value("size", 24.0f);
            p.Brush.Hardness = e.value("hardness", 0.8f);
            p.Brush.Flow = e.value("flow", 1.0f);
            p.Brush.Spacing = e.value("spacing", 0.25f);
            p.Brush.Roundness = e.value("roundness", 1.0f);
            p.Brush.Angle = e.value("angle", 0.0f);
            m_Presets.push_back(p);
        }
    }

    void ImageMode::SavePresets()
    {
        nlohmann::json j = nlohmann::json::array();
        for (const Preset& p : m_Presets)
            if (!p.BuiltIn)
                j.push_back({ { "name", p.Name },
                              { "size", p.Brush.Size },
                              { "hardness", p.Brush.Hardness },
                              { "flow", p.Brush.Flow },
                              { "spacing", p.Brush.Spacing },
                              { "roundness", p.Brush.Roundness },
                              { "angle", p.Brush.Angle } });
        fs::path dir = Project::GetDirectory() / ".dimension";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream(dir / "ImageBrushes.json") << j.dump(2);
    }

    // ---- Tools

    bool ImageMode::IsBrushTool(Tool tool)
    {
        switch (tool)
        {
        case Tool::Brush:
        case Tool::Pencil:
        case Tool::Eraser:
        case Tool::CloneStamp:
        case Tool::Healing:
        case Tool::Blur:
        case Tool::Sharpen:
        case Tool::Smudge:
        case Tool::Dodge:
        case Tool::Burn:
        case Tool::Sponge: return true;
        default: return false;
        }
    }

    BrushSettings* ImageMode::ToolBrush(Tool tool) { return IsBrushTool(tool) ? &m_Brushes[(size_t)tool] : nullptr; }

    int ImageMode::SlotOf(Tool tool) const
    {
        for (size_t s = 0; s < m_Slots.size(); ++s)
            if (std::find(m_Slots[s].Tools.begin(), m_Slots[s].Tools.end(), tool) != m_Slots[s].Tools.end()) return (int)s;
        return 0;
    }

    void ImageMode::SelectTool(Tool tool)
    {
        if (m_Tool != tool)
        {
            EndPaint();
            CommitTransform();
            EndMove();
            CommitText();
            m_SelDrag = m_LassoActive = m_ShapeDrag = false;
            m_Lasso.clear();
            if (tool == Tool::Crop) m_CropActive = false; // (starts as the whole canvas)
        }
        m_GradientDrag = false;
        m_Tool = tool;
        m_SlotTool[(size_t)SlotOf(tool)] = tool;
    }

    ImageMode::Tool ImageMode::EffectiveTool() const
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && !m_TextEditing && ImGui::IsKeyDown(ImGuiKey_Space)) return Tool::Hand;
        if (io.KeyAlt && (m_Tool == Tool::Brush || m_Tool == Tool::Pencil || m_Tool == Tool::Fill || m_Tool == Tool::Gradient)) return Tool::Eyedropper;
        return m_Tool;
    }

    bool ImageMode::CanEditTarget(Tool tool)
    {
        if (m_Doc.Layers.empty()) return false;
        const Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
        if (!m_Doc.ShownInTree(m_ActiveLayer))
        {
            Note("The layer is hidden: show it to paint on it");
            return false;
        }
        if (m_EditMask && l.HasMask())
        {
            if (tool == Tool::Brush || tool == Tool::Pencil || tool == Tool::Eraser || tool == Tool::Gradient) return true;
            Note(std::string(kToolNames[(int)tool]) + " works on pixels: click the layer's thumbnail (the mask is selected)");
            return false;
        }
        if (l.IsGroup())
        {
            Note("This is a group: select a layer in it to paint, or add a mask to the group");
            return false;
        }
        if (l.IsAdjustment())
        {
            Note("This is an adjustment layer: paint its mask (click the mask's thumbnail)");
            return false;
        }
        if (l.IsVector())
        {
            Note(std::string("This is a ") + (l.Kind == LayerKind::Text ? "text" : "shape") + " layer: Layer > Rasterize to paint on it");
            return false;
        }
        if (l.LockAll || l.LockPixels)
        {
            Note("The layer is locked");
            return false;
        }
        return true;
    }

    void ImageMode::BeginPaint(const glm::vec2& p, bool line)
    {
        BrushSettings* settings = ToolBrush(m_Tool);
        if (!settings || !CanEditTarget(m_Tool)) return;
        BrushSettings brush = *settings;
        brush.Color = m_Foreground;
        brush.Erase = false;
        if (m_Tool == Tool::CloneStamp || m_Tool == Tool::Healing)
        {
            if (!m_HasCloneSource)
            {
                Note("Alt+click where to take the pixels from first");
                return;
            }
            // Aligned: the offset set by the first stroke after Alt+click stays; otherwise each stroke starts at the source.
            glm::vec2 offset = m_CloneSource - p;
            if (m_CloneAligned)
            {
                if (!m_CloneOffsetSet) m_CloneOffset = offset, m_CloneOffsetSet = true;
                offset = m_CloneOffset;
            }
            m_StrokeSource = m_CloneSampleAll ? m_Composite : m_Doc.Layers[(size_t)m_ActiveLayer].Get();
            brush.Source = &m_StrokeSource;
            brush.SourceOffset = offset;
        }
        m_StrokeSelection = m_Doc.Selection;
        brush.Selection = m_StrokeSelection.get();
        if (m_Tool == Tool::Brush || m_Tool == Tool::Pencil || m_Tool == Tool::Eraser)
        {
            brush.Symmetry = m_Symmetry;
            brush.SymmetryCenter = glm::vec2(m_Doc.Width * 0.5f, m_Doc.Height * 0.5f);
        }
        brush.PixelPerfect = m_PixelPerfect && m_Tool == Tool::Pencil;
        Record(m_EditMask ? std::string(kToolNames[(int)m_Tool]) + " (Mask)" : kToolNames[(int)m_Tool]);
        m_Stroke = std::make_unique<Stroke>(m_Doc, m_ActiveLayer, brush, m_EditMask);
        // Shift+click: a straight line from where the last stroke ended.
        if (line && m_HasLastPaint) m_Stroke->To(m_LastPaint);
        m_Stroke->To(p);
        m_LastPaint = m_SmoothPos = m_MousePaint = p;
        Mark(m_Stroke->TakeChanged());
    }

    void ImageMode::ContinuePaint(const glm::vec2& p)
    {
        if (!m_Stroke) return;
        m_MousePaint = p;
        glm::vec2 target = p;
        if (m_Smoothing > 0.0f)
        {
            // The brush on a string behind the mouse: it moves only once the string is taut.
            float string = m_Smoothing * 80.0f / m_Zoom;
            glm::vec2 d = p - m_SmoothPos;
            float len = glm::length(d);
            if (len <= string) return;
            m_SmoothPos += d * (1.0f - string / len);
            target = m_SmoothPos;
        }
        if (target != m_LastPaint) m_Stroke->To(target);
        m_LastPaint = target;
        Mark(m_Stroke->TakeChanged());
    }

    void ImageMode::EndPaint()
    {
        if (!m_Stroke) return;
        if (m_Smoothing > 0.0f && m_MousePaint != m_LastPaint)
        {
            m_Stroke->To(m_MousePaint); // (catches up with the mouse)
            m_LastPaint = m_MousePaint;
        }
        m_Stroke->Finish();
        Mark(m_Stroke->TakeChanged());
        if (m_Stroke->Bounds().Empty() && !m_Undo.empty()) m_Undo.pop_back(); // (nothing painted)
        else Changed();
        m_Stroke.reset();
        m_HasLastPaint = true;
    }

    void ImageMode::PickColor(const glm::vec2& p, bool background)
    {
        glm::ivec2 at((int)std::floor(p.x), (int)std::floor(p.y));
        if (at.x < 0 || at.y < 0 || at.x >= (int)m_Doc.Width || at.y >= (int)m_Doc.Height) return;
        static constexpr int kRadius[] = { 0, 1, 2, 5 };
        const Layer* layer = m_ActiveLayer < (int)m_Doc.Layers.size() ? &m_Doc.Layers[(size_t)m_ActiveLayer] : nullptr;
        const Bitmap& source = m_SampleAll || !layer || !layer->IsRaster() || !layer->Pixels ? m_Composite : layer->Get();
        if (source.Width != m_Doc.Width) return;
        glm::vec4 c = SampleColor(source, at, kRadius[std::clamp(m_SampleSize, 0, 3)]);
        if (c.a <= 0.0f) return; // (nothing there)
        c.a = 1.0f;
        (background ? m_Background : m_Foreground) = c;
    }

    void ImageMode::ApplyGradient(const glm::vec2& a, const glm::vec2& b)
    {
        if (glm::length(b - a) < 0.5f) return;
        GradientSettings g = m_Gradient;
        g.From = m_GradientColors == 2 ? glm::vec4(0, 0, 0, 1) : m_Foreground;
        g.To = m_GradientColors == 0 ? m_Background : m_GradientColors == 1 ? glm::vec4(glm::vec3(m_Foreground), 0.0f) : glm::vec4(1.0f);
        g.Selection = m_Doc.Selection.get();
        Record(m_EditMask ? "Gradient (Mask)" : "Gradient");
        Rect r = DrawGradient(m_Doc, m_ActiveLayer, a, b, g, m_EditMask);
        if (r.Empty()) m_Undo.pop_back();
        else
        {
            Mark(r);
            Changed();
        }
    }

    // ---- View

    void ImageMode::SetZoom(float zoom, const ImVec2& anchor)
    {
        zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
        ImVec2 viewCenter = Add(m_ViewMin, Mul(m_ViewSize, 0.5f));
        ImVec2 canvas((float)m_Doc.Width, (float)m_Doc.Height);
        ImVec2 point = Mul(Sub(anchor, m_CanvasMin), 1.0f / m_Zoom); // (image pixels under the anchor)
        ImVec2 newMin = Sub(anchor, Mul(point, zoom));
        m_Pan = Sub(Add(newMin, Mul(canvas, zoom * 0.5f)), viewCenter);
        m_Zoom = zoom;
        m_CanvasMin = newMin;
    }

    void ImageMode::Fit()
    {
        if (!m_Doc.Valid()) return;
        float zx = (m_ViewSize.x - 40.0f) / m_Doc.Width, zy = (m_ViewSize.y - 40.0f) / m_Doc.Height;
        float zoom = std::clamp(std::min(zx, zy), kMinZoom, kMaxZoom);
        if (zoom > 1.0f) zoom = std::max(1.0f, std::floor(zoom)); // (whole steps when enlarged: crisp pixels)
        m_Zoom = zoom;
        m_Pan = ImVec2(0, 0);
    }

    void ImageMode::UpdateTexture()
    {
        if (!m_Doc.Valid()) return;
        const bool point = m_Zoom >= 2.0f; // (pixels shown as squares once enlarged, like Photoshop)
        if (!m_Texture.IsValid() || m_TextureW != m_Doc.Width || m_TextureH != m_Doc.Height || point != m_TexturePoint)
        {
            if (m_Texture.IsValid()) AssetManager::DestroyRuntimeTexture(m_Texture);
            // (not sRGB: the pixels go to the screen as they are, as ImGui's colors do; an sRGB view would darken them)
            m_Texture = AssetManager::CreateRuntimeTexture(m_Doc.Width, m_Doc.Height, false, point, this);
            m_TextureW = m_Doc.Width;
            m_TextureH = m_Doc.Height;
            m_TexturePoint = point;
            m_DirtyAll = true;
        }
        // Blurs (filters, previews) on the GPU too while it composites.
        const bool gpu = m_UseGpu && Imaging::Gpu::Available();
        SetBoxBlurHook(gpu ? &Imaging::Gpu::BoxBlur : nullptr);
        if (!m_DirtyAll && m_Dirty.Empty()) return;
        const auto start = std::chrono::steady_clock::now();
        const bool all = m_DirtyAll || m_Composite.Width != m_Doc.Width || m_Composite.Height != m_Doc.Height;
        bool done = false;
        if (gpu)
        {
            // Only brushes change pixels in place (everything else makes new ones, which go up by themselves): what the
            // active layer (or its mask) changed goes up again.
            if (!all && !m_Doc.Layers.empty())
            {
                const Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
                if (l.Pixels) Imaging::Gpu::Touch(l.Pixels->Pixels.data(), m_Dirty, m_Doc.Width, 4);
                if (l.LayerMask) Imaging::Gpu::Touch(l.LayerMask->Values.data(), m_Dirty, m_Doc.Width, 1);
            }
            done = all ? Imaging::Gpu::Composite(m_Doc, m_Composite) : Imaging::Gpu::Composite(m_Doc, m_Composite, m_Dirty);
        }
        if (!done)
        {
            if (all) m_Doc.Composite(m_Composite);
            else m_Doc.Composite(m_Composite, m_Dirty);
        }
        m_ComposedOnGpu = done;
        m_ComposeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        m_DirtyAll = false;
        m_Dirty = {};
        AssetManager::StreamRuntimeTexturePixels(m_Texture, m_Composite.Pixels.data(), m_Composite.Pixels.size());
    }

    // ---- The window

    void ImageMode::OnImGui()
    {
        if (!IsShown()) return;
        m_StatusTime = std::max(0.0f, m_StatusTime - ImGui::GetIO().DeltaTime);
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
                                 ImGuiWindowFlags_NoNavInputs; // (the arrows nudge; they do not move ImGui's focus)
        ImGui::Begin("##ImageMode", nullptr, flags);
        ImGui::PopStyleVar(2);
        DrawMenuBar();
        DrawTopBar();
        DrawToolOptions();
        ImGui::Separator();

        const ImGuiStyle& style = ImGui::GetStyle();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const float statusH = ImGui::GetFrameHeight();
        const float bodyH = avail.y - statusH - style.ItemSpacing.y;
        const float toolW = 44.0f;
        const float panelW = std::clamp(avail.x * 0.22f, 280.0f, 360.0f);
        ImGui::BeginChild("##tools", ImVec2(toolW, bodyH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNavInputs);
        DrawToolbar();
        ImGui::EndChild();
        ImGui::SameLine();
        ImVec2 canvasSize(std::max(50.0f, avail.x - toolW - panelW - style.ItemSpacing.x * 2.0f), bodyH);
        ImGui::BeginChild("##canvas", canvasSize, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavInputs);
        DrawCanvas(canvasSize);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##panel", ImVec2(panelW, bodyH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNavInputs);
        DrawColors(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginTabBar("##panelTabs"))
        {
            if (ImGui::BeginTabItem("Layers"))
            {
                DrawLayers();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("History"))
            {
                DrawHistory();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Swatches"))
            {
                DrawSwatches();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Properties", nullptr, m_ShowProperties ? ImGuiTabItemFlags_SetSelected : 0))
            {
                DrawProperties();
                ImGui::EndTabItem();
            }
            m_ShowProperties = false;
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        DrawStatusBar();
        DrawDialogs();
        DrawPreviewWindow();
        HandleKeys();
        if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_VectorRecorded = false; // (one undo step per edit)
        ImGui::End();
        if (m_ShowNew) DrawNewImageWindow(&m_ShowNew);
    }

    void ImageMode::DrawTopBar()
    {
        if (ImGui::Button("< Editor")) m_Ctx.RequestCloseImage = true; // (closed by the editor layer, outside this window)
        ImGui::SetItemTooltip("Back to the editor (the image is saved)");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(UI::AccentText(), "%s%s", GetName().c_str(), m_Unsaved ? "*" : "");
        ImGui::SameLine(0.0f, 24.0f);
        if (ImGui::Button("New...")) m_ShowNew = true;
        ImGui::SetItemTooltip("A new image (Assets/Images)");
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Save")) Save();
        ImGui::SetItemTooltip("Ctrl+S: the .dimage and its %s", m_Doc.ExportPath(m_File).filename().string().c_str());
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::BeginDisabled(m_Undo.empty());
        if (ImGui::Button("Undo")) Undo();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Ctrl+Z%s%s", m_Undo.empty() ? "" : ": ", m_Undo.empty() ? "" : m_Undo.back().Label.c_str());
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::BeginDisabled(m_Redo.empty());
        if (ImGui::Button("Redo")) Redo();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Ctrl+Shift+Z%s%s", m_Redo.empty() ? "" : ": ", m_Redo.empty() ? "" : m_Redo.back().Label.c_str());
        ImGui::SameLine(0.0f, 24.0f);
        ImVec2 viewCenter = Add(m_ViewMin, Mul(m_ViewSize, 0.5f));
        if (ImGui::Button("-")) SetZoom(m_Zoom / 1.25f, viewCenter);
        ImGui::SetItemTooltip("Zoom out (Ctrl+-)");
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%5.0f%%", m_Zoom * 100.0f);
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("+")) SetZoom(m_Zoom * 1.25f, viewCenter);
        ImGui::SetItemTooltip("Zoom in (Ctrl+=)");
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Fit")) Fit();
        ImGui::SetItemTooltip("Ctrl+0: the whole image in view");
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("100%")) SetZoom(1.0f, viewCenter);
        ImGui::SetItemTooltip("Ctrl+1: actual pixels");
        ImGui::SameLine(0.0f, 16.0f);
        auto toggle = [](const char* label, bool& on, const char* tip) {
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            bool pressed = ImGui::Button(label);
            if (on) ImGui::PopStyleColor();
            if (pressed) on = !on;
            ImGui::SetItemTooltip("%s", tip);
        };
        toggle("Tile", m_TilePreview, "Show the picture repeated around itself (seams in tiling textures)");
        ImGui::SameLine(0.0f, 2.0f);
        toggle("Grid", m_ShowGrid, "A grid of cells (tiles, sprite frames); Ctrl+'");
        if (m_ShowGrid)
        {
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::SetNextItemWidth(90.0f);
            ImGui::InputInt2("##gridSize", m_GridSize);
            m_GridSize[0] = std::clamp(m_GridSize[0], 1, 4096);
            m_GridSize[1] = std::clamp(m_GridSize[1], 1, 4096);
        }
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::BeginDisabled(!Imaging::Gpu::Available());
        toggle("GPU", m_UseGpu, "Composite and blur on the graphics card (compute shaders); off: on the CPU");
        ImGui::EndDisabled();
        if (m_UseGpu != m_ComposedOnGpu) MarkAll(); // (shown again the other way)
        ImGui::SameLine(0.0f, 16.0f);
        if (ImGui::Button("Export...")) m_Dialog = 9, m_OpenDialog = true;
        ImGui::SetItemTooltip("Another file: a format and a scale (Image > Export As)");
        char size[64];
        std::snprintf(size, sizeof(size), "%u x %u px   %d layer%s", m_Doc.Width, m_Doc.Height, (int)m_Doc.Layers.size(), m_Doc.Layers.size() == 1 ? "" : "s");
        float w = ImGui::CalcTextSize(size).x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowWidth() - w - 16.0f));
        ImGui::TextDisabled("%s", size);
    }

    void ImageMode::DrawBrushOptions(BrushSettings& b)
    {
        const Tool t = m_Tool;
        // Presets (not for the pencil, whose point is its exact pixels).
        if (t != Tool::Pencil)
        {
            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::BeginCombo("##preset", "Presets", ImGuiComboFlags_HeightLarge))
            {
                int remove = -1;
                for (size_t i = 0; i < m_Presets.size(); ++i)
                {
                    const Preset& p = m_Presets[i];
                    ImGui::PushID((int)i);
                    char label[160];
                    std::snprintf(label, sizeof(label), "%s  (%.0f px)", p.Name.c_str(), p.Brush.Size);
                    if (ImGui::Selectable(label))
                    {
                        b.Size = p.Brush.Size;
                        b.Hardness = p.Brush.Hardness;
                        b.Flow = p.Brush.Flow;
                        b.Spacing = p.Brush.Spacing;
                        b.Roundness = p.Brush.Roundness;
                        b.Angle = p.Brush.Angle;
                    }
                    if (!p.BuiltIn && ImGui::BeginPopupContextItem("##presetMenu"))
                    {
                        if (ImGui::MenuItem("Delete Preset")) remove = (int)i;
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (ImGui::Selectable("Save Current Brush...")) m_OpenSavePreset = true;
                ImGui::EndCombo();
                if (remove >= 0)
                {
                    m_Presets.erase(m_Presets.begin() + remove);
                    SavePresets();
                }
            }
            ImGui::SetItemTooltip("Brush presets (right-click your own to delete them)");
            ImGui::SameLine(0.0f, 12.0f);
        }
        Label("Size");
        ImGui::SetNextItemWidth(130.0f);
        ImGui::SliderFloat("##size", &b.Size, 1.0f, 1000.0f, "%.0f px", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("[ and ] make the brush smaller and larger");
        ImGui::SameLine(0.0f, 12.0f);
        if (t != Tool::Pencil)
        {
            Label("Hardness");
            Percent("##hardness", &b.Hardness, 80.0f);
            ImGui::SetItemTooltip("Shift+[ and Shift+] soften and harden it");
            ImGui::SameLine(0.0f, 12.0f);
        }
        const char* strength = t == Tool::Blur || t == Tool::Sharpen || t == Tool::Smudge || t == Tool::Sponge ? "Strength"
                               : t == Tool::Dodge || t == Tool::Burn                                            ? "Exposure"
                                                                                                                 : "Opacity";
        Label(strength);
        Percent("##opacity", &b.Opacity, 80.0f);
        ImGui::SetItemTooltip("1-0 keys: 10%%-100%%");
        ImGui::SameLine(0.0f, 12.0f);
        if (t == Tool::Brush || t == Tool::Eraser || t == Tool::CloneStamp || t == Tool::Healing)
        {
            Label("Flow");
            Percent("##flow", &b.Flow, 80.0f);
            ImGui::SetItemTooltip("How fast paint builds up as the brush goes over a spot");
            ImGui::SameLine(0.0f, 12.0f);
        }
        // The tip: spacing, roundness, angle.
        if (t != Tool::Pencil)
        {
            if (ImGui::Button("Tip...")) ImGui::OpenPopup("##tip");
            ImGui::SetItemTooltip("Spacing, roundness and angle of the brush tip");
            if (ImGui::BeginPopup("##tip"))
            {
                ImGui::SetNextItemWidth(180.0f);
                float s = b.Spacing * 100.0f;
                if (ImGui::SliderFloat("Spacing", &s, 1.0f, 200.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) b.Spacing = s / 100.0f;
                Percent("Roundness", &b.Roundness, 180.0f);
                b.Roundness = std::max(b.Roundness, 0.05f);
                ImGui::SetNextItemWidth(180.0f);
                ImGui::SliderFloat("Angle", &b.Angle, -180.0f, 180.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
                // A preview of the tip.
                ImVec2 at = ImGui::GetCursorScreenPos();
                const float box = 90.0f;
                ImGui::Dummy(ImVec2(box, box));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(at, Add(at, ImVec2(box, box)), IM_COL32(230, 230, 230, 255));
                BrushSettings preview = b;
                preview.Size = box * 0.8f;
                for (int y = 0; y < (int)box; y += 2)
                    for (int x = 0; x < (int)box; x += 2)
                        if (float wgt = DabWeight(preview, glm::vec2(x + 1.0f - box * 0.5f, y + 1.0f - box * 0.5f)); wgt > 0.0f)
                            dl->AddRectFilled(Add(at, ImVec2((float)x, (float)y)), Add(at, ImVec2(x + 2.0f, y + 2.0f)), IM_COL32(0, 0, 0, (int)(wgt * 255)));
                ImGui::EndPopup();
            }
            ImGui::SameLine(0.0f, 12.0f);
        }
        if (t == Tool::Brush || t == Tool::Pencil || t == Tool::Eraser || t == Tool::CloneStamp || t == Tool::Healing)
        {
            Label("Smoothing");
            Percent("##smoothing", &m_Smoothing, 80.0f);
            ImGui::SetItemTooltip("The brush follows the mouse on a string: steadier lines");
            ImGui::SameLine(0.0f, 12.0f);
        }
        if (t == Tool::Brush || t == Tool::Pencil || t == Tool::Eraser)
        {
            bool mx = m_Symmetry & 1, my = m_Symmetry & 2;
            if (ImGui::Checkbox("Mirror X", &mx)) m_Symmetry = (m_Symmetry & ~1) | (mx ? 1 : 0);
            ImGui::SetItemTooltip("Paint mirrored left and right (about the canvas's middle)");
            ImGui::SameLine();
            if (ImGui::Checkbox("Mirror Y", &my)) m_Symmetry = (m_Symmetry & ~2) | (my ? 2 : 0);
            ImGui::SetItemTooltip("Paint mirrored top and bottom");
            ImGui::SameLine(0.0f, 12.0f);
            if (t == Tool::Pencil)
            {
                ImGui::Checkbox("Pixel Perfect", &m_PixelPerfect);
                ImGui::SetItemTooltip("One-pixel lines without doubled corners on their diagonals");
                ImGui::SameLine(0.0f, 12.0f);
            }
        }
        if (t == Tool::CloneStamp || t == Tool::Healing)
        {
            ImGui::Checkbox("Aligned", &m_CloneAligned);
            ImGui::SetItemTooltip("Keep the source moving with the brush from stroke to stroke (else each stroke starts at the source)");
            ImGui::SameLine(0.0f, 12.0f);
            int which = m_CloneSampleAll ? 1 : 0;
            static const char* sources[] = { "Current Layer", "All Layers" };
            Label("Sample");
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::Combo("##cloneSample", &which, sources, 2)) m_CloneSampleAll = which == 1;
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::TextDisabled(m_HasCloneSource ? "Alt+click: a new source" : "Alt+click to set the source");
        }
        if (t == Tool::Dodge || t == Tool::Burn)
        {
            int range = (int)b.Range;
            static const char* ranges[] = { "Shadows", "Midtones", "Highlights" };
            Label("Range");
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::Combo("##range", &range, ranges, 3)) b.Range = (ToneRange)range;
        }
        if (t == Tool::Sponge)
        {
            int mode = b.Saturate ? 1 : 0;
            static const char* modes[] = { "Desaturate", "Saturate" };
            Label("Mode");
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::Combo("##spongeMode", &mode, modes, 2)) b.Saturate = mode == 1;
        }
    }

    void ImageMode::DrawToolOptions()
    {
        ImGui::AlignTextToFramePadding();
        if (m_Transform.Active)
        {
            DrawTransformOptions();
            return;
        }
        ImGui::TextColored(UI::AccentText(), "%s", kToolNames[(int)m_Tool]);
        ImGui::SameLine(0.0f, 16.0f);
        if (IsSelectTool(m_Tool)) DrawSelectOptions();
        else if (m_Tool == Tool::Move)
            ImGui::TextDisabled("Drag to move the layer (a group: its layers; with a selection: the selected pixels). Arrows nudge 1 px, Shift 10. Ctrl+T: Free Transform");
        else if (m_Tool == Tool::Crop) DrawCropOptions();
        else if (m_Tool == Tool::Type) DrawTextOptions();
        else if (IsShapeTool(m_Tool)) DrawShapeOptions();
        else if (BrushSettings* b = ToolBrush(m_Tool)) DrawBrushOptions(*b);
        else if (m_Tool == Tool::Gradient)
        {
            for (int s = 0; s < (int)GradientShape::Count; ++s)
            {
                bool on = (int)m_Gradient.Shape == s;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(GradientShapeName((GradientShape)s))) m_Gradient.Shape = (GradientShape)s;
                if (on) ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, 2.0f);
            }
            ImGui::SameLine(0.0f, 12.0f);
            static const char* colors[] = { "Foreground to Background", "Foreground to Transparent", "Black to White" };
            ImGui::SetNextItemWidth(200.0f);
            ImGui::Combo("##gradColors", &m_GradientColors, colors, 3);
            ImGui::SameLine(0.0f, 12.0f);
            Label("Opacity");
            Percent("##gradOpacity", &m_Gradient.Opacity, 80.0f);
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::Checkbox("Reverse", &m_Gradient.Reverse);
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::Checkbox("Dither", &m_Gradient.Dither);
            ImGui::SetItemTooltip("A little noise against banding");
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::TextDisabled("Drag across the image (Shift: 45-degree steps)");
        }
        else if (m_Tool == Tool::Fill)
        {
            Label("Tolerance");
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("##tolerance", &m_Fill.Tolerance, 0.0f, 255.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SameLine(0.0f, 12.0f);
            Label("Opacity");
            Percent("##fillOpacity", &m_Fill.Opacity, 90.0f);
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::Checkbox("Contiguous", &m_Fill.Contiguous);
            ImGui::SetItemTooltip("Only the pixels joined to the one clicked (else every matching pixel)");
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::Checkbox("All Layers", &m_Fill.SampleAllLayers);
            ImGui::SetItemTooltip("Match colors in the whole picture (else the current layer's)");
        }
        else if (m_Tool == Tool::Eyedropper)
        {
            static const char* sizes[] = { "Point Sample", "3 by 3 Average", "5 by 5 Average", "11 by 11 Average" };
            Label("Sample Size");
            ImGui::SetNextItemWidth(160.0f);
            ImGui::Combo("##sampleSize", &m_SampleSize, sizes, 4);
            ImGui::SameLine(0.0f, 12.0f);
            int which = m_SampleAll ? 1 : 0;
            static const char* sources[] = { "Current Layer", "All Layers" };
            Label("Sample");
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("##sample", &which, sources, 2)) m_SampleAll = which == 1;
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::TextDisabled("Alt+click picks the background color");
        }
        else if (m_Tool == Tool::Hand)
            ImGui::TextDisabled("Drag to move the view (Space or the middle button with any tool; the wheel zooms)");
        else if (m_Tool == Tool::Zoom)
            ImGui::TextDisabled("Click to zoom in, Alt+click to zoom out");
        if (ImGui::GetCursorPosX() > ImGui::GetStyle().WindowPadding.x + 1.0f) ImGui::NewLine();

        if (m_OpenSavePreset)
        {
            ImGui::OpenPopup("Save Brush Preset");
            m_PresetName = "My Brush";
            m_OpenSavePreset = false;
        }
        if (ImGui::BeginPopupModal("Save Brush Preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            bool ok = ImGui::InputText("Name", &m_PresetName, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::TextDisabled("Size, hardness, flow and the tip (kept with the project)");
            if (ImGui::Button("Save", ImVec2(100, 0))) ok = true;
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
            if (ok && !m_PresetName.empty())
            {
                if (BrushSettings* b = ToolBrush(m_Tool))
                {
                    Preset p;
                    p.Name = m_PresetName;
                    p.Brush = *b;
                    m_Presets.push_back(p);
                    SavePresets();
                    Note("Saved the brush preset '" + m_PresetName + "'");
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void ImageMode::DrawToolIcon(Tool tool, const ImVec2& a, const ImVec2& b, ImU32 col)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float s = b.x - a.x;
        auto P = [&](float x, float y) { return ImVec2(a.x + x * s, a.y + y * s); };
        switch (tool)
        {
        case Tool::Move:
            dl->AddLine(P(0.5f, 0.12f), P(0.5f, 0.88f), col, 2.0f);
            dl->AddLine(P(0.12f, 0.5f), P(0.88f, 0.5f), col, 2.0f);
            dl->AddTriangleFilled(P(0.5f, 0.06f), P(0.38f, 0.22f), P(0.62f, 0.22f), col);
            dl->AddTriangleFilled(P(0.5f, 0.94f), P(0.62f, 0.78f), P(0.38f, 0.78f), col);
            dl->AddTriangleFilled(P(0.06f, 0.5f), P(0.22f, 0.62f), P(0.22f, 0.38f), col);
            dl->AddTriangleFilled(P(0.94f, 0.5f), P(0.78f, 0.38f), P(0.78f, 0.62f), col);
            break;
        case Tool::RectMarquee:
            for (int i = 0; i < 4; ++i)
            {
                float t0 = 0.16f + i * 0.18f, t1 = t0 + 0.1f;
                dl->AddLine(P(t0, 0.2f), P(t1, 0.2f), col, 1.8f);
                dl->AddLine(P(t0, 0.8f), P(t1, 0.8f), col, 1.8f);
                dl->AddLine(P(0.16f, t0 + 0.04f), P(0.16f, t1 + 0.04f), col, 1.8f);
                dl->AddLine(P(0.84f, t0 + 0.04f), P(0.84f, t1 + 0.04f), col, 1.8f);
            }
            break;
        case Tool::EllipseMarquee:
            for (int i = 0; i < 12; i += 2)
            {
                dl->PathArcTo(P(0.5f, 0.5f), s * 0.34f, i * IM_PI / 6.0f, (i + 1) * IM_PI / 6.0f);
                dl->PathStroke(col, 0, 1.8f);
            }
            break;
        case Tool::Lasso:
            dl->AddEllipse(P(0.52f, 0.38f), ImVec2(s * 0.32f, s * 0.2f), col, -0.3f, 0, 1.8f);
            dl->AddBezierCubic(P(0.3f, 0.52f), P(0.28f, 0.72f), P(0.42f, 0.74f), P(0.34f, 0.9f), col, 1.8f);
            break;
        case Tool::PolyLasso:
        {
            ImVec2 pts[5] = { P(0.2f, 0.3f), P(0.6f, 0.14f), P(0.86f, 0.42f), P(0.62f, 0.66f), P(0.28f, 0.62f) };
            dl->AddPolyline(pts, 5, col, ImDrawFlags_Closed, 1.8f);
            dl->AddLine(P(0.28f, 0.62f), P(0.22f, 0.88f), col, 1.8f);
            break;
        }
        case Tool::MagicWand:
            dl->AddLine(P(0.16f, 0.86f), P(0.6f, 0.42f), col, 3.0f);
            for (int i = 0; i < 4; ++i)
            {
                float ang = i * IM_PI / 4.0f;
                ImVec2 c = P(0.72f, 0.28f);
                dl->AddLine(ImVec2(c.x - std::cos(ang) * s * 0.16f, c.y - std::sin(ang) * s * 0.16f), ImVec2(c.x + std::cos(ang) * s * 0.16f, c.y + std::sin(ang) * s * 0.16f), col, 1.5f);
            }
            break;
        case Tool::Crop:
            dl->AddLine(P(0.28f, 0.1f), P(0.28f, 0.72f), col, 2.2f);
            dl->AddLine(P(0.28f, 0.72f), P(0.9f, 0.72f), col, 2.2f);
            dl->AddLine(P(0.1f, 0.28f), P(0.72f, 0.28f), col, 2.2f);
            dl->AddLine(P(0.72f, 0.28f), P(0.72f, 0.9f), col, 2.2f);
            break;
        case Tool::Brush:
            dl->AddLine(P(0.78f, 0.18f), P(0.42f, 0.55f), col, 2.5f);
            dl->AddCircleFilled(P(0.32f, 0.68f), s * 0.13f, col);
            dl->AddTriangleFilled(P(0.22f, 0.72f), P(0.36f, 0.82f), P(0.14f, 0.88f), col);
            break;
        case Tool::Pencil:
            dl->AddQuadFilled(P(0.70f, 0.14f), P(0.86f, 0.30f), P(0.40f, 0.76f), P(0.24f, 0.60f), col);
            dl->AddTriangleFilled(P(0.24f, 0.62f), P(0.38f, 0.76f), P(0.14f, 0.86f), col);
            break;
        case Tool::Eraser:
            dl->AddQuadFilled(P(0.55f, 0.18f), P(0.85f, 0.48f), P(0.60f, 0.73f), P(0.30f, 0.43f), col);
            dl->AddQuad(P(0.30f, 0.43f), P(0.60f, 0.73f), P(0.45f, 0.86f), P(0.15f, 0.56f), col, 1.5f);
            break;
        case Tool::CloneStamp:
            dl->AddRectFilled(P(0.18f, 0.66f), P(0.82f, 0.80f), col, 2.0f);
            dl->AddRectFilled(P(0.40f, 0.40f), P(0.60f, 0.66f), col);
            dl->AddCircleFilled(P(0.50f, 0.28f), s * 0.14f, col);
            break;
        case Tool::Healing:
        {
            ImVec2 c = P(0.5f, 0.5f);
            dl->AddQuadFilled(P(0.20f, 0.62f), P(0.62f, 0.20f), P(0.80f, 0.38f), P(0.38f, 0.80f), col);
            dl->AddRectFilled(Sub(c, ImVec2(s * 0.06f, s * 0.06f)), Add(c, ImVec2(s * 0.06f, s * 0.06f)), ImGui::GetColorU32(ImGuiCol_WindowBg));
            break;
        }
        case Tool::Gradient:
            dl->AddRectFilledMultiColor(P(0.14f, 0.26f), P(0.86f, 0.74f), IM_COL32(20, 20, 20, 255), col, col, IM_COL32(20, 20, 20, 255));
            dl->AddRect(P(0.14f, 0.26f), P(0.86f, 0.74f), col);
            break;
        case Tool::Fill:
            dl->AddQuad(P(0.45f, 0.18f), P(0.78f, 0.50f), P(0.48f, 0.80f), P(0.15f, 0.48f), col, 1.8f);
            dl->AddLine(P(0.15f, 0.48f), P(0.78f, 0.50f), col, 1.5f);
            dl->AddCircleFilled(P(0.84f, 0.74f), s * 0.08f, col);
            dl->AddTriangleFilled(P(0.77f, 0.72f), P(0.91f, 0.72f), P(0.84f, 0.58f), col);
            break;
        case Tool::Blur:
            dl->AddCircleFilled(P(0.5f, 0.62f), s * 0.22f, col);
            dl->AddTriangleFilled(P(0.30f, 0.54f), P(0.70f, 0.54f), P(0.50f, 0.14f), col);
            break;
        case Tool::Sharpen: dl->AddTriangleFilled(P(0.50f, 0.12f), P(0.80f, 0.86f), P(0.20f, 0.86f), col); break;
        case Tool::Smudge:
            dl->AddLine(P(0.80f, 0.20f), P(0.42f, 0.58f), col, 4.0f);
            dl->AddCircleFilled(P(0.36f, 0.66f), s * 0.12f, col);
            dl->AddLine(P(0.16f, 0.88f), P(0.30f, 0.74f), col, 1.5f);
            break;
        case Tool::Dodge:
            dl->AddCircle(P(0.58f, 0.36f), s * 0.20f, col, 0, 2.2f);
            dl->AddLine(P(0.44f, 0.50f), P(0.16f, 0.84f), col, 2.5f);
            break;
        case Tool::Burn:
            dl->AddCircleFilled(P(0.50f, 0.30f), s * 0.16f, col);
            dl->AddRectFilled(P(0.30f, 0.46f), P(0.70f, 0.84f), col, s * 0.10f);
            break;
        case Tool::Sponge:
            dl->AddRectFilled(P(0.16f, 0.28f), P(0.84f, 0.72f), col, s * 0.12f);
            for (int i = 0; i < 3; ++i) dl->AddCircleFilled(P(0.32f + i * 0.18f, 0.50f), s * 0.05f, ImGui::GetColorU32(ImGuiCol_WindowBg));
            break;
        case Tool::Type:
            dl->AddLine(P(0.2f, 0.18f), P(0.8f, 0.18f), col, 3.0f);
            dl->AddLine(P(0.5f, 0.18f), P(0.5f, 0.86f), col, 3.0f);
            dl->AddLine(P(0.36f, 0.86f), P(0.64f, 0.86f), col, 1.5f);
            break;
        case Tool::RectShape: dl->AddRectFilled(P(0.16f, 0.26f), P(0.84f, 0.74f), col); break;
        case Tool::RoundShape: dl->AddRectFilled(P(0.16f, 0.26f), P(0.84f, 0.74f), col, s * 0.14f); break;
        case Tool::EllipseShape: dl->AddEllipseFilled(P(0.5f, 0.5f), ImVec2(s * 0.36f, s * 0.26f), col); break;
        case Tool::PolygonShape: dl->AddNgonFilled(P(0.5f, 0.52f), s * 0.36f, col, 6); break;
        case Tool::StarShape:
        {
            const ImVec2 c = P(0.5f, 0.54f);
            ImVec2 pts[10];
            for (int i = 0; i < 10; ++i)
            {
                float ang = -IM_PI * 0.5f + i * IM_PI / 5.0f, r = (i % 2 ? 0.16f : 0.38f) * s;
                pts[i] = ImVec2(c.x + std::cos(ang) * r, c.y + std::sin(ang) * r);
            }
            for (int i = 0; i < 10; ++i) dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], col);
            break;
        }
        case Tool::LineShape: dl->AddLine(P(0.16f, 0.84f), P(0.84f, 0.16f), col, 2.5f); break;
        case Tool::Eyedropper:
            dl->AddLine(P(0.22f, 0.80f), P(0.62f, 0.40f), col, 2.5f);
            dl->AddCircleFilled(P(0.70f, 0.30f), s * 0.13f, col);
            dl->AddLine(P(0.52f, 0.36f), P(0.66f, 0.50f), col, 2.0f);
            break;
        case Tool::Hand:
            dl->AddRectFilled(P(0.28f, 0.45f), P(0.74f, 0.84f), col, s * 0.12f);
            for (int i = 0; i < 4; ++i) dl->AddRectFilled(P(0.29f + i * 0.115f, 0.18f + (i == 0 || i == 3 ? 0.06f : 0.0f)), P(0.37f + i * 0.115f, 0.55f), col, s * 0.04f);
            dl->AddLine(P(0.28f, 0.62f), P(0.14f, 0.46f), col, 3.0f);
            break;
        case Tool::Zoom:
            dl->AddCircle(P(0.42f, 0.42f), s * 0.22f, col, 0, 2.2f);
            dl->AddLine(P(0.58f, 0.58f), P(0.84f, 0.84f), col, 3.0f);
            break;
        default: break;
        }
    }

    void ImageMode::DrawToolbar()
    {
        const Tool current = EffectiveTool();
        const float size = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (size_t s = 0; s < m_Slots.size(); ++s)
        {
            const Slot& slot = m_Slots[s];
            const bool inSlot = std::find(slot.Tools.begin(), slot.Tools.end(), current) != slot.Tools.end();
            Tool shown = inSlot ? current : m_SlotTool[s];
            ImGui::PushID((int)s);
            if (ImGui::InvisibleButton("##tool", ImVec2(size, size), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight))
            {
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && slot.Tools.size() > 1) ImGui::OpenPopup("##slotTools");
                else SelectTool(m_SlotTool[s]);
            }
            bool hovered = ImGui::IsItemHovered();
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            if (inSlot || hovered) dl->AddRectFilled(a, b, ImGui::GetColorU32(inSlot ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), 4.0f);
            DrawToolIcon(shown, Add(a, ImVec2(5, 5)), Sub(b, ImVec2(5, 5)), ImGui::GetColorU32(ImGuiCol_Text));
            if (slot.Tools.size() > 1) dl->AddTriangleFilled(Sub(b, ImVec2(2, 7)), Sub(b, ImVec2(2, 2)), Sub(b, ImVec2(7, 2)), ImGui::GetColorU32(ImGuiCol_TextDisabled));
            if (slot.Tools.size() > 1) ImGui::SetItemTooltip("%s (%s; Shift+%s or right-click: the others here)", kToolNames[(int)shown], slot.KeyName, slot.KeyName);
            else ImGui::SetItemTooltip("%s (%s)", kToolNames[(int)shown], slot.KeyName);
            if (ImGui::BeginPopup("##slotTools"))
            {
                for (Tool t : slot.Tools)
                {
                    ImVec2 at = ImGui::GetCursorScreenPos();
                    float h = ImGui::GetFrameHeight();
                    if (ImGui::Selectable((std::string("      ") + kToolNames[(int)t]).c_str(), t == m_Tool)) SelectTool(t);
                    DrawToolIcon(t, at, Add(at, ImVec2(h, h)), ImGui::GetColorU32(ImGuiCol_Text));
                    ImGui::SameLine(180.0f);
                    ImGui::TextDisabled("%s", t == Tool::Pencil ? "N" : slot.KeyName);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        // The two colors, Photoshop's way: the foreground over the background; X swaps, D resets.
        ImGui::Separator();
        ImVec2 at = ImGui::GetCursorScreenPos();
        const float sw = size * 0.62f;
        ImVec2 bgMin = Add(at, ImVec2(size - sw, sw * 0.6f)), fgMin = at;
        ImGui::SetCursorScreenPos(bgMin);
        if (ImGui::InvisibleButton("##bgSwatch", ImVec2(sw, sw))) m_EditBackground = true;
        ImGui::SetItemTooltip("Background color");
        dl->AddRectFilled(bgMin, Add(bgMin, ImVec2(sw, sw)), ToU32(m_Background));
        dl->AddRect(bgMin, Add(bgMin, ImVec2(sw, sw)), ImGui::GetColorU32(m_EditBackground ? UI::AccentText() : ImGui::GetStyleColorVec4(ImGuiCol_Border)), 0.0f, 0, 2.0f);
        ImGui::SetCursorScreenPos(fgMin);
        if (ImGui::InvisibleButton("##fgSwatch", ImVec2(sw, sw))) m_EditBackground = false;
        ImGui::SetItemTooltip("Foreground color");
        dl->AddRectFilled(fgMin, Add(fgMin, ImVec2(sw, sw)), ToU32(m_Foreground));
        dl->AddRect(fgMin, Add(fgMin, ImVec2(sw, sw)), ImGui::GetColorU32(!m_EditBackground ? UI::AccentText() : ImGui::GetStyleColorVec4(ImGuiCol_Border)), 0.0f, 0, 2.0f);
        ImGui::SetCursorScreenPos(Add(at, ImVec2(0, sw * 1.6f + 4.0f)));
        if (ImGui::Button("X", ImVec2(size, 0))) std::swap(m_Foreground, m_Background);
        ImGui::SetItemTooltip("Swap the colors (X)");
        if (ImGui::Button("D", ImVec2(size, 0))) m_Foreground = { 0, 0, 0, 1 }, m_Background = { 1, 1, 1, 1 };
        ImGui::SetItemTooltip("Black and white (D)");
    }

    void ImageMode::DrawCanvas(const ImVec2& size)
    {
        const ImGuiIO& io = ImGui::GetIO();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        m_ViewMin = pos;
        m_ViewSize = size;
        if (m_FitPending)
        {
            Fit();
            m_FitPending = false;
        }
        const ImVec2 canvas((float)m_Doc.Width, (float)m_Doc.Height);
        const ImVec2 viewCenter = Add(pos, Mul(size, 0.5f));
        m_CanvasMin = Sub(Add(viewCenter, m_Pan), Mul(canvas, m_Zoom * 0.5f));
        m_CanvasMin = ImVec2(std::floor(m_CanvasMin.x), std::floor(m_CanvasMin.y));
        if (m_Transform.Active && m_Transform.Dirty) ApplyTransform();
        UpdatePreview();
        UpdateTexture();

        ImGui::InvisibleButton("##canvasArea", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c0 = m_CanvasMin, c1 = Add(m_CanvasMin, Mul(canvas, m_Zoom));
        auto toScreen = [&](const glm::vec2& p) { return ImVec2(c0.x + p.x * m_Zoom, c0.y + p.y * m_Zoom); };
        dl->PushClipRect(pos, Add(pos, size), true);
        dl->AddRectFilled(pos, Add(pos, size), IM_COL32(38, 38, 40, 255));
        // Transparency: a checkerboard in screen cells behind the picture (only where it shows).
        {
            ImVec2 v0(std::max(c0.x, pos.x), std::max(c0.y, pos.y)), v1(std::min(c1.x, pos.x + size.x), std::min(c1.y, pos.y + size.y));
            if (v0.x < v1.x && v0.y < v1.y)
            {
                dl->AddRectFilled(v0, v1, IM_COL32(204, 204, 204, 255));
                const float cell = 8.0f;
                int i0 = (int)std::floor((v0.x - c0.x) / cell), i1 = (int)std::ceil((v1.x - c0.x) / cell);
                int j0 = (int)std::floor((v0.y - c0.y) / cell), j1 = (int)std::ceil((v1.y - c0.y) / cell);
                for (int j = j0; j < j1; ++j)
                    for (int i = i0 + ((i0 + j) & 1); i < i1; i += 2)
                    {
                        ImVec2 a(std::max(v0.x, c0.x + i * cell), std::max(v0.y, c0.y + j * cell));
                        ImVec2 b(std::min(v1.x, c0.x + (i + 1) * cell), std::min(v1.y, c0.y + (j + 1) * cell));
                        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 255));
                    }
            }
        }
        if (m_Texture.IsValid())
            if (Ref<Texture2D> tex = AssetManager::GetTexture(m_Texture)) dl->AddImage(ImTextureRef((ImTextureID)tex->GetImGuiId()), c0, c1);
        DrawGameOverlay(dl);
        dl->AddRect(Sub(c0, ImVec2(1, 1)), Add(c1, ImVec2(1, 1)), IM_COL32(0, 0, 0, 160));
        // A pixel grid when far enough in.
        if (m_Zoom >= 12.0f)
        {
            ImU32 grid = IM_COL32(128, 128, 128, 70);
            int x0 = std::max(0, (int)((pos.x - c0.x) / m_Zoom)), x1 = std::min((int)m_Doc.Width, (int)((pos.x + size.x - c0.x) / m_Zoom) + 1);
            int y0 = std::max(0, (int)((pos.y - c0.y) / m_Zoom)), y1 = std::min((int)m_Doc.Height, (int)((pos.y + size.y - c0.y) / m_Zoom) + 1);
            for (int x = x0; x <= x1; ++x) dl->AddLine(ImVec2(c0.x + x * m_Zoom, c0.y + y0 * m_Zoom), ImVec2(c0.x + x * m_Zoom, c0.y + y1 * m_Zoom), grid);
            for (int y = y0; y <= y1; ++y) dl->AddLine(ImVec2(c0.x + x0 * m_Zoom, c0.y + y * m_Zoom), ImVec2(c0.x + x1 * m_Zoom, c0.y + y * m_Zoom), grid);
        }

        // The mouse in image pixels.
        const glm::vec2 p((io.MousePos.x - c0.x) / m_Zoom, (io.MousePos.y - c0.y) / m_Zoom);
        m_Hover = p;
        m_HoverInside = hovered && p.x >= 0.0f && p.y >= 0.0f && p.x < m_Doc.Width && p.y < m_Doc.Height;
        const Tool tool = EffectiveTool();

        // Moving the view: the hand (or Space), the middle button; the wheel zooms where the mouse is.
        if (hovered && io.MouseWheel != 0.0f) SetZoom(m_Zoom * std::pow(1.2f, io.MouseWheel), io.MousePos);
        if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) || (tool == Tool::Hand && ImGui::IsMouseDown(ImGuiMouseButton_Left))))
        {
            m_Pan = Add(m_Pan, io.MouseDelta);
            m_Panning = true;
        }
        else
            m_Panning = false;

        // Selections, moving, transforming and cropping take the mouse first.
        // (while an adjustment, filter or style window is open, the canvas only shows)
        const bool routed = m_Preview.Kind != 0 || m_Transform.Active || m_Move.Active || IsSelectTool(tool) || tool == Tool::Move || tool == Tool::Crop ||
                            tool == Tool::Type || IsShapeTool(tool);
        if (m_Preview.Kind != 0) {}
        else if (m_Transform.Active) TransformInput(p, hovered);
        else if (tool == Tool::Crop) CropInput(p, hovered);
        else if (tool == Tool::Type || IsShapeTool(tool)) VectorInput(p, hovered, tool);
        else if (routed) SelectionInput(p, hovered, tool);
        if (!routed && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if ((tool == Tool::CloneStamp || tool == Tool::Healing) && io.KeyAlt)
            {
                m_CloneSource = p;
                m_HasCloneSource = true;
                m_CloneOffsetSet = false;
                Note("Clone source set");
            }
            else
                switch (tool)
                {
                case Tool::Fill:
                    if (m_HoverInside && CanEditTarget(Tool::Fill))
                    {
                        Record("Paint Bucket");
                        FillSettings fill = m_Fill;
                        fill.Color = m_Foreground;
                        fill.Selection = m_Doc.Selection.get();
                        Rect r = Imaging::Fill(m_Doc, m_ActiveLayer, { (int)p.x, (int)p.y }, fill);
                        if (r.Empty()) m_Undo.pop_back();
                        else
                        {
                            Mark(r);
                            Changed();
                        }
                    }
                    break;
                case Tool::Gradient:
                    if (CanEditTarget(Tool::Gradient))
                    {
                        m_GradientDrag = true;
                        m_GradientStart = p;
                    }
                    break;
                case Tool::Eyedropper: PickColor(p, io.KeyAlt && m_Tool == Tool::Eyedropper); break;
                case Tool::Zoom: SetZoom(io.KeyAlt ? m_Zoom / 1.5f : m_Zoom * 1.5f, io.MousePos); break;
                case Tool::Hand: break;
                default:
                    if (IsBrushTool(tool)) BeginPaint(p, io.KeyShift);
                    break;
                }
        }
        if (m_Stroke)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) ContinuePaint(p);
            else EndPaint();
        }
        // The gradient's line while dragging; applied when the button comes up.
        if (m_GradientDrag)
        {
            glm::vec2 end = p;
            if (io.KeyShift)
            {
                glm::vec2 d = end - m_GradientStart;
                float len = glm::length(d), ang = std::atan2(d.y, d.x);
                ang = std::round(ang / (glm::pi<float>() * 0.25f)) * glm::pi<float>() * 0.25f;
                end = m_GradientStart + glm::vec2(std::cos(ang), std::sin(ang)) * len;
            }
            ImVec2 a = toScreen(m_GradientStart), b = toScreen(end);
            dl->AddLine(a, b, IM_COL32(0, 0, 0, 200), 3.0f);
            dl->AddLine(a, b, IM_COL32(255, 255, 255, 230), 1.5f);
            dl->AddCircleFilled(a, 4.0f, IM_COL32(255, 255, 255, 230));
            dl->AddCircle(b, 4.0f, IM_COL32(255, 255, 255, 230));
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                m_GradientDrag = false;
                ApplyGradient(m_GradientStart, end);
            }
        }
        if (tool == Tool::Eyedropper && active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            PickColor(p, io.KeyAlt && m_Tool == Tool::Eyedropper);

        if (hovered)
        {
            if (const BrushSettings* b = ToolBrush(tool))
            {
                // The brush's outline where it would paint (the tip's shape).
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);
                if (tool == Tool::Pencil)
                {
                    float r = std::max(b->Size * 0.5f, 0.5f);
                    ImVec2 center(c0.x + (std::floor(p.x) + 0.5f) * m_Zoom, c0.y + (std::floor(p.y) + 0.5f) * m_Zoom);
                    ImVec2 half(r * m_Zoom, r * m_Zoom);
                    dl->AddRect(Sub(center, half), Add(center, half), IM_COL32(0, 0, 0, 200), 0.0f, 0, 2.0f);
                    dl->AddRect(Sub(center, half), Add(center, half), IM_COL32(255, 255, 255, 220));
                }
                else
                {
                    float r = std::max(b->Size * 0.5f * m_Zoom, 2.0f);
                    float round = std::clamp(b->Roundness, 0.05f, 1.0f), ang = glm::radians(b->Angle);
                    ImVec2 pts[48];
                    for (int i = 0; i < 48; ++i)
                    {
                        float t = i / 48.0f * glm::two_pi<float>();
                        float x = std::cos(t) * r, y = std::sin(t) * r * round;
                        pts[i] = ImVec2(io.MousePos.x + x * std::cos(ang) + y * std::sin(ang), io.MousePos.y - x * std::sin(ang) + y * std::cos(ang));
                    }
                    dl->AddPolyline(pts, 48, IM_COL32(0, 0, 0, 200), ImDrawFlags_Closed, 2.0f);
                    dl->AddPolyline(pts, 48, IM_COL32(255, 255, 255, 220), ImDrawFlags_Closed, 1.0f);
                }
                dl->AddLine(Sub(io.MousePos, ImVec2(4, 0)), Add(io.MousePos, ImVec2(5, 0)), IM_COL32(255, 255, 255, 200));
                dl->AddLine(Sub(io.MousePos, ImVec2(0, 4)), Add(io.MousePos, ImVec2(0, 5)), IM_COL32(255, 255, 255, 200));
                // Clone and heal: where the pixels come from.
                if ((tool == Tool::CloneStamp || tool == Tool::Healing) && m_HasCloneSource)
                {
                    glm::vec2 src = m_Stroke ? m_LastPaint + (m_CloneAligned && m_CloneOffsetSet ? m_CloneOffset : m_CloneSource - m_LastPaint)
                                             : (m_CloneAligned && m_CloneOffsetSet ? p + m_CloneOffset : m_CloneSource);
                    ImVec2 s = toScreen(src);
                    dl->AddCircle(s, 6.0f, IM_COL32(0, 0, 0, 200), 0, 2.0f);
                    dl->AddLine(Sub(s, ImVec2(9, 0)), Add(s, ImVec2(9, 0)), IM_COL32(255, 255, 255, 220));
                    dl->AddLine(Sub(s, ImVec2(0, 9)), Add(s, ImVec2(0, 9)), IM_COL32(255, 255, 255, 220));
                }
            }
            else if (tool == Tool::Hand)
                ImGui::SetMouseCursor(m_Panning ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
        }
        DrawOverlay(dl, p);
        DrawVectorOverlay(dl, p);
        dl->PopClipRect();
    }

    void ImageMode::DrawColors(float width)
    {
        const bool mask = m_EditMask;
        ImGui::SeparatorText(m_EditBackground ? "Background Color" : "Foreground Color");
        if (mask)
        {
            ImGui::SameLine();
            ImGui::TextColored(UI::AccentText(), "(mask: painting in gray)");
        }
        glm::vec4& c = m_EditBackground ? m_Background : m_Foreground;
        ImGui::SetNextItemWidth(width);
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_DisplayRGB |
                                    ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_NoLabel;
        ImGui::ColorPicker4("##picker", &c.r, flags);
    }

    void ImageMode::DrawThumbnail(const ImVec2& at, float ts, const Layer& layer, bool mask)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (!mask && layer.IsAdjustment())
        {
            // A circle half black, half white.
            ImVec2 c = Add(at, ImVec2(ts * 0.5f, ts * 0.5f));
            dl->AddCircleFilled(c, ts * 0.36f, IM_COL32(240, 240, 240, 255));
            dl->PathArcTo(c, ts * 0.36f, IM_PI * 0.5f, IM_PI * 1.5f);
            dl->PathFillConvex(IM_COL32(20, 20, 20, 255));
            dl->AddCircle(c, ts * 0.36f, ImGui::GetColorU32(ImGuiCol_Text));
            return;
        }
        if (!mask && layer.IsGroup())
        {
            // A folder.
            ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            dl->AddRectFilled(Add(at, ImVec2(ts * 0.10f, ts * 0.22f)), Add(at, ImVec2(ts * 0.45f, ts * 0.34f)), col, 2.0f);
            dl->AddRectFilled(Add(at, ImVec2(ts * 0.10f, ts * 0.30f)), Add(at, ImVec2(ts * 0.90f, ts * 0.80f)), col, 2.0f);
            return;
        }
        dl->AddRectFilled(at, Add(at, ImVec2(ts, ts)), IM_COL32(204, 204, 204, 255));
        dl->AddRectFilled(at, Add(at, ImVec2(ts * 0.5f, ts * 0.5f)), IM_COL32(255, 255, 255, 255));
        dl->AddRectFilled(Add(at, ImVec2(ts * 0.5f, ts * 0.5f)), Add(at, ImVec2(ts, ts)), IM_COL32(255, 255, 255, 255));
        // (sampled on a coarse grid: cheap enough each frame)
        const int n = 8;
        const float cell = ts / n;
        const uint32_t w = m_Doc.Width, h = m_Doc.Height;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                uint32_t sx = std::min(w - 1, (uint32_t)((x + 0.5f) * w / n)), sy = std::min(h - 1, (uint32_t)((y + 0.5f) * h / n));
                uint32_t c;
                if (mask)
                {
                    uint32_t v = layer.LayerMask->At(sx, sy);
                    c = v | (v << 8) | (v << 16) | 0xFF000000u;
                }
                else
                {
                    if (!layer.Pixels) continue;
                    c = layer.Get().At(sx, sy);
                    if ((c >> 24) == 0) continue;
                }
                ImVec2 a = Add(at, ImVec2(x * cell, y * cell));
                dl->AddRectFilled(a, Add(a, ImVec2(cell + 0.5f, cell + 0.5f)), c);
            }
        if (!mask && layer.Kind == LayerKind::Text)
        {
            dl->AddRectFilled(Add(at, ImVec2(ts * 0.55f, ts * 0.55f)), Add(at, ImVec2(ts, ts)), IM_COL32(40, 40, 40, 230));
            dl->AddText(Add(at, ImVec2(ts * 0.62f, ts * 0.5f)), IM_COL32(255, 255, 255, 255), "T");
        }
        if (mask && !layer.MaskEnabled)
        {
            dl->AddLine(at, Add(at, ImVec2(ts, ts)), IM_COL32(230, 40, 40, 255), 2.0f);
            dl->AddLine(Add(at, ImVec2(ts, 0)), Add(at, ImVec2(0, ts)), IM_COL32(230, 40, 40, 255), 2.0f);
        }
    }

    void ImageMode::DrawLayerRow(int i, int depth, int& moveFrom, int& moveTo, bool& into)
    {
        Layer& layer = m_Doc.Layers[(size_t)i];
        const float rowH = 34.0f, ts = rowH - 6.0f;
        ImGui::PushID((int)layer.Id);
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowW = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool selected = i == m_ActiveLayer;
        // The whole row selects the layer (the controls on it overlap it); it is also what drags.
        if (m_RenamingLayer != i)
        {
            if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowH)))
            {
                SetActive(i, selected && m_EditMask);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    m_RenamingLayer = i;
                    m_RenameBuffer = layer.Name;
                }
            }
            if (ImGui::BeginPopupContextItem("##layerMenu"))
            {
                SetActive(i, m_EditMask && selected);
                DrawLayerMenu(i);
                ImGui::EndPopup();
            }
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("IMAGE_LAYER", &layer.Id, sizeof(uint32_t));
                ImGui::TextUnformatted(layer.Name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("IMAGE_LAYER"))
                {
                    moveFrom = m_Doc.IndexOf(*(const uint32_t*)payload->Data);
                    moveTo = i;
                    into = layer.IsGroup(); // (onto a group: into it; onto a layer: above it)
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SetCursorScreenPos(rowMin);
        }
        // Visibility.
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##eye", ImVec2(24, rowH)))
        {
            Record(layer.Visible ? "Hide Layer" : "Show Layer");
            m_Doc.Layers[(size_t)i].Visible = !m_Doc.Layers[(size_t)i].Visible;
            Changed();
            MarkAll();
        }
        ImGui::SetItemTooltip(layer.Visible ? "Hide the layer" : "Show the layer");
        {
            ImVec2 c = Add(rowMin, ImVec2(12, rowH * 0.5f));
            ImU32 col = ImGui::GetColorU32(layer.Visible ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            if (layer.Visible)
            {
                dl->PathArcTo(Add(c, ImVec2(0, 5)), 8.0f, IM_PI * 1.2f, IM_PI * 1.8f);
                dl->PathStroke(col, 0, 1.5f);
                dl->PathArcTo(Sub(c, ImVec2(0, 5)), 8.0f, IM_PI * 0.2f, IM_PI * 0.8f);
                dl->PathStroke(col, 0, 1.5f);
                dl->AddCircleFilled(c, 2.5f, col);
            }
            else
                dl->AddCircle(c, 2.0f, col);
        }
        ImGui::SameLine(0.0f, 2.0f + depth * 14.0f);
        // A group's fold arrow.
        if (layer.IsGroup())
        {
            ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##fold", ImVec2(14, rowH))) m_Doc.Layers[(size_t)i].Expanded = !layer.Expanded; // (a view setting: no undo)
            ImVec2 c = Add(at, ImVec2(7, rowH * 0.5f));
            ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            if (layer.Expanded) dl->AddTriangleFilled(Add(c, ImVec2(-4, -2)), Add(c, ImVec2(4, -2)), Add(c, ImVec2(0, 3)), col);
            else dl->AddTriangleFilled(Add(c, ImVec2(-2, -4)), Add(c, ImVec2(-2, 4)), Add(c, ImVec2(3, 0)), col);
            ImGui::SameLine(0.0f, 2.0f);
        }
        // The thumbnail (selects the pixels) and the mask's (selects the mask; Shift+click turns it off and on).
        {
            ImVec2 at = Add(ImGui::GetCursorScreenPos(), ImVec2(0, 3));
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##thumb", ImVec2(ts, rowH)))
            {
                if (ImGui::GetIO().KeyCtrl && layer.HasPixels()) SetSelection(SelectAlpha(layer.Get()), OpFromKeys(), "Load Selection");
                else if (layer.IsAdjustment()) SetActive(i, false), m_ShowProperties = true;
                else SetActive(i, false);
            }
            ImGui::SetItemTooltip("Ctrl+click: select the layer's pixels");
            DrawThumbnail(at, ts, layer, false);
            if (selected && !m_EditMask && layer.HasPixels()) dl->AddRect(Sub(at, ImVec2(2, 2)), Add(at, ImVec2(ts + 2, ts + 2)), ImGui::GetColorU32(UI::AccentText()), 0.0f, 0, 2.0f);
            else dl->AddRect(at, Add(at, ImVec2(ts, ts)), IM_COL32(0, 0, 0, 120));
        }
        if (layer.HasMask())
        {
            ImGui::SameLine(0.0f, 4.0f);
            ImVec2 at = Add(ImGui::GetCursorScreenPos(), ImVec2(0, 3));
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##mask", ImVec2(ts, rowH)))
            {
                if (ImGui::GetIO().KeyCtrl) SetSelection(*layer.LayerMask, SelectOp::Replace, "Load Selection");
                else if (ImGui::GetIO().KeyShift)
                {
                    Record(layer.MaskEnabled ? "Disable Layer Mask" : "Enable Layer Mask");
                    m_Doc.Layers[(size_t)i].MaskEnabled = !layer.MaskEnabled;
                    Changed();
                    MarkAll();
                }
                else
                    SetActive(i, true);
            }
            ImGui::SetItemTooltip("The layer mask (Shift+click turns it off and on)");
            DrawThumbnail(at, ts, layer, true);
            if (selected && m_EditMask) dl->AddRect(Sub(at, ImVec2(2, 2)), Add(at, ImVec2(ts + 2, ts + 2)), ImGui::GetColorU32(UI::AccentText()), 0.0f, 0, 2.0f);
            else dl->AddRect(at, Add(at, ImVec2(ts, ts)), IM_COL32(0, 0, 0, 120));
        }
        ImGui::SameLine(0.0f, 8.0f);
        // The name (double-click renames), and its settings when not the usual ones.
        if (m_RenamingLayer == i)
        {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - ImGui::GetFrameHeight()) * 0.5f);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
            bool done = ImGui::InputText("##rename", &m_RenameBuffer, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (done || ImGui::IsItemDeactivated())
            {
                if (!m_RenameBuffer.empty() && m_RenameBuffer != layer.Name)
                {
                    Record("Rename Layer");
                    m_Doc.Layers[(size_t)i].Name = m_RenameBuffer;
                    Changed();
                }
                m_RenamingLayer = -1;
            }
        }
        else
        {
            ImVec2 textAt(ImGui::GetCursorScreenPos().x, rowMin.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f);
            dl->AddText(textAt, ImGui::GetColorU32(m_Doc.ShownInTree(i) ? ImGuiCol_Text : ImGuiCol_TextDisabled), layer.Name.c_str());
            std::string info;
            auto add = [&](const std::string& s) { info += (info.empty() ? "" : "  ") + s; };
            if (layer.Blend != LayerBlend::Normal && layer.Blend != LayerBlend::PassThrough) add(LayerBlendName(layer.Blend));
            if (layer.Opacity < 1.0f) add(std::to_string((int)std::lround(layer.Opacity * 100.0f)) + "%");
            if (layer.Effects.Any()) add("fx");
            if (layer.LockAll) add("locked");
            else if (layer.LockPixels) add("pixels locked");
            else if (layer.LockTransparency) add("alpha locked");
            if (!info.empty())
            {
                float w = ImGui::CalcTextSize(info.c_str()).x;
                dl->AddText(ImVec2(rowMin.x + rowW - w - 6.0f, textAt.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), info.c_str());
            }
            ImGui::Dummy(ImVec2(1, rowH));
        }
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMin.y + rowH + ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
        // A group's contents under it, indented (top first).
        if (layer.IsGroup() && m_Doc.Layers[(size_t)i].Expanded)
        {
            std::vector<int> kids = m_Doc.Children(m_Doc.Layers[(size_t)i].Id);
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) DrawLayerRow(*it, depth + 1, moveFrom, moveTo, into);
        }
    }

    void ImageMode::DrawLayerMenu(int i)
    {
        const Layer& l = m_Doc.Layers[(size_t)i];
        if (ImGui::MenuItem("Rename")) m_RenamingLayer = i, m_RenameBuffer = l.Name;
        if (ImGui::MenuItem(l.IsGroup() ? "Duplicate Group" : "Duplicate Layer", "Ctrl+J")) DuplicateLayer();
        if (ImGui::MenuItem(l.IsGroup() ? "Delete Group" : "Delete Layer")) DeleteLayer();
        ImGui::Separator();
        if (ImGui::MenuItem("Group", "Ctrl+G")) GroupLayer();
        if (l.IsGroup() && ImGui::MenuItem("Ungroup", "Ctrl+Shift+G")) Ungroup();
        if (ImGui::MenuItem(l.IsGroup() ? "Merge Group" : "Merge Down", "Ctrl+E")) MergeDown();
        if (l.IsRaster() && ImGui::MenuItem("Layer Style...")) OpenLayerStyle();
        if (l.IsVector() && ImGui::MenuItem("Rasterize", "Ctrl+Shift+R")) RasterizeLayer();
        if (l.IsAdjustment() && ImGui::MenuItem("Properties")) m_ShowProperties = true;
        ImGui::Separator();
        if (!l.HasMask())
        {
            if (ImGui::MenuItem("Add Mask: Reveal All")) AddMask(true);
            if (ImGui::MenuItem("Add Mask: Hide All")) AddMask(false);
        }
        else
        {
            if (ImGui::MenuItem(l.MaskEnabled ? "Disable Mask" : "Enable Mask"))
            {
                Record(l.MaskEnabled ? "Disable Layer Mask" : "Enable Layer Mask");
                m_Doc.Layers[(size_t)i].MaskEnabled = !l.MaskEnabled;
                Changed();
                MarkAll();
            }
            if (ImGui::MenuItem("Invert Mask", "Ctrl+I"))
            {
                Record("Invert Mask");
                m_Doc.InvertMask(i);
                Changed();
                MarkAll();
            }
            if (l.HasPixels() && ImGui::MenuItem("Apply Mask"))
            {
                Record("Apply Layer Mask");
                m_Doc.ApplyMask(i);
                m_EditMask = false;
                Changed();
                MarkAll();
            }
            if (ImGui::MenuItem("Delete Mask"))
            {
                Record("Delete Layer Mask");
                m_Doc.RemoveMask(i);
                m_EditMask = false;
                Changed();
                MarkAll();
            }
        }
    }

    void ImageMode::DrawLayers()
    {
        if (m_Doc.Layers.empty()) return;
        m_ActiveLayer = std::clamp(m_ActiveLayer, 0, (int)m_Doc.Layers.size() - 1);
        Layer& active = m_Doc.Layers[(size_t)m_ActiveLayer];
        // The current layer's settings: blend mode (Pass Through for groups), opacity, locks.
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
        if (ImGui::BeginCombo("##blend", LayerBlendName(active.Blend), ImGuiComboFlags_HeightLargest))
        {
            for (int m = active.IsGroup() ? -1 : 0; m < (int)LayerBlend::PassThrough; ++m)
            {
                LayerBlend mode = m < 0 ? LayerBlend::PassThrough : (LayerBlend)m;
                if (m >= 0 && LayerBlendStartsGroup(mode)) ImGui::Separator();
                if (ImGui::Selectable(LayerBlendName(mode), active.Blend == mode) && active.Blend != mode)
                {
                    Record("Blend Mode");
                    m_Doc.Layers[(size_t)m_ActiveLayer].Blend = mode;
                    Changed();
                    MarkAll();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Blend mode: how the layer mixes with the ones under it");
        ImGui::SameLine();
        float opacity = active.Opacity * 100.0f;
        ImGui::SetNextItemWidth(-FLT_MIN);
        bool changed = ImGui::SliderFloat("##layerOpacity", &opacity, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActivated()) Record("Layer Opacity");
        ImGui::SetItemTooltip("Layer opacity");
        if (changed)
        {
            m_Doc.Layers[(size_t)m_ActiveLayer].Opacity = opacity / 100.0f;
            Changed();
            MarkAll();
        }
        Label("Lock");
        auto lock = [&](const char* label, bool Layer::*field, const char* tip) {
            bool v = m_Doc.Layers[(size_t)m_ActiveLayer].*field;
            if (ImGui::Checkbox(label, &v))
            {
                Record("Lock");
                m_Doc.Layers[(size_t)m_ActiveLayer].*field = v;
                Changed();
            }
            ImGui::SetItemTooltip("%s", tip);
            ImGui::SameLine();
        };
        lock("Alpha", &Layer::LockTransparency, "Painting keeps the layer's clear and see-through parts as they are");
        lock("Pixels", &Layer::LockPixels, "No painting on the layer");
        lock("All", &Layer::LockAll, "No painting, merging into or deleting the layer");
        ImGui::NewLine();

        // The list: top first, groups' contents indented under them.
        const float buttonsH = ImGui::GetFrameHeightWithSpacing() * 2.0f;
        float listH = std::max(60.0f, ImGui::GetContentRegionAvail().y - buttonsH);
        ImGui::BeginChild("##layerList", ImVec2(0, listH), ImGuiChildFlags_Borders);
        int moveFrom = -1, moveTo = -1;
        bool into = false;
        std::vector<int> top = m_Doc.Children(0);
        for (auto it = top.rbegin(); it != top.rend(); ++it) DrawLayerRow(*it, 0, moveFrom, moveTo, into);
        ImGui::EndChild();
        if (moveFrom >= 0) MoveLayerTo(moveFrom, moveTo, into);

        // The buttons under the list.
        const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4.0f) / 5.0f;
        const Layer& l = m_Doc.Layers[(size_t)m_ActiveLayer];
        if (ImGui::Button("New", ImVec2(bw, 0))) NewLayer();
        ImGui::SetItemTooltip("A new layer above this one (Ctrl+Shift+N)");
        ImGui::SameLine();
        if (ImGui::Button("Group", ImVec2(bw, 0))) GroupLayer();
        ImGui::SetItemTooltip("Put the layer in a new group (Ctrl+G; Ctrl+Shift+G ungroups)");
        ImGui::SameLine();
        if (ImGui::Button("Mask", ImVec2(bw, 0))) AddMask(!ImGui::GetIO().KeyAlt);
        ImGui::SetItemTooltip(l.HasMask() ? "Select the layer's mask" : "Add a mask that shows the whole layer (Alt+click: hides it)");
        ImGui::SameLine();
        if (ImGui::Button("Adj", ImVec2(bw, 0))) ImGui::OpenPopup("##newAdjustment");
        ImGui::SetItemTooltip("A new adjustment layer (levels, curves, hue/saturation...)");
        if (ImGui::BeginPopup("##newAdjustment"))
        {
            DrawAdjustmentMenu(true);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("fx", ImVec2(bw, 0))) OpenLayerStyle();
        ImGui::SetItemTooltip("Layer Style: shadows, glows, stroke, overlays");
        if (ImGui::Button("Copy", ImVec2(bw, 0))) DuplicateLayer();
        ImGui::SetItemTooltip("Duplicate the layer (Ctrl+J)");
        ImGui::SameLine();
        if (ImGui::Button("Up", ImVec2(bw, 0))) StepLayer(+1);
        ImGui::SetItemTooltip("Move the layer up (Ctrl+])");
        ImGui::SameLine();
        if (ImGui::Button("Down", ImVec2(bw, 0))) StepLayer(-1);
        ImGui::SetItemTooltip("Move the layer down (Ctrl+[)");
        ImGui::SameLine();
        if (ImGui::Button("Merge", ImVec2(bw, 0))) MergeDown();
        ImGui::SetItemTooltip(l.IsGroup() ? "Merge the group into one layer (Ctrl+E)" : "Merge down into the layer under it (Ctrl+E)");
        ImGui::SameLine();
        if (ImGui::Button("Delete", ImVec2(bw, 0))) DeleteLayer();
        ImGui::SetItemTooltip("Delete the layer (a group with its contents)");
    }

    void ImageMode::DrawHistory()
    {
        // The states, oldest first: clicking one goes back (or forward) to it; later ones stay until something new is done.
        ImGui::BeginChild("##history", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const int n = (int)m_Undo.size(), total = n + (int)m_Redo.size();
        int go = -1;
        for (int s = 0; s <= total; ++s)
        {
            const std::string& label = s == 0 ? m_StartLabel : s <= n ? m_Undo[(size_t)s - 1].Label : m_Redo[m_Redo.size() - (size_t)(s - n)].Label;
            ImGui::PushID(s);
            if (s > n) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(label.c_str(), s == n)) go = s;
            if (s > n) ImGui::PopStyleColor();
            if (s == n && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (go >= 0 && go < n)
            for (int k = 0; k < n - go; ++k) Undo();
        else if (go > n)
            for (int k = 0; k < go - n; ++k) Redo();
    }

    void ImageMode::DrawStatusBar()
    {
        ImGui::AlignTextToFramePadding();
        if (m_HoverInside && m_Composite.Width == m_Doc.Width && m_Composite.Height == m_Doc.Height)
        {
            uint32_t x = (uint32_t)m_Hover.x, y = (uint32_t)m_Hover.y;
            glm::vec4 c = Unpack(m_Composite.At(x, y));
            ImGui::Text("%u, %u", x, y);
            ImGui::SameLine(0.0f, 16.0f);
            ImVec2 at = ImGui::GetCursorScreenPos();
            float h = ImGui::GetTextLineHeight();
            at.y += (ImGui::GetFrameHeight() - h) * 0.5f;
            ImGui::GetWindowDrawList()->AddRectFilled(at, Add(at, ImVec2(h, h)), ToU32(glm::vec4(glm::vec3(c), 1.0f)));
            ImGui::Dummy(ImVec2(h, h));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Hex(c).c_str());
        }
        else
            ImGui::TextDisabled("%s", kToolNames[(int)EffectiveTool()]);
        if (!m_Doc.Layers.empty())
        {
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextDisabled("%s%s", m_Doc.Layers[(size_t)m_ActiveLayer].Name.c_str(), m_EditMask ? " (mask)" : "");
        }
        if (m_StatusTime > 0.0f && !m_Status.empty())
        {
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextColored(UI::AccentText(), "%s", m_Status.c_str());
        }
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextDisabled("%s %.1f ms", m_ComposedOnGpu ? "GPU" : "CPU", m_ComposeMs);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The last update of the picture (where it changed) and what made it");
        const char* hint = "Shift+click: a straight line   Alt: eyedropper   Space: pan   Wheel: zoom";
        float w = ImGui::CalcTextSize(hint).x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f, ImGui::GetWindowWidth() - w - 16.0f));
        ImGui::TextDisabled("%s", hint);
    }

    void ImageMode::HandleKeys()
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) || m_ShowNew) return;
        auto pressed = [](ImGuiKeyChord chord) { return ImGui::IsKeyChordPressed(chord, ImGuiInputFlags_None); };
        if (m_Preview.Kind)
        {
            if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) FinishPreview(true);
            else if (pressed(ImGuiKey_Escape)) FinishPreview(false);
            return;
        }
        if (m_TextEditing)
        {
            TextKeys(); // (keys type; shortcuts wait until the text is done)
            return;
        }
        if (pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_R)) RasterizeLayer();
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_Apostrophe)) m_ShowGrid = !m_ShowGrid;
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_L)) OpenAdjustment(AdjustmentType::Levels);
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_M)) OpenAdjustment(AdjustmentType::Curves);
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_U)) OpenAdjustment(AdjustmentType::HueSaturation);
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_B)) OpenAdjustment(AdjustmentType::ColorBalance);
        if (pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_U)) OpenAdjustment(AdjustmentType::HueSaturation); // (Shift: Desaturate)
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_S)) Save();
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_Z)) Undo();
        if (pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) || pressed(ImGuiMod_Ctrl | ImGuiKey_Y)) Redo();
        ImVec2 viewCenter = Add(m_ViewMin, Mul(m_ViewSize, 0.5f));
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_0)) Fit();
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_1)) SetZoom(1.0f, viewCenter);
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_Equal)) SetZoom(m_Zoom * 1.25f, viewCenter);
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_Minus)) SetZoom(m_Zoom / 1.25f, viewCenter);
        if (pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N)) NewLayer();
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_J))
        {
            if (m_Doc.Selection && m_Doc.Layers[(size_t)m_ActiveLayer].HasPixels()) LayerVia(false);
            else DuplicateLayer();
        }
        if (pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_J)) LayerVia(true);
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_A)) SelectAll();
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_D)) Deselect();
        if (pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_D)) Reselect();
        if (pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_I)) InverseSelection();
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_C)) Copy(false);
        if (pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C)) Copy(true);
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_X)) Cut();
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_V)) Paste(false);
        if (pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V)) Paste(true);
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_T)) BeginTransform();
        if (pressed(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_I)) m_Dialog = 1, m_OpenDialog = true;
        if (pressed(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_C)) m_Dialog = 2, m_OpenDialog = true;
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_Backspace)) FillWith(m_Background, "Fill");
        if (pressed(ImGuiMod_Alt | ImGuiKey_Backspace)) FillWith(m_Foreground, "Fill");
        if (pressed(ImGuiMod_Shift | ImGuiKey_F6) && m_Doc.Selection) m_Dialog = 6, m_OpenDialog = true, m_DialogValue = 4.0f;
        // Enter finishes, Esc cancels (a transform, the crop, a polygonal lasso).
        if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter))
        {
            if (m_Transform.Active) CommitTransform();
            else if (m_Tool == Tool::Crop && m_CropActive) CommitCrop();
            else if (m_LassoActive && m_Tool == Tool::PolyLasso)
            {
                m_LassoActive = false;
                if (m_Lasso.size() >= 3)
                {
                    Mask shape = SelectPolygon(m_Doc.Width, m_Doc.Height, m_Lasso, m_SelAntialias);
                    if (m_SelFeather > 0.0f) Feather(shape, m_SelFeather);
                    SetSelection(std::move(shape), m_DragOp, "Polygonal Lasso");
                }
                m_Lasso.clear();
            }
        }
        if (pressed(ImGuiKey_Escape))
        {
            if (m_Transform.Active) CancelTransform();
            else if (m_LassoActive) m_LassoActive = false, m_Lasso.clear();
            else if (m_Tool == Tool::Crop) m_CropActive = false;
        }
        if (pressed(ImGuiKey_Delete) || pressed(ImGuiKey_Backspace))
        {
            if (m_LassoActive && m_Tool == Tool::PolyLasso)
            {
                if (m_Lasso.size() > 1) m_Lasso.pop_back();
            }
            else if (!m_Transform.Active)
                Clear();
        }
        // Arrows: the Move tool nudges pixels; selection tools, the selection.
        if (!m_Transform.Active && (m_Tool == Tool::Move || (IsSelectTool(m_Tool) && m_Doc.Selection)) && !io.KeyCtrl && !io.KeyAlt)
        {
            int step = io.KeyShift ? 10 : 1;
            glm::ivec2 by(0);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) by.x -= step;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) by.x += step;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) by.y -= step;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) by.y += step;
            if (by != glm::ivec2(0)) Nudge(by);
        }
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_E)) MergeDown();
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_G)) GroupLayer();
        if (pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_G)) Ungroup();
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_RightBracket)) StepLayer(+1);
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_LeftBracket)) StepLayer(-1);
        if (pressed(ImGuiMod_Ctrl | ImGuiKey_I))
        {
            if (m_EditMask)
            {
                Record("Invert Mask");
                m_Doc.InvertMask(m_ActiveLayer);
                Changed();
                MarkAll();
            }
            else
                OpenAdjustment(AdjustmentType::Invert);
        }
        if (io.KeyCtrl || io.KeyAlt) return;
        // Tools: a slot's key picks its tool; Shift+key goes on to the next tool sharing the slot.
        for (size_t s = 0; s < m_Slots.size(); ++s)
        {
            if (!ImGui::IsKeyPressed(m_Slots[s].Key, false)) continue;
            const std::vector<Tool>& tools = m_Slots[s].Tools;
            if (io.KeyShift && tools.size() > 1)
            {
                auto it = std::find(tools.begin(), tools.end(), m_SlotTool[s]);
                SelectTool(tools[(size_t)((it - tools.begin() + 1) % (long)tools.size())]);
            }
            else
                SelectTool(m_SlotTool[s]);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) SelectTool(Tool::Pencil);
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) std::swap(m_Foreground, m_Background);
        if (ImGui::IsKeyPressed(ImGuiKey_D, false)) m_Foreground = { 0, 0, 0, 1 }, m_Background = { 1, 1, 1, 1 };
        if (BrushSettings* b = ToolBrush(m_Tool))
        {
            // [ and ]: the size by about a tenth (with Shift: the hardness by a quarter); digits: the opacity.
            if (io.KeyShift)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) b->Hardness = std::max(0.0f, b->Hardness - 0.25f);
                if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) b->Hardness = std::min(1.0f, b->Hardness + 0.25f);
            }
            else
            {
                if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) b->Size = std::max(1.0f, std::floor(b->Size / 1.12f));
                if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) b->Size = std::min(1000.0f, std::ceil(b->Size * 1.12f));
            }
            for (int d = 0; d <= 9; ++d)
                if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + d), false)) b->Opacity = d == 0 ? 1.0f : d / 10.0f;
        }
    }

    void ImageMode::DrawNewImageWindow(bool* open)
    {
        ImGui::SetNextWindowSize(ImVec2(400.0f, 0.0f), ImGuiCond_Always);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.4f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("New Image", open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
        {
            ImGui::End();
            return;
        }
        bool create = false;
        if (UI::BeginProperties("##newImage"))
        {
            UI::PropertyLabel("Name");
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##name", &m_NewName, ImGuiInputTextFlags_EnterReturnsTrue)) create = true;
            UI::PropertyLabel("Size");
            ImGui::InputInt2("##size", m_NewSize);
            m_NewSize[0] = std::clamp(m_NewSize[0], 1, 16384);
            m_NewSize[1] = std::clamp(m_NewSize[1], 1, 16384);
            UI::PropertyLabel("Presets");
            for (int s : { 64, 256, 512, 1024, 2048 })
            {
                if (ImGui::SmallButton(std::to_string(s).c_str())) m_NewSize[0] = m_NewSize[1] = s;
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("1920x1080")) m_NewSize[0] = 1920, m_NewSize[1] = 1080;
            UI::PropertyLabel("Background");
            static const char* backgrounds[] = { "White", "Black", "Transparent" };
            ImGui::Combo("##background", &m_NewBackground, backgrounds, 3);
            UI::EndProperties();
        }
        ImGui::TextDisabled("Saved to Assets/Images: the .dimage keeps the layers,\nthe .png next to it is what the game uses.");
        bool named = m_NewName.find_first_not_of(' ') != std::string::npos;
        ImGui::BeginDisabled(!named);
        if (ImGui::Button("Create", ImVec2(120.0f, 0.0f))) create = true;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) *open = false;
        ImGui::End();
        if (create && named)
        {
            fs::path folder = AssetManager::GetAssetsDirectory() / "Images";
            std::error_code ec;
            fs::create_directories(folder, ec);
            fs::path file = UniqueImageFile(folder, m_NewName);
            glm::vec4 bg = m_NewBackground == 0 ? glm::vec4(1.0f) : m_NewBackground == 1 ? glm::vec4(0, 0, 0, 1) : glm::vec4(0.0f);
            Document doc = Document::Create((uint32_t)m_NewSize[0], (uint32_t)m_NewSize[1], bg);
            doc.Export = file.stem().string() + ".png";
            if (doc.Save(file) && doc.Flatten().Save(doc.ExportPath(file)))
            {
                AssetManager::Import(file);
                AssetManager::Import(doc.ExportPath(file));
                m_Ctx.RequestOpenImage = file;
                *open = false;
            }
            else
                Log::Error("Could not write the image {}", file.string());
        }
    }
}
