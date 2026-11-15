#include "Shortcuts.h"
#include <imgui_internal.h>
#include <cstring>

namespace Editor::Shortcuts
{
    namespace
    {
        std::string s_Injected; // Press (automation): reads as pressed once
    }

    void Press(const std::string& id) { s_Injected = id; }

    std::vector<Shortcut>& All()
    {
        static std::vector<Shortcut> shortcuts = [] {
            std::vector<Shortcut> s = {
                { "file.new", "File", "New Scene...", ImGuiMod_Ctrl | ImGuiKey_N },
                { "file.open", "File", "Open Scene", ImGuiMod_Ctrl | ImGuiKey_O },
                { "file.save", "File", "Save Scene", ImGuiMod_Ctrl | ImGuiKey_S },
                { "file.saveAs", "File", "Save Scene As", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S },
                { "file.buildSettings", "File", "Build Settings", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_B },
                { "file.buildAndRun", "File", "Build And Run", ImGuiMod_Ctrl | ImGuiKey_B },
                { "file.buildModule", "File", "Build Game Module", ImGuiMod_Ctrl | ImGuiKey_R },
                { "edit.undo", "Edit", "Undo", ImGuiMod_Ctrl | ImGuiKey_Z, 0, true },
                { "edit.redo", "Edit", "Redo", ImGuiMod_Ctrl | ImGuiKey_Y, 0, true },
                { "edit.duplicate", "Edit", "Duplicate", ImGuiMod_Ctrl | ImGuiKey_D },
                { "edit.copy", "Edit", "Copy", ImGuiMod_Ctrl | ImGuiKey_C },
                { "edit.paste", "Edit", "Paste", ImGuiMod_Ctrl | ImGuiKey_V },
                { "edit.delete", "Edit", "Delete", ImGuiKey_Delete },
                { "edit.group", "Edit", "Group", ImGuiMod_Ctrl | ImGuiKey_G },
                { "edit.ungroup", "Edit", "Ungroup", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_G },
                { "edit.search", "Edit", "Search", ImGuiMod_Ctrl | ImGuiKey_K },
                { "edit.selectBack", "Edit", "Previous Selection", ImGuiMod_Alt | ImGuiKey_LeftArrow },
                { "edit.selectForward", "Edit", "Next Selection", ImGuiMod_Alt | ImGuiKey_RightArrow },
                { "edit.selectionSet1", "Edit", "Select Selection 1", ImGuiMod_Alt | ImGuiKey_1 },
                { "edit.selectionSet2", "Edit", "Select Selection 2", ImGuiMod_Alt | ImGuiKey_2 },
                { "edit.selectionSet3", "Edit", "Select Selection 3", ImGuiMod_Alt | ImGuiKey_3 },
                { "edit.selectionSet4", "Edit", "Select Selection 4", ImGuiMod_Alt | ImGuiKey_4 },
                { "edit.selectionSet5", "Edit", "Select Selection 5", ImGuiMod_Alt | ImGuiKey_5 },
                { "edit.selectionSet6", "Edit", "Select Selection 6", ImGuiMod_Alt | ImGuiKey_6 },
                { "edit.selectionSet7", "Edit", "Select Selection 7", ImGuiMod_Alt | ImGuiKey_7 },
                { "edit.selectionSet8", "Edit", "Select Selection 8", ImGuiMod_Alt | ImGuiKey_8 },
                { "edit.selectionSet9", "Edit", "Select Selection 9", ImGuiMod_Alt | ImGuiKey_9 },
                { "edit.saveSelectionSet1", "Edit", "Save Selection 1", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_1 },
                { "edit.saveSelectionSet2", "Edit", "Save Selection 2", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_2 },
                { "edit.saveSelectionSet3", "Edit", "Save Selection 3", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_3 },
                { "edit.saveSelectionSet4", "Edit", "Save Selection 4", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_4 },
                { "edit.saveSelectionSet5", "Edit", "Save Selection 5", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_5 },
                { "edit.saveSelectionSet6", "Edit", "Save Selection 6", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_6 },
                { "edit.saveSelectionSet7", "Edit", "Save Selection 7", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_7 },
                { "edit.saveSelectionSet8", "Edit", "Save Selection 8", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_8 },
                { "edit.saveSelectionSet9", "Edit", "Save Selection 9", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_9 },
                { "play.play", "Play", "Play / Stop", ImGuiMod_Ctrl | ImGuiKey_P },
                { "play.pause", "Play", "Pause", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P },
                { "scene.hide", "Scene View", "Hide or Show Selection", ImGuiKey_H },
                { "scene.isolate", "Scene View", "Isolate Selection", ImGuiMod_Shift | ImGuiKey_H },
                { "scene.frame", "Scene View", "Frame Selected", ImGuiKey_F },
                { "scene.frameAll", "Scene View", "Frame All", ImGuiMod_Shift | ImGuiKey_F },
                { "scene.moveToView", "Scene View", "Move To View", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_F },
                { "scene.alignWithView", "Scene View", "Align With View", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F },
                { "scene.alignViewToSelected", "Scene View", "Align View to Selected", 0 },
                { "navigation.bakeAll", "Navigation", "Bake All (saved with the scene)", 0 },
                { "scene.view1", "Scene View", "Go to View 1", ImGuiMod_Ctrl | ImGuiKey_1 },
                { "scene.view2", "Scene View", "Go to View 2", ImGuiMod_Ctrl | ImGuiKey_2 },
                { "scene.view3", "Scene View", "Go to View 3", ImGuiMod_Ctrl | ImGuiKey_3 },
                { "scene.view4", "Scene View", "Go to View 4", ImGuiMod_Ctrl | ImGuiKey_4 },
                { "scene.view5", "Scene View", "Go to View 5", ImGuiMod_Ctrl | ImGuiKey_5 },
                { "scene.view6", "Scene View", "Go to View 6", ImGuiMod_Ctrl | ImGuiKey_6 },
                { "scene.view7", "Scene View", "Go to View 7", ImGuiMod_Ctrl | ImGuiKey_7 },
                { "scene.view8", "Scene View", "Go to View 8", ImGuiMod_Ctrl | ImGuiKey_8 },
                { "scene.view9", "Scene View", "Go to View 9", ImGuiMod_Ctrl | ImGuiKey_9 },
                { "scene.saveView1", "Scene View", "Save View 1", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_1 },
                { "scene.saveView2", "Scene View", "Save View 2", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_2 },
                { "scene.saveView3", "Scene View", "Save View 3", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_3 },
                { "scene.saveView4", "Scene View", "Save View 4", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_4 },
                { "scene.saveView5", "Scene View", "Save View 5", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_5 },
                { "scene.saveView6", "Scene View", "Save View 6", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_6 },
                { "scene.saveView7", "Scene View", "Save View 7", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_7 },
                { "scene.saveView8", "Scene View", "Save View 8", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_8 },
                { "scene.saveView9", "Scene View", "Save View 9", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_9 },
                { "tool.view", "Tools", "View Tool", ImGuiKey_Q },
                { "tool.move", "Tools", "Move Tool", ImGuiKey_W },
                { "tool.rotate", "Tools", "Rotate Tool", ImGuiKey_E },
                { "tool.scale", "Tools", "Scale Tool", ImGuiKey_R },
                { "tool.measure", "Tools", "Measure", ImGuiKey_M },
                // Edit Mode (modeling in the Scene view): Blender's keys. They apply only while editing a mesh.
                { "mesh.editMode", "Mesh Edit", "Enter or Leave Edit Mode", ImGuiKey_Tab },
                { "mesh.vertexMode", "Mesh Edit", "Vertex Select Mode", ImGuiKey_1 },
                { "mesh.edgeMode", "Mesh Edit", "Edge Select Mode", ImGuiKey_2 },
                { "mesh.faceMode", "Mesh Edit", "Face Select Mode", ImGuiKey_3 },
                { "mesh.selectAll", "Mesh Edit", "Select All", ImGuiKey_A },
                { "mesh.deselectAll", "Mesh Edit", "Select None", ImGuiMod_Alt | ImGuiKey_A },
                { "mesh.invert", "Mesh Edit", "Invert Selection", ImGuiMod_Ctrl | ImGuiKey_I },
                { "mesh.selectLinked", "Mesh Edit", "Select Linked", ImGuiMod_Ctrl | ImGuiKey_L },
                { "mesh.linkedPick", "Mesh Edit", "Select Linked Under Mouse", ImGuiKey_L },
                { "mesh.selectMore", "Mesh Edit", "Select More", ImGuiMod_Ctrl | ImGuiKey_Equal },
                { "mesh.selectLess", "Mesh Edit", "Select Less", ImGuiMod_Ctrl | ImGuiKey_Minus },
                { "mesh.grab", "Mesh Edit", "Move", ImGuiKey_G },
                { "mesh.rotate", "Mesh Edit", "Rotate", ImGuiKey_R },
                { "mesh.scale", "Mesh Edit", "Scale", ImGuiKey_S },
                { "mesh.delete", "Mesh Edit", "Delete Menu", ImGuiKey_X },
                { "mesh.merge", "Mesh Edit", "Merge Menu", ImGuiKey_M },
                { "mesh.normals", "Mesh Edit", "Normals Menu", ImGuiMod_Alt | ImGuiKey_N },
                { "mesh.recalcNormals", "Mesh Edit", "Recalculate Normals Outside", ImGuiMod_Shift | ImGuiKey_N },
                { "mesh.add", "Mesh Edit", "Add Mesh Menu", ImGuiMod_Shift | ImGuiKey_A },
                { "mesh.edgeMenu", "Mesh Edit", "Edge Menu", ImGuiMod_Ctrl | ImGuiKey_E },
                { "mesh.hide", "Mesh Edit", "Hide Selected", ImGuiKey_H },
                { "mesh.hideUnselected", "Mesh Edit", "Hide Unselected", ImGuiMod_Shift | ImGuiKey_H },
                { "mesh.reveal", "Mesh Edit", "Reveal Hidden", ImGuiMod_Alt | ImGuiKey_H },
                { "mesh.proportional", "Mesh Edit", "Toggle Proportional Editing", ImGuiKey_O },
                { "mesh.xray", "Mesh Edit", "Toggle X-Ray", ImGuiMod_Alt | ImGuiKey_Z },
                { "mesh.frame", "Mesh Edit", "Frame Selected", ImGuiKey_KeypadDecimal },
                { "mesh.extrude", "Mesh Edit", "Extrude", ImGuiKey_E },
                { "mesh.extrudeMenu", "Mesh Edit", "Extrude Menu", ImGuiMod_Alt | ImGuiKey_E },
                { "mesh.inset", "Mesh Edit", "Inset Faces", ImGuiKey_I },
                { "mesh.bevel", "Mesh Edit", "Bevel Edges", ImGuiMod_Ctrl | ImGuiKey_B },
                { "mesh.bevelVertices", "Mesh Edit", "Bevel Vertices", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_B },
                { "mesh.loopCut", "Mesh Edit", "Loop Cut and Slide", ImGuiMod_Ctrl | ImGuiKey_R },
                { "mesh.knife", "Mesh Edit", "Knife", ImGuiKey_K },
                { "mesh.fill", "Mesh Edit", "Make Edge/Face", ImGuiKey_F },
                { "mesh.connect", "Mesh Edit", "Connect Vertex Path", ImGuiKey_J },
                { "mesh.dissolve", "Mesh Edit", "Dissolve (by select mode)", ImGuiMod_Ctrl | ImGuiKey_X },
                { "mesh.duplicate", "Mesh Edit", "Duplicate", ImGuiMod_Shift | ImGuiKey_D },
                { "mesh.separate", "Mesh Edit", "Separate Menu", ImGuiKey_P },
                { "mesh.shrinkFatten", "Mesh Edit", "Shrink/Fatten", ImGuiMod_Alt | ImGuiKey_S },
                { "mesh.faceMenu", "Mesh Edit", "Face Menu", ImGuiMod_Ctrl | ImGuiKey_F },
                { "mesh.uvMenu", "Mesh Edit", "UV Menu (Unwrap)", ImGuiKey_U },
                { "mesh.sculptMode", "Mesh Edit", "Sculpt Mode (and back)", ImGuiMod_Shift | ImGuiKey_Tab },
                { "sculpt.draw", "Sculpt", "Draw Brush", ImGuiKey_X },
                { "sculpt.clay", "Sculpt", "Clay Brush", ImGuiKey_C },
                { "sculpt.inflate", "Sculpt", "Inflate Brush", ImGuiKey_I },
                { "sculpt.smooth", "Sculpt", "Smooth Brush", ImGuiKey_S },
                { "sculpt.flatten", "Sculpt", "Flatten Brush", ImGuiMod_Shift | ImGuiKey_T },
                { "sculpt.scrape", "Sculpt", "Scrape Brush", ImGuiMod_Shift | ImGuiKey_S },
                { "sculpt.pinch", "Sculpt", "Pinch Brush", ImGuiKey_P },
                { "sculpt.crease", "Sculpt", "Crease Brush", ImGuiMod_Shift | ImGuiKey_C },
                { "sculpt.grab", "Sculpt", "Grab Brush", ImGuiKey_G },
                { "sculpt.snakeHook", "Sculpt", "Snake Hook Brush", ImGuiKey_K },
                { "sculpt.mask", "Sculpt", "Mask Brush", ImGuiKey_M },
                { "sculpt.pose", "Sculpt", "Pose Brush", 0 },
                { "sculpt.boundary", "Sculpt", "Boundary Brush", 0 },
                { "sculpt.cloth", "Sculpt", "Cloth Brush", 0 },
                { "sculpt.radius", "Sculpt", "Brush Radius (drag)", ImGuiKey_F },
                { "sculpt.strength", "Sculpt", "Brush Strength (drag)", ImGuiMod_Shift | ImGuiKey_F },
                { "sculpt.smaller", "Sculpt", "Smaller Brush", ImGuiKey_LeftBracket },
                { "sculpt.larger", "Sculpt", "Larger Brush", ImGuiKey_RightBracket },
                { "sculpt.dyntopo", "Sculpt", "Toggle Dynamic Topology", ImGuiMod_Ctrl | ImGuiKey_D },
                { "sculpt.remesh", "Sculpt", "Voxel Remesh", ImGuiMod_Ctrl | ImGuiKey_R },
                { "sculpt.clearMask", "Sculpt", "Clear Mask", ImGuiMod_Alt | ImGuiKey_M },
                { "sculpt.invertMask", "Sculpt", "Invert Mask", ImGuiMod_Ctrl | ImGuiKey_I },
                { "mesh.vertexPaint", "Mesh Edit", "Vertex Paint", 0 },
                { "mesh.texturePaint", "Mesh Edit", "Texture Paint", 0 },
                { "mesh.rigMode", "Mesh Edit", "Rig (Bones)", 0 },
                { "rig.selectAll", "Rig", "Select All Bones", ImGuiKey_A },
                { "rig.deselectAll", "Rig", "Select No Bones", ImGuiMod_Alt | ImGuiKey_A },
                { "rig.grab", "Rig", "Move", ImGuiKey_G },
                { "rig.rotate", "Rig", "Rotate", ImGuiKey_R },
                { "rig.scale", "Rig", "Scale", ImGuiKey_S },
                { "rig.addBone", "Rig", "Add Bone", ImGuiMod_Shift | ImGuiKey_A },
                { "rig.extrude", "Rig", "Extrude Bone", ImGuiKey_E },
                { "rig.delete", "Rig", "Delete Bones", ImGuiKey_X },
                { "rig.parent", "Rig", "Parent to Active Bone", ImGuiMod_Ctrl | ImGuiKey_P },
                { "rig.clearParent", "Rig", "Clear Parent", ImGuiMod_Alt | ImGuiKey_P },
                { "rig.clearRotation", "Rig", "Clear Pose Rotation", ImGuiMod_Alt | ImGuiKey_R },
                { "rig.clearLocation", "Rig", "Clear Pose Location", ImGuiMod_Alt | ImGuiKey_G },
                { "rig.clearScale", "Rig", "Clear Pose Scale", ImGuiMod_Alt | ImGuiKey_S },
                { "rig.insertKey", "Rig", "Insert Keyframe", ImGuiKey_I },
                { "rig.autoWeights", "Rig", "Automatic Weights", 0 },
                { "rig.boneMode", "Rig", "Bones", 0 },
                { "rig.poseMode", "Rig", "Pose", 0 },
                { "rig.weightMode", "Rig", "Weights", 0 },
                { "mesh.curveMode", "Mesh Edit", "Edit Curves", 0 },
                { "mesh.snapSurface", "Mesh Edit", "Toggle Snapping to Surfaces", 0 },
                { "curve.selectAll", "Curve", "Select All Points", ImGuiKey_A },
                { "curve.deselectAll", "Curve", "Select No Points", ImGuiMod_Alt | ImGuiKey_A },
                { "curve.grab", "Curve", "Move Points", ImGuiKey_G },
                { "curve.radius", "Curve", "Scale Radius", ImGuiMod_Alt | ImGuiKey_S },
                { "curve.extrude", "Curve", "Extrude", ImGuiKey_E },
                { "curve.delete", "Curve", "Delete Points", ImGuiKey_X },
                { "curve.cyclic", "Curve", "Toggle Cyclic", ImGuiMod_Alt | ImGuiKey_C },
                { "curve.sharp", "Curve", "Toggle Sharp", ImGuiKey_V },
                { "curve.handleType", "Curve", "Cycle Handle Type (Auto, Aligned, Free)", ImGuiKey_H },
                { "curve.add", "Curve", "Add Curve", ImGuiMod_Shift | ImGuiKey_A },
                { "paint.radius", "Paint", "Brush Radius (drag)", ImGuiKey_F },
                { "paint.strength", "Paint", "Brush Strength (drag)", ImGuiMod_Shift | ImGuiKey_F },
                { "paint.sample", "Paint", "Pick Color Under the Mouse", ImGuiKey_S },
                { "paint.smaller", "Paint", "Smaller Brush", ImGuiKey_LeftBracket },
                { "paint.larger", "Paint", "Larger Brush", ImGuiKey_RightBracket },
                { "uv.vertexMode", "UV Editor", "Vertex Select Mode", ImGuiKey_1 },
                { "uv.edgeMode", "UV Editor", "Edge Select Mode", ImGuiKey_2 },
                { "uv.faceMode", "UV Editor", "Face Select Mode", ImGuiKey_3 },
                { "uv.islandMode", "UV Editor", "Island Select Mode", ImGuiKey_4 },
                { "uv.selectAll", "UV Editor", "Select All", ImGuiKey_A },
                { "uv.deselectAll", "UV Editor", "Select None", ImGuiMod_Alt | ImGuiKey_A },
                { "uv.invert", "UV Editor", "Invert Selection", ImGuiMod_Ctrl | ImGuiKey_I },
                { "uv.selectLinked", "UV Editor", "Select Linked", ImGuiMod_Ctrl | ImGuiKey_L },
                { "uv.linkedPick", "UV Editor", "Select Island Under Mouse", ImGuiKey_L },
                { "uv.grab", "UV Editor", "Move", ImGuiKey_G },
                { "uv.rotate", "UV Editor", "Rotate", ImGuiKey_R },
                { "uv.scale", "UV Editor", "Scale", ImGuiKey_S },
                { "uv.pin", "UV Editor", "Pin", ImGuiKey_P },
                { "uv.unpin", "UV Editor", "Unpin", ImGuiMod_Alt | ImGuiKey_P },
                { "uv.unwrapMenu", "UV Editor", "UV Menu", ImGuiKey_U },
                { "uv.merge", "UV Editor", "Merge Menu", ImGuiKey_M },
                { "uv.stitch", "UV Editor", "Stitch", ImGuiKey_V },
                { "uv.sync", "UV Editor", "Toggle Sync Selection", 0 },
                { "uv.paint", "UV Editor", "Toggle Painting", 0 },
                { "uv.frame", "UV Editor", "Frame Selected", ImGuiKey_KeypadDecimal },
                { "uv.frameAll", "UV Editor", "Frame All", ImGuiKey_Home },
                { "object.join", "Object", "Join Meshes", ImGuiMod_Ctrl | ImGuiKey_J },
            };
            for (Shortcut& x : s) x.Key = x.Default;
            return s;
        }();
        return shortcuts;
    }

    Shortcut* Find(const std::string& id)
    {
        for (Shortcut& s : All())
            if (id == s.Id) return &s;
        return nullptr;
    }

    bool Pressed(const char* id)
    {
        if (!s_Injected.empty() && s_Injected == id)
        {
            s_Injected.clear();
            return true;
        }
        const Shortcut* s = Find(id);
        if (!s || s->Key == 0) return false;
        ImGuiKey key = (ImGuiKey)(s->Key & ~ImGuiMod_Mask_);
        int mods = s->Key & ImGuiMod_Mask_;
        return ImGui::GetIO().KeyMods == mods && ImGui::IsKeyPressed(key, s->Repeat);
    }

    std::string ChordName(ImGuiKeyChord chord) { return chord ? ImGui::GetKeyChordName(chord) : ""; }

    std::string Label(const char* id)
    {
        const Shortcut* s = Find(id);
        return s ? ChordName(s->Key) : std::string();
    }

    bool ShadowedInEditMode(const char* id)
    {
        const Shortcut* s = Find(id);
        auto editing = [](const Shortcut& x) {
            std::string_view c = x.Category;
            return c == "Mesh Edit" || c == "Sculpt" || c == "Paint" || c == "Rig" || c == "Curve";
        };
        if (!s || s->Key == 0 || editing(*s)) return false;
        for (const Shortcut& other : All())
            if (other.Key == s->Key && editing(other)) return true;
        return false;
    }

    std::vector<const Shortcut*> Conflicts(const Shortcut& s)
    {
        std::vector<const Shortcut*> out;
        if (s.Key == 0) return out;
        // Edit Mode's keys replace the others while a mesh is edited (except Undo and Redo, which it uses), and the UV
        // Editor's work only in its window, so those never clash with keys of another scope.
        auto contextual = [](const Shortcut& x) {
            std::string_view id = x.Id;
            return id != "edit.undo" && id != "edit.redo";
        };
        auto scope = [](const Shortcut& x) {
            std::string_view c = x.Category;
            return c == "Mesh Edit" ? 1 : c == "UV Editor" ? 2 : c == "Sculpt" ? 3 : c == "Paint" ? 4 : c == "Rig" ? 5 : c == "Curve" ? 6 : 0;
        };
        for (const Shortcut& other : All())
        {
            if (&other == &s || other.Key != s.Key) continue;
            int a = scope(s), b = scope(other);
            if (a != b && ((a != 0 && b != 0) || contextual(a == 0 ? s : other))) continue;
            out.push_back(&other);
        }
        return out;
    }

    nlohmann::json Save()
    {
        nlohmann::json changed = nlohmann::json::object();
        for (const Shortcut& s : All())
            if (s.Key != s.Default) changed[s.Id] = s.Key;
        return changed;
    }

    void Load(const nlohmann::json& changed)
    {
        for (Shortcut& s : All()) s.Key = s.Default;
        if (!changed.is_object()) return;
        for (auto& [id, key] : changed.items())
            if (Shortcut* s = Find(id); s && key.is_number_integer()) s->Key = key.get<int>();
    }
}
