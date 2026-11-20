#include "MeshEditMode.h"
#include "Shortcuts.h"
#include <Dimension/Core/Log.h>
#include <Dimension/Scene/Components.h>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

// Edit Mode's rigging: the armature's bones, posing them (and keying actions), weight painting, and shape keys.
namespace Editor
{
    using namespace Dimension::Modeling;
    namespace rig = Dimension::Modeling::Rig;

    namespace
    {
        constexpr ImU32 kBone = IM_COL32(150, 150, 150, 230);
        constexpr ImU32 kBonePose = IM_COL32(110, 160, 230, 230);
        constexpr ImU32 kBoneSelected = IM_COL32(255, 160, 40, 255);
        constexpr ImU32 kBoneActive = IM_COL32(255, 220, 140, 255);

        float Dist(ImVec2 a, ImVec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

        float SegmentDist(ImVec2 p, ImVec2 a, ImVec2 b)
        {
            ImVec2 ab(b.x - a.x, b.y - a.y);
            float len2 = ab.x * ab.x + ab.y * ab.y;
            float t = len2 > 0.0f ? std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2, 0.0f, 1.0f) : 0.0f;
            return Dist(p, ImVec2(a.x + ab.x * t, a.y + ab.y * t));
        }

        bool RayPlane(const glm::vec3& from, const glm::vec3& dir, const glm::vec3& point, const glm::vec3& normal, glm::vec3& hit)
        {
            float d = glm::dot(dir, normal);
            if (std::abs(d) < 1e-8f) return false;
            hit = from + dir * (glm::dot(point - from, normal) / d);
            return true;
        }

        glm::quat RotationOf(const glm::mat4& m)
        {
            glm::mat3 r(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2])));
            return glm::normalize(glm::quat_cast(r));
        }

        const char* kRigModeNames[] = { "", "Bones", "Pose", "Weights" };
    }

    // ---- Modes and preview

    void MeshEditMode::SetRigMode(int mode)
    {
        if (!m_Active || mode == m_RigMode) return;
        if (m_Modal.Kind != ModalKind::None) EndModal(false);
        if (m_BoneModal.Kind) EndBoneModal(false);
        m_Tool = Tool::None;
        if (m_Sculpting) SetSculpting(false);
        if (m_PaintMode) SetPaintMode(0);
        if (mode && m_CurveMode) SetCurveMode(false);
        m_RigMode = std::clamp(mode, 0, 3);
        m_WeightPainting = m_ActionPlaying = false;
        m_PaintRaysVersion = ~0ull;
        if (m_ActiveBone >= (int)m_Mesh.Bones.size()) m_ActiveBone = m_Mesh.Bones.empty() ? -1 : 0;
        if (m_RigMode == 3 && m_ActiveBone < 0 && !m_Mesh.Bones.empty()) m_ActiveBone = 0;
        if (m_RigMode == 0) ++m_GeometryVersion;
        SyncRigPreview();
    }

    void MeshEditMode::SyncRigPreview()
    {
        if (m_RigMode == 2 && m_Mesh.HasRig() && !rig::AtRest(m_Mesh))
        {
            MeshData data = m_Mesh.ToMeshData();
            rig::PoseMeshData(data, m_Mesh);
            ++m_GeometryVersion;
            AssetManager::SetMeshData(m_Asset, std::move(data));
            return;
        }
        if (m_RigMode == 3 && m_ActiveBone >= 0 && m_ActiveBone < (int)m_Mesh.Bones.size())
        {
            // The active bone's weights as colors (blue none .. red full).
            EditableMesh view = m_Mesh.EditedShape >= 0 ? m_Mesh.Canonical() : m_Mesh;
            view.Bones.clear();
            view.ShapeKeys.clear();
            view.Modifiers.clear();
            for (Face& f : view.Faces)
            {
                f.Colors.resize(f.Verts.size());
                for (size_t c = 0; c < f.Verts.size(); ++c) f.Colors[c] = PackRGBA8(rig::WeightColor(rig::GetWeight(m_Mesh, f.Verts[c], m_ActiveBone)));
            }
            ++m_GeometryVersion;
            AssetManager::SetMeshData(m_Asset, view.ToMeshData(false));
            return;
        }
        SyncRenderMesh();
    }

    void MeshEditMode::RigOperate(const char* name, const std::function<void()>& fn)
    {
        Record(name);
        fn();
        m_Dirty = true;
        SyncRigPreview();
    }

    // ---- Bones on screen

    bool MeshEditMode::BoneScreen(int b, ImVec2& head, ImVec2& tail) const
    {
        const Bone& bone = m_Mesh.Bones[(size_t)b];
        glm::vec3 h = bone.Head, t = bone.Tail;
        if (m_RigMode == 2)
        {
            std::vector<glm::mat4> pose = rig::PoseMatrices(m_Mesh);
            float len = glm::length(bone.Tail - bone.Head);
            h = glm::vec3(pose[(size_t)b] * glm::vec4(0, 0, 0, 1));
            t = glm::vec3(pose[(size_t)b] * glm::vec4(0, len, 0, 1));
        }
        glm::mat4 mvp = m_ViewProj * m_World;
        auto project = [&](const glm::vec3& p, ImVec2& out) {
            glm::vec4 c = mvp * glm::vec4(p, 1.0f);
            if (c.w <= 1e-6f) return false;
            out = ImVec2(m_Origin.x + (c.x / c.w * 0.5f + 0.5f) * m_Size.x, m_Origin.y + (0.5f - c.y / c.w * 0.5f) * m_Size.y);
            return true;
        };
        return project(h, head) && project(t, tail);
    }

    void MeshEditMode::DrawBoneOverlay(ImDrawList* dl)
    {
        if (m_Mesh.Bones.empty()) return;
        glm::mat4 mvp = m_ViewProj * m_World;
        std::vector<glm::mat4> frames(m_Mesh.Bones.size());
        if (m_RigMode == 2) frames = rig::PoseMatrices(m_Mesh);
        else
            for (size_t b = 0; b < m_Mesh.Bones.size(); ++b) frames[b] = rig::RestMatrix(m_Mesh.Bones[b]);
        auto project = [&](const glm::vec3& p, ImVec2& out) {
            glm::vec4 c = mvp * glm::vec4(p, 1.0f);
            if (c.w <= 1e-6f) return false;
            out = ImVec2(m_Origin.x + (c.x / c.w * 0.5f + 0.5f) * m_Size.x, m_Origin.y + (0.5f - c.y / c.w * 0.5f) * m_Size.y);
            return true;
        };
        for (size_t b = 0; b < m_Mesh.Bones.size(); ++b)
        {
            const Bone& bone = m_Mesh.Bones[b];
            float len = glm::length(bone.Tail - bone.Head);
            if (len < 1e-8f) continue;
            const glm::mat4& f = frames[b];
            float r = len * 0.1f;
            glm::vec3 local[6] = { { 0, 0, 0 }, { r, len * 0.1f, 0 }, { 0, len * 0.1f, r }, { -r, len * 0.1f, 0 }, { 0, len * 0.1f, -r }, { 0, len, 0 } };
            ImVec2 p[6];
            bool ok = true;
            for (int i = 0; i < 6; ++i) ok &= project(glm::vec3(f * glm::vec4(local[i], 1.0f)), p[i]);
            if (!ok) continue;
            bool selected = (bone.Flags & BoneBody) != 0;
            ImU32 col = (int)b == m_ActiveBone && selected ? kBoneActive : selected ? kBoneSelected : (m_RigMode == 2 ? kBonePose : kBone);
            float w = selected ? 2.0f : 1.25f;
            for (int i = 1; i <= 4; ++i)
            {
                dl->AddLine(p[0], p[i], col, w);
                dl->AddLine(p[i], p[5], col, w);
                dl->AddLine(p[i], p[i % 4 + 1], col, w);
            }
            // Parent link (dashed look: a thin line from the parent's tail).
            if (bone.Parent >= 0 && bone.Parent < (int)m_Mesh.Bones.size())
            {
                const Bone& parent = m_Mesh.Bones[(size_t)bone.Parent];
                float plen = glm::length(parent.Tail - parent.Head);
                ImVec2 pt;
                if (project(glm::vec3(frames[(size_t)bone.Parent] * glm::vec4(0, plen, 0, 1)), pt) && Dist(pt, p[0]) > 2.0f)
                    dl->AddLine(pt, p[0], IM_COL32(200, 200, 200, 90), 1.0f);
            }
            if (m_RigMode == 1)
            {
                dl->AddCircleFilled(p[0], 4.0f, (bone.Flags & BoneHead) ? kBoneSelected : IM_COL32(40, 40, 40, 255));
                dl->AddCircleFilled(p[5], 4.0f, (bone.Flags & BoneTail) ? kBoneSelected : IM_COL32(40, 40, 40, 255));
            }
            // A B-bone's curve: its segments, with a tick across each joint between them.
            if (bone.Segments > 1)
            {
                std::vector<glm::mat4> segs = rig::BBoneSegments(m_Mesh, (int)b, frames);
                std::vector<ImVec2> pts;
                for (const glm::mat4& s : segs)
                {
                    ImVec2 q;
                    if (project(glm::vec3(s[3]), q)) pts.push_back(q);
                }
                ImVec2 tail;
                if (project(glm::vec3(f * glm::vec4(0, len, 0, 1)), tail)) pts.push_back(tail);
                ImU32 curve = IM_COL32(255, 170, 60, 230);
                for (size_t i = 1; i < pts.size(); ++i) dl->AddLine(pts[i - 1], pts[i], curve, 2.5f);
                for (size_t k = 1; k < segs.size(); ++k)
                {
                    ImVec2 a, c;
                    glm::vec3 x = glm::vec3(segs[k][0]) * (len * 0.06f);
                    if (project(glm::vec3(segs[k][3]) - x, a) && project(glm::vec3(segs[k][3]) + x, c)) dl->AddLine(a, c, curve, 1.5f);
                }
            }
            // The envelope: full weight inside the inner outline, none past the outer one.
            if (m_ShowEnvelopes && m_RigMode == 1 && bone.Deform)
            {
                glm::vec3 right = glm::normalize(glm::vec3(glm::inverse(m_View)[0]));
                glm::mat3 toModel = glm::inverse(glm::mat3(m_World));
                glm::vec3 side = glm::normalize(toModel * right);
                glm::vec3 headP = glm::vec3(f * glm::vec4(0, 0, 0, 1)), tailP = glm::vec3(f * glm::vec4(0, len, 0, 1));
                for (int ring = 0; ring < 2; ++ring)
                {
                    float extra = ring ? bone.EnvelopeDistance : 0.0f;
                    float rh = bone.HeadRadius + extra, rt = bone.TailRadius + extra;
                    ImVec2 h0, h1, t0, t1;
                    if (!project(headP, h0) || !project(headP + side * rh, h1) || !project(tailP, t0) || !project(tailP + side * rt, t1)) continue;
                    float sh = Dist(h0, h1), st = Dist(t0, t1);
                    ImU32 col = ring ? IM_COL32(200, 200, 255, 70) : IM_COL32(200, 200, 255, 150);
                    dl->AddCircle(h0, sh, col, 32, 1.0f);
                    dl->AddCircle(t0, st, col, 32, 1.0f);
                    // The capsule's sides (perpendicular to the bone on screen).
                    ImVec2 d(t0.x - h0.x, t0.y - h0.y);
                    float dl2 = std::sqrt(d.x * d.x + d.y * d.y);
                    if (dl2 > 1e-3f)
                    {
                        ImVec2 n(-d.y / dl2, d.x / dl2);
                        dl->AddLine(ImVec2(h0.x + n.x * sh, h0.y + n.y * sh), ImVec2(t0.x + n.x * st, t0.y + n.y * st), col, 1.0f);
                        dl->AddLine(ImVec2(h0.x - n.x * sh, h0.y - n.y * sh), ImVec2(t0.x - n.x * st, t0.y - n.y * st), col, 1.0f);
                    }
                }
            }
        }
    }

    int MeshEditMode::PickBone(const ImVec2& mouse, int* joint) const
    {
        int best = -1, bestJoint = BoneBody;
        float bestD = 9.0f;
        for (size_t b = 0; b < m_Mesh.Bones.size(); ++b)
        {
            ImVec2 h, t;
            if (!BoneScreen((int)b, h, t)) continue;
            if (m_RigMode == 1)
            {
                if (float d = Dist(mouse, t); d < bestD + 3.0f && d < 12.0f)
                {
                    bestD = d - 3.0f;
                    best = (int)b;
                    bestJoint = BoneTail;
                }
                if (float d = Dist(mouse, h); d < bestD + 3.0f && d < 12.0f)
                {
                    bestD = d - 3.0f;
                    best = (int)b;
                    bestJoint = BoneHead;
                }
            }
            if (float d = SegmentDist(mouse, h, t); d < bestD)
            {
                bestD = d;
                best = (int)b;
                bestJoint = BoneBody;
            }
        }
        if (joint) *joint = bestJoint;
        return best;
    }

    // ---- Moving bones (G, R, S): joints in Bones, poses in Pose

    void MeshEditMode::BeginBoneModal(int kind, bool recorded)
    {
        bool any = false;
        glm::vec3 sum(0.0f);
        int count = 0;
        std::vector<glm::mat4> pose = rig::PoseMatrices(m_Mesh);
        for (size_t b = 0; b < m_Mesh.Bones.size(); ++b)
        {
            const Bone& bone = m_Mesh.Bones[b];
            if (m_RigMode == 1)
            {
                if (bone.Flags & (BoneHead | BoneBody)) { sum += bone.Head; ++count; }
                if (bone.Flags & (BoneTail | BoneBody)) { sum += bone.Tail; ++count; }
            }
            else if (bone.Flags & BoneBody)
            {
                sum += glm::vec3(pose[b][3]);
                ++count;
            }
            any |= (bone.Flags & (BoneHead | BoneTail | BoneBody)) != 0;
        }
        if (!any || count == 0) return;
        static const char* names[] = { "", "Move Bones", "Rotate Bones", "Scale Bones" };
        if (!recorded) Record(names[kind]);
        m_BoneModal = {};
        m_BoneModal.Kind = kind;
        m_BoneModal.Recorded = recorded;
        m_BoneModal.Start = m_Mesh.Bones;
        m_BoneModal.Pivot = sum / (float)count;
        m_BoneModal.MouseStart = ImGui::GetIO().MousePos;
        m_BoneModal.StartFrame = ImGui::GetFrameCount();
        glm::vec3 from, dir;
        SculptRay(m_BoneModal.MouseStart, from, dir);
        RayPlane(from, dir, m_BoneModal.Pivot, glm::normalize(m_BoneModal.Pivot - m_CameraLocal), m_BoneModal.PlaneStart);
        glm::vec4 c = m_ViewProj * m_World * glm::vec4(m_BoneModal.Pivot, 1.0f);
        ImVec2 ps(m_Origin.x + (c.x / c.w * 0.5f + 0.5f) * m_Size.x, m_Origin.y + (0.5f - c.y / c.w * 0.5f) * m_Size.y);
        m_BoneModal.LastAngle = std::atan2(m_BoneModal.MouseStart.y - ps.y, m_BoneModal.MouseStart.x - ps.x);
    }

    void MeshEditMode::UpdateBoneModal()
    {
        BoneModal& md = m_BoneModal;
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mouse = io.MousePos;
        for (int a = 0; a < 3; ++a)
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_X + a), false)) md.Axis = md.Axis == a ? -1 : a;
        glm::vec3 viewDir = glm::normalize(md.Pivot - m_CameraLocal);
        glm::vec4 c = m_ViewProj * m_World * glm::vec4(md.Pivot, 1.0f);
        ImVec2 ps(m_Origin.x + (c.x / c.w * 0.5f + 0.5f) * m_Size.x, m_Origin.y + (0.5f - c.y / c.w * 0.5f) * m_Size.y);
        m_Mesh.Bones = md.Start;
        std::vector<glm::mat4> startPose = rig::PoseMatrices(m_Mesh);
        // The change: a move (model space), a turn about the view axis (or the locked axis), or a scale.
        glm::vec3 move(0.0f);
        glm::quat turn(1, 0, 0, 0);
        float scale = 1.0f;
        if (md.Kind == 1)
        {
            glm::vec3 from, dir, hit;
            SculptRay(mouse, from, dir);
            if (RayPlane(from, dir, md.Pivot, viewDir, hit)) move = hit - md.PlaneStart;
            if (md.Axis >= 0)
            {
                float along = move[md.Axis];
                move = glm::vec3(0.0f);
                move[md.Axis] = along;
            }
            if (io.KeyCtrl) move = glm::round(move * 10.0f) / 10.0f;
        }
        else if (md.Kind == 2)
        {
            float a = std::atan2(mouse.y - ps.y, mouse.x - ps.x), d = a - md.LastAngle;
            while (d > glm::pi<float>()) d -= glm::two_pi<float>();
            while (d < -glm::pi<float>()) d += glm::two_pi<float>();
            md.Angle += d;
            md.LastAngle = a;
            float angle = io.KeyCtrl ? glm::radians(std::round(glm::degrees(md.Angle) / 5.0f) * 5.0f) : md.Angle;
            glm::vec3 axis = -viewDir;
            if (md.Axis >= 0)
            {
                axis = glm::vec3(0.0f);
                axis[md.Axis] = glm::dot(-viewDir, glm::vec3(md.Axis == 0, md.Axis == 1, md.Axis == 2)) < 0.0f ? -1.0f : 1.0f;
            }
            turn = glm::angleAxis(-angle, axis); // screen y runs down: clockwise on screen
        }
        else
        {
            scale = Dist(mouse, ps) / std::max(Dist(md.MouseStart, ps), 1.0f);
            if (io.KeyCtrl) scale = std::round(scale * 10.0f) / 10.0f;
        }
        auto transformPoint = [&](const glm::vec3& p) {
            if (md.Kind == 1) return p + move;
            if (md.Kind == 2) return md.Pivot + turn * (p - md.Pivot);
            glm::vec3 d = (p - md.Pivot) * scale;
            if (md.Axis >= 0)
                for (int a = 0; a < 3; ++a)
                    if (a != md.Axis) d[a] = (p - md.Pivot)[a];
            return md.Pivot + d;
        };
        for (size_t b = 0; b < m_Mesh.Bones.size(); ++b)
        {
            Bone& bone = m_Mesh.Bones[b];
            const Bone& start = md.Start[b];
            if (m_RigMode == 1)
            {
                if (start.Flags & (BoneHead | BoneBody)) bone.Head = transformPoint(start.Head);
                if (start.Flags & (BoneTail | BoneBody)) bone.Tail = transformPoint(start.Tail);
                continue;
            }
            if (!(start.Flags & BoneBody)) continue;
            // Pose: the change taken into the bone's own frame (the one its pose rotation and location act in).
            glm::mat4 frame = (start.Parent >= 0 && start.Parent < (int)startPose.size() ? startPose[(size_t)start.Parent] : glm::mat4(1.0f)) *
                              rig::RestLocal(m_Mesh, (int)b);
            glm::quat fr = RotationOf(frame);
            if (md.Kind == 1)
            {
                // Auto IK: a bone with parents isn't moved but reached for (its tail follows the mouse, its chain bends).
                if (m_AutoIK && start.Parent >= 0)
                {
                    float len = glm::length(start.Tail - start.Head);
                    glm::vec3 tail = glm::vec3(startPose[b] * glm::vec4(0, len, 0, 1));
                    rig::SolveIK(m_Mesh, (int)b, tail + move, std::max(m_AutoIKChain, 2));
                    continue;
                }
                bone.PoseLocation = start.PoseLocation + glm::inverse(fr) * move;
            }
            else if (md.Kind == 2) bone.PoseRotation = glm::normalize(glm::inverse(fr) * turn * fr * start.PoseRotation);
            else
            {
                glm::vec3 k(scale);
                if (md.Axis >= 0)
                {
                    k = glm::vec3(1.0f);
                    k[md.Axis] = scale;
                }
                bone.PoseScale = start.PoseScale * k;
            }
        }
        // In Bones: joints sitting on a moved joint (connected chains) move with it.
        if (m_RigMode == 1)
            for (size_t b = 0; b < m_Mesh.Bones.size(); ++b)
                for (size_t o = 0; o < m_Mesh.Bones.size(); ++o)
                {
                    if (o == b) continue;
                    const Bone& sb = md.Start[b];
                    if ((sb.Flags & (BoneTail | BoneBody)) && !(md.Start[o].Flags & (BoneHead | BoneBody)) && glm::length(md.Start[o].Head - sb.Tail) < 1e-5f)
                        m_Mesh.Bones[o].Head = m_Mesh.Bones[b].Tail;
                    if ((sb.Flags & (BoneHead | BoneBody)) && !(md.Start[o].Flags & (BoneTail | BoneBody)) && glm::length(md.Start[o].Tail - sb.Head) < 1e-5f)
                        m_Mesh.Bones[o].Tail = m_Mesh.Bones[b].Head;
                }
        if (m_RigMode == 2)
        {
            rig::ApplyConstraints(m_Mesh);
            SyncRigPreview();
        }
        bool later = ImGui::GetFrameCount() > md.StartFrame;
        if (later && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
            EndBoneModal(true);
        else if (later && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)))
            EndBoneModal(false);
    }

    void MeshEditMode::EndBoneModal(bool confirm)
    {
        if (!m_BoneModal.Kind) return;
        if (!confirm)
        {
            if (m_BoneModal.Recorded && !m_Undo.empty()) m_Mesh = std::move(m_Undo.back().Mesh); // (back to before the operation)
            else m_Mesh.Bones = m_BoneModal.Start;
            if (!m_Undo.empty()) m_Undo.pop_back();
            if (m_ActiveBone >= (int)m_Mesh.Bones.size()) m_ActiveBone = (int)m_Mesh.Bones.size() - 1;
        }
        else
            m_Dirty = true;
        m_BoneModal = {};
        SyncRigPreview();
    }

    // ---- Keys and mouse

    void MeshEditMode::HandleRigKeys(bool hovered)
    {
        using Shortcuts::Pressed;
        auto selected = [&]() {
            std::vector<int> out;
            for (size_t b = 0; b < m_Mesh.Bones.size(); ++b)
                if (m_Mesh.Bones[b].Flags & BoneBody) out.push_back((int)b);
            return out;
        };
        if (Pressed("rig.selectAll"))
            RigOperate("Select All Bones", [&] {
                for (Bone& b : m_Mesh.Bones) b.Flags = BoneHead | BoneTail | BoneBody;
            });
        if (Pressed("rig.deselectAll"))
            RigOperate("Select No Bones", [&] {
                for (Bone& b : m_Mesh.Bones) b.Flags = 0;
            });
        if (m_RigMode == 1 || m_RigMode == 2)
        {
            if (Pressed("rig.grab")) BeginBoneModal(1);
            if (Pressed("rig.rotate")) BeginBoneModal(2);
            if (Pressed("rig.scale")) BeginBoneModal(3);
        }
        if (m_RigMode == 1)
        {
            if (Pressed("rig.addBone"))
                RigOperate("Add Bone", [&] {
                    AABB bounds = m_Mesh.Bounds();
                    float size = m_Mesh.VertexCount() ? std::max(glm::length(bounds.Max - bounds.Min) * 0.25f, 0.05f) : 0.5f;
                    for (Bone& b : m_Mesh.Bones) b.Flags = 0;
                    m_ActiveBone = rig::AddBone(m_Mesh, "Bone", glm::vec3(0.0f), glm::vec3(0.0f, size, 0.0f));
                    m_Mesh.Bones[(size_t)m_ActiveBone].Flags = BoneHead | BoneTail | BoneBody;
                });
            if (Pressed("rig.extrude"))
            {
                // A new child bone from each selected tail, then moved with the mouse.
                bool any = false;
                for (const Bone& b : m_Mesh.Bones) any |= (b.Flags & (BoneTail | BoneBody)) != 0;
                if (any)
                {
                    Record("Extrude Bones");
                    size_t n = m_Mesh.Bones.size();
                    std::vector<int> made;
                    for (size_t b = 0; b < n; ++b)
                        if (m_Mesh.Bones[b].Flags & (BoneTail | BoneBody))
                            made.push_back(rig::AddBone(m_Mesh, m_Mesh.Bones[b].Name, m_Mesh.Bones[b].Tail, m_Mesh.Bones[b].Tail, (int)b));
                    for (Bone& b : m_Mesh.Bones) b.Flags = 0;
                    for (int b : made) m_Mesh.Bones[(size_t)b].Flags = BoneTail;
                    m_ActiveBone = made.back();
                    BeginBoneModal(1, true); // (cancelling takes the new bones away too)
                }
            }
            if (Pressed("rig.delete"))
            {
                std::vector<int> gone;
                for (size_t b = 0; b < m_Mesh.Bones.size(); ++b)
                    if ((m_Mesh.Bones[b].Flags & BoneBody) || (m_Mesh.Bones[b].Flags & (BoneHead | BoneTail)) == (BoneHead | BoneTail)) gone.push_back((int)b);
                if (!gone.empty())
                    RigOperate("Delete Bones", [&] {
                        rig::DeleteBones(m_Mesh, gone);
                        m_ActiveBone = m_Mesh.Bones.empty() ? -1 : std::min(m_ActiveBone, (int)m_Mesh.Bones.size() - 1);
                    });
            }
            if (Pressed("rig.parent") && m_ActiveBone >= 0)
                RigOperate("Parent Bones", [&] {
                    for (int b : selected())
                        if (b != m_ActiveBone && !rig::WouldLoop(m_Mesh, b, m_ActiveBone)) m_Mesh.Bones[(size_t)b].Parent = m_ActiveBone;
                });
            if (Pressed("rig.clearParent"))
                RigOperate("Clear Parent", [&] {
                    for (int b : selected()) m_Mesh.Bones[(size_t)b].Parent = -1;
                });
        }
        if (m_RigMode == 2)
        {
            auto clear = [&](const char* name, int what) {
                RigOperate(name, [&] {
                    for (int b : selected())
                    {
                        Bone& bone = m_Mesh.Bones[(size_t)b];
                        if (what == 0) bone.PoseRotation = glm::quat(1, 0, 0, 0);
                        if (what == 1) bone.PoseLocation = glm::vec3(0.0f);
                        if (what == 2) bone.PoseScale = glm::vec3(1.0f);
                    }
                });
            };
            if (Pressed("rig.clearRotation")) clear("Clear Rotation", 0);
            if (Pressed("rig.clearLocation")) clear("Clear Location", 1);
            if (Pressed("rig.clearScale")) clear("Clear Scale", 2);
            if (Pressed("rig.insertKey"))
                RigOperate("Insert Keyframe", [&] {
                    if (m_Action < 0 || m_Action >= (int)m_Mesh.Actions.size()) m_Action = rig::AddAction(m_Mesh, "Action");
                    rig::InsertKey(m_Mesh, m_Action, m_ActionTime, m_KeySelectedOnly, m_KeyShapes);
                    m_SelectedKey = m_ActionTime;
                    Log::Info("Keyed the pose at {:.2f} s in '{}'", m_ActionTime, m_Mesh.Actions[(size_t)m_Action].Name);
                });
        }
        if (m_RigMode == 3)
        {
            if (Pressed("paint.radius") || Pressed("paint.smaller")) m_WeightRadiusPx = std::max(2.0f, m_WeightRadiusPx / 1.15f);
            if (Pressed("paint.larger")) m_WeightRadiusPx = std::min(2000.0f, m_WeightRadiusPx * 1.15f);
        }
        if (Pressed("rig.autoWeights")) RigOperate("Automatic Weights", [&] { rig::AutomaticWeights(m_Mesh); });
        if (Pressed("rig.envelopeWeights"))
            RigOperate("Envelope Weights", [&] { Log::Info("Envelope Weights: {} vertices weighted", rig::EnvelopeWeights(m_Mesh)); });
        if (Pressed("rig.boneMode")) SetRigMode(1);
        if (Pressed("rig.poseMode")) SetRigMode(2);
        if (Pressed("rig.weightMode")) SetRigMode(3);
        (void)hovered;
    }

    void MeshEditMode::HandleRigMouse(bool hovered, bool blocked)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mouse = io.MousePos;
        bool overToolbar = mouse.x >= m_ToolbarMin.x && mouse.x <= m_ToolbarMax.x && mouse.y >= m_ToolbarMin.y && mouse.y <= m_ToolbarMax.y;
        if (!(hovered && !blocked && !m_OverUI && !overToolbar && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) return;
        if (m_RigMode == 3 && !io.KeyCtrl) return; // (painting; Ctrl+click picks the bone)
        int joint = BoneBody;
        int b = PickBone(mouse, &joint);
        Record("Select Bone");
        if (!io.KeyShift)
            for (Bone& bone : m_Mesh.Bones) bone.Flags = 0;
        if (b >= 0)
        {
            Bone& bone = m_Mesh.Bones[(size_t)b];
            uint8_t bits = joint == BoneBody ? (uint8_t)(BoneHead | BoneTail | BoneBody) : (uint8_t)joint;
            if (io.KeyShift && (bone.Flags & bits) == bits && m_ActiveBone == b) bone.Flags &= (uint8_t)~bits;
            else bone.Flags |= bits;
            m_ActiveBone = b;
        }
        else if (m_Undo.size() && io.KeyShift)
            m_Undo.pop_back(); // (nothing changed)
        m_Dirty = true;
        if (m_RigMode == 3) SyncRigPreview();
    }

    void MeshEditMode::UpdateWeightStroke(bool hovered, bool blocked)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mouse = io.MousePos;
        bool overToolbar = mouse.x >= m_ToolbarMin.x && mouse.x <= m_ToolbarMax.x && mouse.y >= m_ToolbarMin.y && mouse.y <= m_ToolbarMax.y;
        auto dab = [&](const ImVec2& at) {
            glm::vec3 point, dir;
            uint32_t face;
            if (!PaintHit(at, point, face, dir)) return;
            rig::WeightBrush b;
            b.Bone = m_ActiveBone;
            b.Mode = io.KeyShift ? rig::WeightMode::Smooth : m_WeightMode;
            b.Value = m_WeightValue;
            b.Strength = m_WeightStrength;
            b.Radius = m_WeightRadiusPx * ModelPerPixel(point);
            b.FrontFacesOnly = m_WeightFrontFaces;
            b.ViewDir = dir;
            b.Symmetry = m_WeightSymmetry;
            if (rig::PaintWeights(m_Mesh, point, b) > 0) SyncRigPreview();
        };
        if (!m_WeightPainting)
        {
            if (!(hovered && !blocked && !m_OverUI && !overToolbar && !io.KeyAlt && !io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) return;
            if (m_ActiveBone < 0 || m_ActiveBone >= (int)m_Mesh.Bones.size())
            {
                Log::Warn("Weight Paint: add a bone first (Bones), then pick it here");
                return;
            }
            Record("Weight Paint");
            m_WeightPainting = true;
            m_WeightMouse = mouse;
            dab(mouse);
            return;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_WeightPainting = false;
            m_Dirty = true;
            return;
        }
        float spacing = std::max(2.0f, m_WeightRadiusPx * 0.2f);
        int steps = std::min((int)(Dist(mouse, m_WeightMouse) / spacing), 8);
        for (int i = 1; i <= steps; ++i)
        {
            float k = (float)i / (float)steps;
            dab(ImVec2(m_WeightMouse.x + (mouse.x - m_WeightMouse.x) * k, m_WeightMouse.y + (mouse.y - m_WeightMouse.y) * k));
        }
        if (steps > 0) m_WeightMouse = mouse;
    }

    // ---- The view

    bool MeshEditMode::UpdateRigView(bool hovered, bool focused, bool blocked)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Playing an action: the pose follows the time.
        if (m_RigMode == 2 && m_ActionPlaying && m_Action >= 0 && m_Action < (int)m_Mesh.Actions.size())
        {
            float length = m_Mesh.Actions[(size_t)m_Action].Duration();
            m_ActionTime += ImGui::GetIO().DeltaTime;
            if (length > 0.0f) m_ActionTime = std::fmod(m_ActionTime, length);
            rig::EvaluateAction(m_Mesh, m_Action, m_ActionTime);
            rig::ApplyConstraints(m_Mesh);
            SyncRigPreview();
        }
        DrawBoneOverlay(dl);
        if (m_RigMode == 2 && m_ShowGraph) DrawGraphEditor(dl);
        else if (m_RigMode == 2 && m_ShowDopeSheet) DrawDopeSheet(dl);
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        DrawRigToolbar(m_Origin);
        ImGui::SetCursorScreenPos(cursor);
        m_OverUI = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        if (m_BoneModal.Kind) UpdateBoneModal();
        else
        {
            if ((hovered || focused) && !m_WeightPainting && !ImGui::GetIO().WantTextInput) HandleRigKeys(hovered);
            if (m_RigMode && !m_BoneModal.Kind)
            {
                HandleRigMouse(hovered, blocked);
                if (m_RigMode == 3) UpdateWeightStroke(hovered, blocked);
            }
        }
        if (m_RigMode == 3 && (hovered || m_WeightPainting) && !m_OverUI)
            dl->AddCircle(ImGui::GetIO().MousePos, m_WeightRadiusPx, IM_COL32(255, 255, 255, 200), 64, 1.5f);
        std::string status;
        char buf[256];
        if (m_RigMode == 1)
            std::snprintf(buf, sizeof(buf), "Bones: %zu   click a joint or bone (Shift adds)   %s add   %s extrude   G/R/S move   %s parent to active   %s delete",
                          m_Mesh.Bones.size(), Shortcuts::Label("rig.addBone").c_str(), Shortcuts::Label("rig.extrude").c_str(),
                          Shortcuts::Label("rig.parent").c_str(), Shortcuts::Label("rig.delete").c_str());
        else if (m_RigMode == 2)
            std::snprintf(buf, sizeof(buf), "Pose: click a bone (Shift adds)   R rotate, G move, S scale (X/Y/Z axis)   Alt+R/G/S clear   %s key the pose at %.2f s",
                          Shortcuts::Label("rig.insertKey").c_str(), m_ActionTime);
        else
            std::snprintf(buf, sizeof(buf), "Weights: %s   paint (Shift smooths)   Ctrl+click a bone to paint it   blue 0 .. red 1",
                          m_ActiveBone >= 0 && m_ActiveBone < (int)m_Mesh.Bones.size() ? m_Mesh.Bones[(size_t)m_ActiveBone].Name.c_str() : "(no bone)");
        status = buf;
        if (m_BoneModal.Kind)
        {
            static const char* names[] = { "", "Move", "Rotate", "Scale" };
            status = std::string(names[m_BoneModal.Kind]) + (m_BoneModal.Axis >= 0 ? std::string("  along ") + "XYZ"[m_BoneModal.Axis] : "") +
                     "   X/Y/Z axis, Ctrl snap; click or Enter: done, right-click or Esc: cancel";
        }
        dl->AddText(ImVec2(m_Origin.x + 8.0f, m_Origin.y + m_Size.y - 24.0f), IM_COL32(255, 255, 255, 230), status.c_str());
        return m_BoneModal.Kind != 0 || m_WeightPainting;
    }

    void MeshEditMode::DrawRigToolbar(const ImVec2& origin)
    {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, origin.y + 28.0f));
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
        auto toggle = [](const char* label, bool on, const char* tip) {
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            bool pressed = ImGui::Button(label);
            if (on) ImGui::PopStyleColor();
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            return pressed;
        };
        if (toggle("Edit", false, "Back to Edit Mode")) SetRigMode(0);
        static const char* tips[] = { "", "The armature: add, extrude, move and parent bones", "Pose the bones to try the rig, and key actions",
                                      "Paint how much each bone moves the vertices" };
        for (int mode = 1; mode <= 3; ++mode)
        {
            ImGui::SameLine(0.0f, 2.0f);
            if (toggle(kRigModeNames[mode], m_RigMode == mode, tips[mode])) SetRigMode(mode);
        }
        ImGui::SameLine();
        if (ImGui::Button("Shape Keys...")) ImGui::OpenPopup("ShapeKeys");
        DrawShapeKeysPopup();
        ImGui::SameLine();
        if (ImGui::Button("Done")) Exit();

        // Second row.
        if (m_RigMode == 1)
        {
            if (ImGui::Button("Add Bone")) Shortcuts::Press("rig.addBone");
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::Button("Delete")) Shortcuts::Press("rig.delete");
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::Button("Symmetrize"))
                RigOperate("Symmetrize Bones", [&] { Log::Info("Symmetrize: {} bones made", rig::Symmetrize(m_Mesh)); });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bones on +X named .L/_L/Left... mirrored to the other side");
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::Button("Automatic Weights")) Shortcuts::Press("rig.autoWeights");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Weights from each vertex's distance to the bones it can see (replaces the weights)");
            if (m_ActiveBone >= 0 && m_ActiveBone < (int)m_Mesh.Bones.size())
            {
                Bone& bone = m_Mesh.Bones[(size_t)m_ActiveBone];
                ImGui::SameLine();
                char name[128];
                std::snprintf(name, sizeof(name), "%s", bone.Name.c_str());
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::InputText("##bonename", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) && name[0])
                    RigOperate("Rename Bone", [&] { m_Mesh.Bones[(size_t)m_ActiveBone].Name = rig::FindBone(m_Mesh, name) < 0 ? std::string(name) : rig::UniqueBoneName(m_Mesh, name); });
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("The active bone's name (Enter to rename; .L/.R sides mirror)");
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::SetNextItemWidth(120.0f);
                std::string parent = bone.Parent >= 0 && bone.Parent < (int)m_Mesh.Bones.size() ? m_Mesh.Bones[(size_t)bone.Parent].Name : "(no parent)";
                if (ImGui::BeginCombo("##boneparent", parent.c_str()))
                {
                    if (ImGui::Selectable("(no parent)", bone.Parent < 0)) RigOperate("Clear Parent", [&] { m_Mesh.Bones[(size_t)m_ActiveBone].Parent = -1; });
                    for (size_t p = 0; p < m_Mesh.Bones.size(); ++p)
                    {
                        if ((int)p == m_ActiveBone || rig::WouldLoop(m_Mesh, m_ActiveBone, (int)p)) continue;
                        if (ImGui::Selectable((m_Mesh.Bones[p].Name + "##p" + std::to_string(p)).c_str(), bone.Parent == (int)p))
                            RigOperate("Parent Bone", [&] { m_Mesh.Bones[(size_t)m_ActiveBone].Parent = (int)p; });
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::SetNextItemWidth(80.0f);
                float roll = glm::degrees(bone.Roll);
                if (ImGui::DragFloat("##roll", &roll, 0.5f, -180.0f, 180.0f, "roll %.0f"))
                {
                    if (ImGui::IsItemActivated()) Record("Bone Roll");
                    m_Mesh.Bones[(size_t)m_ActiveBone].Roll = glm::radians(roll);
                    m_Dirty = true;
                }
                ImGui::SameLine(0.0f, 2.0f);
                bool deform = bone.Deform;
                if (ImGui::Checkbox("Deform", &deform)) RigOperate("Bone Deform", [&] { m_Mesh.Bones[(size_t)m_ActiveBone].Deform = deform; });
                // An IK constraint: this bone's tail reaching for another bone (a target to move in Pose).
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                std::string ik = bone.IKTarget >= 0 && bone.IKTarget < (int)m_Mesh.Bones.size() ? "IK: " + m_Mesh.Bones[(size_t)bone.IKTarget].Name : "No IK";
                if (ImGui::BeginCombo("##ik", ik.c_str()))
                {
                    if (ImGui::Selectable("No IK", bone.IKTarget < 0)) RigOperate("IK Constraint", [&] { m_Mesh.Bones[(size_t)m_ActiveBone].IKTarget = -1; });
                    for (size_t t = 0; t < m_Mesh.Bones.size(); ++t)
                    {
                        if ((int)t == m_ActiveBone) continue;
                        if (ImGui::Selectable((m_Mesh.Bones[t].Name + "##ik" + std::to_string(t)).c_str(), bone.IKTarget == (int)t))
                            RigOperate("IK Constraint", [&] { m_Mesh.Bones[(size_t)m_ActiveBone].IKTarget = (int)t; });
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("IK constraint: in Pose this bone's tail reaches for the target bone's head (move the target)");
                if (bone.IKTarget >= 0)
                {
                    ImGui::SameLine(0.0f, 2.0f);
                    ImGui::SetNextItemWidth(70.0f);
                    int chain = bone.IKChain;
                    if (ImGui::SliderInt("##chain", &chain, 1, 8, "chain %d")) RigOperate("IK Chain", [&] { m_Mesh.Bones[(size_t)m_ActiveBone].IKChain = chain; });
                }
                // Third row: envelope weights, then the bone's B-bone curve and envelope (live edits: one undo step per drag).
                if (ImGui::Button("Envelope Weights")) Shortcuts::Press("rig.envelopeWeights");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Weights from the bones' envelopes (the vertices inside them; others keep theirs). Show Envelopes to see them.");
                ImGui::SameLine(0.0f, 2.0f);
                if (toggle("Envelopes", m_ShowEnvelopes, "Draws each bone's envelope: full weight inside, fading to nothing at the outer line"))
                    m_ShowEnvelopes = !m_ShowEnvelopes;
                auto liveFloat = [&](const char* id, float* value, float speed, float lo, float hi, const char* format, const char* undo,
                                     const char* tip) {
                    ImGui::SetNextItemWidth(84.0f);
                    if (ImGui::DragFloat(id, value, speed, lo, hi, format))
                    {
                        if (ImGui::IsItemActivated()) Record(undo);
                        m_Dirty = true;
                        SyncRigPreview();
                    }
                    else if (ImGui::IsItemActivated())
                        Record(undo);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                };
                Bone& active = m_Mesh.Bones[(size_t)m_ActiveBone];
                ImGui::SameLine();
                ImGui::SetNextItemWidth(96.0f);
                int segments = active.Segments;
                if (ImGui::SliderInt("##segments", &segments, 1, 16, segments > 1 ? "B-bone %d" : "B-bone off"))
                    RigOperate("B-Bone Segments", [&] { m_Mesh.Bones[(size_t)m_ActiveBone].Segments = segments; });
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Segments: more than one makes a bendy bone, curving towards the bones joined to its ends "
                                      "(its parent and a child at its tail) and twisting along to meet the child");
                if (active.Segments > 1)
                {
                    ImGui::SameLine(0.0f, 2.0f);
                    liveFloat("##easein", &active.EaseIn, 0.01f, 0.0f, 5.0f, "ease in %.2f", "B-Bone Ease",
                              "How far the curve follows the parent's direction from the head");
                    ImGui::SameLine(0.0f, 2.0f);
                    liveFloat("##easeout", &active.EaseOut, 0.01f, 0.0f, 5.0f, "ease out %.2f", "B-Bone Ease",
                              "How far the curve follows the child's direction into the tail");
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Envelope");
                ImGui::SameLine(0.0f, 4.0f);
                liveFloat("##headr", &active.HeadRadius, 0.002f, 0.0f, 100.0f, "head %.3f", "Envelope", "Full weight within this of the head");
                ImGui::SameLine(0.0f, 2.0f);
                liveFloat("##tailr", &active.TailRadius, 0.002f, 0.0f, 100.0f, "tail %.3f", "Envelope", "Full weight within this of the tail");
                ImGui::SameLine(0.0f, 2.0f);
                liveFloat("##envdist", &active.EnvelopeDistance, 0.002f, 0.0f, 100.0f, "fade %.3f", "Envelope",
                          "Beyond the radius the weight fades out over this distance");
                ImGui::SameLine(0.0f, 2.0f);
                liveFloat("##envweight", &active.EnvelopeWeight, 0.01f, 0.0f, 10.0f, "weight %.2f", "Envelope",
                          "The envelope's strength against other bones' where they overlap");
            }
        }
        if (m_RigMode == 1 && (m_ActiveBone < 0 || m_ActiveBone >= (int)m_Mesh.Bones.size()))
        {
            if (ImGui::Button("Envelope Weights")) Shortcuts::Press("rig.envelopeWeights");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Weights from the bones' envelopes (the vertices inside them; others keep theirs). Show Envelopes to see them.");
            ImGui::SameLine(0.0f, 2.0f);
            if (toggle("Envelopes", m_ShowEnvelopes, "Draws each bone's envelope: full weight inside, fading to nothing at the outer line"))
                m_ShowEnvelopes = !m_ShowEnvelopes;
        }
        else if (m_RigMode == 2)
        {
            ImGui::SetNextItemWidth(110.0f);
            std::string current = m_Action >= 0 && m_Action < (int)m_Mesh.Actions.size() ? m_Mesh.Actions[(size_t)m_Action].Name : "(no action)";
            if (ImGui::BeginCombo("##action", current.c_str()))
            {
                for (size_t a = 0; a < m_Mesh.Actions.size(); ++a)
                    if (ImGui::Selectable((m_Mesh.Actions[a].Name + "##a" + std::to_string(a)).c_str(), (int)a == m_Action)) m_Action = (int)a;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Actions are the model's clips: an Animator plays them by name");
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::Button("New Action")) RigOperate("New Action", [&] { m_Action = rig::AddAction(m_Mesh, "Action"); });
            if (m_Action >= 0 && m_Action < (int)m_Mesh.Actions.size())
            {
                Action& action = m_Mesh.Actions[(size_t)m_Action];
                ImGui::SameLine(0.0f, 2.0f);
                char name[128];
                std::snprintf(name, sizeof(name), "%s", action.Name.c_str());
                ImGui::SetNextItemWidth(100.0f);
                if (ImGui::InputText("##actionname", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) && name[0])
                    RigOperate("Rename Action", [&] { m_Mesh.Actions[(size_t)m_Action].Name = name; });
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160.0f);
                float end = std::max(action.Duration() + 1.0f, 1.0f);
                if (ImGui::SliderFloat("##time", &m_ActionTime, 0.0f, end, "%.2f s"))
                {
                    m_ActionPlaying = false;
                    rig::EvaluateAction(m_Mesh, m_Action, m_ActionTime);
                    rig::ApplyConstraints(m_Mesh);
                    SyncRigPreview();
                }
                ImGui::SameLine(0.0f, 2.0f);
                if (ImGui::Button("Key")) Shortcuts::Press("rig.insertKey");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keys every bone's pose at this time (%s)", Shortcuts::Label("rig.insertKey").c_str());
                ImGui::SameLine(0.0f, 2.0f);
                if (ImGui::Button("Delete Key"))
                    RigOperate("Delete Keyframe", [&] { rig::DeleteKey(m_Mesh, m_Action, m_ActionTime); });
                ImGui::SameLine(0.0f, 2.0f);
                if (toggle(m_ActionPlaying ? "Stop" : "Play", m_ActionPlaying, "Plays the action in a loop")) m_ActionPlaying = !m_ActionPlaying;
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::TextDisabled("%zu keys", action.Keys.size());
            }
            // Second line of options.
            ImGui::Checkbox("Selected Only", &m_KeySelectedOnly);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Key only the selected bones (else every bone)");
            ImGui::SameLine();
            ImGui::Checkbox("Key Shapes", &m_KeyShapes);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Key the shape keys' values too (they play as blend shape weights)");
            ImGui::SameLine();
            bool autoIK = m_AutoIK;
            if (ImGui::Checkbox("Auto IK", &autoIK)) m_AutoIK = autoIK;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("G on a bone with parents bends its chain so its tail follows the mouse");
            if (m_AutoIK)
            {
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::SetNextItemWidth(70.0f);
                ImGui::SliderInt("##ikchain", &m_AutoIKChain, 2, 8, "chain %d");
            }
            ImGui::SameLine();
            ImGui::Checkbox("Dope Sheet", &m_ShowDopeSheet);
            ImGui::SameLine();
            ImGui::Checkbox("Graph", &m_ShowGraph);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The Graph Editor: the selected bones' location, rotation and scale as curves (drag keys and handles; "
                                  "%s interpolation, %s handle type)",
                                  Shortcuts::Label("rig.interpolation").c_str(), Shortcuts::Label("rig.handleType").c_str());
            ImGui::SameLine();
            if (ImGui::Button("Rest Pose"))
                RigOperate("Clear Pose", [&] {
                    m_ActionPlaying = false;
                    rig::ClearPose(m_Mesh);
                });
        }
        else if (m_RigMode == 3)
        {
            ImGui::SetNextItemWidth(120.0f);
            std::string current = m_ActiveBone >= 0 && m_ActiveBone < (int)m_Mesh.Bones.size() ? m_Mesh.Bones[(size_t)m_ActiveBone].Name : "(no bone)";
            if (ImGui::BeginCombo("##wbone", current.c_str()))
            {
                for (size_t b = 0; b < m_Mesh.Bones.size(); ++b)
                    if (ImGui::Selectable((m_Mesh.Bones[b].Name + "##w" + std::to_string(b)).c_str(), (int)b == m_ActiveBone))
                    {
                        m_ActiveBone = (int)b;
                        SyncRigPreview();
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine(0.0f, 2.0f);
            const char* modes[] = { "Add", "Subtract", "Mix", "Smooth" };
            int mode = (int)m_WeightMode;
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::Combo("##wmode", &mode, modes, 4)) m_WeightMode = (rig::WeightMode)mode;
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::SetNextItemWidth(80.0f);
            ImGui::SliderFloat("##wvalue", &m_WeightValue, 0.0f, 1.0f, "weight %.2f");
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::SetNextItemWidth(70.0f);
            ImGui::DragFloat("##wradius", &m_WeightRadiusPx, 0.5f, 2.0f, 2000.0f, "%.0f px");
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::SetNextItemWidth(70.0f);
            ImGui::SliderFloat("##wstrength", &m_WeightStrength, 0.0f, 1.0f, "%.2f");
            ImGui::SameLine();
            if (toggle("Front Faces", m_WeightFrontFaces, "Only vertices facing the camera")) m_WeightFrontFaces = !m_WeightFrontFaces;
            ImGui::SameLine(0.0f, 2.0f);
            if (toggle("Mirror X", m_WeightSymmetry & 1, "Strokes mirrored across X paint the mirrored bone (.L/.R)")) m_WeightSymmetry ^= 1;
            ImGui::SameLine();
            if (ImGui::Button("Normalize All")) RigOperate("Normalize Weights", [&] { rig::NormalizeWeights(m_Mesh); });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each vertex's four strongest bones, adding up to one (as the renderer uses them)");
        }
        ImGui::PopStyleVar();
        ImGui::EndGroup();
        m_ToolbarMin = ImGui::GetItemRectMin();
        m_ToolbarMax = ImGui::GetItemRectMax();
    }

    // ---- The dope sheet: the action's keys on a timeline (drag to move, Delete to remove, click the ruler to scrub)

    void MeshEditMode::DrawDopeSheet(ImDrawList* dl)
    {
        if (m_Action < 0 || m_Action >= (int)m_Mesh.Actions.size()) return;
        Action& action = m_Mesh.Actions[(size_t)m_Action];
        // Rows: all keys, then the selected bones (up to 6), then the shape keys keyed anywhere.
        std::vector<std::pair<std::string, int>> rows = { { "Keys", -1 } }; // (label, bone >= 0 or -2 - shape index)
        for (size_t b = 0; b < m_Mesh.Bones.size() && rows.size() < 7; ++b)
            if (m_Mesh.Bones[b].Flags & BoneBody) rows.push_back({ m_Mesh.Bones[b].Name, (int)b });
        for (size_t s = 0; s < m_Mesh.ShapeKeys.size(); ++s)
            for (const ActionKey& k : action.Keys)
                if (s < k.Shapes.size())
                {
                    rows.push_back({ m_Mesh.ShapeKeys[s].Name, -2 - (int)s });
                    break;
                }
        const float rowH = 16.0f, labelW = 110.0f;
        float height = 18.0f + rowH * (float)rows.size();
        ImVec2 mn(m_Origin.x + 8.0f, m_Origin.y + m_Size.y - 32.0f - height), mx(m_Origin.x + m_Size.x - 8.0f, m_Origin.y + m_Size.y - 32.0f);
        float end = std::max(action.Duration() + 0.5f, 1.0f);
        float x0 = mn.x + labelW, x1 = mx.x - 8.0f;
        auto toX = [&](float t) { return x0 + (x1 - x0) * t / end; };
        auto toTime = [&](float x) { return std::clamp((x - x0) / (x1 - x0) * end, 0.0f, end); };
        dl->AddRectFilled(mn, mx, IM_COL32(25, 25, 28, 220), 4.0f);
        // The ruler.
        for (float t = 0.0f; t <= end + 1e-4f; t += end > 4.0f ? 1.0f : 0.25f)
        {
            float x = toX(t);
            dl->AddLine(ImVec2(x, mn.y + 2.0f), ImVec2(x, mx.y - 2.0f), IM_COL32(255, 255, 255, 25));
            char label[16];
            std::snprintf(label, sizeof(label), "%.2g", t);
            dl->AddText(ImVec2(x + 2.0f, mn.y + 1.0f), IM_COL32(200, 200, 200, 160), label);
        }
        // The input area (so clicks here don't reach the view).
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(mn);
        ImGui::InvisibleButton("##dopesheet", ImVec2(mx.x - mn.x, mx.y - mn.y));
        bool hovered = ImGui::IsItemHovered();
        ImGui::SetCursorScreenPos(cursor);
        ImVec2 mouse = ImGui::GetIO().MousePos;
        int hitKey = -1;
        for (size_t r = 0; r < rows.size(); ++r)
        {
            float y = mn.y + 18.0f + rowH * (float)r + rowH * 0.5f;
            dl->AddText(ImVec2(mn.x + 6.0f, y - 7.0f), IM_COL32(220, 220, 220, 220), rows[r].first.c_str());
            for (size_t k = 0; k < action.Keys.size(); ++k)
            {
                const ActionKey& key = action.Keys[k];
                int row = rows[r].second;
                bool has = row == -1 ? true : row >= 0 ? key.HasBone((size_t)row) : (size_t)(-2 - row) < key.Shapes.size();
                if (!has) continue;
                float x = key.Time == m_DragKeyFrom && m_DragKeyFrom >= 0.0f ? toX(toTime(mouse.x)) : toX(key.Time);
                bool sel = std::abs(key.Time - m_SelectedKey) < 1e-4f;
                ImVec2 c(x, y);
                dl->AddQuadFilled(ImVec2(c.x, c.y - 5), ImVec2(c.x + 5, c.y), ImVec2(c.x, c.y + 5), ImVec2(c.x - 5, c.y), sel ? IM_COL32(255, 180, 60, 255) : IM_COL32(220, 220, 220, 230));
                if (hovered && std::abs(mouse.x - c.x) < 6.0f && std::abs(mouse.y - c.y) < 7.0f) hitKey = (int)k;
            }
        }
        float px = toX(m_ActionTime);
        dl->AddLine(ImVec2(px, mn.y), ImVec2(px, mx.y), IM_COL32(80, 160, 255, 255), 2.0f);
        // Clicking a key selects it (and starts dragging it); the ruler or an empty spot moves the playhead.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (hitKey >= 0)
            {
                m_SelectedKey = action.Keys[(size_t)hitKey].Time;
                m_DragKeyFrom = m_SelectedKey;
            }
            else
            {
                m_ActionPlaying = false;
                m_ActionTime = toTime(mouse.x);
                rig::EvaluateAction(m_Mesh, m_Action, m_ActionTime);
                rig::ApplyConstraints(m_Mesh);
                SyncRigPreview();
            }
        }
        if (m_DragKeyFrom >= 0.0f && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            float to = std::round(toTime(mouse.x) * 100.0f) / 100.0f; // (hundredths of a second)
            if (std::abs(to - m_DragKeyFrom) > 1e-4f)
            {
                Record("Move Keyframe");
                rig::MoveKey(m_Mesh, m_Action, m_DragKeyFrom, to);
                m_SelectedKey = to;
                m_Dirty = true;
            }
            m_DragKeyFrom = -1.0f;
        }
        if (hovered && m_SelectedKey >= 0.0f && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_X)))
        {
            RigOperate("Delete Keyframe", [&] { rig::DeleteKey(m_Mesh, m_Action, m_SelectedKey); });
            m_SelectedKey = -1.0f;
        }
    }

    // ---- The Graph Editor: curves of the selected bones' channels (drag key points and handles, click to scrub)

    void MeshEditMode::DrawGraphEditor(ImDrawList* dl)
    {
        if (m_Action < 0 || m_Action >= (int)m_Mesh.Actions.size()) return;
        Action& action = m_Mesh.Actions[(size_t)m_Action];
        static const ImU32 kAxis[3] = { IM_COL32(240, 90, 90, 255), IM_COL32(110, 210, 90, 255), IM_COL32(90, 150, 250, 255) };
        // Channels: each selected bone (up to 4) that the action keys, its nine channels.
        struct Row
        {
            int Bone, Channel;
        };
        std::vector<Row> rows;
        std::vector<int> bones;
        for (size_t b = 0; b < m_Mesh.Bones.size() && bones.size() < 4; ++b)
        {
            if (!(m_Mesh.Bones[b].Flags & BoneBody)) continue;
            bool keyed = false;
            for (const ActionKey& k : action.Keys) keyed |= k.HasBone(b);
            if (!keyed) continue;
            bones.push_back((int)b);
            for (int c = 0; c < kBoneChannels; ++c) rows.push_back({ (int)b, c });
        }
        auto hidden = [&](int b, int c) {
            return std::find(m_GraphHidden.begin(), m_GraphHidden.end(), ((uint32_t)b << 8) | (uint32_t)c) != m_GraphHidden.end();
        };
        // Rotations show in degrees.
        auto shown = [](int c, float v) { return c >= 3 && c < 6 ? glm::degrees(v) : v; };
        auto stored = [](int c, float v) { return c >= 3 && c < 6 ? glm::radians(v) : v; };

        float height = std::clamp(m_Size.y * 0.42f, 160.0f, 420.0f);
        ImVec2 mn(m_Origin.x + 8.0f, m_Origin.y + m_Size.y - 32.0f - height), mx(m_Origin.x + m_Size.x - 8.0f, m_Origin.y + m_Size.y - 32.0f);
        const float listW = 150.0f;
        float end = std::max(action.Duration() + 0.5f, 1.0f);
        float x0 = mn.x + listW + 10.0f, x1 = mx.x - 10.0f, y0 = mn.y + 20.0f, y1 = mx.y - 8.0f;
        auto toX = [&](float t) { return x0 + (x1 - x0) * t / end; };
        auto toTime = [&](float x) { return std::clamp((x - x0) / (x1 - x0) * end, 0.0f, end); };
        dl->AddRectFilled(mn, mx, IM_COL32(25, 25, 28, 225), 4.0f);
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(mn);
        ImGui::SetNextItemAllowOverlap(); // (the value field drawn over it stays usable)
        ImGui::InvisibleButton("##graph", ImVec2(mx.x - mn.x, mx.y - mn.y));
        bool hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
        ImGui::SetCursorScreenPos(cursor);
        ImVec2 mouse = ImGui::GetIO().MousePos;
        if (rows.empty())
        {
            dl->AddText(ImVec2(mn.x + 10.0f, mn.y + 10.0f), IM_COL32(200, 200, 200, 200), "Select keyed bones to see their curves");
            return;
        }

        // The value range: fitted to the visible curves (held while dragging, so the view doesn't run away).
        const int samples = std::max(16, (int)((x1 - x0) / 4.0f));
        std::vector<std::vector<float>> curves(rows.size());
        float lo = 1e30f, hi = -1e30f;
        for (size_t r = 0; r < rows.size(); ++r)
        {
            if (hidden(rows[r].Bone, rows[r].Channel)) continue;
            curves[r].resize((size_t)samples + 1);
            for (int i = 0; i <= samples; ++i)
            {
                float v = shown(rows[r].Channel, rig::ChannelValue(action, (size_t)rows[r].Bone, rows[r].Channel, end * (float)i / (float)samples));
                curves[r][(size_t)i] = v;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        if (!m_GraphDrag)
        {
            if (lo > hi) lo = -1.0f, hi = 1.0f;
            float pad = std::max((hi - lo) * 0.1f, 0.05f);
            m_GraphLo = lo - pad;
            m_GraphHi = hi + pad;
        }
        auto toY = [&](float v) { return y1 - (y1 - y0) * (v - m_GraphLo) / std::max(m_GraphHi - m_GraphLo, 1e-6f); };
        auto toValue = [&](float y) { return m_GraphLo + (y1 - y) / (y1 - y0) * (m_GraphHi - m_GraphLo); };

        // Grid: time across, values up.
        for (float t = 0.0f; t <= end + 1e-4f; t += end > 4.0f ? 1.0f : 0.25f)
        {
            float x = toX(t);
            dl->AddLine(ImVec2(x, mn.y + 2.0f), ImVec2(x, y1), IM_COL32(255, 255, 255, 22));
            char label[16];
            std::snprintf(label, sizeof(label), "%.2g", t);
            dl->AddText(ImVec2(x + 2.0f, mn.y + 2.0f), IM_COL32(200, 200, 200, 150), label);
        }
        float range = m_GraphHi - m_GraphLo, step = std::pow(10.0f, std::floor(std::log10(std::max(range, 1e-6f) / 4.0f)));
        if (range / step > 10.0f) step *= range / step > 20.0f ? 5.0f : 2.0f;
        for (float v = std::ceil(m_GraphLo / step) * step; v <= m_GraphHi; v += step)
        {
            float y = toY(v);
            dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), std::abs(v) < step * 0.01f ? IM_COL32(255, 255, 255, 60) : IM_COL32(255, 255, 255, 18));
            char label[24];
            std::snprintf(label, sizeof(label), "%.3g", std::abs(v) < step * 0.01f ? 0.0f : v);
            dl->AddText(ImVec2(x0 - 2.0f - ImGui::CalcTextSize(label).x, y - 7.0f), IM_COL32(200, 200, 200, 120), label);
        }

        // The channel list (click a channel to show or hide it).
        float rowH = std::min(15.0f, (mx.y - mn.y - 8.0f) / (float)(rows.size() + bones.size()));
        float ly = mn.y + 4.0f;
        int lastBone = -1;
        for (const Row& r : rows)
        {
            if (r.Bone != lastBone)
            {
                dl->AddText(ImVec2(mn.x + 6.0f, ly), IM_COL32(230, 230, 230, 230), m_Mesh.Bones[(size_t)r.Bone].Name.c_str());
                ly += rowH;
                lastBone = r.Bone;
            }
            bool off = hidden(r.Bone, r.Channel);
            ImU32 col = kAxis[r.Channel % 3];
            dl->AddCircleFilled(ImVec2(mn.x + 16.0f, ly + rowH * 0.5f), 3.5f, off ? IM_COL32(90, 90, 90, 255) : col);
            dl->AddText(ImVec2(mn.x + 24.0f, ly + rowH * 0.5f - 7.0f), off ? IM_COL32(120, 120, 120, 200) : IM_COL32(210, 210, 210, 220), rig::ChannelName(r.Channel));
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouse.x < mn.x + listW && mouse.y >= ly && mouse.y < ly + rowH)
            {
                uint32_t id = ((uint32_t)r.Bone << 8) | (uint32_t)r.Channel;
                auto it = std::find(m_GraphHidden.begin(), m_GraphHidden.end(), id);
                if (it == m_GraphHidden.end()) m_GraphHidden.push_back(id);
                else m_GraphHidden.erase(it);
            }
            ly += rowH;
        }
        dl->AddLine(ImVec2(mn.x + listW, mn.y + 4.0f), ImVec2(mn.x + listW, mx.y - 4.0f), IM_COL32(255, 255, 255, 40));

        // Curves, key points, the selected point's handles.
        dl->PushClipRect(ImVec2(x0 - 6.0f, mn.y), ImVec2(mx.x, mx.y), true);
        struct Hit
        {
            int Row = -1, Key = -1, Part = 0; // part: 1 point, 2 handle in, 3 handle out
            float Dist = 1e30f;
        } hit;
        auto consider = [&](int row, int key, int part, ImVec2 p) {
            float d = std::sqrt((mouse.x - p.x) * (mouse.x - p.x) + (mouse.y - p.y) * (mouse.y - p.y));
            if (d < 7.0f && d < hit.Dist) hit = { row, key, part, d };
        };
        for (size_t r = 0; r < rows.size(); ++r)
        {
            if (curves[r].empty()) continue;
            int b = rows[r].Bone, c = rows[r].Channel;
            ImU32 col = kAxis[c % 3];
            if (c >= 3 && c < 6) col = (col & 0x00FFFFFFu) | 0xD0000000u; // (rotation a little fainter, then scale)
            if (c >= 6) col = (col & 0x00FFFFFFu) | 0x90000000u;
            for (int i = 1; i <= samples; ++i)
                dl->AddLine(ImVec2(toX(end * (float)(i - 1) / (float)samples), toY(curves[r][(size_t)i - 1])),
                            ImVec2(toX(end * (float)i / (float)samples), toY(curves[r][(size_t)i])), col, 1.5f);
            rig::BoneTrack track = rig::TrackOf(action, (size_t)b);
            for (size_t i = 0; i < track.Keys.size(); ++i)
            {
                float v = shown(c, track.Values[i][(size_t)c]);
                ImVec2 p(toX(track.Times[i]), toY(v));
                bool sel = m_GraphBone == b && m_GraphChannel == c && m_GraphKey == (int)track.Keys[i];
                dl->AddCircleFilled(p, sel ? 4.5f : 3.5f, sel ? IM_COL32(255, 200, 80, 255) : IM_COL32(230, 230, 230, 240));
                consider((int)r, (int)track.Keys[i], 1, p);
                if (!sel) continue;
                // Handles: a third of the way to the neighbouring keys, along the curve's slope there (Bezier sides).
                float in, out;
                rig::KeySlopes(action, track, (size_t)b, i, c, in, out);
                bool free = (action.Keys[track.Keys[i]].CurveOf((size_t)b).Free >> c) & 1;
                ImU32 hcol = free ? IM_COL32(255, 160, 220, 230) : IM_COL32(200, 200, 200, 200);
                if (i > 0 && action.Keys[track.Keys[i - 1]].CurveOf((size_t)b).Interp == KeyInterp::Bezier)
                {
                    float dt = (track.Times[i] - track.Times[i - 1]) / 3.0f;
                    ImVec2 h(toX(track.Times[i] - dt), toY(shown(c, track.Values[i][(size_t)c] - in * dt)));
                    dl->AddLine(p, h, hcol, 1.0f);
                    dl->AddCircle(h, 3.5f, hcol, 0, 1.5f);
                    consider((int)r, (int)track.Keys[i], 2, h);
                }
                if (i + 1 < track.Keys.size() && action.Keys[track.Keys[i]].CurveOf((size_t)b).Interp == KeyInterp::Bezier)
                {
                    float dt = (track.Times[i + 1] - track.Times[i]) / 3.0f;
                    ImVec2 h(toX(track.Times[i] + dt), toY(shown(c, track.Values[i][(size_t)c] + out * dt)));
                    dl->AddLine(p, h, hcol, 1.0f);
                    dl->AddCircle(h, 3.5f, hcol, 0, 1.5f);
                    consider((int)r, (int)track.Keys[i], 3, h);
                }
            }
        }
        float px = toX(m_ActionTime);
        dl->AddLine(ImVec2(px, mn.y), ImVec2(px, y1), IM_COL32(80, 160, 255, 255), 2.0f);
        dl->PopClipRect();

        auto refresh = [&] {
            m_Dirty = true;
            rig::EvaluateAction(m_Mesh, m_Action, m_ActionTime);
            rig::ApplyConstraints(m_Mesh);
            SyncRigPreview();
        };
        // Clicks: a point or handle selects it and starts dragging it; elsewhere in the graph moves the playhead.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouse.x >= mn.x + listW)
        {
            if (hit.Row >= 0)
            {
                m_GraphBone = rows[(size_t)hit.Row].Bone;
                m_GraphChannel = rows[(size_t)hit.Row].Channel;
                m_GraphKey = hit.Key;
                m_SelectedKey = action.Keys[(size_t)hit.Key].Time;
                m_GraphDrag = hit.Part;
                m_GraphDragMouse = mouse;
                Record(hit.Part == 1 ? "Move Key Value" : "Move Handle");
            }
            else
            {
                m_ActionPlaying = false;
                m_ActionTime = toTime(mouse.x);
                refresh();
            }
        }
        bool valid = m_GraphKey >= 0 && m_GraphKey < (int)action.Keys.size() && m_GraphBone >= 0 && m_GraphBone < (int)m_Mesh.Bones.size() &&
                     action.Keys[(size_t)m_GraphKey].HasBone((size_t)m_GraphBone);
        if (m_GraphDrag && valid && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            size_t key = (size_t)m_GraphKey, bone = (size_t)m_GraphBone;
            if (m_GraphDrag == 1)
                rig::SetChannelValue(m_Mesh, m_Action, key, bone, m_GraphChannel, stored(m_GraphChannel, toValue(mouse.y)));
            else
            {
                // The handle through the mouse: its slope (the other side turns with it, aligned).
                rig::BoneTrack track = rig::TrackOf(action, bone);
                size_t i = 0;
                while (i < track.Keys.size() && track.Keys[i] != key) ++i;
                if (i < track.Keys.size())
                {
                    float dt = toTime(mouse.x) - track.Times[i];
                    if (m_GraphDrag == 2) dt = std::min(dt, -1e-3f);
                    else dt = std::max(dt, 1e-3f);
                    float slope = (stored(m_GraphChannel, toValue(mouse.y)) - track.Values[i][(size_t)m_GraphChannel]) / dt;
                    rig::SetHandles(m_Mesh, m_Action, key, bone, m_GraphChannel, slope, slope);
                }
            }
            refresh();
        }
        else if (m_GraphDrag)
        {
            // Released: a key point dragged sideways moves the whole key to that time.
            if (m_GraphDrag == 1 && valid && std::abs(mouse.x - m_GraphDragMouse.x) > 4.0f)
            {
                float from = action.Keys[(size_t)m_GraphKey].Time, to = std::round(toTime(mouse.x) * 100.0f) / 100.0f;
                if (std::abs(to - from) > 1e-4f && rig::MoveKey(m_Mesh, m_Action, from, to))
                {
                    m_SelectedKey = to;
                    m_GraphKey = -1;
                    for (size_t k = 0; k < action.Keys.size(); ++k)
                        if (std::abs(action.Keys[k].Time - to) < 1e-4f) m_GraphKey = (int)k;
                    refresh();
                }
            }
            m_GraphDrag = 0;
        }

        // Keys over the graph: T interpolation, V handle type, Delete/X the key.
        if (hovered && valid && !m_GraphDrag)
        {
            if (Shortcuts::Pressed("rig.interpolation")) ImGui::OpenPopup("GraphInterp");
            if (Shortcuts::Pressed("rig.handleType")) ImGui::OpenPopup("GraphHandles");
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_X))
            {
                RigOperate("Delete Keyframe", [&] { rig::DeleteKey(m_Mesh, m_Action, action.Keys[(size_t)m_GraphKey].Time); });
                m_GraphKey = -1;
                m_SelectedKey = -1.0f;
                return;
            }
        }
        if (hovered && valid && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("GraphInterp");
        if (ImGui::BeginPopup("GraphInterp"))
        {
            ImGui::TextDisabled("Interpolation (%s, to the next key)", m_Mesh.Bones[(size_t)std::max(m_GraphBone, 0)].Name.c_str());
            const char* names[] = { "Linear", "Constant", "Bezier" };
            for (int i = 0; i < 3; ++i)
                if (ImGui::MenuItem(names[i]) && valid)
                    RigOperate("Interpolation Mode", [&] { rig::SetInterpolation(m_Mesh, m_Action, (size_t)m_GraphKey, (size_t)m_GraphBone, (KeyInterp)i); });
            ImGui::Separator();
            if (ImGui::MenuItem("Bezier: Every Key of the Bone") && valid)
                RigOperate("Interpolation Mode", [&] {
                    for (size_t k = 0; k < action.Keys.size(); ++k)
                        rig::SetInterpolation(m_Mesh, m_Action, k, (size_t)m_GraphBone, KeyInterp::Bezier);
                });
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("GraphHandles"))
        {
            ImGui::TextDisabled("Handles (%s)", rig::ChannelName(std::max(m_GraphChannel, 0)));
            if (ImGui::MenuItem("Auto Clamped") && valid)
                RigOperate("Handle Type", [&] { rig::SetAutoHandles(m_Mesh, m_Action, (size_t)m_GraphKey, (size_t)m_GraphBone, m_GraphChannel); });
            if (ImGui::MenuItem("Flat") && valid)
                RigOperate("Handle Type", [&] { rig::SetHandles(m_Mesh, m_Action, (size_t)m_GraphKey, (size_t)m_GraphBone, m_GraphChannel, 0.0f, 0.0f); });
            ImGui::EndPopup();
        }
        // The selected key's value, typed in.
        if (valid)
        {
            const ActionKey& k = action.Keys[(size_t)m_GraphKey];
            rig::BoneTrack track = rig::TrackOf(action, (size_t)m_GraphBone);
            size_t i = 0;
            while (i < track.Keys.size() && track.Keys[i] != (size_t)m_GraphKey) ++i;
            if (i < track.Keys.size())
            {
                static const char* interpNames[] = { "linear", "constant", "bezier" };
                char info[160];
                std::snprintf(info, sizeof(info), "%s %s at %.2f s:", m_Mesh.Bones[(size_t)m_GraphBone].Name.c_str(), rig::ChannelName(m_GraphChannel), k.Time);
                ImVec2 infoSize = ImGui::CalcTextSize(info);
                dl->AddRectFilled(ImVec2(x0, y1 - 40.0f), ImVec2(x0 + infoSize.x + 190.0f, y1 - 14.0f), IM_COL32(20, 20, 22, 235), 3.0f);
                dl->AddText(ImVec2(x0 + 4.0f, y1 - 34.0f), IM_COL32(230, 230, 230, 230), info);
                ImVec2 save = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(ImVec2(x0 + 8.0f + ImGui::CalcTextSize(info).x, y1 - 37.0f));
                float v = shown(m_GraphChannel, track.Values[i][(size_t)m_GraphChannel]);
                ImGui::SetNextItemWidth(90.0f);
                if (ImGui::DragFloat("##graphvalue", &v, m_GraphChannel >= 3 && m_GraphChannel < 6 ? 0.5f : 0.01f))
                {
                    if (ImGui::IsItemActivated()) Record("Key Value");
                    rig::SetChannelValue(m_Mesh, m_Action, (size_t)m_GraphKey, (size_t)m_GraphBone, m_GraphChannel, stored(m_GraphChannel, v));
                    refresh();
                }
                else if (ImGui::IsItemActivated())
                    Record("Key Value");
                ImGui::SameLine();
                ImGui::TextDisabled("%s", interpNames[(int)k.CurveOf((size_t)m_GraphBone).Interp]);
                ImGui::SetCursorScreenPos(save);
            }
        }
    }

    // ---- Shape keys

    void MeshEditMode::DrawShapeKeysPopup()
    {
        if (!ImGui::BeginPopup("ShapeKeys")) return;
        ImGui::TextDisabled("Shape keys (the model's blend shapes)");
        auto editing = [&](int key, const char* label) {
            if (ImGui::RadioButton(label, m_Mesh.EditedShape == key))
            {
                Record("Edit Shape Key");
                rig::SetEditedShape(m_Mesh, key);
                ++m_GeometryVersion;
                Commit();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Edit Mode moves this shape's vertices");
        };
        editing(-1, "Basis##edit");
        int remove = -1;
        for (int k = 0; k < (int)m_Mesh.ShapeKeys.size(); ++k)
        {
            ImGui::PushID(k);
            editing(k, "##edit");
            ImGui::SameLine();
            char name[128];
            std::snprintf(name, sizeof(name), "%s", m_Mesh.ShapeKeys[(size_t)k].Name.c_str());
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::InputText("##name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) && name[0])
            {
                Record("Rename Shape Key");
                m_Mesh.ShapeKeys[(size_t)k].Name = name;
                Commit();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            float value = m_Mesh.ShapeKeys[(size_t)k].Value;
            if (ImGui::SliderFloat("##value", &value, 0.0f, 1.0f, "%.2f"))
            {
                if (ImGui::IsItemActivated()) Record("Shape Key Value");
                m_Mesh.ShapeKeys[(size_t)k].Value = value;
                Commit();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much of it shows by default (a Mesh Renderer's Blend Shape Weights and animations set it too)");
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) remove = k;
            ImGui::PopID();
        }
        if (remove >= 0)
        {
            Record("Remove Shape Key");
            rig::RemoveShapeKey(m_Mesh, remove);
            ++m_GeometryVersion;
            Commit();
        }
        if (ImGui::Button("New Shape Key"))
        {
            Record("New Shape Key");
            rig::AddShapeKey(m_Mesh, "Key");
            Commit();
        }
        ImGui::SameLine();
        if (ImGui::Button("New from Mix"))
        {
            Record("New Shape Key");
            rig::AddShapeKey(m_Mesh, "Key", true);
            Commit();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A new key of the shapes mixed at their values");
        ImGui::EndPopup();
    }
}
