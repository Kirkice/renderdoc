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

#include "MCPAnalysisModel.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QPair>
#include <QSet>
#include <algorithm>
#include <string.h>
#include "MCPQueryAdapter.h"
#include "MCPTypes.h"

namespace MCP
{

// rdcstr has no DoStringise() overload, so it can never be passed to ToQStr().
// Convert explicitly, exactly like MCPQueryAdapter.cpp does.
static QString FromRdcStr(const rdcstr &str)
{
  return QString::fromUtf8(str.c_str(), (int)str.size());
}

// The private uint64_t inside ResourceId, extracted with the same memcpy trick
// used by ResourceIdToString() and DoStringise<ResourceId>.
static uint64_t ResourceIdValue(ResourceId id)
{
  uint64_t v = 0;
  memcpy(&v, &id, sizeof(v));
  return v;
}

//
// ---- Evidence and confidence ---------------------------------------------
//

QString ConfidenceName(Confidence c)
{
  switch(c)
  {
    case Confidence::Confirmed: return QStringLiteral("confirmed");
    case Confidence::Inferred: return QStringLiteral("inferred");
    case Confidence::ToVerify: return QStringLiteral("toVerify");
  }
  return QStringLiteral("inferred");
}

EvidenceRef CaptureEvidence(const QString &capturePath)
{
  EvidenceRef ev;
  ev.kind = QStringLiteral("capture");
  ev.label = capturePath;
  ev.confidence = Confidence::Confirmed;
  return ev;
}

EvidenceRef EventEvidence(uint32_t eventId, const QString &name)
{
  EvidenceRef ev;
  ev.kind = QStringLiteral("event");
  ev.eventId = eventId;
  ev.label = name;
  ev.confidence = Confidence::Confirmed;
  return ev;
}

EvidenceRef ResourceEvidence(ResourceId id, const QString &name)
{
  EvidenceRef ev;
  ev.kind = QStringLiteral("resource");
  ev.resourceValue = ResourceIdValue(id);
  ev.resourceName = name;
  ev.confidence = Confidence::Confirmed;
  return ev;
}

EvidenceRef ShaderEvidence(ResourceId id, const QString &stage, const QString &name)
{
  EvidenceRef ev;
  ev.kind = QStringLiteral("shader");
  ev.resourceValue = ResourceIdValue(id);
  ev.shaderStage = stage;
  ev.resourceName = name;
  ev.confidence = Confidence::Confirmed;
  return ev;
}

EvidenceRef TimingEvidence(uint32_t eventId, double durationUs, const QString &name)
{
  EvidenceRef ev;
  ev.kind = QStringLiteral("timing");
  ev.eventId = eventId;
  ev.durationUs = durationUs;
  ev.label = name;
  ev.confidence = Confidence::Confirmed;
  return ev;
}

QJsonObject EvidenceToJson(const EvidenceRef &ev)
{
  QJsonObject j;
  j[QStringLiteral("kind")] = ev.kind;
  j[QStringLiteral("confidence")] = ConfidenceName(ev.confidence);

  if(ev.kind == QStringLiteral("event") || ev.kind == QStringLiteral("timing"))
    j[QStringLiteral("eventId")] = double(ev.eventId);

  if(ev.kind == QStringLiteral("resource") || ev.kind == QStringLiteral("shader"))
  {
    j[QStringLiteral("resourceValue")] = QJsonValue(double(ev.resourceValue));
    if(!ev.resourceName.isEmpty())
      j[QStringLiteral("resourceName")] = ev.resourceName;
  }

  if(ev.kind == QStringLiteral("shader") && !ev.shaderStage.isEmpty())
    j[QStringLiteral("shaderStage")] = ev.shaderStage;

  if(ev.kind == QStringLiteral("timing"))
    j[QStringLiteral("durationUs")] = ev.durationUs;

  if(!ev.label.isEmpty())
    j[QStringLiteral("label")] = ev.label;

  return j;
}

//
// ---- Unified events -------------------------------------------------------
//

bool IsRealEffect(ActionFlags flags)
{
  const ActionFlags real = ActionFlags::Drawcall | ActionFlags::Dispatch |
                           ActionFlags::MeshDispatch | ActionFlags::Clear;
  return bool(flags & real);
}

bool IsMarkerGroup(const ActionDescription &action)
{
  return !action.children.isEmpty() && !IsRealEffect(action.flags);
}

static void FlattenEvent(const ActionDescription &a, const SDFile &sdfile, uint32_t parentEventId,
                         uint32_t depth, rdcarray<EventNode> &out)
{
  EventNode node;
  node.eventId = a.eventId;
  node.actionId = a.actionId;
  node.parentEventId = parentEventId;
  node.depth = depth;
  node.childCount = (uint32_t)a.children.size();
  node.flags = a.flags;
  node.name = FromRdcStr(a.GetName(sdfile));
  node.numIndices = a.numIndices;
  node.numInstances = a.numInstances;
  out.push_back(node);

  // Children are stored in the action tree in pre-order; recursing here yields a
  // flat pre-order list that preserves the browser ordering.
  for(const ActionDescription &child : a.children)
    FlattenEvent(child, sdfile, a.eventId, depth + 1, out);
}

void FlattenEvents(const rdcarray<ActionDescription> &roots, const SDFile &sdfile,
                   rdcarray<EventNode> &out)
{
  for(const ActionDescription &root : roots)
    FlattenEvent(root, sdfile, 0, 0, out);
}

EventCounts CountEffects(const rdcarray<EventNode> &events)
{
  EventCounts c;
  for(const EventNode &e : events)
  {
    c.total++;

    // Flags are exact bits, so every one of these counts is a fact rather than a
    // heuristic (the plugin had to pattern-match flag strings for the same job).
    if(e.flags & ActionFlags::Drawcall)
      c.draws++;
    if(e.flags & (ActionFlags::Dispatch | ActionFlags::MeshDispatch))
      c.dispatches++;
    if(e.flags & ActionFlags::Clear)
      c.clears++;
    if(e.flags & ActionFlags::Copy)
      c.copies++;
    if(e.flags & ActionFlags::Present)
      c.presents++;
    if(e.flags & (ActionFlags::PushMarker | ActionFlags::PopMarker | ActionFlags::SetMarker))
      c.markers++;
  }
  return c;
}

QJsonObject EventNodeToJson(const EventNode &node)
{
  QJsonObject j;
  j[QStringLiteral("eventId")] = double(node.eventId);
  j[QStringLiteral("actionId")] = double(node.actionId);
  j[QStringLiteral("parentEventId")] = double(node.parentEventId);
  j[QStringLiteral("depth")] = double(node.depth);
  j[QStringLiteral("name")] = node.name;
  j[QStringLiteral("flags")] = double((uint32_t)node.flags);
  j[QStringLiteral("numIndices")] = double(node.numIndices);
  j[QStringLiteral("numInstances")] = double(node.numInstances);
  if(node.childCount > 0)
    j[QStringLiteral("childCount")] = double(node.childCount);
  return j;
}

QJsonObject EventCountsToJson(const EventCounts &counts)
{
  QJsonObject j;
  j[QStringLiteral("total")] = double(counts.total);
  j[QStringLiteral("draws")] = double(counts.draws);
  j[QStringLiteral("dispatches")] = double(counts.dispatches);
  j[QStringLiteral("clears")] = double(counts.clears);
  j[QStringLiteral("copies")] = double(counts.copies);
  j[QStringLiteral("presents")] = double(counts.presents);
  j[QStringLiteral("markers")] = double(counts.markers);
  return j;
}

//
// ---- Resource accesses, shaders and timings -------------------------------
//

QString ResourceUsageName(ResourceUsage usage)
{
  switch(usage)
  {
    case ResourceUsage::RenderTarget: return QStringLiteral("renderTarget");
    case ResourceUsage::DepthTarget: return QStringLiteral("depthTarget");
    case ResourceUsage::Sampled: return QStringLiteral("sampled");
    case ResourceUsage::Constant: return QStringLiteral("constant");
    case ResourceUsage::ReadWrite: return QStringLiteral("readWrite");
    case ResourceUsage::Vertex: return QStringLiteral("vertex");
    case ResourceUsage::Index: return QStringLiteral("index");
    case ResourceUsage::Indirect: return QStringLiteral("indirect");
    case ResourceUsage::Unknown: break;
  }
  return QStringLiteral("unknown");
}

//
// ---- Frame analysis -------------------------------------------------------
//

void BuildFrameStatistics(const FrameDescription &frame, FrameStatistics &out)
{
  out.frameNumber = frame.frameNumber;
  out.uncompressedFileSize = frame.uncompressedFileSize;
  out.compressedFileSize = frame.compressedFileSize;
  out.persistentSize = frame.persistentSize;
  out.initDataSize = frame.initDataSize;
  out.captureTime = frame.captureTime;
}

// Threshold-based findings. Kept deliberately small and explicit so the rules
// can be unit tested; every result carries its own Confidence and evidence.
static void DetectIssues(FrameAnalysis &out, const FrameAnalysisOptions &options)
{
  if(out.counts.draws > options.drawIssueThreshold)
  {
    PotentialIssue issue;
    issue.severity = QStringLiteral("warning");
    issue.code = QStringLiteral("high-draw-count");
    issue.confidence = Confidence::Inferred;
    issue.message = QStringLiteral("Frame issues %1 draw calls, above the %2 threshold. "
                                   "Consider batching or instancing.")
                        .arg(out.counts.draws)
                        .arg(options.drawIssueThreshold);
    out.potentialIssues.push_back(issue);
  }

  if(out.counts.clears > options.clearIssueThreshold)
  {
    PotentialIssue issue;
    issue.severity = QStringLiteral("warning");
    issue.code = QStringLiteral("excessive-clears");
    issue.confidence = Confidence::Inferred;
    issue.message = QStringLiteral("Frame contains %1 clears, above the %2 threshold. "
                                   "Redundant clears can waste bandwidth.")
                        .arg(out.counts.clears)
                        .arg(options.clearIssueThreshold);
    out.potentialIssues.push_back(issue);
  }
}

void BuildFrameAnalysis(const CaptureSnapshot &snapshot, const rdcarray<ActionDescription> &roots,
                        const SDFile &sdfile, const rdcarray<ResourceDescription> &resources,
                        const rdcarray<TextureDescription> &textures,
                        const rdcarray<BufferDescription> &buffers, FrameAnalysis &out,
                        const FrameAnalysisOptions &options, const FrameDescription *frame)
{
  out.api = snapshot.api;
  out.driver = options.driver;
  out.rdocVersion = options.rdocVersion;

  if(frame)
    BuildFrameStatistics(*frame, out.statistics);

  rdcarray<EventNode> events;
  FlattenEvents(roots, sdfile, events);
  out.counts = CountEffects(events);

  // Resource inventory. Textures carry their own byte size and dimensions, so
  // both the memory total and the "large texture" rule are facts read directly
  // from the texture descriptions.
  uint32_t largeCount = 0;
  for(const TextureDescription &t : textures)
  {
    out.resources.textureCount++;
    out.resources.totalTextureMemory += t.byteSize;

    const bool large = t.width >= options.largeTextureMinDimension ||
                       t.height >= options.largeTextureMinDimension ||
                       t.depth >= options.largeTextureMinDimension ||
                       t.byteSize >= options.largeTextureMinBytes;

    if(!large)
      continue;

    largeCount++;

    if((int)out.resources.largeTextures.size() < options.maxLargeTextures)
    {
      LargeTexture lt;
      lt.resourceId = ResourceIdToString(t.resourceId);
      lt.width = t.width;
      lt.height = t.height;
      lt.depth = t.depth;
      lt.mips = t.mips;
      lt.arraySize = t.arraysize;
      lt.format = detail::ResourceFormatString(t.format);
      lt.byteSize = t.byteSize;
      out.resources.largeTextures.push_back(lt);

      PotentialIssue issue;
      issue.severity = QStringLiteral("info");
      issue.code = QStringLiteral("large-texture");
      issue.confidence = Confidence::Inferred;
      issue.message = QStringLiteral("Texture %1 is %2x%3x%4 at %5 bytes; large textures can "
                                     "dominate memory and bandwidth.")
                          .arg(lt.resourceId)
                          .arg(t.width)
                          .arg(t.height)
                          .arg(t.depth)
                          .arg(t.byteSize);
      issue.evidence.push_back(ResourceEvidence(t.resourceId, lt.resourceId));
      out.potentialIssues.push_back(issue);
    }
  }
  out.resources.largeTextureCount = largeCount;

  for(const BufferDescription &b : buffers)
  {
    out.resources.bufferCount++;
    out.resources.totalBufferMemory += b.length;
  }

  for(const ResourceDescription &r : resources)
  {
    if(r.type == ResourceType::Shader)
      out.resources.shaderCount++;
  }

  DetectIssues(out, options);
}

QJsonObject FrameStatisticsToJson(const FrameStatistics &s)
{
  QJsonObject j;
  j[QStringLiteral("frameNumber")] = double(s.frameNumber);
  j[QStringLiteral("uncompressedFileSize")] = QJsonValue(double(s.uncompressedFileSize));
  j[QStringLiteral("compressedFileSize")] = QJsonValue(double(s.compressedFileSize));
  j[QStringLiteral("persistentSize")] = QJsonValue(double(s.persistentSize));
  j[QStringLiteral("initDataSize")] = QJsonValue(double(s.initDataSize));
  j[QStringLiteral("captureTime")] = QJsonValue(double(s.captureTime));
  return j;
}

static QJsonObject ResourceInventoryToJson(const ResourceInventory &r)
{
  QJsonObject j;
  j[QStringLiteral("textureCount")] = double(r.textureCount);
  j[QStringLiteral("bufferCount")] = double(r.bufferCount);
  j[QStringLiteral("shaderCount")] = double(r.shaderCount);
  j[QStringLiteral("largeTextureCount")] = double(r.largeTextureCount);
  j[QStringLiteral("totalTextureMemory")] = QJsonValue(double(r.totalTextureMemory));
  j[QStringLiteral("totalBufferMemory")] = QJsonValue(double(r.totalBufferMemory));

  QJsonArray large;
  for(const LargeTexture &lt : r.largeTextures)
  {
    QJsonObject o;
    o[QStringLiteral("resourceId")] = lt.resourceId;
    o[QStringLiteral("name")] = lt.name;
    o[QStringLiteral("width")] = double(lt.width);
    o[QStringLiteral("height")] = double(lt.height);
    o[QStringLiteral("depth")] = double(lt.depth);
    o[QStringLiteral("mips")] = double(lt.mips);
    o[QStringLiteral("arraySize")] = double(lt.arraySize);
    o[QStringLiteral("format")] = lt.format;
    o[QStringLiteral("byteSize")] = QJsonValue(double(lt.byteSize));
    large.append(o);
  }
  j[QStringLiteral("largeTextures")] = large;
  return j;
}

static QJsonObject IssueToJson(const PotentialIssue &issue)
{
  QJsonObject j;
  j[QStringLiteral("severity")] = issue.severity;
  j[QStringLiteral("code")] = issue.code;
  j[QStringLiteral("confidence")] = ConfidenceName(issue.confidence);
  j[QStringLiteral("message")] = issue.message;

  QJsonArray evidence;
  for(const EvidenceRef &ev : issue.evidence)
    evidence.append(EvidenceToJson(ev));
  j[QStringLiteral("evidence")] = evidence;
  return j;
}

QJsonObject FrameAnalysisToJson(const FrameAnalysis &analysis)
{
  QJsonObject j;
  j[QStringLiteral("api")] = analysis.api;
  j[QStringLiteral("driver")] = analysis.driver;
  j[QStringLiteral("rdocVersion")] = analysis.rdocVersion;
  j[QStringLiteral("statistics")] = FrameStatisticsToJson(analysis.statistics);
  j[QStringLiteral("counts")] = EventCountsToJson(analysis.counts);
  j[QStringLiteral("resources")] = ResourceInventoryToJson(analysis.resources);

  QJsonArray issues;
  for(const PotentialIssue &issue : analysis.potentialIssues)
    issues.append(IssueToJson(issue));
  j[QStringLiteral("potentialIssues")] = issues;
  return j;
}

//
// ---- Pass graph -----------------------------------------------------------
//

QJsonObject PassInfoToJson(const PassInfo &pass)
{
  QJsonObject j;
  j[QStringLiteral("id")] = pass.id;
  j[QStringLiteral("name")] = pass.name;
  j[QStringLiteral("eventIdStart")] = double(pass.eventIdStart);
  j[QStringLiteral("eventIdEnd")] = double(pass.eventIdEnd);
  j[QStringLiteral("drawCount")] = double(pass.drawCount);
  j[QStringLiteral("durationUs")] = pass.durationUs;
  j[QStringLiteral("confidence")] = ConfidenceName(pass.confidence);

  QJsonArray rts;
  for(const ResourceId &id : pass.renderTargets)
    rts.append(ResourceIdToString(id));
  j[QStringLiteral("renderTargets")] = rts;

  QJsonArray sampled;
  for(const ResourceId &id : pass.sampledTextures)
    sampled.append(ResourceIdToString(id));
  j[QStringLiteral("sampledTextures")] = sampled;

  QJsonArray shaders;
  for(const ShaderRef &s : pass.shaders)
  {
    QJsonObject o;
    o[QStringLiteral("resourceId")] = ResourceIdToString(s.id);
    o[QStringLiteral("name")] = s.name;
    o[QStringLiteral("stage")] = s.stage;
    o[QStringLiteral("confidence")] = ConfidenceName(s.confidence);
    shaders.append(o);
  }
  j[QStringLiteral("shaders")] = shaders;
  return j;
}

QJsonObject PassGraphToJson(const PassGraph &graph)
{
  QJsonObject j;

  QJsonArray passes;
  for(const PassInfo &p : graph.passes)
    passes.append(PassInfoToJson(p));
  j[QStringLiteral("passes")] = passes;

  QJsonArray edges;
  for(const PassGraphEdge &e : graph.edges)
  {
    QJsonObject o;
    o[QStringLiteral("fromPassId")] = e.fromPassId;
    o[QStringLiteral("fromPassName")] = e.fromPassName;
    o[QStringLiteral("toPassId")] = e.toPassId;
    o[QStringLiteral("toPassName")] = e.toPassName;
    o[QStringLiteral("resourceId")] = ResourceIdToString(e.resourceId);
    o[QStringLiteral("resourceName")] = e.resourceName;
    o[QStringLiteral("usage")] = e.usage;
    o[QStringLiteral("confidence")] = ConfidenceName(e.confidence);
    edges.append(o);
  }
  j[QStringLiteral("edges")] = edges;

  j[QStringLiteral("edgeCount")] = edges.size();
  j[QStringLiteral("totalDurationUs")] = graph.totalDurationUs;
  return j;
}

//
// ---- Resource access ------------------------------------------------------
//

QJsonObject ResourceAccessToJson(const ResourceAccess &access)
{
  QJsonObject j;
  j[QStringLiteral("resourceId")] = ResourceIdToString(access.id);
  j[QStringLiteral("name")] = access.name;
  j[QStringLiteral("usage")] = ResourceUsageName(access.usage);
  j[QStringLiteral("eventId")] = double(access.eventId);
  j[QStringLiteral("confidence")] = ConfidenceName(access.confidence);
  return j;
}

//
// ---- Reverse search -------------------------------------------------------
//

QJsonObject ReverseSearchResultToJson(const ReverseSearchResult &result)
{
  QJsonObject j;
  j[QStringLiteral("query")] = result.query;
  j[QStringLiteral("term")] = result.term;
  j[QStringLiteral("scannedEventCount")] = double(result.scannedEventCount);
  j[QStringLiteral("truncated")] = result.truncated;
  j[QStringLiteral("matchCount")] = double(result.hits.size());

  // The plugin returned a flat, ascending `eventIds` array alongside the
  // richer per-hit details. Emit both so existing clients keep working and new
  // ones can read the evidence.
  QJsonArray eventIds;
  QJsonArray hits;
  for(const ReverseSearchHit &hit : result.hits)
  {
    eventIds.append(double(hit.eventId));

    QJsonObject h;
    h[QStringLiteral("eventId")] = double(hit.eventId);
    h[QStringLiteral("name")] = hit.name;
    if(ResourceIdValue(hit.resource) != 0)
      h[QStringLiteral("resourceId")] = ResourceIdToString(hit.resource);
    h[QStringLiteral("confidence")] = ConfidenceName(hit.confidence);

    QJsonArray resources;
    for(const ResourceAccess &access : hit.resources)
      resources.append(ResourceAccessToJson(access));
    h[QStringLiteral("resources")] = resources;

    hits.append(h);
  }
  j[QStringLiteral("eventIds")] = eventIds;
  j[QStringLiteral("hits")] = hits;
  return j;
}

//
// ---- Pass graph result ----------------------------------------------------
//

QJsonObject PassGraphResultToJson(const PassGraphResult &result)
{
  QJsonObject j;
  j[QStringLiteral("scannedEventCount")] = double(result.scannedEventCount);
  j[QStringLiteral("truncated")] = result.truncated;

  QJsonArray edges;
  for(const ResourceEdge &e : result.edges)
  {
    QJsonObject o;
    o[QStringLiteral("fromEventId")] = double(e.fromEventId);
    o[QStringLiteral("toEventId")] = double(e.toEventId);
    o[QStringLiteral("resourceId")] = ResourceIdToString(e.resource);
    o[QStringLiteral("resourceName")] = e.resourceName;
    o[QStringLiteral("usage")] = e.usage;
    o[QStringLiteral("confidence")] = ConfidenceName(e.confidence);
    edges.append(o);
  }
  j[QStringLiteral("edges")] = edges;
  j[QStringLiteral("edgeCount")] = edges.size();
  return j;
}

//
// ---- Resource memory audit ------------------------------------------------
//

static QJsonObject TextureAuditToJson(const TextureAudit &t)
{
  QJsonObject j;
  j[QStringLiteral("count")] = double(t.count);

  // byFormat is emitted as an object keyed by format name, matching the plugin.
  QJsonObject byFormat;
  for(const QPair<QString, uint32_t> &entry : t.byFormat)
    byFormat[entry.first] = double(entry.second);
  j[QStringLiteral("byFormat")] = byFormat;

  QJsonArray formats;
  for(const QPair<QString, uint32_t> &entry : t.byFormat)
    formats.append(entry.first);
  j[QStringLiteral("formats")] = formats;

  j[QStringLiteral("mipLevelsAvailable")] = double(t.mipLevelsAvailable);
  j[QStringLiteral("missingMipChain")] = double(t.missingMipChain);
  j[QStringLiteral("fullMipChain")] = double(t.fullMipChain);

  QJsonArray dims;
  for(const QString &d : t.dimensions)
    dims.append(d);
  j[QStringLiteral("dimensions")] = dims;
  return j;
}

QJsonObject ResourceMemoryAuditToJson(const ResourceMemoryAudit &audit)
{
  QJsonObject j;
  j[QStringLiteral("totalResources")] = double(audit.totalResources);
  j[QStringLiteral("totalBytes")] = double(audit.totalBytes);
  j[QStringLiteral("totalMiB")] = audit.totalMiB;
  j[QStringLiteral("textureCount")] = double(audit.textureCount);
  j[QStringLiteral("bufferCount")] = double(audit.bufferCount);
  j[QStringLiteral("shaderCount")] = double(audit.shaderCount);
  j[QStringLiteral("totalTextureBytes")] = double(audit.totalTextureBytes);
  j[QStringLiteral("totalBufferBytes")] = double(audit.totalBufferBytes);

  QJsonArray largest;
  for(const ResourceMemoryRow &r : audit.largestResources)
  {
    QJsonObject o;
    o[QStringLiteral("resourceId")] = ResourceIdToString(r.resourceId);
    o[QStringLiteral("name")] = r.name;
    o[QStringLiteral("type")] = r.type;
    if(!r.textureType.isEmpty())
      o[QStringLiteral("textureType")] = r.textureType;
    o[QStringLiteral("byteSize")] = double(r.byteSize);
    if(r.width != 0)
    {
      o[QStringLiteral("width")] = double(r.width);
      o[QStringLiteral("height")] = double(r.height);
      o[QStringLiteral("depth")] = double(r.depth);
      o[QStringLiteral("mips")] = double(r.mips);
      o[QStringLiteral("arraySize")] = double(r.arraySize);
    }
    if(!r.format.isEmpty())
      o[QStringLiteral("format")] = r.format;
    if(!r.usage.isEmpty())
      o[QStringLiteral("usage")] = r.usage;
    largest.append(o);
  }
  j[QStringLiteral("largestResources")] = largest;
  j[QStringLiteral("largestResourceCount")] = largest.size();
  j[QStringLiteral("textureAudit")] = TextureAuditToJson(audit.textures);

  QJsonArray notes;
  for(const QString &n : audit.notes)
    notes.append(n);
  j[QStringLiteral("notes")] = notes;
  j[QStringLiteral("confidence")] = ConfidenceName(audit.confidence);
  return j;
}

//
// ---- Resource lifetime ----------------------------------------------------
//

QJsonObject ResourceLifetimeToJson(const ResourceLifetimeResult &result)
{
  QJsonObject j;
  j[QStringLiteral("available")] = result.available;
  j[QStringLiteral("scannedEventCount")] = double(result.scannedEventCount);
  j[QStringLiteral("truncated")] = result.truncated;
  j[QStringLiteral("rowCount")] = double(result.rows.size());

  QJsonArray rows;
  for(const ResourceLifetimeRow &r : result.rows)
  {
    QJsonObject o;
    o[QStringLiteral("resourceId")] = ResourceIdToString(r.resourceId);
    o[QStringLiteral("name")] = r.name;
    o[QStringLiteral("type")] = r.type;
    o[QStringLiteral("byteSize")] = double(r.byteSize);
    o[QStringLiteral("observedEventCount")] = double(r.observedEventCount);
    o[QStringLiteral("firstEventId")] = double(r.firstEventId);
    o[QStringLiteral("lastEventId")] = double(r.lastEventId);
    o[QStringLiteral("confidence")] = ConfidenceName(r.confidence);

    QJsonArray accesses;
    for(const ResourceAccess &a : r.accesses)
      accesses.append(ResourceAccessToJson(a));
    o[QStringLiteral("accesses")] = accesses;
    rows.append(o);
  }
  j[QStringLiteral("rows")] = rows;
  return j;
}

//
// ---- Unused-resource identification ---------------------------------------
//

QJsonObject UnusedResourceResultToJson(const UnusedResourceResult &result)
{
  QJsonObject j;
  j[QStringLiteral("available")] = result.available;
  j[QStringLiteral("scannedEventCount")] = double(result.scannedEventCount);
  j[QStringLiteral("candidateCount")] = double(result.candidates.size());
  j[QStringLiteral("confidence")] = ConfidenceName(result.confidence);
  j[QStringLiteral("limitation")] = result.limitation;

  QJsonArray candidates;
  for(const UnusedResource &u : result.candidates)
  {
    QJsonObject o;
    o[QStringLiteral("resourceId")] = ResourceIdToString(u.resourceId);
    o[QStringLiteral("name")] = u.name;
    o[QStringLiteral("type")] = u.type;
    o[QStringLiteral("byteSize")] = double(u.byteSize);
    candidates.append(o);
  }
  j[QStringLiteral("candidates")] = candidates;
  return j;
}

//
// ---- Suspected resource leaks ---------------------------------------------
//

QJsonObject LeakResultToJson(const LeakResult &result)
{
  QJsonObject j;
  j[QStringLiteral("available")] = result.available;
  j[QStringLiteral("scannedEventCount")] = double(result.scannedEventCount);
  j[QStringLiteral("candidateCount")] = double(result.candidates.size());
  j[QStringLiteral("confidence")] = ConfidenceName(result.confidence);
  j[QStringLiteral("limitation")] = result.limitation;

  QJsonArray candidates;
  for(const LeakCandidate &c : result.candidates)
  {
    QJsonObject o;
    o[QStringLiteral("resourceId")] = ResourceIdToString(c.resourceId);
    o[QStringLiteral("name")] = c.name;
    o[QStringLiteral("type")] = c.type;
    o[QStringLiteral("byteSize")] = double(c.byteSize);
    o[QStringLiteral("createdEventId")] = double(c.createdEventId);
    o[QStringLiteral("lastEventId")] = double(c.lastEventId);
    o[QStringLiteral("observedEventCount")] = double(c.observedEventCount);
    o[QStringLiteral("reason")] = c.reason;
    o[QStringLiteral("confidence")] = ConfidenceName(c.confidence);
    candidates.append(o);
  }
  j[QStringLiteral("candidates")] = candidates;
  return j;
}

//
// ---- P1-5 pipeline / shader snapshot and diff ----------------------------
//

// Compact rendering of a snapshot value for the before/after columns, matching
// the plugin's stringify-style messages but staying readable for the common
// scalar/enum cases (which is most of a pipeline state).
static QString SnapshotValueString(const QJsonValue &v)
{
  if(v.isNull() || v.isUndefined())
    return QString::fromUtf8("none");
  if(v.isBool())
    return v.toBool() ? QString::fromUtf8("true") : QString::fromUtf8("false");
  if(v.isDouble())
    return QString::number(v.toDouble());
  if(v.isString())
    return v.toString();
  if(v.isArray())
    return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
  if(v.isObject())
    return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
  return QString();
}

// Flatten a snapshot into dotted paths. Nested objects recurse; arrays index
// their elements; leaf scalars/strings/bools are recorded. An empty container
// is recorded as a single leaf so that "no output targets" vs "one output
// target" shows up as a change at the container path.
static void FlattenSnapshot(const QJsonValue &value, const QString &prefix,
                            QVector<QPair<QString, QJsonValue>> &out)
{
  if(value.isObject())
  {
    const QJsonObject o = value.toObject();
    if(o.isEmpty())
    {
      if(!prefix.isEmpty())
        out.push_back(qMakePair(prefix, value));
      return;
    }
    for(auto it = o.constBegin(); it != o.constEnd(); ++it)
    {
      const QString key = prefix.isEmpty() ? it.key() : (prefix + QString::fromUtf8(".") + it.key());
      FlattenSnapshot(it.value(), key, out);
    }
    return;
  }

  if(value.isArray())
  {
    const QJsonArray a = value.toArray();
    if(a.isEmpty())
    {
      if(!prefix.isEmpty())
        out.push_back(qMakePair(prefix, value));
      return;
    }
    for(int i = 0; i < a.size(); i++)
      FlattenSnapshot(a[i], prefix + QString::fromUtf8(".") + QString::number(i), out);
    return;
  }

  if(!prefix.isEmpty())
    out.push_back(qMakePair(prefix, value));
}

QJsonObject PipelineDiffResultToJson(const PipelineDiffResult &result)
{
  QJsonObject j;
  j[QStringLiteral("available")] = result.available;
  j[QStringLiteral("eventIdA")] = double(result.eventIdA);
  j[QStringLiteral("eventIdB")] = double(result.eventIdB);
  j[QStringLiteral("hasChanges")] = result.hasChanges;
  j[QStringLiteral("changeCount")] = double(result.changes.size());

  QJsonArray stages;
  for(const QString &s : result.inspectedStages)
    stages.append(s);
  j[QStringLiteral("inspectedStages")] = stages;

  // Both sorted by path so the diff is deterministic and easy to read.
  QVector<PipelineFieldChange> ordered = result.changes;
  std::sort(ordered.begin(), ordered.end(),
            [](const PipelineFieldChange &a, const PipelineFieldChange &b) {
              return a.path < b.path;
            });

  QJsonArray changes;
  for(const PipelineFieldChange &c : ordered)
  {
    QJsonObject o;
    o[QStringLiteral("path")] = c.path;
    o[QStringLiteral("group")] = c.group;
    o[QStringLiteral("before")] = c.before;
    o[QStringLiteral("after")] = c.after;
    o[QStringLiteral("beforeValue")] = c.beforeValue;
    o[QStringLiteral("afterValue")] = c.afterValue;
    changes.append(o);
  }
  j[QStringLiteral("changes")] = changes;

  // Human-readable one-liners, matching the plugin's `changes` array so clients
  // written against the extension keep working.
  QJsonArray messages;
  if(result.shaderChanges.isEmpty())
  {
    for(const PipelineFieldChange &c : ordered)
      messages.append(QString::fromUtf8("%1: %2 -> %3").arg(c.path, c.before, c.after));
  }
  else
  {
    QVector<PipelineShaderChange> shaders = result.shaderChanges;
    std::sort(shaders.begin(), shaders.end(),
              [](const PipelineShaderChange &a, const PipelineShaderChange &b) {
                return a.stage < b.stage;
              });
    QSet<QString> shaderPaths;
    for(const PipelineShaderChange &s : shaders)
    {
      messages.append(QString::fromUtf8("Shader %1: %2 -> %3")
                          .arg(s.stage, s.before.isEmpty() ? QString::fromUtf8("none") : s.before,
                               s.after.isEmpty() ? QString::fromUtf8("none") : s.after));
      shaderPaths.insert(QString::fromUtf8("shaders.") + s.stage);
    }
    for(const PipelineFieldChange &c : ordered)
    {
      bool covered = false;
      for(const QString &p : shaderPaths)
      {
        if(c.path.startsWith(p))
        {
          covered = true;
          break;
        }
      }
      if(!covered)
        messages.append(QString::fromUtf8("%1: %2 -> %3").arg(c.path, c.before, c.after));
    }
  }
  j[QStringLiteral("changesText")] = messages;

  QJsonArray shaderStages;
  for(const PipelineShaderChange &s : result.shaderChanges)
  {
    QJsonObject o;
    o[QStringLiteral("stage")] = s.stage;
    o[QStringLiteral("before")] = s.before;
    o[QStringLiteral("after")] = s.after;
    shaderStages.append(o);
  }
  j[QStringLiteral("shaderChanges")] = shaderStages;

  j[QStringLiteral("confidence")] = ConfidenceName(result.confidence);
  j[QStringLiteral("limitation")] = result.limitation;
  return j;
}

QStringList SnapshotStagesFromPipelineJson(const QJsonObject &stateA, const QJsonObject &stateB)
{
  // Canonical order, matching the plugin's stage list plus the extended native
  // names. Only stages present in at least one snapshot are returned.
  static const char *kStages[] = {"vertex",     "hull",      "domain", "geometry",
                                  "fragment",   "pixel",     "compute", "task",
                                  "mesh",       "amplification"};
  static const int kStageCount = int(sizeof(kStages) / sizeof(kStages[0]));

  const QJsonObject sa = stateA.value(QStringLiteral("shaders")).toObject();
  const QJsonObject sb = stateB.value(QStringLiteral("shaders")).toObject();

  QStringList out;
  for(int i = 0; i < kStageCount; i++)
  {
    const QString name = QString::fromUtf8(kStages[i]);
    if(sa.contains(name) || sb.contains(name))
      out.push_back(name);
  }
  return out;
}

}    // namespace MCP
