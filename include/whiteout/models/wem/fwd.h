// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file fwd.h
 * @brief Forward declarations of WEM's types, for headers that only name them.
 *
 * A header whose functions take a `Document&` or return a `BakeRun*` does not
 * need the document model, which is some forty headers: it needs the names.
 * Include this there, and the defining header in the source file that reads
 * the type. Each name is listed under the header that defines it.
 */

namespace whiteout {
namespace models {
namespace wem {

// document.h, model.h
struct Document;
struct Model;

// nodes/
class NodeTree;
struct Node;
struct Transform;

// geometry/mesh.h
class Mesh;

// materials/
class Material;
struct TextureRef;

// anim/
struct AnimChannel;
struct Clip;
struct KeyReduceOptions;
struct Mix;
struct PoseStage;
struct StageHooks;
class StageRunner;
struct TrackTarget;

// physics/
struct BakeReport;
struct BakeRun;
struct BodySwitch;

// rigging/
struct RebindResult;
struct RigDirections;
struct TPoseResult;
struct TPoseRules;
struct TPoseScore;

// converters.h, optimize.h, scene_ops.h
struct DrawnElements;
struct OptimizeOptions;
struct Placement;

} // namespace wem
} // namespace models
} // namespace whiteout
