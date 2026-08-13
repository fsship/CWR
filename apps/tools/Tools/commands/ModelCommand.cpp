#include "ModelCommand.hpp"
#include "../SDLPreview.hpp"
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Asset/Probes/AssetInfo.hpp>
#include <Poseidon/Asset/Probes/AssetPreview.hpp>
#include <Poseidon/Asset/Formats/Common/FormatDetector.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdint.h>
#include <CLI/App.hpp>
#include <CLI/Error.hpp>
#include <CLI/Option.hpp>
#include <CLI/TypeTools.hpp>
#include <CLI/Validators.hpp>
#include <cstdio>
#include <functional>
#include <string>
#include <system_error>
#include <utility>
#include <memory>
#include <limits>

namespace PoseidonTools
{

// Resolve a model's texture reference (e.g. "data\foo.pac") to a file on disk:
// try the reference relative to each root, then a recursive match on its basename.
// `<default>` / `#default#` placeholders resolve to nothing.
static std::filesystem::path resolveTexture(const std::string& texName, const std::filesystem::path& texRoot,
                                            const std::filesystem::path& modelDir)
{
    namespace fs = std::filesystem;
    if (texName.empty() || texName.front() == '<' || texName.front() == '#')
        return {};

    std::string norm = texName;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    fs::path rel(norm);
    fs::path base = rel.filename();

    std::vector<fs::path> roots;
    if (!texRoot.empty())
        roots.push_back(texRoot);
    if (!modelDir.empty())
        roots.push_back(modelDir);

    for (const auto& root : roots)
    {
        std::error_code ec;
        fs::path direct = root / rel;
        if (fs::exists(direct, ec))
            return direct;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec))
        {
            if (ec)
                break;
            if (it->is_regular_file(ec) && it->path().filename() == base)
                return it->path();
        }
    }
    if (fs::exists(norm))
        return fs::path(norm);
    return {};
}

static void setupModelInspect(CLI::App& model)
{
    auto* cmd = model.add_subcommand("inspect", "Inspect P3D model details");
    static std::string inputPath;
    static bool showTextures = false;
    static bool showSelections = false;
    static bool showSections = false;
    static bool showAll = false;
    static bool classify = false;
    static std::string texRoot;

    cmd->add_option("input", inputPath, "Input P3D file path")->required()->check(CLI::ExistingFile);
    cmd->add_flag("-t,--textures", showTextures, "Show texture details");
    cmd->add_flag("-s,--selections", showSelections, "Show named selections");
    cmd->add_flag("--sections", showSections, "Show section details with render hints");
    cmd->add_flag("-a,--all", showAll, "Show all details");
    cmd->add_flag("--classify", classify, "Classify each texture's alpha (opaque/cutout/blend) + render route");
    cmd->add_option("--texroot", texRoot,
                    "Directory to resolve texture paths from (recursive basename match; defaults to model dir)");

    cmd->callback(
        [&]()
        {
            auto info = Poseidon::InspectModel(inputPath);

            std::cout << "File: " << inputPath << std::endl;
            std::cout << "Format: " << info.format << std::endl;
            std::cout << "Version: " << info.version << std::endl;

            if (!info.isSupported)
            {
                std::cerr << "Warning: Format is not fully supported" << std::endl;
                if (!info.errorMessage.empty())
                    std::cerr << "  " << info.errorMessage << std::endl;
            }

            if (!info.valid)
            {
                std::cerr << "Error: Failed to load " << inputPath << std::endl;
                throw CLI::RuntimeError(1);
            }
            std::cout << std::endl;

            std::cout << "LOD Levels: " << info.lodCount << std::endl;
            std::cout << std::endl;

            std::unique_ptr<Poseidon::Model::Model> decodedModel;
            if (info.format == "ODOL")
            {
                try
                {
                    decodedModel = std::make_unique<Poseidon::Model::Model>(
                        Poseidon::Asset::Formats::ODOLLoader::load(inputPath));
                    const auto& bounds = decodedModel->boundingBox;
                    std::cout << "Model Bounds: min=(" << bounds.min.x << ", " << bounds.min.y << ", "
                              << bounds.min.z << ") max=(" << bounds.max.x << ", " << bounds.max.y << ", "
                              << bounds.max.z << ") radius=" << decodedModel->boundingSphere.radius << std::endl;
                    std::cout << std::endl;
                }
                catch (const std::exception&)
                {
                    // InspectModel already validated the file; bounds/proxies are optional extended diagnostics.
                }
            }

            for (const auto& lod : info.lods)
            {
                std::cout << "LOD " << lod.index << " (Resolution: " << lod.resolution << ")" << std::endl;
                std::cout << "  Points:     " << std::setw(6) << lod.points << std::endl;
                std::cout << "  Faces:      " << std::setw(6) << lod.faces << std::endl;
                std::cout << "  Textures:   " << std::setw(6) << lod.textures << std::endl;
                std::cout << "  Selections: " << std::setw(6) << lod.selections << std::endl;
                if (decodedModel && lod.index >= 0 && lod.index < static_cast<int>(decodedModel->lodLevels.size()))
                {
                    const auto& bounds = decodedModel->lodLevels[static_cast<size_t>(lod.index)].mesh.boundingBox;
                    std::cout << "  Bounds: min=(" << bounds.min.x << ", " << bounds.min.y << ", " << bounds.min.z
                              << ") max=(" << bounds.max.x << ", " << bounds.max.y << ", " << bounds.max.z << ")"
                              << std::endl;
                }

                if (showAll || showTextures)
                {
                    std::cout << "  Texture List:" << std::endl;
                    for (size_t t = 0; t < lod.textureNames.size(); ++t)
                        std::cout << "    [" << t << "] " << lod.textureNames[t] << std::endl;
                }

                if (showAll || showSelections)
                {
                    std::cout << "  Named Selections:" << std::endl;
                    for (size_t s = 0; s < lod.selectionNames.size(); ++s)
                        std::cout << "    [" << s << "] " << lod.selectionNames[s].first << " ("
                                  << lod.selectionNames[s].second << " points)" << std::endl;
                }

                if (showAll || showSections)
                {
                    std::cout << "  Sections: " << lod.sectionInfos.size() << std::endl;
                    for (const auto& sec : lod.sectionInfos)
                    {
                        std::cout << "    [" << sec.index << "] tex=" << sec.textureName
                                  << " tris=" << sec.triangleCount << " flags=0x" << std::hex << sec.hints << std::dec
                                  << " (" << sec.hintsStr << ")" << std::endl;
                    }
                }
                if ((showAll || showSelections) && decodedModel && lod.index >= 0 &&
                    lod.index < static_cast<int>(decodedModel->lodLevels.size()))
                {
                    const auto& decodedLod = decodedModel->lodLevels[static_cast<size_t>(lod.index)];
                    if (!decodedLod.mesh.proxies.empty())
                    {
                        std::cout << "  Proxies: " << decodedLod.mesh.proxies.size() << std::endl;
                        for (const auto& proxy : decodedLod.mesh.proxies)
                        {
                            const auto position = proxy.transform.GetTranslation();
                            const auto& m = proxy.transform.m;
                            std::cout << "    " << proxy.name << "." << std::setw(2) << std::setfill('0')
                                      << std::max(proxy.id, 0) << std::setfill(' ') << " sel=" << proxy.selectionIndex
                                      << " pos=(" << position.x << ", " << position.y << ", " << position.z
                                      << ") fwd=(" << m[2][0] << ", " << m[2][1] << ", " << m[2][2] << ")"
                                      << std::endl;
                        }
                    }
                }
                std::cout << std::endl;
            }

            if (classify)
            {
                std::filesystem::path root = texRoot;
                std::filesystem::path modelDir = std::filesystem::path(inputPath).parent_path();
                std::cout << "Alpha classification";
                if (!root.empty())
                    std::cout << " (texroot: " << root.string() << ")";
                std::cout << ":" << std::endl;

                std::set<std::string> seen;
                int nOpaque = 0, nCutout = 0, nBlend = 0, nMissing = 0;
                for (const auto& lod : info.lods)
                {
                    for (const auto& name : lod.textureNames)
                    {
                        if (name.empty() || name.front() == '<' || name.front() == '#')
                            continue;
                        if (!seen.insert(name).second)
                            continue;

                        std::filesystem::path p = resolveTexture(name, root, modelDir);
                        if (p.empty())
                        {
                            ++nMissing; // not under texroot; counted, not listed (use --texroot to resolve)
                            continue;
                        }
                        Poseidon::DecodedImage img = Poseidon::DecodePAAFile(p.string());
                        if (!img.valid())
                        {
                            std::cout << "  " << name << " -> (decode failed)" << std::endl;
                            ++nMissing;
                            continue;
                        }
                        const size_t n = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
                        const Poseidon::AlphaStats a = Poseidon::ClassifyAlpha(img.rgba.data(), n);
                        const char* route =
                            a.kind == Poseidon::AlphaStats::Blend    ? "back-to-front alpha pass, NO depth-write"
                            : a.kind == Poseidon::AlphaStats::Cutout ? "opaque pass, depth-write, discard holes"
                                                                     : "opaque pass, depth-write";
                        std::cout << "  " << name << " -> " << Poseidon::AlphaKindName(a.kind) << "  [" << route << "]"
                                  << std::endl;
                        if (a.kind == Poseidon::AlphaStats::Blend)
                            ++nBlend;
                        else if (a.kind == Poseidon::AlphaStats::Cutout)
                            ++nCutout;
                        else
                            ++nOpaque;
                    }
                }
                std::cout << "  Summary: " << nBlend << " blend (deferred), " << nCutout << " cutout, " << nOpaque
                          << " opaque";
                if (nMissing > 0)
                    std::cout << ", " << nMissing << " unresolved (not under texroot)";
                std::cout << std::endl << std::endl;
            }
        });
}

static void setupModelConvert(CLI::App& model)
{
    auto* cmd = model.add_subcommand("convert", "Convert P3D model formats (MLOD/ODOL)");
    static std::string inputPath;
    static std::string outputPath;
    static bool verbose = false;

    cmd->add_option("input", inputPath, "Input P3D file path")->required()->check(CLI::ExistingFile);
    cmd->add_option("output", outputPath, "Output P3D file path")->required();
    cmd->add_flag("-v,--verbose", verbose, "Verbose output");

    cmd->callback(
        [&]()
        {
            if (verbose)
                std::cout << "Converting: " << inputPath << " -> " << outputPath << std::endl;

            auto* shape = new LODShapeWithShadow();
            if (!shape->LoadOptimized(inputPath.c_str()))
            {
                std::cerr << "Error: Failed to load " << inputPath << std::endl;
                delete shape;
                throw CLI::RuntimeError(1);
            }

            if (verbose)
            {
                std::cout << "Loaded successfully" << std::endl;
                std::cout << "LOD levels: " << static_cast<int>(shape->NLevels()) << std::endl;
                for (int i = 0; i < shape->NLevels(); ++i)
                {
                    Shape* lod = shape->Level(i);
                    if (lod)
                    {
                        std::cout << "  LOD " << i << " (res=" << shape->Resolution(i) << "): " << lod->NPoints()
                                  << " points, " << lod->NFaces() << " faces, " << lod->NTextures() << " textures, "
                                  << lod->NNamedSel() << " selections" << std::endl;
                    }
                }
            }

            shape->SaveOptimized(outputPath.c_str());

            if (verbose)
                std::cout << "Saved successfully as ODOL v7" << std::endl;
            else
                std::cout << "Converted: " << inputPath << " -> " << outputPath << std::endl;

            delete shape;
        });
}

namespace
{
namespace P3DM = Poseidon::Model;

constexpr float kPi = 3.14159265358979323846f;

static P3DM::Vector3 add(const P3DM::Vector3& a, const P3DM::Vector3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

static P3DM::Vector3 sub(const P3DM::Vector3& a, const P3DM::Vector3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static P3DM::Vector3 mul(const P3DM::Vector3& a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

static float length(const P3DM::Vector3& a)
{
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

static P3DM::Vector3 normalized(const P3DM::Vector3& a)
{
    const float len = length(a);
    return len > 1e-6f ? mul(a, 1.0f / len) : P3DM::Vector3{0.0f, 1.0f, 0.0f};
}

static P3DM::Vector3 cross(const P3DM::Vector3& a, const P3DM::Vector3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static uint32_t addVertex(P3DM::Mesh& mesh, const P3DM::Vector3& p, const P3DM::Vector3& n = {0, 1, 0},
                          float u = 0.0f, float v = 0.0f)
{
    mesh.vertices.emplace_back(p, normalized(n), P3DM::Vector2{u, v});
    return static_cast<uint32_t>(mesh.vertices.size() - 1);
}

static uint32_t nextFaceIndex(const P3DM::Mesh& mesh)
{
    uint32_t next = 0;
    for (const auto& tri : mesh.triangles)
        next = std::max(next, tri.originalIndex + 1);
    for (const auto& quad : mesh.quads)
        next = std::max(next, quad.originalIndex + 1);
    return next;
}

static void addTri(P3DM::Mesh& mesh, uint32_t a, uint32_t b, uint32_t c, uint32_t material, uint32_t& faceIndex,
                   P3DM::FaceFlags flags = P3DM::FaceFlags::None)
{
    P3DM::Triangle tri(a, b, c, material, flags);
    tri.originalIndex = faceIndex++;
    mesh.triangles.push_back(tri);
}

static void addQuad(P3DM::Mesh& mesh, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t material,
                    uint32_t& faceIndex, P3DM::FaceFlags flags = P3DM::FaceFlags::None)
{
    P3DM::Quad quad(a, b, c, d, material, flags);
    quad.originalIndex = faceIndex++;
    mesh.quads.push_back(quad);
}

static std::vector<uint32_t> addBox(P3DM::Mesh& mesh, const P3DM::Vector3& center, const P3DM::Vector3& half,
                                    uint32_t material, uint32_t& faceIndex)
{
    std::vector<uint32_t> v;
    v.reserve(8);
    for (int y = -1; y <= 1; y += 2)
        for (int z = -1; z <= 1; z += 2)
            for (int x = -1; x <= 1; x += 2)
                v.push_back(addVertex(mesh, {center.x + x * half.x, center.y + y * half.y, center.z + z * half.z}));

    // Index layout: y-major, then z, then x.
    addQuad(mesh, v[0], v[1], v[3], v[2], material, faceIndex);
    addQuad(mesh, v[4], v[6], v[7], v[5], material, faceIndex);
    addQuad(mesh, v[0], v[4], v[5], v[1], material, faceIndex);
    addQuad(mesh, v[2], v[3], v[7], v[6], material, faceIndex);
    addQuad(mesh, v[0], v[2], v[6], v[4], material, faceIndex);
    addQuad(mesh, v[1], v[5], v[7], v[3], material, faceIndex);
    return v;
}

static std::vector<uint32_t> addCylinder(P3DM::Mesh& mesh, const P3DM::Vector3& start, const P3DM::Vector3& end,
                                         float radius, int sides, uint32_t material, uint32_t& faceIndex)
{
    const P3DM::Vector3 axis = normalized(sub(end, start));
    P3DM::Vector3 right = normalized(cross(axis, std::fabs(axis.y) < 0.95f ? P3DM::Vector3{0, 1, 0}
                                                                          : P3DM::Vector3{1, 0, 0}));
    const P3DM::Vector3 up = normalized(cross(right, axis));
    std::vector<uint32_t> vertices;
    vertices.reserve(static_cast<size_t>(sides) * 2 + 2);
    for (int ring = 0; ring < 2; ++ring)
    {
        const auto& center = ring == 0 ? start : end;
        for (int i = 0; i < sides; ++i)
        {
            const float angle = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(sides);
            const P3DM::Vector3 radial = add(mul(right, std::cos(angle)), mul(up, std::sin(angle)));
            vertices.push_back(addVertex(mesh, add(center, mul(radial, radius)), radial,
                                         static_cast<float>(i) / static_cast<float>(sides), static_cast<float>(ring)));
        }
    }
    const uint32_t c0 = addVertex(mesh, start, mul(axis, -1.0f), 0.5f, 0.5f);
    const uint32_t c1 = addVertex(mesh, end, axis, 0.5f, 0.5f);
    for (int i = 0; i < sides; ++i)
    {
        const uint32_t j = static_cast<uint32_t>((i + 1) % sides);
        const uint32_t a = vertices[static_cast<size_t>(i)];
        const uint32_t b = vertices[j];
        const uint32_t c = vertices[static_cast<size_t>(sides) + j];
        const uint32_t d = vertices[static_cast<size_t>(sides) + static_cast<size_t>(i)];
        addQuad(mesh, a, b, c, d, material, faceIndex);
        addTri(mesh, c0, b, a, material, faceIndex);
        addTri(mesh, c1, d, c, material, faceIndex);
    }
    return vertices;
}

static void expandBounds(P3DM::BoundingBox& box, const P3DM::Vector3& p)
{
    box.min.x = std::min(box.min.x, p.x);
    box.min.y = std::min(box.min.y, p.y);
    box.min.z = std::min(box.min.z, p.z);
    box.max.x = std::max(box.max.x, p.x);
    box.max.y = std::max(box.max.y, p.y);
    box.max.z = std::max(box.max.z, p.z);
}

static void recalculateBounds(P3DM::Mesh& mesh)
{
    if (mesh.vertices.empty())
        return;
    P3DM::BoundingBox box(mesh.vertices.front().position, mesh.vertices.front().position);
    for (const auto& vertex : mesh.vertices)
        expandBounds(box, vertex.position);
    mesh.boundingBox = box;
    mesh.bCenter = box.GetCenter();
    mesh.boundingSphere.center = mesh.bCenter;
    mesh.bRadius = 0.0f;
    for (const auto& vertex : mesh.vertices)
        mesh.bRadius = std::max(mesh.bRadius, length(sub(vertex.position, mesh.bCenter)));
    mesh.boundingSphere.radius = mesh.bRadius;
}

static uint32_t ensureMaterial(P3DM::Mesh& mesh, const std::string& path)
{
    for (size_t i = 0; i < mesh.materials.size(); ++i)
        if (mesh.materials[i].texturePath == path)
            return static_cast<uint32_t>(i);
    mesh.materials.emplace_back(path, path);
    return static_cast<uint32_t>(mesh.materials.size() - 1);
}

static void appendSelection(P3DM::Mesh& mesh, const std::string& name, const std::vector<uint32_t>& vertices,
                            const std::vector<uint32_t>& faces = {})
{
    P3DM::NamedSelection selection(name);
    selection.vertexIndices = vertices;
    selection.vertexWeights.assign(vertices.size(), 255);
    selection.triangleIndices = faces;
    mesh.selections.push_back(std::move(selection));
}

static void appendRadarHMMWVVisuals(P3DM::Mesh& mesh, unsigned detail)
{
    const uint32_t green = ensureMaterial(mesh, "humr\\hmmwv_kapota.pac");
    const uint32_t black = ensureMaterial(mesh, "data\\blck_sum.pac");
    uint32_t faceIndex = nextFaceIndex(mesh);
    std::vector<uint32_t> launcherVertices;

    // HMMWV coordinate system: +X right, +Y up, +Z forward. Both banks start at 45 degrees elevation.
    const P3DM::Vector3 forwardUp{0.0f, std::sin(kPi / 4.0f), std::cos(kPi / 4.0f)};
    const P3DM::Vector3 launcherAxisCenter{0.0f, 1.82f, 0.08f};

    // A central cross beam and two AH-64-style four-rail banks.
    const auto beam = addBox(mesh, launcherAxisCenter, {1.42f, 0.07f, 0.08f}, black, faceIndex);
    launcherVertices.insert(launcherVertices.end(), beam.begin(), beam.end());

    int proxyNumber = 1;
    for (int sideSign : {-1, 1})
    {
        const float sideX = static_cast<float>(sideSign) * 1.26f;
        const auto pylon = addBox(mesh, {sideX, 1.88f, 0.10f}, {0.15f, 0.21f, 0.20f}, green, faceIndex);
        launcherVertices.insert(launcherVertices.end(), pylon.begin(), pylon.end());

        for (int rail = 0; rail < 4; ++rail)
        {
            const float column = (rail & 1) ? 0.13f : -0.13f;
            const float row = (rail < 2) ? 0.11f : -0.11f;
            const P3DM::Vector3 railCenter{sideX + column, 1.89f + row, 0.11f};
            const P3DM::Vector3 tail = sub(railCenter, mul(forwardUp, 0.70f));
            const P3DM::Vector3 nose = add(railCenter, mul(forwardUp, 0.70f));
            auto tubeVertices = addCylinder(mesh, tail, nose, detail < 2 ? 0.095f : 0.08f, detail == 0 ? 8 : 6,
                                            green, faceIndex);
            launcherVertices.insert(launcherVertices.end(), tubeVertices.begin(), tubeVertices.end());

            // A three-point proxy dummy triangle. Its vertices are part of launcher_bank so configured rotation
            // animates both the placeholder and the real proxy transform around launcher_axis.
            const P3DM::Vector3 proxyOrigin = sub(railCenter, mul(forwardUp, 0.38f));
            const P3DM::Vector3 proxyAside{0.11f, 0.0f, 0.0f};
            const P3DM::Vector3 proxyUp = mul(normalized(cross(proxyAside, forwardUp)), 0.17f);
            const uint32_t p0 = addVertex(mesh, proxyOrigin);
            const uint32_t p1 = addVertex(mesh, add(proxyOrigin, mul(forwardUp, 0.10f)));
            const uint32_t p2 = addVertex(mesh, add(proxyOrigin, proxyUp));
            launcherVertices.insert(launcherVertices.end(), {p0, p1, p2});
            const uint32_t proxyFace = faceIndex;
            addTri(mesh, p0, p1, p2, UINT32_MAX, faceIndex);

            char selectionName[64];
            std::snprintf(selectionName, sizeof(selectionName), "proxy:maverik_proxy.%02d", proxyNumber);
            const uint32_t selectionIndex = static_cast<uint32_t>(mesh.selections.size());
            appendSelection(mesh, selectionName, {p0, p1, p2}, {proxyFace});

            P3DM::Matrix4x3 transform;
            // right, up, forward, translation in the Matrix4x3 convention used by ODOLLoader/ShapeAdapter.
            const P3DM::Vector3 right{1.0f, 0.0f, 0.0f};
            const P3DM::Vector3 up = normalized(cross(forwardUp, right));
            transform.Set(right.x, right.y, right.z, proxyOrigin.x, up.x, up.y, up.z, proxyOrigin.y, forwardUp.x,
                          forwardUp.y, forwardUp.z, proxyOrigin.z);
            mesh.proxies.emplace_back("proxy:maverik_proxy", transform, selectionIndex, proxyNumber);
            ++proxyNumber;
        }
    }

    // Tall mast, rotating base, and a conspicuous rectangular AESA-like radar panel.
    auto mast = addCylinder(mesh, {0.0f, 1.78f, -0.32f}, {0.0f, 2.72f, -0.32f}, 0.075f, detail == 0 ? 10 : 6,
                            black, faceIndex);
    auto radarBase = addCylinder(mesh, {0.0f, 2.64f, -0.32f}, {0.0f, 2.79f, -0.32f}, 0.27f,
                                 detail == 0 ? 12 : 8, green, faceIndex);
    const auto panel = addBox(mesh, {0.0f, 3.00f, -0.32f}, {0.68f, 0.32f, 0.055f}, black, faceIndex);
    const auto panelFrameTop = addBox(mesh, {0.0f, 3.33f, -0.32f}, {0.75f, 0.035f, 0.09f}, green, faceIndex);
    const auto panelFrameBottom = addBox(mesh, {0.0f, 2.67f, -0.32f}, {0.75f, 0.035f, 0.09f}, green, faceIndex);

    appendSelection(mesh, "launcher_bank", launcherVertices);

    // The source sections and edge maps describe only the unmodified ODOL face/vertex streams. Rebuild both.
    mesh.sections.clear();
    mesh.edges.mlodIndices.clear();
    mesh.edges.vertexIndices.clear();
    recalculateBounds(mesh);
}

static void appendLauncherAxis(P3DM::Mesh& mesh)
{
    const uint32_t first = addVertex(mesh, {-0.50f, 1.82f, 0.08f});
    const uint32_t second = addVertex(mesh, {0.50f, 1.82f, 0.08f});
    appendSelection(mesh, "launcher_axis", {first, second});
    mesh.edges.mlodIndices.clear();
    mesh.edges.vertexIndices.clear();
    recalculateBounds(mesh);
}

static void recalculateModelBounds(P3DM::Model& model)
{
    bool initialized = false;
    P3DM::BoundingBox bounds;
    for (const auto& lod : model.lodLevels)
    {
        if (lod.resolution >= 1000.0f || lod.mesh.vertices.empty())
            continue;
        if (!initialized)
        {
            bounds = lod.mesh.boundingBox;
            initialized = true;
        }
        else
        {
            expandBounds(bounds, lod.mesh.boundingBox.min);
            expandBounds(bounds, lod.mesh.boundingBox.max);
        }
    }
    if (!initialized)
        return;
    model.boundingBox = bounds;
    model.boundingCenter = bounds.GetCenter();
    model.boundingSphere.center = model.boundingCenter;
    model.boundingSphere.radius = 0.0f;
    for (const auto& lod : model.lodLevels)
        if (lod.resolution < 1000.0f)
            for (const auto& vertex : lod.mesh.vertices)
                model.boundingSphere.radius =
                    std::max(model.boundingSphere.radius, length(sub(vertex.position, model.boundingCenter)));
}
} // namespace

static void setupModelRadarHMMWV(CLI::App& model)
{
    auto* cmd = model.add_subcommand("radar-hmmwv", "Create a radar and eight-missile HMMWV from an ODOL base model");
    static std::string inputPath;
    static std::string outputPath;
    cmd->add_option("input", inputPath, "Base HMMWV ODOL P3D path")->required()->check(CLI::ExistingFile);
    cmd->add_option("output", outputPath, "Generated ODOL P3D path")->required();

    cmd->callback(
        [&]()
        {
            P3DM::Model generated;
            try
            {
                generated = Poseidon::Asset::Formats::ODOLLoader::load(inputPath);
            }
            catch (const std::exception& e)
            {
                std::cerr << "Error: Failed to load base ODOL: " << e.what() << std::endl;
                throw CLI::RuntimeError(1);
            }

            unsigned visualIndex = 0;
            for (auto& lod : generated.lodLevels)
                if (lod.resolution < 1000.0f)
                    appendRadarHMMWVVisuals(lod.mesh, visualIndex++);

            const int memoryIndex = static_cast<int>(generated.memoryIdx);
            if (memoryIndex < 0 || memoryIndex >= static_cast<int>(generated.lodLevels.size()))
            {
                std::cerr << "Error: Base ODOL has no valid Memory LOD index" << std::endl;
                throw CLI::RuntimeError(1);
            }
            appendLauncherAxis(generated.lodLevels[static_cast<size_t>(memoryIndex)].mesh);
            recalculateModelBounds(generated);

            std::filesystem::path output(outputPath);
            if (!output.parent_path().empty())
                std::filesystem::create_directories(output.parent_path());

            std::unique_ptr<LODShapeWithShadow> shape(P3DM::ShapeAdapter::convertToLODShape(generated));
            if (!shape)
            {
                std::cerr << "Error: Failed to save generated model: " << outputPath << std::endl;
                throw CLI::RuntimeError(1);
            }
            shape->SaveOptimized(outputPath.c_str());

            std::cout << "Generated radar HMMWV: " << outputPath << std::endl;
            std::cout << "  Visual LODs modified: " << visualIndex << std::endl;
            std::cout << "  Per visual LOD: 2 x four-rail banks, 8 Maverick proxies, radar mast/panel, launcher_bank"
                      << std::endl;
            std::cout << "  Memory LOD " << memoryIndex << ": launcher_axis along +X" << std::endl;
            std::cout << "  Initial elevation: 45 degrees; rotate launcher_bank -45 degrees about launcher_axis for vertical"
                      << std::endl;
        });
}

static void setupModelRender(CLI::App& model)
{
    auto* cmd = model.add_subcommand("render", "Render P3D model wireframe to image file");
    static std::string inputPath;
    static std::string outputPath;
    static int width = 512;
    static int height = 512;
    static int lodIndex = 0;
    static std::string view = "front";

    cmd->add_option("input", inputPath, "Input P3D file path")->required()->check(CLI::ExistingFile);
    cmd->add_option("-o,--output", outputPath, "Output image path (.png, .bmp, .tga)")->required();
    cmd->add_option("-W,--width", width, "Image width in pixels")->default_val(512);
    cmd->add_option("-H,--height", height, "Image height in pixels")->default_val(512);
    cmd->add_option("-l,--lod", lodIndex, "LOD level to render")->default_val(0);
    cmd->add_option("--view", view, "View: front, back, top, bottom, right, left, 3d, quad")->default_val("front");

    cmd->callback(
        [&]()
        {
            Poseidon::ModelPreviewOptions opts;
            opts.width = width;
            opts.height = height;
            opts.lodIndex = lodIndex;
            opts.view = view;

            auto preview = Poseidon::PreviewModel(inputPath, opts);
            if (!preview.valid())
            {
                std::cerr << "Error: Failed to render model: " << inputPath << std::endl;
                throw CLI::RuntimeError(1);
            }

            if (!preview.saveToFile(outputPath))
            {
                std::cerr << "Error: Failed to save: " << outputPath << std::endl;
                throw CLI::RuntimeError(1);
            }

            std::cout << "Rendered: " << inputPath << " (LOD " << lodIndex << ", " << view << " view, " << width << "x"
                      << height << ") -> " << outputPath << std::endl;
        });
}

static void setupModelShow(CLI::App& model)
{
    auto* cmd = model.add_subcommand("show", "Display P3D model wireframe in a window");
    static std::string inputPath;
    static std::string screenshotPath;
    static int lodIndex = 0;
    static std::string view = "front";

    cmd->add_option("input", inputPath, "Input P3D file path")->required()->check(CLI::ExistingFile);
    cmd->add_option("--screenshot", screenshotPath, "Save screenshot to file and exit");
    cmd->add_option("-l,--lod", lodIndex, "LOD level to render")->default_val(0);
    cmd->add_option("--view", view, "View: front, back, top, bottom, right, left, 3d, quad")->default_val("front");

    cmd->callback(
        [&]()
        {
            int imgW = (view == "quad") ? 900 : 800;
            int imgH = imgW;

            Poseidon::ModelPreviewOptions opts;
            opts.width = imgW;
            opts.height = imgH;
            opts.lodIndex = lodIndex;
            opts.view = view;

            if (!screenshotPath.empty())
            {
                auto preview = Poseidon::PreviewModel(inputPath, opts);
                if (!preview.valid())
                {
                    std::cerr << "Error: Failed to render model" << std::endl;
                    throw CLI::RuntimeError(1);
                }
                if (!preview.saveToFile(screenshotPath))
                    throw CLI::RuntimeError(1);
                std::cout << "Screenshot: " << screenshotPath << " (" << imgW << "x" << imgH << ")" << std::endl;
                return;
            }

            auto preview = Poseidon::PreviewModel(inputPath, opts);
            if (!preview.valid())
            {
                std::cerr << "Error: Failed to render model" << std::endl;
                throw CLI::RuntimeError(1);
            }

            char title[256];
            std::snprintf(title, sizeof(title), "PoseidonTools - %s (LOD %d, %s)", inputPath.c_str(), lodIndex,
                          view.c_str());
            std::string viewCopy = view;
            std::string pathCopy = inputPath;
            int lodCopy = lodIndex;
            DisplayWindowRGB(title, imgW, imgH, preview.data.data(),
                             [pathCopy, lodCopy, viewCopy](int w, int h) -> std::vector<uint8_t>
                             {
                                 Poseidon::ModelPreviewOptions resizeOpts;
                                 resizeOpts.width = w;
                                 resizeOpts.height = h;
                                 resizeOpts.lodIndex = lodCopy;
                                 resizeOpts.view = viewCopy;
                                 auto resized = Poseidon::PreviewModel(pathCopy, resizeOpts);
                                 return resized.valid() ? std::move(resized.data) : std::vector<uint8_t>{};
                             });
        });
}

void ModelCommand::Setup(CLI::App& app)
{
    auto* model = app.add_subcommand("model", "P3D model operations");
    model->require_subcommand(1);

    setupModelInspect(*model);
    setupModelConvert(*model);
    setupModelRadarHMMWV(*model);
    setupModelRender(*model);
    setupModelShow(*model);
}

} // namespace PoseidonTools
