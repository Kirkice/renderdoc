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

QJsonObject ResourceAccessToJson(const ResourceAccess &access);

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

// Serialise capture-level statistics. Exposed (not file-static) because the
// query adapter reuses the exact same shape for the Frame Summary tool.
QJsonObject FrameStatisticsToJson(const FrameStatistics &stats);

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

//
// ---- Reverse search -------------------------------------------------------
//

// One event that matched a reverse search, with the resources/shader stage it
// matched on and the evidence backing the match.
struct ReverseSearchHit
{
  uint32_t eventId = 0;
  QString name;
  ResourceId resource;                  // for resource matches; null otherwise
  QVector<ResourceAccess> resources;    // every matched resource touch (may be empty)
  Confidence confidence = Confidence::Confirmed;
};

// The result of a reverse search for one query string.
struct ReverseSearchResult
{
  QString query;                    // "shader" | "texture" | "resource"
  QString term;                     // the searched term
  uint32_t scannedEventCount = 0;   // leaf events that were inspected
  bool truncated = false;           // true when a scan cap stopped the search early
  QVector<ReverseSearchHit> hits;
};

QJsonObject ReverseSearchResultToJson(const ReverseSearchResult &result);

// A dependency edge between two passes discovered by the reverse resource
// search: `toEventId` samples a texture that `fromEventId` rendered.
struct ResourceEdge
{
  uint32_t fromEventId = 0;
  uint32_t toEventId = 0;
  ResourceId resource;
  QString resourceName;
  QString usage;   // "sampledFromRenderTarget"
  Confidence confidence = Confidence::Confirmed;
};

// The pass/resource dependency graph derived from the reverse index.
struct PassGraphResult
{
  uint32_t scannedEventCount = 0;
  bool truncated = false;
  QVector<ResourceEdge> edges;
};

QJsonObject PassGraphResultToJson(const PassGraphResult &result);

//
// ---- Resource memory audit ------------------------------------------------
//

// One resource as reported by the memory audit: its creation-time footprint and
// (for textures) the shape/format facts used by the audit summary.
struct ResourceMemoryRow
{
  ResourceId resourceId;
  QString name;
  QString type;                 // ResourceTypeName() e.g. "Texture"/"Buffer"
  QString textureType;          // TextureTypeName() for textures, else empty
  uint64_t byteSize = 0;        // creation-time byte size (0 if unknown)
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
  uint32_t mips = 0;
  uint32_t arraySize = 0;
  QString format;               // ResourceFormatString() for textures
  QString usage;                // "|"-joined creation flags (textures/buffers)
};

// Aggregate facts over the texture inventory, mirroring the plugin's textureAudit
// block: how many textures, their format histogram, whether mip chains and
// dimensions are consistent enough to reason about residency.
struct TextureAudit
{
  uint32_t count = 0;
  QVector<QPair<QString, uint32_t>> byFormat;   // (format name, count), desc
  uint32_t mipLevelsAvailable = 0;              // textures with mips > 1
  uint32_t missingMipChain = 0;                 // textures whose mips != ideal
  uint32_t fullMipChain = 0;                    // textures whose mips == ideal
  QVector<QString> dimensions;                  // distinct "WxHxD" strings
};

// The whole-capture memory audit. Facts (bytes, counts) are Confirmed; the
// notes call out heuristic observations that a client should not treat as ground
// truth.
struct ResourceMemoryAudit
{
  uint32_t totalResources = 0;
  uint64_t totalBytes = 0;
  double totalMiB = 0.0;
  uint32_t textureCount = 0;
  uint32_t bufferCount = 0;
  uint32_t shaderCount = 0;
  uint64_t totalTextureBytes = 0;
  uint64_t totalBufferBytes = 0;
  QVector<ResourceMemoryRow> largestResources;
  TextureAudit textures;
  QStringList notes;            // heuristic remarks, explicitly not facts
  Confidence confidence = Confidence::Confirmed;
};

QJsonObject ResourceMemoryAuditToJson(const ResourceMemoryAudit &audit);

//
// ---- Resource lifetime ----------------------------------------------------
//

// One resource's observed lifetime within the captured frame: every event that
// produced or consumed it, the first/last such event and how many events touched
// it. `observedEventCount == 0` means the resource was created but never touched
// by any pipeline-bound event in this frame.
struct ResourceLifetimeRow
{
  ResourceId resourceId;
  QString name;
  QString type;
  uint64_t byteSize = 0;
  uint32_t observedEventCount = 0;
  uint32_t firstEventId = 0;
  uint32_t lastEventId = 0;
  QVector<ResourceAccess> accesses;   // producer/consumer touches, event order
  Confidence confidence = Confidence::Confirmed;
};

struct ResourceLifetimeResult
{
  bool available = true;
  uint32_t scannedEventCount = 0;
  bool truncated = false;
  QVector<ResourceLifetimeRow> rows;
};

QJsonObject ResourceLifetimeToJson(const ResourceLifetimeResult &result);

//
// ---- Unused-resource identification ---------------------------------------
//

// A resource the pipeline never observed being touched in the frame. This is a
// heuristic: a resource legitimately read via an unbound descriptor, indirect
// access or outside the captured frame will also appear here.
struct UnusedResource
{
  ResourceId resourceId;
  QString name;
  QString type;
  uint64_t byteSize = 0;
};

struct UnusedResourceResult
{
  bool available = true;
  uint32_t scannedEventCount = 0;
  uint32_t candidateCount = 0;
  QVector<UnusedResource> candidates;
  Confidence confidence = Confidence::Low;
  QString limitation;
};

QJsonObject UnusedResourceResultToJson(const UnusedResourceResult &result);

//
// ---- Suspected resource leaks ---------------------------------------------
//

// A resource that is a candidate for a leak: created early, touched only
// transiently (or not at all after its first touch) and still resident at the end
// of the frame. This is a heuristic ranking, not a verdict.
struct LeakCandidate
{
  ResourceId resourceId;
  QString name;
  QString type;
  uint64_t byteSize = 0;
  uint32_t createdEventId = 0;      // first observed touch (0 = never touched)
  uint32_t lastEventId = 0;
  uint32_t observedEventCount = 0;
  QString reason;                   // human-readable heuristic reason
  Confidence confidence = Confidence::Low;
};

struct LeakResult
{
  bool available = true;
  uint32_t scannedEventCount = 0;
  uint32_t candidateCount = 0;
  QVector<LeakCandidate> candidates;
  Confidence confidence = Confidence::Low;
  QString limitation;
};

QJsonObject LeakResultToJson(const LeakResult &result);

//
// ---- P1-5 pipeline / shader snapshot and diff ----------------------------
//
// The ported plugin DiffPipelineStateTool compares the pipeline state of two
// events in the SAME capture. We reuse the native PipelineStateToJson() (the
// renderdoc_getPipelineState serialiser) as the "structured snapshot" and add a
// UI-independent diff model on top of it:
//
//   * a structured snapshot is just that JSON object, plus the enumeration of
//     the shader stages actually present (SnapshotStagesFromPipelineJson);
//   * the diff flattens both snapshots into dotted field paths and reports every
//     path whose value differs, with a compact before/after rendering for the
//     common case and the raw JSON values when a client needs to go deeper.
//
// PipelineStateToJson() output is pure data (other than the nested ResourceRef
// objects), so diffing it needs neither Qt widgets nor the replay controller.

// One flattened field that differs between the two events. `path` uses dotted
// notation into the snapshot JSON (e.g. "rasterizer.cullMode",
// "shaders.vertex.entryPoint", "outputTargets.0.resourceId"). `group` is the
// leading path segment so clients can filter ("shaders", "rasterizer", ...).
struct PipelineFieldChange
{
  QString path;
  QString group;
  QString before;    // compact human-readable rendering of the old value
  QString after;     // compact human-readable rendering of the new value
  QJsonValue beforeValue;    // raw JSON, for clients that need the details
  QJsonValue afterValue;
};

// A shader-stage binding change, denormalised out of the field list so callers
// can reason about program changes without parsing dotted paths. `before` and
// `after` are the shader display names (resource name, else entry point, else
// JSON) and are empty when the stage was unbound at that event.
struct PipelineShaderChange
{
  QString stage;     // "vertex" | "pixel" | ... (matches detail::ShaderStageName)
  QString before;
  QString after;
};

struct PipelineDiffResult
{
  bool available = true;
  uint32_t eventIdA = 0;
  uint32_t eventIdB = 0;
  bool hasChanges = false;

  // Stages the snapshot enumerates that were non-empty at either event, plus the
  // stages that were bound at only one event (reported in `changes`).
  QStringList inspectedStages;

  QVector<PipelineFieldChange> changes;
  QVector<PipelineShaderChange> shaderChanges;

  Confidence confidence = Confidence::Confirmed;
  QString limitation;
};

QJsonObject PipelineDiffResultToJson(const PipelineDiffResult &result);

// Enumerate the shader stages that are bound at either event's snapshot. Kept
// here (rather than inline in the adapter) so the stage set stays in one place
// and so the test can exercise it without a replay controller. Returns the
// stages in canonical order; the returned list may be empty.
QStringList SnapshotStagesFromPipelineJson(const QJsonObject &stateA, const QJsonObject &stateB);

}    // namespace MCP
