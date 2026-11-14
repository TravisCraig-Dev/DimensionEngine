#pragma once
#include "Dimension/Renderer/Mesh.h"

namespace Dimension::Modeling
{
    // Writes a mesh as glTF 2.0: a binary .glb, or a .gltf with its buffer embedded (by the path's extension). One
    // primitive per sub-mesh, with positions, normals, UVs and tangents; material slots become named materials.
    bool ExportGltf(const MeshData& mesh, const fs::path& path);
}
