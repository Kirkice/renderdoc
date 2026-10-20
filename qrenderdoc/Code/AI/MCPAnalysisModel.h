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

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
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
// typically because the relevant replay call was unavailable. "Low" marks a
// heuristic ranking whose whole result is subject to a stated limitation (used by
// the resource-memory heuristics in P1-4): the individual rows are still facts,
// but the ordering/selection is a best-effort signal rather than a verdict.
enum class Confidence
{
  Confirmed,
  Inferred,
  ToVerify,
  Low,
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

//
// ---- P1-6 structured performance report ----------------------------------
//
// The ported plugin GeneratePerformanceReportTool ranks the frame's GPU-timed
// leaf events, aggregates those timings per pass and, for each hotspot, attaches
// the pipeline/shader/resource/mesh evidence that explains it. We keep that
// shape but anchor every entry to a typed EvidenceRef so a conclusion can always
// be traced back to an EID, a resource, a shader stage or a timing sample:
//
//   * hottestPasses  - per-pass summed GPU time over the frame's leaf events;
//   * hottestEvents  - the top timed leaf events, each carrying an evidence list
//                      (timing + shader + resource, and, when requested, a
//                      pipeline snapshot / mesh-data evidence pointer);
//   * conclusions    - split by provenance: `confirmed` are facts read from
//                      replay, `inferred` are heuristic deductions and
//                      `toVerify` is what a follow-up query or the user still has
//                      to confirm. When GPU timings are unavailable the ranking
//                      cannot be a fact, so nothing lands in `confirmed`.
//
// Every field here is pure data, so the report can be serialised and tested
// without a capture, a replay controller or Qt widgets.

// A pass and the total GPU time of the frame's leaf events under it. `confidence`
// is Confirmed when the total comes from real EventGPUDuration samples and
// Inferred when it is partly derived (some children had no timing).
struct PerformanceHotPass
{
  QString passId;         // stable pass identifier (BuildPassId)
  QString name;           // marker/action name, "Frame" for the implicit root
  uint32_t firstEventId = 0;
  uint32_t lastEventId = 0;
  uint32_t eventCount = 0;    // leaf events aggregated into this pass
  double durationUs = 0.0;
  Confidence confidence = Confidence::Confirmed;
};

// One hotspot event plus the evidence that supports its ranking. The evidence
// vector always contains the timing sample (kind == "timing") and, when the
// pipeline could be read, the bound shaders (kind == "shader") and the touched
// resources (kind == "resource"). `pipelineEvidence` / `meshEvidence` are
// additional pointers a client can follow up on (they are not expanded here so
// the report stays compact).
struct PerformanceHotEvent
{
  uint32_t eventId = 0;
  QString name;
  QString passId;
  QString passName;
  double durationUs = 0.0;
  double share = 0.0;        // fraction of totalDurationUs, 0..1
  bool pipelineAvailable = false;
  bool meshAvailable = false;
  QVector<EvidenceRef> evidence;
  Confidence confidence = Confidence::Confirmed;
};

// The provenance-split conclusion lists. Each string is human readable; the
// machine-checkable anchor for a conclusion is the `evidence` on the
// PerformanceHotEvent it refers to.
struct PerformanceConclusions
{
  QStringList confirmed;
  QStringList inferred;
  QStringList toVerify;
};

struct PerformanceReport
{
  bool available = true;
  bool timingAvailable = false;      // EventGPUDuration was measurable
  QString capturePath;
  QString api;                       // human readable graphics API, from snapshot
  uint32_t frameNumber = 0;
  uint32_t totalTimedEvents = 0;     // leaf events with a positive measurement
  double totalDurationUs = 0.0;
  uint32_t hotEventLimit = 0;        // cap applied to hottestEvents
  QVector<PerformanceHotPass> hottestPasses;
  QVector<PerformanceHotEvent> hottestEvents;
  PerformanceConclusions conclusions;
  QVector<EvidenceRef> evidence;     // report-level anchors (capture, timings)
  Confidence confidence = Confidence::Confirmed;
  QString limitation;
};

QJsonObject PerformanceHotPassToJson(const PerformanceHotPass &pass);
QJsonObject PerformanceHotEventToJson(const PerformanceHotEvent &hotEvent);
QJsonObject PerformanceConclusionsToJson(const PerformanceConclusions &conclusions);
QJsonObject PerformanceReportToJson(const PerformanceReport &report);

// Render the report as a Markdown document. `evidence` is the report-level
// anchor block the tool passes through (same strings the plugin's template used);
// this function only formats, it never invents facts.
QString PerformanceReportToMarkdown(const PerformanceReport &report);

//
// ---- Event Browser advanced filtering and AI context (P1-7) ---------------
//
// The native Event Browser owns the event tree and its expression-based filter
// box; nothing here replaces either. This model augments that view with the
// plugin's *combined* filter set (marker scope, debug/empty-operation exclusion,
// event-id bounds, draw-only) applied over the same action tree, plus a renderer
// that turns the surviving rows into an AI-ready context block. The filter walk
// is pure data, so it can be tested without a capture or a replay controller.
//
// The native filter box evaluates `$action()`/`$marker` style expressions in
// EventBrowser's own proxy model for the on-screen tree. This model is the
// programmatic counterpart a tool can call to get a flattened, capped,
// machine-readable subset - the two coexist and neither replaces the other.

// One row in the filtered, flattened event list handed back to a client. All
// fields except the duration are facts read from the action tree; the duration is
// present only when the capture exposes EventGPUDuration.
struct EventBrowserContextRow
{
  uint32_t eventId = 0;
  QString name;
  QString flags;        // human readable flag names, e.g. "Drawcall|Indexed"
  uint32_t depth = 0;   // tree depth, 0 = root
  uint32_t numIndices = 0;
  uint32_t numInstances = 0;
  double durationUs = 0.0;
  bool hasDuration = false;
  Confidence confidence = Confidence::Confirmed;
};

// The combined filter set, echoed back in the result so a client can reproduce
// the query. An empty markerFilter and zero event-id bounds mean "no constraint".
struct EventBrowserFilterOptions
{
  QString filter;                        // name substring; applied after advanced filtering
  QString markerFilter;                  // case-insensitive substring on the action name
  bool excludeMarkers = false;           // drop marker groups
  bool onlyDrawCalls = false;            // keep only real draw/dispatch/clear effects
  bool excludeDebugMarkers = false;      // drop debug/annotation marker groups
  bool excludeEmptyOperations = false;   // drop zero-index/zero-instance effects
  bool sortByDuration = false;           // order rows by GPU duration, hottest first
  uint32_t eventIdMin = 0;
  uint32_t eventIdMax = 0;
  bool hasEventIdMin = false;           // legacy nonzero values also constrain the query
  bool hasEventIdMax = false;
};

// Boundary parsing is shared by tool handlers; omission remains distinct from explicit zero.
struct ToolResult;
ToolResult ParseEventBrowserFilters(const QJsonObject &args, EventBrowserFilterOptions &out);

struct ActionTimingOptions
{
  QVector<uint32_t> eventIds;             // empty means all; duplicates are removed in source order
  uint32_t eventId = 0;                  // legacy single-event intersection
  QString markerFilter;
  QStringList excludeMarkers;           // ancestor marker name substrings, not a boolean
  bool onlyDrawCalls = false;
  int limit = 200;                       // extension implementation clamps 0 to 1, max 2000
  int top = 50;                          // legacy slowest list cap
  bool legacyCounters = false;           // parsed old-only calls retain all measured counter rows
};
ToolResult ParseActionTimingOptions(const QJsonObject &args, ActionTimingOptions &out);
ToolResult SelectActionTimings(const rdcarray<ActionDescription> &roots, const SDFile &sdfile,
                              const QHash<quint32, double> &timings,
                              const ActionTimingOptions &options);

// A filtered tree is owned here, so adapters never reinterpret JSON or marker semantics.
// nameFirst mirrors getDrawCalls; false mirrors buildEventBrowserContext.
rdcarray<ActionDescription> FilterActionTree(const rdcarray<ActionDescription> &roots,
                                           const SDFile &sdfile,
                                           const EventBrowserFilterOptions &options,
                                           bool nameFirst = false);

// Counters gathered during the filter walk.
struct EventBrowserFilterStats
{
  uint32_t totalEvents = 0;     // nodes visited (the whole frame, pre-filter)
  uint32_t matchedEvents = 0;   // rows produced (post-filter, pre-cap)
};

struct EventBrowserContext
{
  bool available = true;
  QString capturePath;
  QString api;
  uint32_t frameNumber = 0;
  EventBrowserFilterOptions filters;
  uint32_t totalEvents = 0;
  uint32_t matchedEvents = 0;
  bool truncated = false;       // rows were dropped by the caller's cap
  QVector<EventBrowserContextRow> rows;
  QString aiContext;            // prose block a model can read directly
  Confidence confidence = Confidence::Confirmed;
  QString limitation;
};

// Human readable names for the set bits in `flags`, e.g. "Drawcall|Indexed".
QString ActionFlagNames(ActionFlags flags);

// True if `flags` describe a real draw/dispatch/mesh-dispatch/clear effect.
bool IsRealDrawCall(ActionFlags flags);

// True if `name` looks like a debug/annotation marker (contains "debug" or
// "marker", or starts with "rdoc"). Heuristic, so hits should be treated as
// Inferred rather than a fact.
bool IsDebugMarkerName(const QString &name);

// True for an "empty" operation: a leaf real effect with no indices and no
// instances, i.e. a draw/dispatch that writes nothing.
bool IsEmptyOperation(const ActionDescription &action);

// Apply the combined filter set to the action tree, appending the surviving
// actions to `out` as a flat pre-order list and folding counters into `stats`.
// Semantics mirror the plugin's `filterDrawCallsAdvanced`:
//   * a markerFilter name match turns on "in scope" for the entire subtree;
//   * a node that fails but has surviving descendants is elided and its
//     descendants promoted, so a container never hides matching content;
//   * when markerFilter is set, a node is kept only if it is in scope or one of
//     its descendants survived.
// `timings` supplies EventGPUDuration per event id (may be empty).
void FilterEventBrowserActions(const rdcarray<ActionDescription> &roots, const SDFile &sdfile,
                               const EventBrowserFilterOptions &options,
                               const QHash<quint32, double> &timings,
                               QVector<EventBrowserContextRow> &out,
                               EventBrowserFilterStats &stats);

QJsonObject EventBrowserContextRowToJson(const EventBrowserContextRow &row);
QJsonObject EventBrowserFilterOptionsToJson(const EventBrowserFilterOptions &options);
QJsonObject EventBrowserContextToJson(const EventBrowserContext &context);

// Render the context as a compact prose block for an AI client: the capture,
// the applied filters, the frame totals and one line per surviving row.
QString EventBrowserContextToText(const EventBrowserContext &context);

// ---------------------------------------------------------------------------
// P1-8 investigation bookmark model
// ---------------------------------------------------------------------------
// The native capture format stores one bookmark per event: an event id plus a
// free-form note. The VS Code extension adds a richer record - an id, tags,
// an open issue, a conclusion, a screenshot and an AI analysis reference - that
// is not persisted into the capture at all.
//
// Rather than widen the persisted format (which would break every other reader
// of the `renderdoc/ui/bookmarks` section, including the Python API and the
// native Event Browser strip), the extended fields are kept in a dedicated,
// versioned, backward-compatible section. The record below is the canonical
// in-memory shape; the serialisers keep the two halves apart:
//
//   * `text` is the portable subset - it round-trips through the native
//     bookmark section, so a bookmark placed by the AI tool still shows up in
//     the Event Browser's bookmark strip and a rename there is still visible
//     to the AI;
//   * everything else lives only in the sidecar section, so a capture written
//     by a stock RenderDoc build loses no fields it never knew about, and an
//     extended capture opened by a stock build degrades to the plain bookmark.
//
// `confidence` marks the boundary between a human/AI-recorded judgement
// (Confirmed for the fields the user typed, Inferred for machine-derived ones)
// and a fact read out of the capture, exactly like the rest of the model.

// One investigation bookmark. `id` is stable and locally unique within a
// capture; an empty id means "not yet assigned" and the writer derives one from
// the event id so old records without ids still load.
struct BookmarkTaggedItem
{
  QString id;
  QString eventId;      // informational: which event the note was recorded against
  QString title;
  QString note;         // the portable bookmark text
  QString conclusion;
  QString analysis;              // free-form AI analysis prose
  QString screenshotPath;
  Confidence confidence = Confidence::Confirmed;
};

// Aggregate written into the sidecar section.
struct BookmarkFile
{
  uint32_t version = 1;
  QString capturePath;
  QVector<BookmarkTaggedItem> items;
};

// A bookmark paired with the file path its sidecar lives in, so a read does not
// have to re-derive the path from the capture.
struct BookmarkSidecar
{
  bool available = false;       // the capture is open locally and its sidecar is readable
  bool sidecarPresent = false;  // the sidecar section exists (false == no extended fields yet)
  uint32_t version = 0;
  QString capturePath;
  QString sidecarPath;
  QVector<BookmarkTaggedItem> items;
  QString limitation;
};

// Derive the sidecar path for a capture: "<capture>.rdc.bookmarks.json" next to
// the capture. An empty capture path yields an empty sidecar path.
QString BookmarkSidecarPath(const QString &capturePath);

// Fold a sidecar document into the bookmark list read from the capture. Items
// are matched on event id and the sidecar's non-empty fields win; bookmarks that
// only exist in the capture keep their `text` and gain no extended fields. The
// result preserves the capture's ordering and appends sidecar-only records.
QVector<BookmarkTaggedItem> MergeBookmarkSidecar(const rdcarray<EventBookmark> &bookmarks,
                                                 const BookmarkFile &sidecar);

// Parse/serialise the sidecar document. Parsing is defensive: an unknown
// version, a missing list or a malformed record yields a usable (possibly empty)
// document rather than a failure, so a partially-written file never blocks a
// capture from loading.
BookmarkFile ParseBookmarkFile(const QByteArray &json);
QByteArray SerializeBookmarkFile(const BookmarkFile &file);

// True if the record carries at least one extended field, i.e. it is more than
// a plain capture bookmark and therefore needs the sidecar to survive.
bool BookmarkHasExtendedFields(const BookmarkTaggedItem &item);

QJsonObject BookmarkTaggedItemToJson(const BookmarkTaggedItem &item);
QJsonObject BookmarkFileToJson(const BookmarkFile &file);
QJsonObject BookmarkSidecarToJson(const BookmarkSidecar &sidecar);

// Render the sidecar as a compact prose block for an AI client.
QString BookmarkSidecarToText(const BookmarkSidecar &sidecar);

//
// ---- Investigation report (P1-9) ------------------------------------------
//
// The plugin's `ExportInvestigationReportTool` is the one place where the whole
// investigation is packaged into a single artefact: capture metadata, the
// user/AI bookmarks, the performance hotspots and the resource-memory footprint,
// rendered either as JSON or as a Markdown document. The native port keeps the
// same shape but composes it from the *already-built* P1-4/P1-6/P1-8 models
// instead of re-deriving JSON at the tool layer, so there is exactly one
// definition of every field.
//
// The struct is deliberately clock-free and capture-free: `generatedAt` is
// supplied by the caller and the three sub-reports are filled by the adapter. It
// therefore serialises and renders without a capture, a replay controller, a
// filesystem or Qt widgets, which is what makes it testable.

// A bookmark as it appears in the report. This is the P1-8 record verbatim, so a
// report written today and a bookmark edited later cannot disagree about field
// names. The report adds no bookmark fields of its own.
// (BookmarkTaggedItem is defined above in the P1-8 section.)

struct InvestigationReport
{
  bool available = true;          // a capture is loaded and the report could be built
  QString generatedAt;            // ISO 8601 UTC, supplied by the caller
  QString capturePath;
  QString api;                    // human readable graphics API, from the snapshot
  uint32_t frameNumber = 0;
  uint32_t currentEvent = 0;      // selected EID at build time, 0 when none

  // P1-8 bookmarks. `bookmarksAvailable` is false only when the capture is not
  // open locally; `bookmarkLimitation` then explains why.
  QVector<BookmarkTaggedItem> bookmarks;
  bool bookmarksAvailable = false;
  QString bookmarkLimitation;

  // P1-6 performance half. `performance.timingAvailable` drives whether the
  // hotspot section is a fact or a hypothesis.
  PerformanceReport performance;

  // P1-4 resource half.
  ResourceMemoryAudit resources;

  // Report-level anchors (capture, timings) plus the per-hotspot evidence carried
  // on `performance.hottestEvents[].evidence`.
  QVector<EvidenceRef> evidence;

  // Everything the report cannot claim to know, in one place.
  QStringList limitations;
  Confidence confidence = Confidence::Confirmed;
};

// The effective limitation list: the caller-supplied `limitations` merged with
// the standard ones derived from the sub-reports (bookmark sidecar unavailable,
// GPU timings unavailable, resource sizes are estimates). Duplicates are dropped
// so a reason is never repeated.
QStringList InvestigationReportLimitations(const InvestigationReport &report);

QJsonObject InvestigationReportToJson(const InvestigationReport &report);

// Render the report as a Markdown document. Section titles match the plugin
// ("## Bookmarks", "## Performance hotspots", "## Limitations") so a client
// written against the extension keeps working; the extra sections only add
// detail, they never remove one of those three.
QString InvestigationReportToMarkdown(const InvestigationReport &report);

}    // namespace MCP
