/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once

// P1-1 UI-independent capture analysis domain model
// --------------------------------------------------
// This header defines the *analysis* vocabulary that sits on top of the P0
// transport/DTO layer (MCPTypes.h) and the P0 capture adapter
// (MCPQueryAdapter.h). Everything here is deliberately decoupled from Qt
// widgets, the CaptureContext, and IReplayController: the structs are plain
// data, and the builders take already-materialised replay data.
//
// The model exists so that the analysis tools ported from the VS Code extension
// (frame analysis, pass graph, hot-event analysis, resource audit, pipeline
// diff, reports) can all be expressed against ONE canonical set of types instead
// of each re-deriving its own ad-hoc JSON. It unifies:
//
//   * events            - EventNode / EventCounts      (flattened action tree)
//   * passes            - PassInfo / PassGraph         (producer/consumer graph)
//   * resource accesses - ResourceAccess / ResourceUsage
//   * shaders           - ShaderRef
//   * timings           - EventTiming
//   * evidence          - EvidenceRef + Confidence     (confirmed/inferred/toVerify)
//
// Every conclusion a tool emits can be traced back to an EvidenceRef, and every
// EvidenceRef is tagged with its provenance so a client can tell a fact read
// straight from the capture apart from a heuristic judgement. This is the native
// analogue of the plugin's "evidence path" and confirmed-vs-inferred distinction.
//
// Layering (unchanged from P0): MCPServer -> MCPToolRegistry -> MCPQueryAdapter.
// This model is a peer of MCPTypes.h - both are consumed by the tool layer, and
// neither talks to the UI, the VS Code extension, or any external process.

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <stdint.h>

// Pulls in ActionDescription / ActionFlags / SDFile / ResourceId / the resource
// description structs / ResourceType - the same replay surface the rest of the
// AI module uses.
#include "renderdoc_replay.h"

struct ICaptureContext;
struct IReplayController;

namespace MCP
{
// CaptureSnapshot lives in MCPQueryAdapter.h; forward declared here so the model
// does not have to drag the adapter header into every consumer.
struct CaptureSnapshot;

//
// ---- Evidence and confidence ---------------------------------------------
//

// Provenance of a single claim. "Confirmed" means the value was read directly
// from the capture or replay (a fact). "Inferred" means it was derived by a
// heuristic (a conclusion the tool reached). "ToVerify" marks something the tool
// could not resolve and that the user or a follow-up query should confirm -
// typically because the relevant replay call was unavailable.
enum class Confidence
{
  Confirmed,
  Inferred,
  ToVerify,
};

// Stable string name ("confirmed" / "inferred" / "toVerify"), used in JSON so
// clients can branch on an identifier rather than an ordinal.
QString ConfidenceName(Confidence c);

// A pointer back to the raw data that supports a conclusion. Only the fields
// relevant to `kind` are populated; the rest stay empty so the JSON stays small.
struct EvidenceRef
{
  // "event" | "resource" | "shader" | "timing" | "capture" | "pipeline" | "mesh"
  QString kind;
  uint32_t eventId = 0;         // for kind == "event" / "timing"
  uint64_t resourceValue = 0;   // raw ResourceId value, for kind == "resource"/"shader"
  QString resourceName;         // human readable resource name, may be empty
  QString shaderStage;          // "vs"/"ps"/... for kind == "shader"
  QString label;                // free-form caption
  double durationUs = 0.0;      // for kind == "timing"
  Confidence confidence = Confidence::Confirmed;
};

// Small factories so call sites never have to remember which fields to fill.
EvidenceRef CaptureEvidence(const QString &capturePath);
EvidenceRef EventEvidence(uint32_t eventId, const QString &name = QString());
EvidenceRef ResourceEvidence(ResourceId id, const QString &name = QString());
EvidenceRef ShaderEvidence(ResourceId id, const QString &stage = QString(),
                           const QString &name = QString());
EvidenceRef TimingEvidence(uint32_t eventId, double durationUs, const QString &name = QString());

// Serialise an evidence reference. Emits only the keys relevant to `kind`.
QJsonObject EvidenceToJson(const EvidenceRef &ev);

//
// ---- Unified events -------------------------------------------------------
//

// A flattened view of one action in the capture's action tree. The tree is
// walked once into a pre-order list so analysis code can iterate without
// recursing (and without holding a stack of ActionDescription pointers).
struct EventNode
{
  uint32_t eventId = 0;
  uint32_t actionId = 0;
  uint32_t parentEventId = 0;   // 0 for a root action
  uint32_t depth = 0;           // 0 for a root action
  uint32_t childCount = 0;
  ActionFlags flags = ActionFlags::NoFlags;
  QString name;                 // from ActionDescription::GetName(structuredFile)
  uint32_t numIndices = 0;
  uint32_t numInstances = 1;
};

// Aggregate effect counts for a frame. "draws" counts Drawcall actions,
// "dispatches" counts Dispatch + MeshDispatch, "clears" counts Clear, and
// "markers" counts PushMarker/PopMarker/SetMarker. These are all facts read
// straight from the flags, so a caller can treat them as Confirmed.
struct EventCounts
{
  uint32_t total = 0;
  uint32_t draws = 0;
  uint32_t dispatches = 0;
  uint32_t clears = 0;
  uint32_t copies = 0;
  uint32_t presents = 0;
  uint32_t markers = 0;
};

// True if the flags describe a concrete GPU effect (a draw, dispatch, mesh
// dispatch, or clear) as opposed to a marker or a pure container. Replaces the
// plugin's string-flag `isRealDrawCall` heuristic with an exact bit test.
bool IsRealEffect(ActionFlags flags);

// True if the action is a marker group: it has children but is not itself a real
// effect. Matches the plugin's `isMarkerGroup` shape (children && !realEffect).
bool IsMarkerGroup(const ActionDescription &action);

// Walk `roots` into a flat pre-order list. `out` is appended to (not cleared) so
// callers can accumulate across multiple root sets.
void FlattenEvents(const rdcarray<ActionDescription> &roots, const SDFile &sdfile,
                   rdcarray<EventNode> &out);

// Count effects across a flattened event list.
EventCounts CountEffects(const rdcarray<EventNode> &events);

QJsonObject EventNodeToJson(const EventNode &node);
QJsonObject EventCountsToJson(const EventCounts &counts);

//
// ---- Resource accesses, shaders and timings -------------------------------
//

// How a resource is used at a particular event. This is the unified vocabulary
// shared by the pass graph, lifetime analysis and reverse search.
enum class ResourceUsage
{
  Unknown,
  RenderTarget,
  DepthTarget,
  Sampled,
  Constant,
  ReadWrite,
  Vertex,
  Index,
  Indirect,
};

QString ResourceUsageName(ResourceUsage usage);

// One resource touched by one event.
struct ResourceAccess
{
  ResourceId id;
  QString name;
  ResourceUsage usage = ResourceUsage::Unknown;
  uint32_t eventId = 0;
  Confidence confidence = Confidence::Confirmed;
};

// A shader bound as part of a pipeline state. `stage` is the lower-case stage
// name the plugin used ("vs", "ps", "cs", ...); empty if not known.
struct ShaderRef
{
  ResourceId id;
  QString name;
  QString stage;
  Confidence confidence = Confidence::Inferred;
};

// A per-event duration. Confirmed when it comes from the capture's real GPU
// timing counters, Inferred when it is derived from a parent marker's total.
struct EventTiming
{
  uint32_t eventId = 0;
  QString name;
  double durationUs = 0.0;
  Confidence confidence = Confidence::Confirmed;
};

//
// ---- Frame analysis -------------------------------------------------------
//

// Capture-level file/frame statistics, mirroring FrameDescription.
struct FrameStatistics
{
  uint32_t frameNumber = 0;
  uint64_t uncompressedFileSize = 0;
  uint64_t compressedFileSize = 0;
  uint64_t persistentSize = 0;
  uint64_t initDataSize = 0;
  uint64_t captureTime = 0;   // microseconds since the unix epoch, 0 if unknown
};

void BuildFrameStatistics(const FrameDescription &frame, FrameStatistics &out);

// A texture flagged as large by the frame analysis.
struct LargeTexture
{
  QString resourceId;   // "ResourceId::<n>"
  QString name;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
  uint32_t mips = 1;
  uint32_t arraySize = 1;
  QString format;
  uint64_t byteSize = 0;
};

// Resource counts and memory totals for a frame.
struct ResourceInventory
{
  uint32_t textureCount = 0;
  uint32_t bufferCount = 0;
  uint32_t shaderCount = 0;
  uint32_t largeTextureCount = 0;
  QVector<LargeTexture> largeTextures;
  uint64_t totalTextureMemory = 0;
  uint64_t totalBufferMemory = 0;
};

// A heuristic finding. Always tagged with a Confidence so clients can separate
// measured facts from threshold-based judgements.
struct PotentialIssue
{
  QString severity;   // "info" | "warning"
  QString code;       // stable identifier, e.g. "high-draw-count"
  Confidence confidence = Confidence::Inferred;
  QString message;
  QVector<EvidenceRef> evidence;
};

// Tunable thresholds for the frame analysis. Defaults match the dispositions
// used by the VS Code extension (>500 draws, >10 clears, 4K+ / >256MiB textures).
struct FrameAnalysisOptions
{
  QString driver;
  QString rdocVersion;
  uint32_t largeTextureMinDimension = 4096;
  uint64_t largeTextureMinBytes = 256ull * 1024ull * 1024ull;
  int maxLargeTextures = 16;
  uint32_t drawIssueThreshold = 500;
  uint32_t clearIssueThreshold = 10;
};

// The whole-frame analysis result. This is the native analogue of the plugin's
// `buildFrameAnalysis` output, reshaped around the native resource split.
struct FrameAnalysis
{
  QString api;
  QString driver;
  QString rdocVersion;
  FrameStatistics statistics;
  EventCounts counts;
  ResourceInventory resources;
  QVector<PotentialIssue> potentialIssues;
};

// Build a frame analysis from already-materialised capture data. `frame` is
// optional: pass NULL when no FrameDescription is available and the statistics
// block is left zeroed. No replay calls are issued here.
void BuildFrameAnalysis(const CaptureSnapshot &snapshot, const rdcarray<ActionDescription> &roots,
                        const SDFile &sdfile, const rdcarray<ResourceDescription> &resources,
                        const rdcarray<TextureDescription> &textures,
                        const rdcarray<BufferDescription> &buffers, FrameAnalysis &out,
                        const FrameAnalysisOptions &options = FrameAnalysisOptions(),
                        const FrameDescription *frame = NULL);

QJsonObject FrameAnalysisToJson(const FrameAnalysis &analysis);

//
// ---- Pass graph -----------------------------------------------------------
//

// One pass: a named region of the frame with its event range, effect count,
// duration, the render targets it writes and the textures it samples. Populated
// by the pass-graph builder in P1-3; defined here so the model is complete.
struct PassInfo
{
  QString id;             // stable id, built from name + event range
  QString name;
  uint32_t eventIdStart = 0;
  uint32_t eventIdEnd = 0;
  uint32_t drawCount = 0;
  double durationUs = 0.0;
  QVector<ResourceId> renderTargets;
  QVector<ResourceId> sampledTextures;
  QVector<ShaderRef> shaders;
  Confidence confidence = Confidence::Inferred;
};

// A producer -> consumer edge discovered by matching a pass's render target to a
// later pass's sampled texture.
struct PassGraphEdge
{
  QString fromPassId;
  QString fromPassName;
  QString toPassId;
  QString toPassName;
  ResourceId resourceId;
  QString resourceName;
  QString usage;   // "sampledFromRenderTarget"
  Confidence confidence = Confidence::Inferred;
};

struct PassGraph
{
  QVector<PassInfo> passes;
  QVector<PassGraphEdge> edges;
  double totalDurationUs = 0.0;
};

QJsonObject PassInfoToJson(const PassInfo &pass);
QJsonObject PassGraphToJson(const PassGraph &graph);

}    // namespace MCP
