#pragma once
#include "Dimension/Modeling/Paint.h"
#include "Dimension/Image/ImageStyle.h"
#include "Dimension/Image/ImageVector.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

// Image Mode's documents (like Photoshop's): a canvas of layers, each a full-size RGBA8 picture with a blend mode, an
// opacity and an optional mask, composited bottom to top; groups hold layers (and groups) and composite them together.
// Saved as a .dimage file (the layers kept) and exported flattened to an image file (PNG by default) that the game uses
// like any other texture.
namespace Dimension::Imaging
{
    // RGBA8, straight alpha, sRGB values (row 0 at the top, red in the lowest byte).
    using Bitmap = Modeling::Paint::Image;
    using Rect = Modeling::Paint::Rect;

    // A layer mask: 0 hides the layer's pixel, 255 shows it.
    struct Mask
    {
        uint32_t Width = 0, Height = 0;
        std::vector<uint8_t> Values;
        static Mask Solid(uint32_t width, uint32_t height, uint8_t value);
        uint8_t At(uint32_t x, uint32_t y) const { return Values[(size_t)y * Width + x]; }
    };

    const char* LayerBlendName(LayerBlend mode);
    LayerBlend LayerBlendFromName(const std::string& name); // Normal when unknown
    // Where a separator goes before `mode` in a menu (Photoshop's groups).
    bool LayerBlendStartsGroup(LayerBlend mode);
    // The blended color B(backdrop, source) of the mode (0..1 values; Dissolve, Normal and Pass Through give the source).
    glm::vec3 BlendColors(LayerBlend mode, const glm::vec3& backdrop, const glm::vec3& source);

    glm::vec4 Unpack(uint32_t rgba); // 0..1
    uint32_t Pack(const glm::vec4& color);
    float Luminance(const glm::vec3& color); // (Rec. 601 weights, as Photoshop's gray)

    enum class LayerKind { Pixels, Group, Adjustment, Text, Shape };

    struct Layer
    {
        uint32_t Id = 0;     // unique in the document (never 0)
        uint32_t Parent = 0; // the group it is in (0: the top level)
        LayerKind Kind = LayerKind::Pixels;
        std::string Name = "Layer";
        bool Visible = true;
        float Opacity = 1.0f; // 0..1
        LayerBlend Blend = LayerBlend::Normal;
        bool LockTransparency = false; // painting keeps each pixel's alpha
        bool LockPixels = false;       // no painting at all
        bool LockAll = false;          // no painting, moving or deleting
        bool Expanded = true;          // (groups: open in the Layers panel)
        // Shared between copies of the document (undo snapshots); Edit() makes them this layer's own first. Text and
        // shape layers draw theirs from their settings when those change (hence mutable: drawing is not an edit).
        mutable std::shared_ptr<const Bitmap> Pixels; // (none for groups and adjustments)
        mutable uint64_t VectorKey = 0;                // (text and shape layers: the settings Pixels were drawn from)
        TextData Text;                                 // (text layers)
        ShapeData Shape;                               // (shape layers)
        std::shared_ptr<const Mask> LayerMask;
        bool MaskEnabled = true;
        Imaging::Adjustment Adjust;     // (adjustment layers: what they do to what is under them)
        LayerEffects Effects;           // (pixel layers: shadows, glows, stroke, overlays)
        mutable std::shared_ptr<const StyledLayer> Styled; // (the effects rendered; remade when out of date)

        bool IsGroup() const { return Kind == LayerKind::Group; }
        bool IsAdjustment() const { return Kind == LayerKind::Adjustment; }
        bool HasPixels() const { return Kind == LayerKind::Pixels && Pixels != nullptr; } // (pixels of its own, to paint)
        bool IsVector() const { return Kind == LayerKind::Text || Kind == LayerKind::Shape; }
        bool IsRaster() const { return Kind == LayerKind::Pixels || IsVector(); }      // (shows pixels)
        bool HasMask() const { return LayerMask != nullptr; }
        bool CanPaint() const { return HasPixels() && !LockPixels && !LockAll; }
        const Bitmap& Get() const { return *Pixels; }
        Bitmap& Edit();
        Mask& EditMask();
    };

    class Document
    {
    public:
        uint32_t Width = 0, Height = 0;
        // Every layer and group. Layers with the same Parent stack in the order they appear here (bottom first); a group's
        // contents need not sit next to it.
        std::vector<Layer> Layers;
        uint32_t NextId = 1;
        // The flattened image's file, relative to the .dimage's folder (written on every save); empty = <name>.png.
        std::string Export;
        // The selection (none: everything); kept in undo snapshots, not in the file.
        std::shared_ptr<const Mask> Selection;

        // A canvas with one layer, "Background", filled with `background` (alpha 0: transparent).
        static Document Create(uint32_t width, uint32_t height, const glm::vec4& background);
        // An image file's pixels as the Background layer.
        static Document FromBitmap(Bitmap image);

        bool Valid() const { return Width > 0 && Height > 0; }
        int IndexOf(uint32_t id) const; // -1 when there is none
        std::vector<int> Children(uint32_t parent) const; // in stacking order
        bool IsInside(int index, uint32_t group) const;   // `group` is the layer's parent, or its parent's, ...
        int Depth(int index) const;                       // 0 at the top level
        bool ShownInTree(int index) const;                // it and every group around it visible
        // The sibling directly under / over the layer (-1 when there is none).
        int Below(int index) const;
        int Above(int index) const;

        Layer MakeLayer(const std::string& name); // transparent, canvas-sized, with a new Id
        // A new layer at `index` in the list, in `parent` (clamped); returns where it went.
        int AddLayer(const std::string& name, int index, uint32_t parent = 0);
        // An adjustment layer at `index` (with a white mask); returns where it went.
        int AddAdjustment(const Imaging::Adjustment& adjustment, int index, uint32_t parent = 0);
        // A text or shape layer at `index`; returns where it went.
        int AddText(const TextData& text, int index, uint32_t parent = 0);
        int AddShape(const ShapeData& shape, int index, uint32_t parent = 0);
        // A text or shape layer made a pixel layer (its pixels as they are drawn).
        void Rasterize(int index);
        // Text and shape layers' pixels drawn again where their settings changed.
        void RefreshVectors() const;
        // That, and layer effects rendered again where their layers changed (what compositing needs, CPU or GPU).
        void PrepareStyles() const;
        // A group holding the layer at `index` (where it was); returns the group's index.
        int GroupLayer(int index);
        // The group's contents into the group's place (the group goes); returns the index of its top layer (or -1).
        int Ungroup(int index);
        int DuplicateLayer(int index); // a group with copies of its contents
        void RemoveLayer(int index);   // a group with its contents
        // Within the list (its parent kept).
        void MoveLayer(int from, int to);
        // Into `parent`, just above the sibling `over` (-1: at the bottom of the parent); returns its new index. Refused
        // (returns -1) when that would put a group inside itself.
        int MoveInto(int index, uint32_t parent, int over);
        // Up or down among its siblings by one step; returns its new index.
        int Step(int index, int direction);
        // The layer drawn into the sibling under it (with its blend mode, opacity and mask); returns the merged layer.
        int MergeDown(int index);
        // A group made into one pixel layer of what it shows; returns that layer's index.
        int MergeGroup(int index);
        std::string UniqueLayerName(const std::string& base) const;

        // Masks: added (white reveals the layer, black hides it), removed, applied (into the layer's alpha), inverted.
        void AddMask(int index, bool revealAll);
        void RemoveMask(int index);
        void ApplyMask(int index);
        void InvertMask(int index);

        // The layers composited into `out` (made canvas-sized when it is not) inside `area` (inclusive; empty = all).
        void Composite(Bitmap& out, Rect area = {}) const;
        Bitmap Flatten() const;
        // A group (or layer) alone, as if it were the only thing in the document (its own opacity and mode not applied).
        Bitmap Render(int index) const;
        // Bytes of distinct pixel buffers (shared ones counted once per call through `seen`).
        size_t MemoryBytes(std::vector<const void*>* seen = nullptr) const;

        // .dimage: "DIMG", version, a JSON header (size, layers' settings) and the pixels and masks as PNG.
        std::vector<uint8_t> Serialize() const;
        // pixels = false: the header alone (size, layers' settings; no pixels decoded).
        static bool Deserialize(const std::vector<uint8_t>& bytes, Document& out, bool pixels = true);
        bool Save(const fs::path& file) const;
        static bool Load(const fs::path& file, Document& out, bool pixels = true);
        // Where the flattened image of the document saved at `file` goes.
        fs::path ExportPath(const fs::path& file) const;

    private:
        void CompositeRows(Bitmap& out, const Rect& area, uint32_t root, bool includeRoot) const;
    };
}
