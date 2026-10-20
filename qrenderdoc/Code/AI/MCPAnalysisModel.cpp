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
#include <cmath>
#include <functional>
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
    case Confidence::Low: return QStringLiteral("low");
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

//
// ---- P1-6 structured performance report ----------------------------------
//

QJsonObject PerformanceHotPassToJson(const PerformanceHotPass &pass)
{
  QJsonObject j;
  j[QStringLiteral("passId")] = pass.passId;
  j[QStringLiteral("name")] = pass.name;
  j[QStringLiteral("firstEventId")] = double(pass.firstEventId);
  j[QStringLiteral("lastEventId")] = double(pass.lastEventId);
  j[QStringLiteral("eventCount")] = double(pass.eventCount);
  j[QStringLiteral("durationUs")] = pass.durationUs;
  j[QStringLiteral("durationMs")] = pass.durationUs / 1000.0;
  j[QStringLiteral("confidence")] = ConfidenceName(pass.confidence);
  return j;
}

QJsonObject PerformanceHotEventToJson(const PerformanceHotEvent &hotEvent)
{
  QJsonObject j;
  j[QStringLiteral("eventId")] = double(hotEvent.eventId);
  j[QStringLiteral("name")] = hotEvent.name;
  j[QStringLiteral("passId")] = hotEvent.passId;
  j[QStringLiteral("passName")] = hotEvent.passName;
  j[QStringLiteral("durationUs")] = hotEvent.durationUs;
  j[QStringLiteral("durationMs")] = hotEvent.durationUs / 1000.0;
  j[QStringLiteral("share")] = hotEvent.share;
  j[QStringLiteral("pipelineAvailable")] = hotEvent.pipelineAvailable;
  j[QStringLiteral("meshAvailable")] = hotEvent.meshAvailable;
  j[QStringLiteral("confidence")] = ConfidenceName(hotEvent.confidence);

  QJsonArray ev;
  for(const EvidenceRef &e : hotEvent.evidence)
    ev.push_back(EvidenceToJson(e));
  j[QStringLiteral("evidence")] = ev;

  // Explicit follow-up pointers so a client knows which additional tool to call
  // to expand the evidence: the pipeline snapshot serialiser and the mesh-data
  // tool. They mirror the plugin's `evidence` object keys.
  QJsonObject followup;
  if(hotEvent.pipelineAvailable)
    followup[QStringLiteral("pipeline")] = QStringLiteral("renderdoc_getPipelineStateAtEvent");
  if(hotEvent.meshAvailable)
    followup[QStringLiteral("mesh")] = QStringLiteral("renderdoc_getMeshData");
  j[QStringLiteral("followUp")] = followup;

  return j;
}

QJsonObject PerformanceConclusionsToJson(const PerformanceConclusions &conclusions)
{
  QJsonObject j;
  QJsonArray confirmed;
  for(const QString &s : conclusions.confirmed)
    confirmed.push_back(s);
  QJsonArray inferred;
  for(const QString &s : conclusions.inferred)
    inferred.push_back(s);
  QJsonArray toVerify;
  for(const QString &s : conclusions.toVerify)
    toVerify.push_back(s);
  j[QStringLiteral("confirmed")] = confirmed;
  j[QStringLiteral("inferred")] = inferred;
  j[QStringLiteral("toVerify")] = toVerify;
  return j;
}

QJsonObject PerformanceReportToJson(const PerformanceReport &report)
{
  QJsonObject j;
  j[QStringLiteral("available")] = report.available;
  j[QStringLiteral("timingAvailable")] = report.timingAvailable;
  j[QStringLiteral("capturePath")] = report.capturePath;
  j[QStringLiteral("api")] = report.api;
  j[QStringLiteral("frameNumber")] = double(report.frameNumber);
  j[QStringLiteral("totalTimedEvents")] = double(report.totalTimedEvents);
  j[QStringLiteral("totalDurationUs")] = report.totalDurationUs;
  j[QStringLiteral("totalDurationMs")] = report.totalDurationUs / 1000.0;
  j[QStringLiteral("hotEventLimit")] = double(report.hotEventLimit);
  j[QStringLiteral("confidence")] = ConfidenceName(report.confidence);
  j[QStringLiteral("limitation")] = report.limitation;

  QJsonArray passes;
  for(const PerformanceHotPass &p : report.hottestPasses)
    passes.push_back(PerformanceHotPassToJson(p));
  j[QStringLiteral("hottestPasses")] = passes;

  QJsonArray events;
  for(const PerformanceHotEvent &e : report.hottestEvents)
    events.push_back(PerformanceHotEventToJson(e));
  j[QStringLiteral("hottestEvents")] = events;

  j[QStringLiteral("conclusions")] = PerformanceConclusionsToJson(report.conclusions);

  QJsonArray evidence;
  for(const EvidenceRef &e : report.evidence)
    evidence.push_back(EvidenceToJson(e));
  j[QStringLiteral("evidence")] = evidence;

  return j;
}

QString PerformanceReportToMarkdown(const PerformanceReport &report)
{
  QStringList lines;
  lines << QStringLiteral("# RenderDoc Performance Report");
  lines << QString();
  lines << QStringLiteral("- Capture: %1").arg(
      report.capturePath.isEmpty() ? QStringLiteral("(unknown)") : report.capturePath);
  lines << QStringLiteral("- API: %1").arg(
      report.api.isEmpty() ? QStringLiteral("(unknown)") : report.api);
  lines << QStringLiteral("- GPU timings available: %1")
               .arg(report.timingAvailable ? QStringLiteral("yes") : QStringLiteral("no"));
  lines << QStringLiteral("- Timed events: %1").arg(report.totalTimedEvents);
  lines << QString();

  lines << QStringLiteral("## Hottest passes");
  if(report.hottestPasses.isEmpty())
  {
    lines << QStringLiteral("- None");
  }
  else
  {
    for(const PerformanceHotPass &p : report.hottestPasses)
    {
      lines << QStringLiteral("- %1 (EID %2-%3): %4 us")
                   .arg(p.name)
                   .arg(p.firstEventId)
                   .arg(p.lastEventId)
                   .arg(QString::number(p.durationUs, 'f', 2));
    }
  }
  lines << QString();

  lines << QStringLiteral("## Hottest events");
  if(report.hottestEvents.isEmpty())
  {
    lines << QStringLiteral("- None");
  }
  else
  {
    for(const PerformanceHotEvent &e : report.hottestEvents)
      lines << QStringLiteral("- EID %1: %2 - %3 us")
                   .arg(e.eventId)
                   .arg(e.name)
                   .arg(QString::number(e.durationUs, 'f', 2));
  }
  lines << QString();

  lines << QStringLiteral("## Evidence and limitations");
  for(const EvidenceRef &e : report.evidence)
  {
    lines << QStringLiteral("- %1: %2").arg(e.kind).arg(e.label);
  }
  if(!report.limitation.isEmpty())
    lines << QStringLiteral("- %1").arg(report.limitation);
  for(const QString &s : report.conclusions.confirmed)
    lines << QStringLiteral("- Confirmed: %1").arg(s);
  for(const QString &s : report.conclusions.inferred)
    lines << QStringLiteral("- Inferred: %1").arg(s);
  for(const QString &s : report.conclusions.toVerify)
    lines << QStringLiteral("- Follow-up: %1").arg(s);

  return lines.join(QString::fromUtf8("\n"));
}

//
// ---- Event Browser advanced filtering and AI context (P1-7) ---------------
//

QString ActionFlagNames(ActionFlags flags)
{
  struct Entry
  {
    ActionFlags bit;
    const char *name;
  };

  // Same ordering/naming as the P0 flags serialiser so the two never disagree.
  static const Entry bits[] = {
      {ActionFlags::Clear, "Clear"},
      {ActionFlags::Drawcall, "Drawcall"},
      {ActionFlags::Dispatch, "Dispatch"},
      {ActionFlags::MeshDispatch, "MeshDispatch"},
      {ActionFlags::CmdList, "CmdList"},
      {ActionFlags::SetMarker, "SetMarker"},
      {ActionFlags::PushMarker, "PushMarker"},
      {ActionFlags::PopMarker, "PopMarker"},
      {ActionFlags::Present, "Present"},
      {ActionFlags::MultiAction, "MultiAction"},
      {ActionFlags::Copy, "Copy"},
      {ActionFlags::Resolve, "Resolve"},
      {ActionFlags::GenMips, "GenMips"},
      {ActionFlags::PassBoundary, "PassBoundary"},
      {ActionFlags::DispatchRay, "DispatchRay"},
      {ActionFlags::BuildAccStruct, "BuildAccStruct"},
      {ActionFlags::Indexed, "Indexed"},
      {ActionFlags::Instanced, "Instanced"},
      {ActionFlags::Indirect, "Indirect"},
      {ActionFlags::BeginPass, "BeginPass"},
      {ActionFlags::EndPass, "EndPass"},
  };

  QStringList names;
  for(const Entry &e : bits)
  {
    if((flags & e.bit) != ActionFlags::NoFlags)
      names.push_back(QString::fromLatin1(e.name));
  }
  return names.join(QString::fromUtf8("|"));
}

bool IsRealDrawCall(ActionFlags flags)
{
  // Identical bit test to IsRealEffect: the plugin's string-flag isRealDrawCall
  // and its marker-group check describe the same set, so we share the predicate.
  return IsRealEffect(flags);
}

bool IsDebugMarkerName(const QString &name)
{
  const QString lower = name.toLower();
  return lower.contains(QStringLiteral("debug")) || lower.contains(QStringLiteral("marker")) ||
         lower.startsWith(QStringLiteral("rdoc"));
}

bool IsEmptyOperation(const ActionDescription &action)
{
  // A leaf real effect that produces no work: zero indices and zero instances.
  return action.children.empty() && IsRealDrawCall(action.flags) && action.numIndices == 0 &&
         action.numInstances == 0;
}

namespace
{
bool ParseUnsigned(const QJsonValue &v, uint32_t &out)
{
  const double n = v.toDouble(-1);
  if(!v.isDouble() || !std::isfinite(n) || n < 0 || n > double(UINT32_MAX) || std::floor(n) != n)
    return false;
  out = uint32_t(n);
  return true;
}
ToolResult InvalidFilter(const QString &key)
{
  return ToolResult::Failure(Error(ErrorCode::InvalidParams, key + QStringLiteral(" has an invalid value")));
}
rdcarray<ActionDescription> NameFilteredTree(const rdcarray<ActionDescription> &list,
                                           const SDFile &sdfile, const QString &filter)
{
  rdcarray<ActionDescription> out;
  for(const ActionDescription &a : list)
  {
    // The extension promotes every name hit, preserving the matched node's subtree.
    // Descendant hits are also promoted, even when already inside a matching parent.
    if(FromRdcStr(a.GetName(sdfile)).contains(filter, Qt::CaseInsensitive)) out.push_back(a);
    for(const ActionDescription &child : NameFilteredTree(a.children, sdfile, filter))
      out.push_back(child);
  }
  return out;
}
rdcarray<ActionDescription> AdvancedFilteredTree(const rdcarray<ActionDescription> &list,
    const SDFile &sdfile, const EventBrowserFilterOptions &o, bool ancestorMatch)
{
  rdcarray<ActionDescription> out;
  for(const ActionDescription &a : list)
  {
    const QString name = FromRdcStr(a.GetName(sdfile));
    const bool inScope = ancestorMatch || (!o.markerFilter.isEmpty() && name.contains(o.markerFilter, Qt::CaseInsensitive));
    rdcarray<ActionDescription> children = AdvancedFilteredTree(a.children, sdfile, o, inScope);
    const bool keep = (!(o.hasEventIdMin || o.eventIdMin != 0) || a.eventId >= o.eventIdMin) &&
        (!(o.hasEventIdMax || o.eventIdMax != 0) || a.eventId <= o.eventIdMax) &&
        (!o.onlyDrawCalls || IsRealDrawCall(a.flags)) && (!o.excludeMarkers || !IsMarkerGroup(a)) &&
        (!o.excludeDebugMarkers || !(IsMarkerGroup(a) && IsDebugMarkerName(name))) &&
        (!o.excludeEmptyOperations || !IsEmptyOperation(a)) &&
        (o.markerFilter.isEmpty() || inScope || !children.empty());
    if(keep)
    {
      ActionDescription copy = a;
      copy.children = children;
      out.push_back(copy);
    }
    else for(const ActionDescription &child : children) out.push_back(child);
  }
  return out;
}
}

ToolResult ParseEventBrowserFilters(const QJsonObject &args, EventBrowserFilterOptions &out)
{
  out = EventBrowserFilterOptions();
  for(const char *key : {"filter", "markerFilter"})
    if(args.contains(QString::fromLatin1(key)) && !args.value(QString::fromLatin1(key)).isString())
      return InvalidFilter(QString::fromLatin1(key));
  out.filter = args.value(QStringLiteral("filter")).toString();
  out.markerFilter = args.value(QStringLiteral("markerFilter")).toString();
  struct Boolean { const char *key; bool EventBrowserFilterOptions::*member; };
  for(const Boolean &b : {Boolean{"excludeMarkers", &EventBrowserFilterOptions::excludeMarkers},
      Boolean{"onlyDrawCalls", &EventBrowserFilterOptions::onlyDrawCalls},
      Boolean{"excludeDebugMarkers", &EventBrowserFilterOptions::excludeDebugMarkers},
      Boolean{"excludeEmptyOperations", &EventBrowserFilterOptions::excludeEmptyOperations},
      Boolean{"sortByDuration", &EventBrowserFilterOptions::sortByDuration}})
  {
    const QString key = QString::fromLatin1(b.key);
    if(args.contains(key) && !args.value(key).isBool()) return InvalidFilter(key);
    out.*(b.member) = args.value(key).toBool();
  }
  out.hasEventIdMin = args.contains(QStringLiteral("eventIdMin"));
  out.hasEventIdMax = args.contains(QStringLiteral("eventIdMax"));
  if(out.hasEventIdMin && !ParseUnsigned(args.value(QStringLiteral("eventIdMin")), out.eventIdMin)) return InvalidFilter(QStringLiteral("eventIdMin"));
  if(out.hasEventIdMax && !ParseUnsigned(args.value(QStringLiteral("eventIdMax")), out.eventIdMax)) return InvalidFilter(QStringLiteral("eventIdMax"));
  return ToolResult::Success(QJsonObject());
}

rdcarray<ActionDescription> FilterActionTree(const rdcarray<ActionDescription> &roots,
    const SDFile &sdfile, const EventBrowserFilterOptions &options, bool nameFirst)
{
  const rdcarray<ActionDescription> input = nameFirst && !options.filter.isEmpty()
      ? NameFilteredTree(roots, sdfile, options.filter) : roots;
  const rdcarray<ActionDescription> advanced = AdvancedFilteredTree(input, sdfile, options, false);
  return !nameFirst && !options.filter.isEmpty()
      ? NameFilteredTree(advanced, sdfile, options.filter) : advanced;
}

ToolResult ParseActionTimingOptions(const QJsonObject &args, ActionTimingOptions &out)
{
  out = ActionTimingOptions();
  out.legacyCounters = !args.contains(QStringLiteral("eventIds")) &&
      !args.contains(QStringLiteral("markerFilter")) && !args.contains(QStringLiteral("excludeMarkers")) &&
      !args.contains(QStringLiteral("onlyDrawCalls")) && !args.contains(QStringLiteral("limit"));
  if(args.contains(QStringLiteral("eventIds")))
  {
    if(!args.value(QStringLiteral("eventIds")).isArray()) return InvalidFilter(QStringLiteral("eventIds"));
    for(const QJsonValue &v : args.value(QStringLiteral("eventIds")).toArray())
    {
      uint32_t eid = 0;
      if(!ParseUnsigned(v, eid)) return InvalidFilter(QStringLiteral("eventIds"));
      if(!out.eventIds.contains(eid)) out.eventIds.push_back(eid);
    }
  }
  if(args.contains(QStringLiteral("eventId")) && !ParseUnsigned(args.value(QStringLiteral("eventId")), out.eventId)) return InvalidFilter(QStringLiteral("eventId"));
  if(args.contains(QStringLiteral("markerFilter")) && !args.value(QStringLiteral("markerFilter")).isString()) return InvalidFilter(QStringLiteral("markerFilter"));
  out.markerFilter = args.value(QStringLiteral("markerFilter")).toString();
  if(args.contains(QStringLiteral("onlyDrawCalls")) && !args.value(QStringLiteral("onlyDrawCalls")).isBool()) return InvalidFilter(QStringLiteral("onlyDrawCalls"));
  out.onlyDrawCalls = args.value(QStringLiteral("onlyDrawCalls")).toBool();
  if(args.contains(QStringLiteral("excludeMarkers")))
  {
    if(!args.value(QStringLiteral("excludeMarkers")).isArray()) return InvalidFilter(QStringLiteral("excludeMarkers"));
    for(const QJsonValue &v : args.value(QStringLiteral("excludeMarkers")).toArray())
    {
      if(!v.isString()) return InvalidFilter(QStringLiteral("excludeMarkers"));
      out.excludeMarkers.push_back(v.toString());
    }
  }
  for(const char *key : {"limit", "top"})
  {
    if(!args.contains(QString::fromLatin1(key))) continue;
    uint32_t n = 0;
    if(!ParseUnsigned(args.value(QString::fromLatin1(key)), n)) return InvalidFilter(QString::fromLatin1(key));
    if(QString::fromLatin1(key) == QStringLiteral("limit")) out.limit = int(qBound(1U, n, 2000U));
    else out.top = int(qBound(1U, n, 4096U));
  }
  return ToolResult::Success(QJsonObject());
}

ToolResult SelectActionTimings(const rdcarray<ActionDescription> &roots, const SDFile &sdfile,
    const QHash<quint32, double> &timings, const ActionTimingOptions &options)
{
  // Old native calls return every measured counter (including zero-duration and
  // non-leaf events), and top bounds only the separately ranked slowest list.
  if(options.legacyCounters)
  {
    QVector<quint32> ids = timings.keys().toVector();
    std::sort(ids.begin(), ids.end());
    QJsonArray all, slowest;
    QVector<QJsonObject> ranked;
    for(quint32 eid : ids)
    {
      const double us = timings.value(eid);
      if(!std::isfinite(us) || us < 0 || (options.eventId && options.eventId != eid)) continue;
      const QJsonObject row{{QStringLiteral("eventId"), double(eid)},
          {QStringLiteral("durationMicro"), us}};
      all.append(row); ranked.append(row);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const QJsonObject &a, const QJsonObject &b) {
      return a.value(QStringLiteral("durationMicro")).toDouble() > b.value(QStringLiteral("durationMicro")).toDouble();
    });
    for(int i = 0; i < ranked.size() && i < options.top; i++) slowest.append(ranked[i]);
    return ToolResult::Success(QJsonObject{{QStringLiteral("supported"), true},
        {QStringLiteral("counter"), QStringLiteral("EventGPUDuration")},
        {QStringLiteral("unit"), QStringLiteral("microseconds")},
        {QStringLiteral("eventCount"), all.size()}, {QStringLiteral("timings"), all},
        {QStringLiteral("slowest"), slowest}, {QStringLiteral("truncated"), false}});
  }
  QVector<QJsonObject> rows;
  uint32_t examined = 0;
  std::function<void(const rdcarray<ActionDescription> &, QStringList, bool)> visit;
  visit = [&](const rdcarray<ActionDescription> &list, QStringList path, bool scope) {
    for(const ActionDescription &a : list)
    {
      examined++;
      const QString name = FromRdcStr(a.GetName(sdfile));
      const bool marker = IsMarkerGroup(a);
      QStringList nextPath = path;
      if(marker) nextPath.push_back(name);
      bool excluded = false;
      for(const QString &needle : options.excludeMarkers)
        for(const QString &part : nextPath)
          excluded |= part.contains(needle, Qt::CaseInsensitive);
      // Excluded marker subtrees are proven out of scope without visiting their leaves.
      const bool nextScope = scope || (marker && name.contains(options.markerFilter, Qt::CaseInsensitive));
      if(!a.children.empty())
      {
        if(!excluded) visit(a.children, nextPath, nextScope);
        continue;
      }
      const double us = timings.value(a.eventId, -1.0);
      if(excluded || !nextScope || (options.onlyDrawCalls && !IsRealDrawCall(a.flags)) ||
         (options.eventId && options.eventId != a.eventId) ||
         (!options.eventIds.isEmpty() && !options.eventIds.contains(a.eventId)) ||
         !std::isfinite(us) || us <= 0) continue;
      QJsonArray markerPath;
      for(const QString &part : nextPath) markerPath.append(part);
      rows.push_back(QJsonObject{{QStringLiteral("eventId"), double(a.eventId)},
          {QStringLiteral("name"), name}, {QStringLiteral("flags"), ActionFlagNames(a.flags)},
          {QStringLiteral("durationUs"), us}, {QStringLiteral("durationMicro"), us},
          {QStringLiteral("durationMs"), us / 1000.0},
          {QStringLiteral("markerPath"), markerPath},
          {QStringLiteral("fullPath"), nextPath.isEmpty() ? name : nextPath.join(QStringLiteral(" -> ")) + QStringLiteral(" -> ") + name},
          {QStringLiteral("numIndices"), double(a.numIndices)}, {QStringLiteral("numInstances"), double(a.numInstances)}});
    }
  };
  visit(roots, QStringList(), options.markerFilter.isEmpty());
  std::stable_sort(rows.begin(), rows.end(), [](const QJsonObject &a, const QJsonObject &b) {
    return a.value(QStringLiteral("durationUs")).toDouble() > b.value(QStringLiteral("durationUs")).toDouble();
  });
  QJsonArray missing, returned, slowest;
  for(uint32_t eid : options.eventIds)
  {
    bool found = false;
    for(const QJsonObject &row : rows) found |= row.value(QStringLiteral("eventId")).toDouble() == double(eid);
    if(!found) missing.append(double(eid));
  }
  for(int i = 0; i < rows.size(); ++i)
  {
    if(i < options.limit) returned.append(rows[i]);
    if(i < options.top) slowest.append(rows[i]);
  }
  QJsonObject out{{QStringLiteral("supported"), true}, {QStringLiteral("available"), true},
      {QStringLiteral("counter"), QStringLiteral("EventGPUDuration")}, {QStringLiteral("unit"), QStringLiteral("microseconds")},
      {QStringLiteral("fetchedEventCount"), timings.size()}, {QStringLiteral("examinedEvents"), double(examined)},
      {QStringLiteral("eventCount"), returned.size()}, {QStringLiteral("returned"), returned.size()},
      {QStringLiteral("totalMatched"), rows.size()}, {QStringLiteral("truncated"), rows.size() > options.limit},
      {QStringLiteral("timings"), returned}, {QStringLiteral("slowest"), slowest}};
  if(!missing.isEmpty()) out[QStringLiteral("missingEventIds")] = missing;
  return ToolResult::Success(out);
}

// Flatten the already-filtered tree. Predicate interpretation belongs exclusively
// to AdvancedFilteredTree, not to the output representation.
static void FlattenEventBrowserTree(const rdcarray<ActionDescription> &list, const SDFile &sdfile,
                                   const QHash<quint32, double> &timings,
                                   const QHash<quint32, uint32_t> &depths,
                                   QVector<EventBrowserContextRow> &out)
{
  for(const ActionDescription &a : list)
  {
    const QString name = FromRdcStr(a.GetName(sdfile));
    {
      EventBrowserContextRow row;
      row.eventId = a.eventId;
      row.name = name;
      row.flags = ActionFlagNames(a.flags);
      row.depth = depths.value(a.eventId);
      row.numIndices = a.numIndices;
      row.numInstances = a.numInstances;
      if(timings.contains((quint32)a.eventId))
      {
        row.durationUs = timings.value((quint32)a.eventId, 0.0);
        row.hasDuration = true;
      }
      row.confidence = Confidence::Confirmed;
      out.push_back(row);
    }

    FlattenEventBrowserTree(a.children, sdfile, timings, depths, out);
  }
}

void FilterEventBrowserActions(const rdcarray<ActionDescription> &roots, const SDFile &sdfile,
                               const EventBrowserFilterOptions &options,
                               const QHash<quint32, double> &timings,
                               QVector<EventBrowserContextRow> &out,
                               EventBrowserFilterStats &stats)
{
  const int before = out.size();

  // The shared tree owner applies both filters, including the extension's name-hit
  // promotion semantics. Flatten without re-evaluating advanced predicates.
  const rdcarray<ActionDescription> filtered = FilterActionTree(roots, sdfile, options);
  QHash<quint32, uint32_t> depths;
  std::function<void(const rdcarray<ActionDescription> &, uint32_t)> count =
      [&](const rdcarray<ActionDescription> &list, uint32_t depth) {
    for(const ActionDescription &a : list)
    {
      stats.totalEvents++;
      depths.insert(a.eventId, depth);
      count(a.children, depth + 1);
    }
  };
  count(roots, 0);
  FlattenEventBrowserTree(filtered, sdfile, timings, depths, out);

  // Hottest-first ordering over just the rows this call produced. Rows without a
  // measurement sort last so an untimed capture still yields a stable list.
  if(options.sortByDuration && out.size() - before > 1)
  {
    std::sort(out.begin() + before, out.end(),
              [](const EventBrowserContextRow &l, const EventBrowserContextRow &r) {
                const double lu = l.hasDuration ? l.durationUs : -1.0;
                const double ru = r.hasDuration ? r.durationUs : -1.0;
                if(lu != ru)
                  return lu > ru;
                return l.eventId < r.eventId;
              });
  }

  stats.matchedEvents = (uint32_t)(out.size() - before);
}

QJsonObject EventBrowserContextRowToJson(const EventBrowserContextRow &row)
{
  QJsonObject j;
  j[QStringLiteral("eventId")] = double(row.eventId);
  j[QStringLiteral("name")] = row.name;
  j[QStringLiteral("flags")] = row.flags;
  j[QStringLiteral("depth")] = double(row.depth);
  j[QStringLiteral("numIndices")] = double(row.numIndices);
  j[QStringLiteral("numInstances")] = double(row.numInstances);
  if(row.hasDuration)
    j[QStringLiteral("durationUs")] = row.durationUs;
  j[QStringLiteral("confidence")] = ConfidenceName(row.confidence);
  return j;
}

QJsonObject EventBrowserFilterOptionsToJson(const EventBrowserFilterOptions &options)
{
  QJsonObject j;
  if(!options.filter.isEmpty()) j[QStringLiteral("filter")] = options.filter;
  if(!options.markerFilter.isEmpty())
    j[QStringLiteral("markerFilter")] = options.markerFilter;
  j[QStringLiteral("excludeMarkers")] = options.excludeMarkers;
  j[QStringLiteral("onlyDrawCalls")] = options.onlyDrawCalls;
  j[QStringLiteral("excludeDebugMarkers")] = options.excludeDebugMarkers;
  j[QStringLiteral("excludeEmptyOperations")] = options.excludeEmptyOperations;
  j[QStringLiteral("sortByDuration")] = options.sortByDuration;
  j[QStringLiteral("eventIdMin")] = double(options.eventIdMin);
  j[QStringLiteral("eventIdMax")] = double(options.eventIdMax);
  j[QStringLiteral("hasEventIdMin")] = options.hasEventIdMin || options.eventIdMin != 0;
  j[QStringLiteral("hasEventIdMax")] = options.hasEventIdMax || options.eventIdMax != 0;
  return j;
}

static QJsonArray EventBrowserRowsToJson(const QVector<EventBrowserContextRow> &rows)
{
  QJsonArray arr;
  for(const EventBrowserContextRow &r : rows)
    arr.append(EventBrowserContextRowToJson(r));
  return arr;
}

QJsonObject EventBrowserContextToJson(const EventBrowserContext &context)
{
  QJsonObject j;
  j[QStringLiteral("available")] = context.available;
  j[QStringLiteral("capturePath")] = context.capturePath;
  j[QStringLiteral("api")] = context.api;
  j[QStringLiteral("frameNumber")] = double(context.frameNumber);
  j[QStringLiteral("filters")] = EventBrowserFilterOptionsToJson(context.filters);
  j[QStringLiteral("totalEvents")] = double(context.totalEvents);
  j[QStringLiteral("matchedEvents")] = double(context.matchedEvents);
  j[QStringLiteral("truncated")] = context.truncated;
  j[QStringLiteral("confidence")] = ConfidenceName(context.confidence);
  if(!context.limitation.isEmpty())
    j[QStringLiteral("limitation")] = context.limitation;
  j[QStringLiteral("events")] = EventBrowserRowsToJson(context.rows);
  j[QStringLiteral("aiContext")] = context.aiContext;
  return j;
}

QString EventBrowserContextToText(const EventBrowserContext &context)
{
  QStringList lines;
  lines << QStringLiteral("RenderDoc event context: %1 filtered event(s) of %2.")
               .arg(context.matchedEvents)
               .arg(context.totalEvents);
  lines << QStringLiteral("Capture: %1 (API %2, frame %3)")
               .arg(context.capturePath.isEmpty() ? QStringLiteral("(unknown)")
                                                  : context.capturePath)
               .arg(context.api.isEmpty() ? QStringLiteral("(unknown)") : context.api)
               .arg(context.frameNumber);

  QStringList filterBits;
  if(!context.filters.markerFilter.isEmpty())
    filterBits << QStringLiteral("marker=\"%1\"").arg(context.filters.markerFilter);
  if(context.filters.excludeMarkers)
    filterBits << QStringLiteral("no markers");
  if(context.filters.onlyDrawCalls)
    filterBits << QStringLiteral("draws only");
  if(context.filters.excludeDebugMarkers)
    filterBits << QStringLiteral("no debug markers");
  if(context.filters.excludeEmptyOperations)
    filterBits << QStringLiteral("no empty ops");
  if(context.filters.eventIdMin != 0 || context.filters.eventIdMax != 0)
    filterBits << QStringLiteral("EID %1..%2")
                      .arg(context.filters.eventIdMin)
                      .arg(context.filters.eventIdMax);
  if(context.filters.sortByDuration)
    filterBits << QStringLiteral("sorted by duration");
  lines << QStringLiteral("Filters: %1").arg(filterBits.isEmpty() ? QStringLiteral("(none)")
                                                                  : filterBits.join(QStringLiteral(", ")));

  for(const EventBrowserContextRow &r : context.rows)
  {
    QString line = QStringLiteral("EID %1 %2").arg(r.eventId).arg(r.flags);
    if(!r.name.isEmpty())
      line += QStringLiteral(" \"%1\"").arg(r.name);
    if(r.hasDuration)
      line += QStringLiteral(" %1us").arg(r.durationUs, 0, 'f', 1);
    line += QStringLiteral(" (indices %1, instances %2)").arg(r.numIndices).arg(r.numInstances);
    lines << line;
  }

  if(context.truncated)
    lines << QStringLiteral("(list truncated; raise the limit to see more events)");
  if(!context.limitation.isEmpty())
    lines << context.limitation;

  lines << QStringLiteral("Focus on EIDs, flags, names, GPU duration, indices and instances; "
                          "request pipeline/shader/resource evidence before inferring a cause.");

  return lines.join(QString::fromUtf8("\n"));
}

//
// ---- P1-8 investigation bookmarks ----------------------------------------
//
// The extended bookmark record is split in two halves by design: `note` is the
// portable text that round-trips through the native capture bookmark section, and
// everything else (id, title, conclusion, analysis, screenshot, confidence) lives
// only in the sidecar document written next to the capture. That keeps a stock
// RenderDoc build - and the Python API - reading exactly the `{eventId,text}` it
// always did, while an extended reader still recovers the full investigation
// record. Reading never fails on a missing/partial sidecar: it degrades to the
// capture's plain bookmarks.
//
// The sidecar path is derived from the capture path, so it travels with the
// capture file and never has to be registered anywhere. An empty capture path
// yields an empty sidecar path, which the read/write paths treat as "unavailable".
QString BookmarkSidecarPath(const QString &capturePath)
{
  if(capturePath.isEmpty())
    return QString();
  return capturePath + QStringLiteral(".bookmarks.json");
}

// The enum round-trips by name (ConfidenceName writes the same strings), so a
// sidecar written by a future build that adds a confidence level still loads:
// an unknown name degrades to Confirmed rather than being dropped or guessed at.
static Confidence BookmarkConfidenceFromName(const QString &name)
{
  if(name.compare(QStringLiteral("inferred"), Qt::CaseInsensitive) == 0)
    return Confidence::Inferred;
  if(name.compare(QStringLiteral("toVerify"), Qt::CaseInsensitive) == 0)
    return Confidence::ToVerify;
  if(name.compare(QStringLiteral("low"), Qt::CaseInsensitive) == 0)
    return Confidence::Low;
  return Confidence::Confirmed;
}

// Records are read defensively: a missing field is simply empty, an event id may
// be a number or a string (older writers used a string), and a record that is not
// an object is treated as an empty record the caller can drop. No parse path
// below ever throws, so a partially-written sidecar cannot stop a capture load.
static BookmarkTaggedItem BookmarkTaggedItemFromJson(const QJsonValue &value)
{
  BookmarkTaggedItem item;
  if(!value.isObject())
    return item;

  const QJsonObject o = value.toObject();
  item.id = o.value(QStringLiteral("id")).toString();

  const QJsonValue eid = o.value(QStringLiteral("eventId"));
  if(eid.isString())
    item.eventId = eid.toString();
  else if(eid.isDouble())
    item.eventId = QString::number((qulonglong)eid.toDouble());

  item.title = o.value(QStringLiteral("title")).toString();
  item.note = o.value(QStringLiteral("note")).toString();
  item.conclusion = o.value(QStringLiteral("conclusion")).toString();
  item.analysis = o.value(QStringLiteral("analysis")).toString();
  item.screenshotPath = o.value(QStringLiteral("screenshotPath")).toString();
  item.confidence = BookmarkConfidenceFromName(o.value(QStringLiteral("confidence")).toString());
  return item;
}

QJsonObject BookmarkTaggedItemToJson(const BookmarkTaggedItem &item)
{
  QJsonObject j;
  j[QStringLiteral("id")] = item.id;

  if(!item.eventId.isEmpty())
  {
    // Prefer a numeric event id so the sidecar looks like a capture bookmark
    // record; fall back to a string when the value is not a plain number.
    bool ok = false;
    const qulonglong eid = item.eventId.toULongLong(&ok);
    if(ok)
      j[QStringLiteral("eventId")] = double(eid);
    else
      j[QStringLiteral("eventId")] = item.eventId;
  }

  if(!item.title.isEmpty())
    j[QStringLiteral("title")] = item.title;
  if(!item.note.isEmpty())
    j[QStringLiteral("note")] = item.note;
  if(!item.conclusion.isEmpty())
    j[QStringLiteral("conclusion")] = item.conclusion;
  if(!item.analysis.isEmpty())
    j[QStringLiteral("analysis")] = item.analysis;
  if(!item.screenshotPath.isEmpty())
    j[QStringLiteral("screenshotPath")] = item.screenshotPath;
  j[QStringLiteral("confidence")] = ConfidenceName(item.confidence);
  return j;
}

QJsonObject BookmarkFileToJson(const BookmarkFile &file)
{
  QJsonObject j;
  j[QStringLiteral("version")] = double(file.version);
  if(!file.capturePath.isEmpty())
    j[QStringLiteral("capturePath")] = file.capturePath;

  QJsonArray items;
  for(const BookmarkTaggedItem &item : file.items)
    items.append(BookmarkTaggedItemToJson(item));
  j[QStringLiteral("bookmarks")] = items;
  return j;
}

QJsonObject BookmarkSidecarToJson(const BookmarkSidecar &sidecar)
{
  QJsonObject j;
  j[QStringLiteral("available")] = sidecar.available;
  j[QStringLiteral("sidecarPresent")] = sidecar.sidecarPresent;
  j[QStringLiteral("version")] = double(sidecar.version);
  if(!sidecar.capturePath.isEmpty())
    j[QStringLiteral("capturePath")] = sidecar.capturePath;
  if(!sidecar.sidecarPath.isEmpty())
    j[QStringLiteral("sidecarPath")] = sidecar.sidecarPath;

  QJsonArray items;
  for(const BookmarkTaggedItem &item : sidecar.items)
    items.append(BookmarkTaggedItemToJson(item));
  j[QStringLiteral("bookmarks")] = items;
  j[QStringLiteral("count")] = items.size();
  if(!sidecar.limitation.isEmpty())
    j[QStringLiteral("limitation")] = sidecar.limitation;
  return j;
}

// Parse is intentionally total: any malformed input yields an empty-but-usable
// document whose version is 0, and the caller then simply falls back to the
// capture's own bookmarks. This is the compatibility boundary for "old capture
// data" - a capture with no sidecar takes this exact path.
BookmarkFile ParseBookmarkFile(const QByteArray &json)
{
  BookmarkFile file;
  if(json.isEmpty())
    return file;

  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
  if(err.error != QJsonParseError::NoError || !doc.isObject())
    return file;

  const QJsonObject o = doc.object();
  const QJsonValue ver = o.value(QStringLiteral("version"));
  if(ver.isDouble())
    file.version = (uint32_t)ver.toDouble();
  file.capturePath = o.value(QStringLiteral("capturePath")).toString();

  const QJsonArray arr = o.value(QStringLiteral("bookmarks")).toArray();
  for(const QJsonValue &v : arr)
  {
    const BookmarkTaggedItem item = BookmarkTaggedItemFromJson(v);
    // Drop rows that carry neither an identity nor any content; keep everything
    // else so a note-only record is not lost just because it has no id yet.
    if(!item.id.isEmpty() || !item.note.isEmpty())
      file.items.append(item);
  }
  return file;
}

QByteArray SerializeBookmarkFile(const BookmarkFile &file)
{
  return QJsonDocument(BookmarkFileToJson(file)).toJson(QJsonDocument::Compact);
}

bool BookmarkHasExtendedFields(const BookmarkTaggedItem &item)
{
  // `note` is deliberately excluded: it is the one field the native capture can
  // hold on its own, so a record with only a note needs no sidecar to survive.
  return !item.title.isEmpty() || !item.conclusion.isEmpty() || !item.analysis.isEmpty() ||
         !item.screenshotPath.isEmpty();
}

// Fold the sidecar into the capture's bookmark list. The capture is authoritative
// for the portable `note` text (so a rename done in the Event Browser strip wins);
// sidecar-only fields fill in the extended half. A sidecar entry is matched to a
// capture bookmark by event id, falling back to an exact note match so records
// written before an event id was recorded still bind. Sidecar entries that match
// nothing are appended rather than dropped - an orphaned note is still evidence.
QVector<BookmarkTaggedItem> MergeBookmarkSidecar(const rdcarray<EventBookmark> &bookmarks,
                                                 const BookmarkFile &sidecar)
{
  QVector<BookmarkTaggedItem> merged;
  QVector<bool> used(sidecar.items.size(), false);

  for(const EventBookmark &b : bookmarks)
  {
    BookmarkTaggedItem item;
    item.id = QStringLiteral("bm-%1").arg(b.eventId);
    item.eventId = QString::number((qulonglong)b.eventId);
    item.note = FromRdcStr(b.text);
    item.confidence = Confidence::Confirmed;

    for(int i = 0; i < sidecar.items.size(); i++)
    {
      if(used[i])
        continue;

      const BookmarkTaggedItem &s = sidecar.items[i];

      bool eidOk = false;
      const qulonglong sEid = s.eventId.toULongLong(&eidOk);
      const bool byEid = eidOk && sEid == (qulonglong)b.eventId;
      const bool byNote = s.eventId.isEmpty() && !s.note.isEmpty() && s.note == item.note;

      if(!byEid && !byNote)
        continue;

      if(!s.id.isEmpty())
        item.id = s.id;
      if(!s.title.isEmpty())
        item.title = s.title;
      if(!s.conclusion.isEmpty())
        item.conclusion = s.conclusion;
      if(!s.analysis.isEmpty())
        item.analysis = s.analysis;
      if(!s.screenshotPath.isEmpty())
        item.screenshotPath = s.screenshotPath;
      item.confidence = s.confidence;
      used[i] = true;
      break;
    }

    merged.append(item);
  }

  for(int i = 0; i < sidecar.items.size(); i++)
  {
    if(!used[i])
      merged.append(sidecar.items[i]);
  }

  return merged;
}

// ---- Investigation report (P1-9) ------------------------------------------
//

QStringList InvestigationReportLimitations(const InvestigationReport &report)
{
  QStringList out = report.limitations;

  auto appendOnce = [&out](const QString &s) {
    if(!s.isEmpty() && !out.contains(s))
      out.append(s);
  };

  if(!report.bookmarksAvailable)
    appendOnce(report.bookmarkLimitation.isEmpty()
                   ? QStringLiteral("Investigation bookmarks are unavailable for this capture.")
                   : report.bookmarkLimitation);

  if(!report.performance.timingAvailable)
    appendOnce(QStringLiteral("GPU timings are unavailable, so the performance ranking is a "
                              "hypothesis rather than a measurement."));

  if(!report.resources.notes.isEmpty())
    appendOnce(QStringLiteral("Resource byte sizes are creation-time estimates."));

  return out;
}

QJsonObject InvestigationReportToJson(const InvestigationReport &report)
{
  QJsonObject j;
  j[QStringLiteral("available")] = report.available;
  j[QStringLiteral("generatedAt")] = report.generatedAt;
  j[QStringLiteral("capturePath")] = report.capturePath;
  j[QStringLiteral("api")] = report.api;
  j[QStringLiteral("frameNumber")] = double(report.frameNumber);
  j[QStringLiteral("currentEvent")] = double(report.currentEvent);

  QJsonArray bookmarks;
  for(const BookmarkTaggedItem &b : report.bookmarks)
    bookmarks.append(BookmarkTaggedItemToJson(b));
  j[QStringLiteral("bookmarks")] = bookmarks;
  j[QStringLiteral("bookmarkCount")] = bookmarks.size();
  j[QStringLiteral("bookmarksAvailable")] = report.bookmarksAvailable;
  if(!report.bookmarkLimitation.isEmpty())
    j[QStringLiteral("bookmarkLimitation")] = report.bookmarkLimitation;

  j[QStringLiteral("performance")] = PerformanceReportToJson(report.performance);
  j[QStringLiteral("resources")] = ResourceMemoryAuditToJson(report.resources);

  QJsonArray evidence;
  for(const EvidenceRef &e : report.evidence)
    evidence.append(EvidenceToJson(e));
  j[QStringLiteral("evidence")] = evidence;

  QJsonArray limitations;
  for(const QString &s : InvestigationReportLimitations(report))
    limitations.append(s);
  j[QStringLiteral("limitations")] = limitations;

  j[QStringLiteral("confidence")] = ConfidenceName(report.confidence);
  return j;
}

QString InvestigationReportToMarkdown(const InvestigationReport &report)
{
  QStringList lines;
  lines << QStringLiteral("# RenderDoc Investigation Report");
  lines << QString();

  lines << QStringLiteral("- Generated: %1").arg(
      report.generatedAt.isEmpty() ? QStringLiteral("(unknown)") : report.generatedAt);
  lines << QStringLiteral("- Capture: %1").arg(
      report.capturePath.isEmpty() ? QStringLiteral("(unknown)") : report.capturePath);
  lines << QStringLiteral("- API: %1").arg(
      report.api.isEmpty() ? QStringLiteral("(unknown)") : report.api);
  lines << QStringLiteral("- Frame: %1").arg(report.frameNumber);
  lines << QStringLiteral("- Resources: %1 (%2 MiB)")
               .arg(report.resources.totalResources)
               .arg(QString::number(report.resources.totalMiB, 'f', 2));
  lines << QString();

  // Section title shared with the plugin so an existing client keeps working.
  lines << QStringLiteral("## Bookmarks");
  if(!report.bookmarksAvailable)
  {
    lines << QStringLiteral("- %1").arg(
        report.bookmarkLimitation.isEmpty() ? QStringLiteral("Unavailable")
                                            : report.bookmarkLimitation);
  }
  else if(report.bookmarks.isEmpty())
  {
    lines << QStringLiteral("- None");
  }
  else
  {
    for(const BookmarkTaggedItem &b : report.bookmarks)
    {
      QString line = QStringLiteral("- **%1**")
                         .arg(b.title.isEmpty() ? QStringLiteral("(untitled)") : b.title);
      if(!b.eventId.isEmpty())
        line += QStringLiteral(" (EID %1)").arg(b.eventId);
      if(!b.conclusion.isEmpty())
        line += QStringLiteral(" - %1").arg(b.conclusion);
      else if(!b.note.isEmpty())
        line += QStringLiteral(" - %1").arg(b.note);
      lines << line;
    }
  }
  lines << QString();

  lines << QStringLiteral("## Performance hotspots");
  if(!report.performance.timingAvailable)
  {
    lines << QStringLiteral(
        "- GPU timings unavailable; treat performance conclusions as hypotheses.");
  }
  else if(report.performance.hottestEvents.isEmpty())
  {
    lines << QStringLiteral("- None");
  }
  else
  {
    for(const PerformanceHotEvent &e : report.performance.hottestEvents)
    {
      lines << QStringLiteral("- EID %1: %2 - %3 us")
                   .arg(e.eventId)
                   .arg(e.name)
                   .arg(QString::number(e.durationUs, 'f', 2));
    }
  }
  lines << QString();

  lines << QStringLiteral("## Resource footprint");
  lines << QStringLiteral("- Tracked resources: %1").arg(report.resources.totalResources);
  lines << QStringLiteral("- Textures: %1 / Buffers: %2 / Shaders: %3")
               .arg(report.resources.textureCount)
               .arg(report.resources.bufferCount)
               .arg(report.resources.shaderCount);
  lines << QStringLiteral("- Total: %1 MiB").arg(QString::number(report.resources.totalMiB, 'f', 2));
  if(report.resources.largestResources.isEmpty())
  {
    lines << QStringLiteral("- Largest: None");
  }
  else
  {
    for(const ResourceMemoryRow &r : report.resources.largestResources)
      lines << QStringLiteral("- %1 (%2): %3 bytes")
                   .arg(r.name.isEmpty() ? ResourceIdToString(r.resourceId) : r.name)
                   .arg(r.type)
                   .arg(r.byteSize);
  }
  lines << QString();

  lines << QStringLiteral("## Limitations");
  const QStringList limitations = InvestigationReportLimitations(report);
  if(limitations.isEmpty())
  {
    lines << QStringLiteral("- None");
  }
  else
  {
    for(const QString &s : limitations)
      lines << QStringLiteral("- %1").arg(s);
  }

  return lines.join(QString::fromUtf8("\n"));
}

QString BookmarkSidecarToText(const BookmarkSidecar &sidecar)
{
  QStringList lines;
  lines << QStringLiteral("RenderDoc investigation bookmarks: %1 record(s).").arg(sidecar.items.size());

  if(!sidecar.available)
    lines << QStringLiteral("(capture not open locally; extended bookmark fields are unavailable)");
  else if(!sidecar.sidecarPresent)
    lines << QStringLiteral("(no extended bookmark sidecar yet; capture bookmarks are plain text)");

  if(!sidecar.capturePath.isEmpty())
    lines << QStringLiteral("Capture: %1").arg(sidecar.capturePath);
  if(!sidecar.sidecarPath.isEmpty())
    lines << QStringLiteral("Sidecar: %1").arg(sidecar.sidecarPath);

  for(const BookmarkTaggedItem &item : sidecar.items)
  {
    QString line = QStringLiteral("- [%1]")
                       .arg(item.id.isEmpty() ? QStringLiteral("no-id") : item.id);
    if(!item.eventId.isEmpty())
      line += QStringLiteral(" EID %1").arg(item.eventId);
    if(!item.title.isEmpty())
      line += QStringLiteral(" \"%1\"").arg(item.title);
    if(!item.conclusion.isEmpty())
      line += QStringLiteral(" => %1").arg(item.conclusion);
    lines << line;

    if(!item.note.isEmpty())
      lines << QStringLiteral("    note: %1").arg(item.note);
    if(!item.analysis.isEmpty())
      lines << QStringLiteral("    analysis: %1").arg(item.analysis);
    if(!item.screenshotPath.isEmpty())
      lines << QStringLiteral("    screenshot: %1").arg(item.screenshotPath);
    lines << QStringLiteral("    confidence: %1").arg(ConfidenceName(item.confidence));
  }

  if(!sidecar.limitation.isEmpty())
    lines << sidecar.limitation;

  return lines.join(QString::fromUtf8("\n"));
}

}    // namespace MCP
