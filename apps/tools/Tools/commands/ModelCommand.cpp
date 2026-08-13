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
    // UVs and normals are stored per vertex.  A box therefore cannot share its
    // eight corners between faces: doing so would force every side to sample
    // the same texture coordinate and have the same lighting normal.  This was
    // particularly visible on the radar panel and launcher pylons.
    std::vector<uint32_t> v;
    v.reserve(24);

    const auto point = [&](int x, int y, int z) {
        return P3DM::Vector3{center.x + x * half.x, center.y + y * half.y, center.z + z * half.z};
    };
    const auto addFace = [&](const P3DM::Vector3& normal, const P3DM::Vector3& a, const P3DM::Vector3& b,
                             const P3DM::Vector3& c, const P3DM::Vector3& d) {
        const uint32_t ia = addVertex(mesh, a, normal, 0.0f, 0.0f);
        const uint32_t ib = addVertex(mesh, b, normal, 1.0f, 0.0f);
        const uint32_t ic = addVertex(mesh, c, normal, 1.0f, 1.0f);
        const uint32_t id = addVertex(mesh, d, normal, 0.0f, 1.0f);
        v.insert(v.end(), {ia, ib, ic, id});
        addQuad(mesh, ia, ib, ic, id, material, faceIndex);
    };

    // Counter-clockwise winding viewed from outside.
    addFace({0, -1, 0}, point(-1, -1, -1), point(1, -1, -1), point(1, -1, 1), point(-1, -1, 1));
    addFace({0, 1, 0}, point(-1, 1, -1), point(-1, 1, 1), point(1, 1, 1), point(1, 1, -1));
    addFace({0, 0, -1}, point(-1, -1, -1), point(-1, 1, -1), point(1, 1, -1), point(1, -1, -1));
    addFace({0, 0, 1}, point(-1, -1, 1), point(1, -1, 1), point(1, 1, 1), point(-1, 1, 1));
    addFace({-1, 0, 0}, point(-1, -1, -1), point(-1, -1, 1), point(-1, 1, 1), point(-1, 1, -1));
    addFace({1, 0, 0}, point(1, -1, -1), point(1, 1, -1), point(1, 1, 1), point(1, -1, 1));
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

    // HMMWV shapes are loaded with `reversed=1`, which mirrors X/Z at
    // runtime.  Author the banks toward -Z so their runtime direction is the
    // vehicle's forward +Z. Both banks start at 45 degrees elevation.
    const P3DM::Vector3 forwardUp{0.0f, std::sin(kPi / 4.0f), -std::cos(kPi / 4.0f)};
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

// Keep the legacy LODShape representation when writing the generated model.
// SaveOptimized faithfully round-trips its original per-face texture objects,
// while serializing an ODOL through the generic Model/ShapeAdapter path loses
// the original material sections and causes texture pages to be assigned to
// unrelated HMMWV faces.
static int addLegacyVertex(Shape& mesh, Vector3Par pos, Vector3Par normal, float u = 0.0f, float v = 0.0f)
{
    return mesh.AddVertexFast(pos, normal, ClipAll, u, v);
}

static void addLegacyFace(Shape& mesh, const std::vector<int>& vertices, Texture* texture, int special = 0)
{
    Poly face;
    face.Init();
    face.SetN(static_cast<VertexIndex>(vertices.size()));
    for (int i = 0; i < static_cast<int>(vertices.size()); ++i)
    {
        face.Set(i, static_cast<VertexIndex>(vertices[static_cast<size_t>(i)]));
    }
    face.SetTexture(texture);
    face.SetSpecial(special);
    mesh.AddFace(face);
}

static std::vector<int> addLegacyBox(Shape& mesh, Vector3Par center, Vector3Par half, Texture* texture)
{
    std::vector<int> allVertices;
    allVertices.reserve(24);
    const auto point = [&](float x, float y, float z) {
        return Vector3(center.X() + x * half.X(), center.Y() + y * half.Y(), center.Z() + z * half.Z());
    };
    const auto addFace = [&](Vector3Par normal, Vector3Par a, Vector3Par b, Vector3Par c, Vector3Par d) {
        const int ia = addLegacyVertex(mesh, a, normal, 0.0f, 0.0f);
        const int ib = addLegacyVertex(mesh, b, normal, 1.0f, 0.0f);
        const int ic = addLegacyVertex(mesh, c, normal, 1.0f, 1.0f);
        const int id = addLegacyVertex(mesh, d, normal, 0.0f, 1.0f);
        allVertices.insert(allVertices.end(), {ia, ib, ic, id});
        addLegacyFace(mesh, {ia, ib, ic, id}, texture);
    };

    addFace(Vector3(0, -1, 0), point(-1, -1, -1), point(1, -1, -1), point(1, -1, 1), point(-1, -1, 1));
    addFace(Vector3(0, 1, 0), point(-1, 1, -1), point(-1, 1, 1), point(1, 1, 1), point(1, 1, -1));
    addFace(Vector3(0, 0, -1), point(-1, -1, -1), point(-1, 1, -1), point(1, 1, -1), point(1, -1, -1));
    addFace(Vector3(0, 0, 1), point(-1, -1, 1), point(1, -1, 1), point(1, 1, 1), point(-1, 1, 1));
    addFace(Vector3(-1, 0, 0), point(-1, -1, -1), point(-1, -1, 1), point(-1, 1, 1), point(-1, 1, -1));
    addFace(Vector3(1, 0, 0), point(1, -1, -1), point(1, 1, -1), point(1, 1, 1), point(1, -1, 1));
    return allVertices;
}

static std::vector<int> addLegacyCylinder(Shape& mesh, Vector3Par start, Vector3Par end, float radius, int sides,
                                          Texture* texture)
{
    const Vector3 axis = (end - start).Normalized();
    const Vector3 reference = fabs(axis.Y()) < 0.95f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
    const Vector3 right = axis.CrossProduct(reference).Normalized();
    const Vector3 up = right.CrossProduct(axis).Normalized();
    std::vector<int> vertices;
    vertices.reserve(static_cast<size_t>(sides) * 2 + 2);
    for (int ring = 0; ring < 2; ++ring)
    {
        const Vector3 center = ring == 0 ? start : end;
        for (int i = 0; i < sides; ++i)
        {
            const float angle = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(sides);
            const Vector3 radial = right * cos(angle) + up * sin(angle);
            vertices.push_back(addLegacyVertex(mesh, center + radial * radius, radial,
                                                static_cast<float>(i) / static_cast<float>(sides),
                                                static_cast<float>(ring)));
        }
    }
    const int c0 = addLegacyVertex(mesh, start, -axis, 0.5f, 0.5f);
    const int c1 = addLegacyVertex(mesh, end, axis, 0.5f, 0.5f);
    for (int i = 0; i < sides; ++i)
    {
        const int j = (i + 1) % sides;
        const int a = vertices[static_cast<size_t>(i)];
        const int b = vertices[static_cast<size_t>(j)];
        const int c = vertices[static_cast<size_t>(sides + j)];
        const int d = vertices[static_cast<size_t>(sides + i)];
        addLegacyFace(mesh, {a, b, c, d}, texture);
        addLegacyFace(mesh, {c0, b, a}, texture);
        addLegacyFace(mesh, {c1, d, c}, texture);
    }
    return vertices;
}

static void addLegacySelection(Shape& mesh, const char* name, const std::vector<int>& vertices,
                               const std::vector<int>& faces = {})
{
    std::vector<Poseidon::SelInfo> points;
    points.reserve(vertices.size());
    for (const int vertex : vertices)
    {
        points.emplace_back(static_cast<VertexIndex>(vertex), 255);
    }
    std::vector<VertexIndex> faceIndices;
    faceIndices.reserve(faces.size());
    for (const int face : faces)
    {
        faceIndices.push_back(static_cast<VertexIndex>(face));
    }
    mesh.AddNamedSel(Poseidon::NamedSelection(name, points.empty() ? nullptr : points.data(),
                                               static_cast<int>(points.size()),
                                               faceIndices.empty() ? nullptr : faceIndices.data(),
                                               static_cast<int>(faceIndices.size())));
}

static void appendLegacyRadarHMMWVVisuals(Shape& mesh, unsigned detail)
{
    Texture* black = mesh.FindTexture("data\\blck_sum.pac");
    if (!black)
        black = mesh.NTextures() > 0 ? mesh.GetTexture(0) : nullptr;
    // One known-good opaque stock material keeps the newly-authored rails
    // stable across every HMMWV visual LOD. Some of the old camouflage pages
    // are not safely reusable in a standalone generated ODOL.
    Texture* green = black;

    std::vector<int> launcherVertices;
    // HMMWV shapes are loaded reversed at runtime. Authoring toward -Z makes
    // the launch rails and missile proxies point forward (+Z) in the game.
    const Vector3 forwardUp(0.0f, sin(kPi / 4.0f), -cos(kPi / 4.0f));
    const Vector3 axisCenter(0.0f, 1.82f, 0.08f);

    // The visible Mavericks define the rack silhouette.  Keep the parent
    // beam deliberately face-free: ODOL's legacy material writer can turn a
    // newly authored opaque beam into a white full-size box at runtime.

    int proxyNumber = 1;
    for (const int sideSign : {-1, 1})
    {
        const float sideX = static_cast<float>(sideSign) * 1.26f;

        for (int rail = 0; rail < 4; ++rail)
        {
            const float column = (rail & 1) ? 0.13f : -0.13f;
            const float row = rail < 2 ? 0.11f : -0.11f;
            const Vector3 railCenter(sideX + column, 1.89f + row, 0.11f);

            const Vector3 proxyOrigin = railCenter - forwardUp * 0.38f;
            const Vector3 proxyUp = forwardUp.CrossProduct(Vector3(1, 0, 0)).Normalized() * 0.17f;
            const int p0 = addLegacyVertex(mesh, proxyOrigin, proxyUp, 0.0f, 0.0f);
            const int p1 = addLegacyVertex(mesh, proxyOrigin + forwardUp * 0.10f, proxyUp, 1.0f, 0.0f);
            const int p2 = addLegacyVertex(mesh, proxyOrigin + proxyUp, proxyUp, 0.0f, 1.0f);
            launcherVertices.insert(launcherVertices.end(), {p0, p1, p2});
            const int proxyFace = mesh.NFaces();
            addLegacyFace(mesh, {p0, p1, p2}, nullptr, IsHiddenProxy | NoTexMerger);

            char selectionName[64];
            std::snprintf(selectionName, sizeof(selectionName), "proxy:cwr_maverick.%02d", proxyNumber++);
            addLegacySelection(mesh, selectionName, {p0, p1, p2}, {proxyFace});
        }
    }

    // The radar geometry is appended to the standalone rack model below.

    addLegacySelection(mesh, "launcher_bank", launcherVertices);
    mesh.FindSections();
    mesh.CalculateMinMax();
    mesh.StoreOriginalMinMax();
}

static void appendLegacyLauncherAxis(Shape& mesh)
{
    const int first = addLegacyVertex(mesh, Vector3(-0.50f, 1.82f, 0.08f), VUp);
    const int second = addLegacyVertex(mesh, Vector3(0.50f, 1.82f, 0.08f), VUp);
    addLegacySelection(mesh, "launcher_axis", {first, second});
    mesh.CalculateMinMax();
    mesh.StoreOriginalMinMax();
}

// Keep the original HMMWV mesh out of this generated P3D.  The legacy ODOL
// writer can faithfully handle our simple new geometry, but some original
// HMMWV visual LODs use shared vertices with texture-coordinate layouts the
// writer cannot round-trip.  Rendering the stock model as a proxy leaves its
// bytes (and therefore all body/wheel material mappings) completely intact.
static void addLegacyHMMWVProxy(Shape& mesh)
{
    // The parent vehicle and the stock HMMWV proxy are both reversed. Author
    // +Z here so the parent reversal gives the proxy -Z, cancelling the
    // child's own reversal and preserving the original vehicle orientation.
    const Vector3 origin(0.0f, 0.0f, 0.0f);
    const Vector3 forward(0.0f, 0.0f, 0.10f);
    const Vector3 up(0.0f, 0.10f, 0.0f);
    const int p0 = addLegacyVertex(mesh, origin, VUp);
    const int p1 = addLegacyVertex(mesh, origin + forward, VUp);
    const int p2 = addLegacyVertex(mesh, origin + up, VUp);
    const int proxyFace = mesh.NFaces();
    addLegacyFace(mesh, {p0, p1, p2}, nullptr, IsHiddenProxy | NoTexMerger);
    // GReplaceProxies resolves proxy:plainhmmwv to the hidden ProxyPlainHMMWV class,
    // which inherits the original model and its reversed=1 setting.
    addLegacySelection(mesh, "proxy:plainhmmwv.01", {p0, p1, p2}, {proxyFace});
}

static Shape* makeLegacyRadarHMMWVVisual(const Shape& sourceVisual, Texture* sharedBlack, unsigned detail)
{
    auto* visual = new Shape();
    // The attachments borrow only texture references from the stock model;
    // no stock vertex or face data is copied into this P3D.
    visual->AddTextureUnique(sharedBlack ? sharedBlack : sourceVisual.FindTexture("data\\blck_sum.pac"));
    addLegacyHMMWVProxy(*visual);
    appendLegacyRadarHMMWVVisuals(*visual, detail);
    visual->FindSections();
    visual->CalculateMinMax();
    visual->StoreOriginalMinMax();
    return visual;
}

static Shape* makeLegacyMountedMaverick(Texture* texture, unsigned detail)
{
    auto* missile = new Shape();
    missile->AddTextureUnique(texture);

    // This is deliberately a compact store model, not the fired Maverick
    // mesh. The latter includes long rocket-flame surfaces and is only used
    // after launch. +Z is the model-forward convention used by the proxy.
    addLegacyCylinder(*missile, Vector3(0.0f, 0.0f, -1.05f), Vector3(0.0f, 0.0f, 1.05f), 0.095f,
                      detail == 0 ? 10 : 6, texture);
    addLegacyBox(*missile, Vector3(0.0f, 0.0f, -0.62f), Vector3(0.43f, 0.018f, 0.19f), texture);
    addLegacyBox(*missile, Vector3(0.0f, 0.0f, 0.58f), Vector3(0.30f, 0.014f, 0.14f), texture);
    missile->FindSections();
    missile->CalculateMinMax();
    missile->StoreOriginalMinMax();
    return missile;
}

static Shape* makeLegacyMountedRack(Texture* texture, unsigned detail, float elevation)
{
    auto* rack = new Shape();
    rack->AddTextureUnique(texture);

    const Vector3 forwardUp(0.0f, sin(elevation), cos(elevation));
    const Vector3 axisCenter(0.0f, 1.82f, 0.08f);
    addLegacyBox(*rack, axisCenter, Vector3(1.42f, 0.07f, 0.08f), texture);

    for (const int sideSign : {-1, 1})
    {
        const float sideX = static_cast<float>(sideSign) * 1.26f;
        addLegacyBox(*rack, Vector3(sideX, 1.88f, 0.10f), Vector3(0.15f, 0.21f, 0.20f), texture);
        for (int rail = 0; rail < 4; ++rail)
        {
            const float column = (rail & 1) ? 0.13f : -0.13f;
            const float row = rail < 2 ? 0.11f : -0.11f;
            const Vector3 railCenter(sideX + column, 1.89f + row, 0.11f);
            addLegacyCylinder(*rack, railCenter - forwardUp * 0.70f, railCenter + forwardUp * 0.70f,
                              detail < 2 ? 0.095f : 0.08f, detail == 0 ? 8 : 6, texture);
            // Compact visible store: 2.1 m long and matched to the selected
            // rack elevation. The projectile itself is still spawned by the
            // real Maverick launcher.
            addLegacyCylinder(*rack, railCenter - forwardUp * 1.05f, railCenter + forwardUp * 1.05f, 0.095f,
                              detail == 0 ? 10 : 6, texture);
            const Vector3 right(1.0f, 0.0f, 0.0f);
            const Vector3 up = forwardUp.CrossProduct(right).Normalized();
            const Vector3 finCenter = railCenter - forwardUp * 0.62f;
            addLegacyBox(*rack, finCenter, Vector3(0.34f, 0.015f, 0.15f), texture);
            (void)up;
        }
    }

    // Fixed radar mast and panel, intentionally independent from the
    // elevation setting.
    addLegacyCylinder(*rack, Vector3(0.0f, 1.78f, -0.32f), Vector3(0.0f, 2.72f, -0.32f), 0.075f,
                      detail == 0 ? 10 : 6, texture);
    addLegacyCylinder(*rack, Vector3(0.0f, 2.64f, -0.32f), Vector3(0.0f, 2.79f, -0.32f), 0.27f,
                      detail == 0 ? 12 : 8, texture);
    addLegacyBox(*rack, Vector3(0.0f, 3.00f, -0.32f), Vector3(0.68f, 0.32f, 0.055f), texture);
    addLegacyBox(*rack, Vector3(0.0f, 3.33f, -0.32f), Vector3(0.75f, 0.035f, 0.09f), texture);
    addLegacyBox(*rack, Vector3(0.0f, 2.67f, -0.32f), Vector3(0.75f, 0.035f, 0.09f), texture);
    rack->FindSections();
    rack->CalculateMinMax();
    rack->StoreOriginalMinMax();
    return rack;
}

static void writeLegacyMountedRack(const std::filesystem::path& path, Texture* texture, float elevation)
{
    std::unique_ptr<LODShapeWithShadow> rack(new LODShapeWithShadow());
    for (unsigned detail = 0; detail < 3; ++detail)
        rack->AddShape(makeLegacyMountedRack(texture, detail, elevation), 1.0f + detail * 2.0f);
    rack->CalculateMinMax(true);
    rack->SaveOptimized(path.string().c_str());
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
            std::unique_ptr<LODShapeWithShadow> source(new LODShapeWithShadow());
            if (!source->LoadOptimized(inputPath.c_str()))
            {
                std::cerr << "Error: Failed to load base ODOL: " << inputPath << std::endl;
                throw CLI::RuntimeError(1);
            }

            std::unique_ptr<LODShapeWithShadow> generated(new LODShapeWithShadow());
            Texture* sharedBlack = source->FindTexture("data\\blck_sum.pac");
            unsigned visualIndex = 0;
            for (int i = 0; i < source->NLevels(); ++i)
            {
                const float resolution = source->Resolution(i);
                if (resolution < 1000.0f)
                {
                    generated->AddShape(makeLegacyRadarHMMWVVisual(*source->Level(i), sharedBlack, visualIndex),
                                        resolution);
                    ++visualIndex;
                }
                else if (resolution >= 1.0e13f)
                {
                    // Preserve only collision, memory, path and shadow data.
                    // Cockpit/visual LODs are intentionally excluded so no
                    // stock textured faces are ever reserialized.
                    generated->AddShape(new Shape(*source->Level(i)), resolution);
                }
            }

            Shape* memory = generated->MemoryLevel();
            if (!memory)
            {
                std::cerr << "Error: Base ODOL has no valid Memory LOD index" << std::endl;
                throw CLI::RuntimeError(1);
            }
            appendLegacyLauncherAxis(*memory);
            generated->RescanProxies();
            // Rebuild section ranges only after proxy faces are marked hidden.  This
            // keeps the base model's original per-face texture assignments intact.
            for (int i = 0; i < generated->NLevels(); ++i)
                generated->Level(i)->FindSections();
            generated->CalculateMinMax(true);

            std::filesystem::path output(outputPath);
            if (!output.parent_path().empty())
                std::filesystem::create_directories(output.parent_path());

            generated->SaveOptimized(outputPath.c_str());

            // Write the matching compact Maverick store model next to the
            // vehicle. It is selected only while a round remains on a rack;
            // launch still spawns the original Maverick projectile model.
            std::unique_ptr<LODShapeWithShadow> mountedMaverick(new LODShapeWithShadow());
            for (unsigned detail = 0; detail < 3; ++detail)
                mountedMaverick->AddShape(makeLegacyMountedMaverick(sharedBlack, detail), 1.0f + detail * 2.0f);
            mountedMaverick->CalculateMinMax(true);
            const std::filesystem::path mountedMaverickPath = output.parent_path() / "cwr_maverick_proxy.p3d";
            mountedMaverick->SaveOptimized(mountedMaverickPath.string().c_str());
            writeLegacyMountedRack(output.parent_path() / "cwr_radar_rack_45.p3d", sharedBlack, kPi / 4.0f);
            writeLegacyMountedRack(output.parent_path() / "cwr_radar_rack_90.p3d", sharedBlack, kPi / 2.0f);

            std::cout << "Generated radar HMMWV: " << outputPath << std::endl;
            std::cout << "  Visual LODs modified: " << visualIndex << std::endl;
            std::cout << "  Per visual LOD: 2 x four-rail banks, 8 Maverick proxies, radar mast/panel, launcher_bank"
                      << std::endl;
            std::cout << "  Memory LOD: launcher_axis along +X" << std::endl;
            std::cout << "  Initial elevation: forward 45 degrees; rotate launcher_bank +45 degrees about launcher_axis for vertical"
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
