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

#include "MCPQueryAdapter.h"
#include "MCPResourceTools.h"
#include "MCPSessionTools.h"
#include "MCPCompatibilityTools.h"
#include "MCPDiagnostics.h"
#include "MCPExternalTools.h"
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QSysInfo>

#include <QJsonArray>
#include <QWidget>
#include <QCoreApplication>
#include <exception>
#include <string.h>

#include "Code/Interface/QRDInterface.h"
#include "Code/QRDUtils.h"

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonValue>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

#include "Code/AI/MCPAnalysisModel.h"
#include "MCPComparisonTools.h"
#include <cmath>
#include <mutex>
#include <thread>

namespace MCP
{

// ---------------------------------------------------------------------------
// Field filtering / small helpers
// ---------------------------------------------------------------------------

static bool WantField(const QStringList &fields, const char *name)
{
  return fields.isEmpty() || fields.contains(QString::fromLatin1(name));
}

// rdcstr has no DoStringise() overload, so ToQStr() must never be used on it - it
// would resolve to an undefined template specialisation and fail to link. Convert
// explicitly instead.
static QString FromRdcStr(const rdcstr &str)
{
  return QString::fromUtf8(str.c_str(), (int)str.size());
}

static QJsonValue EventName(const ActionDescription &a, const SDFile &sdfile)
{
  // GetName() only touches the already-materialised structured file, so no
  // exception fallback is needed.
  return FromRdcStr(a.GetName(sdfile));
}

static QJsonObject ActionToJson(const ActionDescription &a, const SDFile &sdfile,
                                const QStringList &fields, int depth)
{
  QJsonObject j;
  if(WantField(fields, "eventId"))
    j[QStringLiteral("eventId")] = double(a.eventId);
  if(WantField(fields, "actionId"))
    j[QStringLiteral("actionId")] = double(a.actionId);
  if(WantField(fields, "name"))
    j[QStringLiteral("name")] = EventName(a, sdfile);
  if(WantField(fields, "flags"))
    j[QStringLiteral("flags")] = double((uint32_t)a.flags);
  if(a.numIndices > 0 && WantField(fields, "numIndices"))
    j[QStringLiteral("numIndices")] = double(a.numIndices);
  if(a.numInstances > 1 && WantField(fields, "numInstances"))
    j[QStringLiteral("numInstances")] = double(a.numInstances);

  const int childCount = (int)a.children.size();
  if(childCount > 0)
  {
    if(WantField(fields, "childCount"))
      j[QStringLiteral("childCount")] = childCount;

    // Bound recursion so a pathological capture can't blow the stack or produce
    // an unbounded document. 64 levels is far deeper than any real action tree.
    if(depth < 64)
    {
      QJsonArray kids;
      for(int i = 0; i < childCount; i++)
        kids.append(ActionToJson(a.children[i], sdfile, fields, depth + 1));
      j[QStringLiteral("children")] = kids;
    }
  }
  return j;
}

namespace
{
QJsonObject FilterFields(const QJsonObject &j, const QStringList &fields);
// Capture-local numeric map key, never a cross-capture identity.
uint64_t ResIdValue(ResourceId id)
{
  uint64_t v = 0;
  memcpy(&v, &id, sizeof(v));
  return v;
}
}

//
// ---- ResourceId and enum formatting ----
//

QString ResourceIdToString(ResourceId id)
{
  // ResourceId is an opaque struct around a private uint64_t. memcpy is the same
  // extraction technique DoStringise<ResourceId> uses in QRDUtils.cpp.
  uint64_t v = 0;
  memcpy(&v, &id, sizeof(v));
  return QStringLiteral("ResourceId::%1").arg((qulonglong)v);
}

namespace detail
{

QString ResourceTypeName(ResourceType t)
{
  return ToQStr(t);
}

QString TextureTypeName(TextureType t)
{
  return ToQStr(t);
}

QString CompTypeName(CompType t)
{
  return ToQStr(t);
}

QString GraphicsAPIName(GraphicsAPI api)
{
  return ToQStr(api);
}

QString TextureUsageString(TextureCategory flags)
{
  QString s;
  if((flags & TextureCategory::ShaderRead) != TextureCategory::NoFlags)
    s += QStringLiteral("ShaderRead|");
  if((flags & TextureCategory::ColorTarget) != TextureCategory::NoFlags)
    s += QStringLiteral("ColorTarget|");
  if((flags & TextureCategory::DepthTarget) != TextureCategory::NoFlags)
    s += QStringLiteral("DepthTarget|");
  if((flags & TextureCategory::ShaderReadWrite) != TextureCategory::NoFlags)
    s += QStringLiteral("ShaderReadWrite|");
  if((flags & TextureCategory::SwapBuffer) != TextureCategory::NoFlags)
    s += QStringLiteral("SwapBuffer|");
  if(!s.isEmpty())
    s.chop(1);
  return s.isEmpty() ? QStringLiteral("None") : s;
}

QString BufferUsageString(BufferCategory flags)
{
  QString s;
  if((flags & BufferCategory::Vertex) != BufferCategory::NoFlags)
    s += QStringLiteral("Vertex|");
  if((flags & BufferCategory::Index) != BufferCategory::NoFlags)
    s += QStringLiteral("Index|");
  if((flags & BufferCategory::Constants) != BufferCategory::NoFlags)
    s += QStringLiteral("Constants|");
  if((flags & BufferCategory::ReadWrite) != BufferCategory::NoFlags)
    s += QStringLiteral("ReadWrite|");
  if((flags & BufferCategory::Indirect) != BufferCategory::NoFlags)
    s += QStringLiteral("Indirect|");
  if(!s.isEmpty())
    s.chop(1);
  return s.isEmpty() ? QStringLiteral("None") : s;
}

QString ResourceFormatString(const ResourceFormat &f)
{
  if(f.type != ResourceFormatType::Regular)
  {
    // For special/packed/compressed formats the base type name plus component
    // type is enough to be unambiguous and matches what the UI shows.
    QString s = ToQStr(f.type);
    if(f.compType != CompType::Typeless)
      s += QStringLiteral("_") + ToQStr(f.compType);
    return s;
  }

  if(f.compCount == 0)
    return ToQStr(f.type);

  QString s;
  if(f.compCount > 0)
    s += QStringLiteral("R");
  if(f.compCount > 1)
    s += QStringLiteral("G");
  if(f.compCount > 2)
    s += QStringLiteral("B");
  if(f.compCount > 3)
    s += QStringLiteral("A");
  s += QString::number(f.compByteWidth * 8);
  s += QStringLiteral("_") + ToQStr(f.compType);
  return s;
}

// The remaining enum-to-name helpers wrap the ToStr<> specialisations emitted
// by QRDUtils.cpp. Keeping the definitions there avoids duplicating the
// non-inline specialisations across qrenderdoc translation units.
QString ShaderStageName(ShaderStage s)
{
  return ToQStr(s);
}

QString TopologyName(Topology t)
{
  return ToQStr(t);
}

QString FillModeName(FillMode m)
{
  return ToQStr(m);
}

QString CullModeName(CullMode m)
{
  return ToQStr(m);
}

QString CompareFunctionName(CompareFunction f)
{
  return ToQStr(f);
}

QString StencilOperationName(StencilOperation o)
{
  return ToQStr(o);
}

QString LogicOperationName(LogicOperation o)
{
  return ToQStr(o);
}

QString BlendMultiplierName(BlendMultiplier b)
{
  return ToQStr(b);
}

QString BlendOperationName(BlendOperation o)
{
  return ToQStr(o);
}

QString FilterModeName(FilterMode f)
{
  return ToQStr(f);
}

QString AddressModeName(AddressMode a)
{
  return ToQStr(a);
}

QString VarTypeName(VarType t)
{
  return ToQStr(t);
}

}    // namespace detail

// ---------------------------------------------------------------------------
// QueryAdapter
// ---------------------------------------------------------------------------

QueryAdapter::QueryAdapter(ICaptureContext *ctx)
    : m_Ctx(ctx), m_ApplicationSession(CreateNativeCaptureSession())
{
}

template <typename T>
T QueryAdapter::ReadOnUIThread(const std::function<T()> &fn) const
{
  if(m_Ctx == NULL)
    return fn();

  // Do not read GetMainWindow() off-thread merely to find the UI dispatcher.
  if(GUIInvoke::onUIThread())
    return fn();

  T ret = T();
  GUIInvoke::blockcall(QCoreApplication::instance(), [&]() { ret = fn(); });
  return ret;
}

ToolResult QueryAdapter::WithCaptureUI(
    CallContext &call, const std::function<ToolResult(ICaptureContext &)> &fn)
{
  // GetMainWindow() itself is CaptureContext access, so marshal using the
  // application object rather than obtaining a widget off the calling worker.
  if(m_Ctx == NULL)
    return ToolResult::Failure(Error(ErrorCode::Unsupported, "No native capture context"));
  ToolResult result;
  auto invoke = [&]() {
    if(call.IsCancelled())
      result = ToolResult::Failure(Error(ErrorCode::Cancelled, "Request cancelled"));
    else if(call.IsExpired())
      result = ToolResult::Failure(Error(ErrorCode::Timeout, "Request deadline passed"));
    else
      result = fn(*m_Ctx);
  };
  if(GUIInvoke::onUIThread())
    invoke();
  else
    GUIInvoke::blockcall(QCoreApplication::instance(), invoke);
  return result;
}

CaptureSnapshot QueryAdapter::Snapshot()
{
  return ReadOnUIThread<CaptureSnapshot>([this]() -> CaptureSnapshot {
    CaptureSnapshot s;
    if(m_Ctx == NULL)
      return s;

    s.hasCapture = m_Ctx->IsCaptureLoaded();
    if(!s.hasCapture)
      return s;

    s.loading = m_Ctx->IsCaptureLoading();
    s.local = m_Ctx->IsCaptureLocal();
    s.temporary = m_Ctx->IsCaptureTemporary();
    s.filename = FromRdcStr(m_Ctx->GetCaptureFilename());

    ResultDetails fatal = m_Ctx->GetFatalError();
    if(!fatal.OK())
      s.fatalError = QString(fatal.Message());

    CaptureModifications mods = m_Ctx->GetCaptureModifications();
    if(mods & CaptureModifications::Renames)
      s.modifications << QStringLiteral("Renames");
    if(mods & CaptureModifications::Bookmarks)
      s.modifications << QStringLiteral("Bookmarks");
    if(mods & CaptureModifications::Notes)
      s.modifications << QStringLiteral("Notes");
    if(mods & CaptureModifications::EditedShaders)
      s.modifications << QStringLiteral("EditedShaders");

    if(!s.loading)
    {
      APIProperties props = m_Ctx->APIProps();
      s.api = detail::GraphicsAPIName(props.pipelineType);
      s.localRenderer = detail::GraphicsAPIName(props.localRenderer);
      s.isRemote = props.remoteReplay;

      RemoteHost host = m_Ctx->Replay().CurrentRemote();
      if(host.IsValid())
        s.remoteHost = FromRdcStr(host.Name());

      FrameDescription frame = m_Ctx->FrameInfo();
      s.frameNumber = frame.frameNumber;

      // FrameStatistics holds one bind-statistics entry per ShaderStage. The
      // in-frame counters below are therefore the total number of bindings
      // recorded for the frame, not a distinct-resource count (the resource /
      // texture / buffer totals come from the ICaptureContext lists below).
      //
      // NOTE: this is the *renderdoc* global FrameStatistics from FrameDescription,
      // not MCP::FrameStatistics (the analysis model pulled in by
      // MCPAnalysisModel.h). Inside namespace MCP the unqualified name would bind
      // to the model type, so the global one must be explicitly qualified.
      const ::FrameStatistics &stats = frame.stats;
      uint32_t totalConstants = 0, totalSamplers = 0, totalResources = 0;
      for(const ConstantBindStats &cs : stats.constants)
        totalConstants += cs.sets;
      for(const SamplerBindStats &ss : stats.samplers)
        totalSamplers += ss.sets;
      for(const ResourceBindStats &rs : stats.resources)
        totalResources += rs.sets;
      s.numConstantsInFrame = totalConstants;
      s.numSamplersInFrame = totalSamplers;
      s.numTexturesInFrame = totalResources;
      s.numBuffersInFrame = totalResources;

      s.currentEvent = m_Ctx->CurEvent();
      s.selectedEvent = m_Ctx->CurSelectedEvent();
      s.numRootActions = (uint32_t)m_Ctx->CurRootActions().size();
      s.numResources = (uint32_t)m_Ctx->GetResources().size();
      s.numTextures = (uint32_t)m_Ctx->GetTextures().size();
      s.numBuffers = (uint32_t)m_Ctx->GetBuffers().size();
    }

    // Whether a usable replay controller exists without touching the replay
    // thread is best-effort here; the authoritative check is in DispatchBlock.
    if(!s.loading)
      s.replayAvailable = m_Ctx->Replay().GetCaptureAccess() != NULL;

    return s;
  });
}

QJsonObject QueryAdapter::SnapshotToJson(const CaptureSnapshot &s)
{
  QJsonObject j;
  j[QStringLiteral("hasCapture")] = s.hasCapture;
  j[QStringLiteral("loading")] = s.loading;
  j[QStringLiteral("local")] = s.local;
  j[QStringLiteral("temporary")] = s.temporary;
  j[QStringLiteral("replayAvailable")] = s.replayAvailable;
  if(s.hasCapture)
  {
    j[QStringLiteral("filename")] = s.filename;
    j[QStringLiteral("api")] = s.api;
    j[QStringLiteral("localRenderer")] = s.localRenderer;
    j[QStringLiteral("frameNumber")] = double(s.frameNumber);
    j[QStringLiteral("currentEvent")] = double(s.currentEvent);
    j[QStringLiteral("selectedEvent")] = double(s.selectedEvent);
    j[QStringLiteral("numRootActions")] = double(s.numRootActions);
    j[QStringLiteral("numResources")] = double(s.numResources);
    j[QStringLiteral("numTextures")] = double(s.numTextures);
    j[QStringLiteral("numBuffers")] = double(s.numBuffers);
    j[QStringLiteral("numTexturesInFrame")] = double(s.numTexturesInFrame);
    j[QStringLiteral("numBuffersInFrame")] = double(s.numBuffersInFrame);
    j[QStringLiteral("numSamplersInFrame")] = double(s.numSamplersInFrame);
    j[QStringLiteral("numConstantsInFrame")] = double(s.numConstantsInFrame);
    j[QStringLiteral("isRemote")] = s.isRemote;
    if(!s.remoteHost.isEmpty())
      j[QStringLiteral("remoteHost")] = s.remoteHost;
    if(!s.modifications.isEmpty())
    {
      QJsonArray mods;
      for(const QString &m : s.modifications)
        mods.append(m);
      j[QStringLiteral("modifications")] = mods;
    }
    if(!s.fatalError.isEmpty())
      j[QStringLiteral("fatalError")] = s.fatalError;
  }
  return j;
}

ToolResult QueryAdapter::ApiProperties(const QStringList &fields)
{
  return ReadOnUIThread<ToolResult>([this, fields]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

    APIProperties p = m_Ctx->APIProps();
    QJsonObject j;
    if(WantField(fields, "pipelineType"))
      j[QStringLiteral("pipelineType")] = detail::GraphicsAPIName(p.pipelineType);
    if(WantField(fields, "localRenderer"))
      j[QStringLiteral("localRenderer")] = detail::GraphicsAPIName(p.localRenderer);
    if(WantField(fields, "vendor"))
      j[QStringLiteral("vendor")] = ToQStr(p.vendor);
    if(WantField(fields, "remoteReplay"))
      j[QStringLiteral("remoteReplay")] = p.remoteReplay;
    if(WantField(fields, "degraded"))
      j[QStringLiteral("degraded")] = p.degraded;
    if(WantField(fields, "shaderDebugging"))
      j[QStringLiteral("shaderDebugging")] = p.shaderDebugging;
    if(WantField(fields, "pixelHistory"))
      j[QStringLiteral("pixelHistory")] = p.pixelHistory;
    if(WantField(fields, "rgpCapture"))
      j[QStringLiteral("rgpCapture")] = p.rgpCapture;

    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::FrameInfo()
{
  return ReadOnUIThread<ToolResult>([this]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

    FrameDescription f = m_Ctx->FrameInfo();
    // Explicitly the renderdoc global FrameStatistics - see the note in Snapshot().
    const ::FrameStatistics &st = f.stats;

    uint32_t totalConstants = 0, totalSamplers = 0, totalResources = 0;
    for(const ConstantBindStats &cs : st.constants)
      totalConstants += cs.sets;
    for(const SamplerBindStats &ss : st.samplers)
      totalSamplers += ss.sets;
    for(const ResourceBindStats &rs : st.resources)
      totalResources += rs.sets;

    QJsonObject stats;
    stats[QStringLiteral("recorded")] = st.recorded;
    stats[QStringLiteral("constantBindings")] = double(totalConstants);
    stats[QStringLiteral("samplerBindings")] = double(totalSamplers);
    stats[QStringLiteral("resourceBindings")] = double(totalResources);

    QJsonObject j;
    j[QStringLiteral("frameNumber")] = double(f.frameNumber);
    j[QStringLiteral("fileOffset")] = double(f.fileOffset);
    j[QStringLiteral("uncompressedFileSize")] = double(f.uncompressedFileSize);
    j[QStringLiteral("compressedFileSize")] = double(f.compressedFileSize);
    j[QStringLiteral("persistentSize")] = double(f.persistentSize);
    j[QStringLiteral("initDataSize")] = double(f.initDataSize);
    j[QStringLiteral("captureTime")] = double(f.captureTime);
    j[QStringLiteral("containsAnnotations")] = f.containsAnnotations;
    j[QStringLiteral("debugMessages")] = double((uint32_t)f.debugMessages.size());
    j[QStringLiteral("statistics")] = stats;

    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::Modifications()
{
  return ReadOnUIThread<ToolResult>([this]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));

    CaptureModifications mods = m_Ctx->GetCaptureModifications();
    QJsonArray arr;
    if(mods & CaptureModifications::Renames)
      arr.append(QStringLiteral("Renames"));
    if(mods & CaptureModifications::Bookmarks)
      arr.append(QStringLiteral("Bookmarks"));
    if(mods & CaptureModifications::Notes)
      arr.append(QStringLiteral("Notes"));
    if(mods & CaptureModifications::EditedShaders)
      arr.append(QStringLiteral("EditedShaders"));

    QJsonObject j;
    j[QStringLiteral("modifications")] = arr;
    j[QStringLiteral("any")] = !arr.isEmpty();
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::RootActions(uint32_t eventId, int maxCount, const QStringList &fields,
                                    const EventBrowserFilterOptions &filters)
{
  return ReadOnUIThread<ToolResult>([this, eventId, maxCount, fields, filters]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

    const SDFile &sdfile = m_Ctx->GetStructuredFile();
    QJsonArray arr;
    int emitted = 0;

    rdcarray<ActionDescription> selected;
    for(const ActionDescription &a : m_Ctx->CurRootActions())
      if(eventId == 0 || a.eventId == eventId) selected.push_back(a);
    const rdcarray<ActionDescription> filtered = FilterActionTree(selected, sdfile, filters, true);
    for(const ActionDescription &a : filtered)
    {
      if(maxCount > 0 && emitted >= maxCount) break;
      arr.append(ActionToJson(a, sdfile, fields, 0));
      emitted++;
    }

    QJsonObject j;
    j[QStringLiteral("actions")] = arr;
    j[QStringLiteral("count")] = emitted;
    j[QStringLiteral("totalMatched")] = double(filtered.size());
    j[QStringLiteral("truncated")] = size_t(emitted) < filtered.size();
    j[QStringLiteral("filters")] = EventBrowserFilterOptionsToJson(filters);
    if(eventId != 0)
      j[QStringLiteral("eventId")] = double(eventId);
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::Action(uint32_t eventId, const QStringList &fields)
{
  return ReadOnUIThread<ToolResult>([this, eventId, fields]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

    const ActionDescription *a = m_Ctx->GetAction(eventId);
    if(a == NULL)
      return ToolResult::Failure(Error(ErrorCode::InvalidParams,
          QStringLiteral("no action at event %1").arg(eventId)));

    const SDFile &sdfile = m_Ctx->GetStructuredFile();
    QJsonObject j = ActionToJson(*a, sdfile, fields, 0);
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::Resources(const QString &typeFilter, int maxCount, const QStringList &fields,
                                  int offset)
{
  return ReadOnUIThread<ToolResult>([this, typeFilter, maxCount, fields, offset]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));
    if(offset < 0 || maxCount < 0)
      return ToolResult::Failure(Error(ErrorCode::InvalidParams, "Invalid resource page"));
    if(!m_Ctx->Config().MCP_AllowSensitiveData)
      return ToolResult::Failure(Error(ErrorCode::Unsupported, "Resources require MCP_AllowSensitiveData"));

    const rdcarray<ResourceDescription> &resources = m_Ctx->GetResources();
    QJsonArray arr;
    int emitted = 0;
    for(const ResourceDescription &r : resources)
    {
      const QString typeName = detail::ResourceTypeName(r.type);
      if(!typeFilter.isEmpty() && typeName.compare(typeFilter, Qt::CaseInsensitive) != 0)
        continue;
      QJsonObject j;
      j[QStringLiteral("resourceId")] = ResourceIdToString(r.resourceId);
      j[QStringLiteral("id")] = double(0);    // reserved: numeric id is in the string form
      j[QStringLiteral("name")] = FromRdcStr(m_Ctx->GetResourceName(r.resourceId));
      j[QStringLiteral("type")] = typeName;
      j[QStringLiteral("autogeneratedName")] = r.autogeneratedName;
      arr.append(FilterFields(j, fields));
      emitted++;
    }

    QJsonObject j = ResourcePage(arr, maxCount > 0 ? maxCount : qMax(1, emitted), offset);
    if(!typeFilter.isEmpty())
      j[QStringLiteral("typeFilter")] = typeFilter;
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::Resource(ResourceId id)
{
  return ReadOnUIThread<ToolResult>([this, id]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));

    const ResourceDescription *r = m_Ctx->GetResource(id);
    if(r == NULL)
      return ToolResult::Failure(Error(ErrorCode::InvalidParams,
          QStringLiteral("no resource with id %1").arg(ResourceIdToString(id))));

    QJsonObject j;
    j[QStringLiteral("resourceId")] = ResourceIdToString(r->resourceId);
    j[QStringLiteral("name")] = FromRdcStr(m_Ctx->GetResourceName(r->resourceId));
    j[QStringLiteral("type")] = detail::ResourceTypeName(r->type);
    j[QStringLiteral("autogeneratedName")] = r->autogeneratedName;
    j[QStringLiteral("initialisationChunks")] = (int)r->initialisationChunks.size();
    j[QStringLiteral("derivedResources")] = (int)r->derivedResources.size();
    j[QStringLiteral("parentResources")] = (int)r->parentResources.size();
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::Textures(const QString &nameFilter, int maxCount, const QStringList &fields)
{
  return ReadOnUIThread<ToolResult>([this, nameFilter, maxCount, fields]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

    const rdcarray<TextureDescription> &textures = m_Ctx->GetTextures();
    QJsonArray arr;
    int emitted = 0;
    for(const TextureDescription &t : textures)
    {
      const QString name = FromRdcStr(m_Ctx->GetResourceName(t.resourceId));
      if(!nameFilter.isEmpty() && !name.contains(nameFilter, Qt::CaseInsensitive))
        continue;
      if(maxCount > 0 && emitted >= maxCount)
        break;

      QJsonObject j;
      if(WantField(fields, "resourceId"))
        j[QStringLiteral("resourceId")] = ResourceIdToString(t.resourceId);
      if(WantField(fields, "name"))
        j[QStringLiteral("name")] = name;
      if(WantField(fields, "format"))
        j[QStringLiteral("format")] = detail::ResourceFormatString(t.format);
      if(WantField(fields, "compCount"))
        j[QStringLiteral("compCount")] = (int)t.format.compCount;
      if(WantField(fields, "textureType"))
        j[QStringLiteral("textureType")] = detail::TextureTypeName(t.type);
      if(WantField(fields, "width"))
        j[QStringLiteral("width")] = double(t.width);
      if(WantField(fields, "height"))
        j[QStringLiteral("height")] = double(t.height);
      if(WantField(fields, "depth"))
        j[QStringLiteral("depth")] = double(t.depth);
      if(WantField(fields, "mips"))
        j[QStringLiteral("mips")] = double(t.mips);
      if(WantField(fields, "arraySize"))
        j[QStringLiteral("arraySize")] = double(t.arraysize);
      if(WantField(fields, "cubemap"))
        j[QStringLiteral("cubemap")] = t.cubemap;
      if(WantField(fields, "msaaSamples"))
        j[QStringLiteral("msaaSamples")] = double(t.msSamp > 1 ? t.msSamp : 1);
      if(WantField(fields, "msaaQuality"))
        j[QStringLiteral("msaaQuality")] = double(t.msQual);
      if(WantField(fields, "byteSize"))
        j[QStringLiteral("byteSize")] = double(t.byteSize);
      if(WantField(fields, "usage"))
        j[QStringLiteral("usage")] = detail::TextureUsageString(t.creationFlags);
      if(WantField(fields, "isSwapBuffer"))
        j[QStringLiteral("isSwapBuffer")] =
            (t.creationFlags & TextureCategory::SwapBuffer) != TextureCategory::NoFlags;
      if(WantField(fields, "isDepthTarget"))
        j[QStringLiteral("isDepthTarget")] =
            (t.creationFlags & TextureCategory::DepthTarget) != TextureCategory::NoFlags;
      if(WantField(fields, "isColorTarget"))
        j[QStringLiteral("isColorTarget")] =
            (t.creationFlags & TextureCategory::ColorTarget) != TextureCategory::NoFlags;
      arr.append(j);
      emitted++;
    }

    QJsonObject j;
    j[QStringLiteral("textures")] = arr;
    j[QStringLiteral("count")] = emitted;
    j[QStringLiteral("total")] = (int)textures.size();
    if(!nameFilter.isEmpty())
      j[QStringLiteral("nameFilter")] = nameFilter;
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::Texture(ResourceId id)
{
  return ReadOnUIThread<ToolResult>([this, id]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));

    TextureDescription *t = m_Ctx->GetTexture(id);
    if(t == NULL)
      return ToolResult::Failure(Error(ErrorCode::InvalidParams,
          QStringLiteral("no texture with id %1").arg(ResourceIdToString(id))));

    QJsonObject j;
    j[QStringLiteral("resourceId")] = ResourceIdToString(t->resourceId);
    j[QStringLiteral("name")] = FromRdcStr(m_Ctx->GetResourceName(t->resourceId));
    j[QStringLiteral("format")] = detail::ResourceFormatString(t->format);
    j[QStringLiteral("compCount")] = (int)t->format.compCount;
    j[QStringLiteral("textureType")] = detail::TextureTypeName(t->type);
    j[QStringLiteral("width")] = double(t->width);
    j[QStringLiteral("height")] = double(t->height);
    j[QStringLiteral("depth")] = double(t->depth);
    j[QStringLiteral("mips")] = double(t->mips);
    j[QStringLiteral("arraySize")] = double(t->arraysize);
    j[QStringLiteral("cubemap")] = t->cubemap;
    j[QStringLiteral("msaaSamples")] = double(t->msSamp > 1 ? t->msSamp : 1);
    j[QStringLiteral("msaaQuality")] = double(t->msQual);
    j[QStringLiteral("byteSize")] = double(t->byteSize);
    j[QStringLiteral("usage")] = detail::TextureUsageString(t->creationFlags);
    j[QStringLiteral("isSwapBuffer")] =
        (t->creationFlags & TextureCategory::SwapBuffer) != TextureCategory::NoFlags;
    j[QStringLiteral("isDepthTarget")] =
        (t->creationFlags & TextureCategory::DepthTarget) != TextureCategory::NoFlags;
    j[QStringLiteral("isColorTarget")] =
        (t->creationFlags & TextureCategory::ColorTarget) != TextureCategory::NoFlags;
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::Buffers(const QString &nameFilter, int maxCount, const QStringList &fields)
{
  return ReadOnUIThread<ToolResult>([this, nameFilter, maxCount, fields]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

    const rdcarray<BufferDescription> &buffers = m_Ctx->GetBuffers();
    QJsonArray arr;
    int emitted = 0;
    for(const BufferDescription &b : buffers)
    {
      const QString name = FromRdcStr(m_Ctx->GetResourceName(b.resourceId));
      if(!nameFilter.isEmpty() && !name.contains(nameFilter, Qt::CaseInsensitive))
        continue;
      if(maxCount > 0 && emitted >= maxCount)
        break;

      QJsonObject j;
      if(WantField(fields, "resourceId"))
        j[QStringLiteral("resourceId")] = ResourceIdToString(b.resourceId);
      if(WantField(fields, "name"))
        j[QStringLiteral("name")] = name;
      if(WantField(fields, "length"))
        j[QStringLiteral("length")] = double(b.length);
      if(WantField(fields, "byteSize"))
        j[QStringLiteral("byteSize")] = double(b.length);
      j[QStringLiteral("usage")] = detail::BufferUsageString(b.creationFlags);
      arr.append(j);
      emitted++;
    }

    QJsonObject j;
    j[QStringLiteral("buffers")] = arr;
    j[QStringLiteral("count")] = emitted;
    j[QStringLiteral("total")] = (int)buffers.size();
    if(!nameFilter.isEmpty())
      j[QStringLiteral("nameFilter")] = nameFilter;
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::Buffer(ResourceId id)
{
  return ReadOnUIThread<ToolResult>([this, id]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));

    BufferDescription *b = m_Ctx->GetBuffer(id);
    if(b == NULL)
      return ToolResult::Failure(Error(ErrorCode::InvalidParams,
          QStringLiteral("no buffer with id %1").arg(ResourceIdToString(id))));

    QJsonObject j;
    j[QStringLiteral("resourceId")] = ResourceIdToString(b->resourceId);
    j[QStringLiteral("name")] = FromRdcStr(m_Ctx->GetResourceName(b->resourceId));
    j[QStringLiteral("length")] = double(b->length);
    j[QStringLiteral("byteSize")] = double(b->length);
    j[QStringLiteral("usage")] = detail::BufferUsageString(b->creationFlags);
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::EventInfo(uint32_t eventId)
{
  return ReadOnUIThread<ToolResult>([this, eventId]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

    uint32_t eid = eventId != 0 ? eventId : m_Ctx->CurEvent();

    QJsonObject j;
    j[QStringLiteral("eventId")] = double(eid);
    j[QStringLiteral("currentEvent")] = double(m_Ctx->CurEvent());
    j[QStringLiteral("selectedEvent")] = double(m_Ctx->CurSelectedEvent());

    const ActionDescription *a = m_Ctx->GetAction(eid);
    if(a != NULL)
    {
      const SDFile &sdfile = m_Ctx->GetStructuredFile();
      j[QStringLiteral("action")] = ActionToJson(*a, sdfile, QStringList(), 1);
    }
    else
    {
      j[QStringLiteral("action")] = QJsonValue();
    }

    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::Bookmarks()
{
  return ReadOnUIThread<ToolResult>([this]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));

    const rdcarray<EventBookmark> marks = m_Ctx->GetBookmarks();
    QJsonArray arr;
    for(const EventBookmark &m : marks)
    {
      QJsonObject j;
      j[QStringLiteral("eventId")] = double(m.eventId);
      j[QStringLiteral("text")] = FromRdcStr(m.text);
      arr.append(j);
    }

    QJsonObject j;
    j[QStringLiteral("bookmarks")] = arr;
    j[QStringLiteral("count")] = arr.size();
    return ToolResult::Success(j);
  });
}

//
// ---- P1-8 investigation bookmarks ----------------------------------------
//
// A bookmark is split in two halves on purpose. The `note` round-trips through
// the capture's own `renderdoc/ui/bookmarks` section, so a bookmark placed by the
// AI tool appears in the Event Browser strip and a rename done there stays visible
// to the AI. The extended fields (id, title, conclusion, analysis, screenshot)
// live only in a sidecar JSON document written next to the capture, so widening
// the capture format - which would break the Event Browser strip, the Python API
// and every older build reading the capture - is never necessary:
//
//   * a stock capture opens with its plain bookmarks and no sidecar, and reads
//     degrade gracefully instead of failing;
//   * an extended capture opened by a stock build shows the plain bookmarks and
//     simply ignores a sidecar file it does not know about.
//
// Reads are snapshot-only (no replay thread). Writes mirror the note into the
// native bookmark list and persist the extended half to the sidecar; because
// ToolRegistry::Call has no side-effect gate, the tool registration - not the
// registry - is what requires the caller to have confirmed the write.
namespace
{
QString CapturePathFor(ICaptureContext *ctx)
{
  if(ctx == NULL)
    return QString();
  return FromRdcStr(ctx->GetCaptureFilename());
}

// Read a whole text file. A missing file is the common case (no sidecar exists
// until the first extended bookmark is written) so it is reported, not thrown.
bool ReadTextFile(const QString &path, QByteArray &out)
{
  out.clear();
  if(path.isEmpty())
    return false;

  QFile f(path);
  if(!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return false;

  out = f.readAll();
  f.close();
  return true;
}

}    // anonymous namespace

BookmarkSidecar QueryAdapter::ReadBookmarkSidecar()
{
  return ReadOnUIThread<BookmarkSidecar>([this]() -> BookmarkSidecar {
    BookmarkSidecar sidecar;

    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
    {
      sidecar.limitation = QStringLiteral("no capture is loaded");
      return sidecar;
    }

    const QString capturePath = CapturePathFor(m_Ctx);
    sidecar.capturePath = capturePath;
    sidecar.sidecarPath = BookmarkSidecarPath(capturePath);

    if(capturePath.isEmpty())
    {
      // A capture with no local filename (opened from a stream) has nowhere to
      // keep a sidecar; the plain bookmarks still read fine.
      sidecar.limitation = QStringLiteral(
          "capture has no local filename; extended bookmark fields cannot be stored");
    }
    else
    {
      QByteArray json;
      if(ReadTextFile(sidecar.sidecarPath, json))
      {
        const BookmarkFile file = ParseBookmarkFile(json);
        sidecar.version = file.version;
        sidecar.sidecarPresent = !file.items.isEmpty();

        // The capture's own bookmarks drive the ordering; the sidecar only adds
        // the extended fields and any sidecar-only records.
        sidecar.items = MergeBookmarkSidecar(m_Ctx->GetBookmarks(), file);
        sidecar.available = true;
        return sidecar;
      }
    }

    // No sidecar (or none readable): every bookmark is a plain capture bookmark.
    sidecar.items = MergeBookmarkSidecar(m_Ctx->GetBookmarks(), BookmarkFile());
    sidecar.available = true;
    return sidecar;
  });
}

ToolResult QueryAdapter::BookmarksExtended(int maxCount, const QStringList &fields)
{
  const int clamped = maxCount > 0 ? (maxCount > 2000 ? 2000 : maxCount) : 200;

  const BookmarkSidecar sidecar = ReadBookmarkSidecar();
  if(!sidecar.available)
    return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));

  BookmarkSidecar capped = sidecar;
  capped.items = sidecar.items.mid(0, clamped);
  const bool truncated = capped.items.size() < sidecar.items.size();

  QJsonObject j = BookmarkSidecarToJson(capped);
  j[QStringLiteral("total")] = sidecar.items.size();
  j[QStringLiteral("truncated")] = truncated;
  j[QStringLiteral("aiContext")] = BookmarkSidecarToText(capped);
  j[QStringLiteral("snapshot")] = SnapshotToJson(Snapshot());

  return ToolResult::Success(FilterFields(j, fields), BookmarkSidecarToText(capped));
}

ToolResult QueryAdapter::BookmarkTool(const QString &operation, const QJsonObject &args,
                                      CallContext &call)
{
  ToolResult authorized = AuthorizeBookmarkMutation(operation, args);
  if(!authorized.ok) return authorized;
  if(m_Ctx == NULL)
    return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "No capture is loaded"));
  return WithCaptureUI(call, [&](ICaptureContext &ctx) -> ToolResult {
    if(!ctx.IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "No capture is loaded"));
    if(ctx.IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "Capture is loading"));
    if(!ctx.IsCaptureLocal())
      return ToolResult::Failure(Error(ErrorCode::Unsupported, "Remote captures have no local bookmark sidecar"));
    const QString capturePath = CapturePathFor(&ctx);
    const QString path = BookmarkSidecarPath(capturePath);
    if(path.isEmpty())
      return ToolResult::Failure(Error(ErrorCode::Unsupported, "Capture has no local filename"));
    if(QFileInfo(path).isSymLink())
      return ToolResult::Failure(Error(ErrorCode::InvalidParams, "Refusing to replace a symlink bookmark sidecar"));
    BookmarkFile file;
    if(QFileInfo::exists(path))
    {
      QFile input(path);
      if(!input.open(QIODevice::ReadOnly))
        return ToolResult::Failure(Error(ErrorCode::InternalError, input.errorString()));
      const QByteArray bytes = input.readAll();
      if(input.error() != QFileDevice::NoError)
        return ToolResult::Failure(Error(ErrorCode::InternalError, input.errorString()));
      QJsonParseError error;
      const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
      if(error.error != QJsonParseError::NoError || !document.isObject() ||
         !document.object().value(QStringLiteral("bookmarks")).isArray() ||
         document.object().value(QStringLiteral("version")).toDouble(-1) != 1)
        return ToolResult::Failure(Error(ErrorCode::InvalidParams,
            "Malformed or unsupported bookmark sidecar; refusing to discard existing evidence"));
      const QJsonObject root = document.object();
      for(auto it = root.begin(); it != root.end(); ++it)
        if(it.key() != QStringLiteral("version") && it.key() != QStringLiteral("capturePath") && it.key() != QStringLiteral("bookmarks"))
          return ToolResult::Failure(Error(ErrorCode::Unsupported, "Unknown sidecar metadata; refusing to discard it"));
      if(root.contains(QStringLiteral("capturePath")) && !root.value(QStringLiteral("capturePath")).isString())
        return ToolResult::Failure(Error(ErrorCode::InvalidParams, "Malformed sidecar capture path"));
      // Reject rows the tolerant read parser would discard or reinterpret.
      for(const QJsonValue &row : document.object().value(QStringLiteral("bookmarks")).toArray())
      {
        if(!row.isObject())
          return ToolResult::Failure(Error(ErrorCode::InvalidParams, "Malformed bookmark row"));
        const QJsonObject object = row.toObject();
        const QStringList knownFields = {QStringLiteral("id"), QStringLiteral("eventId"), QStringLiteral("title"),
            QStringLiteral("note"), QStringLiteral("conclusion"), QStringLiteral("analysis"),
            QStringLiteral("screenshotPath"), QStringLiteral("confidence")};
        for(auto it = object.begin(); it != object.end(); ++it)
          if(!knownFields.contains(it.key()))
            return ToolResult::Failure(Error(ErrorCode::Unsupported, "Unknown bookmark metadata; refusing to discard it"));
        if(object.contains(QStringLiteral("confidence")))
        {
          const QString confidence = object.value(QStringLiteral("confidence")).toString();
          if(confidence != QStringLiteral("confirmed") && confidence != QStringLiteral("inferred") &&
             confidence != QStringLiteral("toVerify") && confidence != QStringLiteral("low"))
            return ToolResult::Failure(Error(ErrorCode::InvalidParams, "Unknown stored bookmark confidence"));
        }
        for(const char *key : {"id", "title", "note", "conclusion", "analysis", "screenshotPath", "confidence"})
          if(object.contains(QString::fromLatin1(key)) && !object.value(QString::fromLatin1(key)).isString())
            return ToolResult::Failure(Error(ErrorCode::InvalidParams, "Malformed bookmark text field"));
        if(object.contains(QStringLiteral("eventId")))
        {
          const QJsonValue event = object.value(QStringLiteral("eventId"));
          bool ok = false;
          const qulonglong n = event.isString() ? event.toString().toULongLong(&ok) : 0;
          if(event.isString() ? (!event.toString().isEmpty() && (!ok || n > UINT32_MAX)) :
             (!event.isDouble() || event.toDouble() < 0 || event.toDouble() > double(UINT32_MAX) ||
              double(uint32_t(event.toDouble())) != event.toDouble()))
            return ToolResult::Failure(Error(ErrorCode::InvalidParams, "Malformed bookmark event identity"));
        }
      }
      file = ParseBookmarkFile(bytes);
      if(file.items.size() != document.object().value(QStringLiteral("bookmarks")).toArray().size())
        return ToolResult::Failure(Error(ErrorCode::InvalidParams, "Bookmark parser would discard existing evidence"));
      QSet<QString> identities;
      for(const BookmarkTaggedItem &item : file.items)
      {
        if(!item.id.isEmpty() && identities.contains(item.id))
          return ToolResult::Failure(Error(ErrorCode::InvalidParams, "Duplicate bookmark sidecar identities"));
        if(!item.id.isEmpty()) identities.insert(item.id);
      }
      if(!file.capturePath.isEmpty() && QDir::cleanPath(file.capturePath) != QDir::cleanPath(capturePath))
        return ToolResult::Failure(Error(ErrorCode::InvalidParams, "Sidecar belongs to another capture"));
    }
    // The native note is authoritative; materialize old native-only bookmarks so
    // ids returned by list can also be used to update/remove them.
    file.items = MergeBookmarkSidecar(ctx.GetBookmarks(), file);
    file.capturePath = capturePath;
    BookmarkMutationPlan plan;
    ToolResult result = PlanBookmarkMutation(file, args, operation, ctx.CurEvent(), plan);
    if(!result.ok) return result;
    if(plan.newEventId && ctx.GetAction(plan.newEventId) == NULL)
      return ToolResult::Failure(Error(ErrorCode::InvalidParams, "Bookmark event is not in this capture"));
    const QByteArray bytes = SerializeBookmarkFile(plan.file);
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if(!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size())
      return ToolResult::Failure(Error(ErrorCode::InternalError, output.errorString()));
    if(call.IsCancelled() || call.IsExpired())
    {
      output.cancelWriting();
      return ToolResult::Failure(Error(call.IsCancelled() ? ErrorCode::Cancelled : ErrorCode::Timeout,
                                       "Bookmark mutation interrupted before commit"));
    }
    if(!output.commit())
      return ToolResult::Failure(Error(ErrorCode::InternalError, output.errorString()));
    // Once committed, finish native mirroring even if cancellation arrives: do
    // not report a cancellation while leaving the two representations divergent.
    if(plan.oldEventId && (plan.removed || plan.oldEventId != plan.newEventId))
      ctx.RemoveBookmark(plan.oldEventId);
    if(!plan.removed && plan.newEventId)
    {
      EventBookmark mark(plan.newEventId);
      const QByteArray note = plan.record.note.toUtf8();
      mark.text = rdcstr(note.constData(), size_t(note.size()));
      ctx.SetBookmark(mark);
    }
    result.structured[QStringLiteral("sidecarPath")] = path;
    result.structured[QStringLiteral("count")] = plan.file.items.size();
    result.structured[QStringLiteral("eventId")] = double(plan.removed ? plan.oldEventId : plan.newEventId);
    result.text = QStringLiteral("%1 investigation bookmark %2").arg(operation, plan.record.id);
    return result;
  });
}

ToolResult QueryAdapter::DiagnoseEnvironment(const QString &format, CallContext &call)
{
  if(call.IsCancelled() || call.IsExpired())
    return ToolResult::Failure(Error(call.IsCancelled() ? ErrorCode::Cancelled : ErrorCode::Timeout,
                                     "Environment diagnosis interrupted"));
  EnvironmentDiagnosticInput input;
  input.platform = QSysInfo::productType();
#if defined(RENDERDOC_PLATFORM_WIN32)
  input.platform = QStringLiteral("windows");
#elif defined(RENDERDOC_PLATFORM_APPLE)
  input.platform = QStringLiteral("macos");
#else
  input.platform = QStringLiteral("linux");
#endif
  ReadOnUIThread<bool>([&]() {
    if(!m_Ctx) return false;
    input.serverEnabled = m_Ctx->Config().MCP_Enabled;
    input.serverPort = m_Ctx->Config().MCP_Port;
    input.sensitiveDataAllowed = m_Ctx->Config().MCP_AllowSensitiveData;
    input.maliConfiguredPath = FromRdcStr(m_Ctx->Config().MCP_MaliCompilerPath);
    return true;
  });
  input.maliResolvedExecutable = ResolveMaliExecutablePath(input.maliConfiguredPath);
  input.maliExecutableExists = QFileInfo(input.maliResolvedExecutable).isFile();
  if(input.maliConfiguredPath.isEmpty() && input.maliExecutableExists)
    input.maliConfiguredPath = input.maliResolvedExecutable;
  input.supportsMali = input.platform == QStringLiteral("windows") || input.platform == QStringLiteral("linux");
  QJsonObject report = EnvironmentDiagnosticReportToJson(BuildEnvironmentDiagnosticReport(input));
  if(!m_Ctx)
  {
    QJsonArray observedChecks;
    for(const QJsonValue &value : report.value(QStringLiteral("checks")).toArray())
    {
      const QString id = value.toObject().value(QStringLiteral("id")).toString();
      if(id != QStringLiteral("mcp.server") && id != QStringLiteral("mcp.sensitiveData"))
        observedChecks.append(value);
    }
    observedChecks.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("mcp.configuration")},
        {QStringLiteral("severity"), QStringLiteral("info")},
        {QStringLiteral("available"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("summary"), QStringLiteral("Native CaptureContext configuration was not observed.")}});
    report[QStringLiteral("checks")] = observedChecks;
  }
  report[QStringLiteral("capture")] = SnapshotToJson(Snapshot());
  report[QStringLiteral("session")] = ApplicationSessionState();
  report[QStringLiteral("native")] = QJsonObject{{QStringLiteral("inProcess"), true},
      {QStringLiteral("applicationPath"), QCoreApplication::applicationFilePath()},
      {QStringLiteral("version"), QCoreApplication::applicationVersion()},
      {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()},
      {QStringLiteral("captureContextAvailable"), m_Ctx != NULL}};
  report[QStringLiteral("mcp")] = QJsonObject{{QStringLiteral("serverName"), QString::fromLatin1(kServerName)},
      {QStringLiteral("supportedProtocolVersion"), QString::fromLatin1(kLatestProtocolVersion)},
      {QStringLiteral("enabledConfiguration"), m_Ctx ? QJsonValue(input.serverEnabled) : QJsonValue(QJsonValue::Null)},
      {QStringLiteral("configuredPort"), m_Ctx ? QJsonValue(input.serverPort) : QJsonValue(QJsonValue::Null)},
      {QStringLiteral("listenerState"), QStringLiteral("unknown: configuration is not listener status")},
      {QStringLiteral("transportSession"), QStringLiteral("not observed; application session is separate")}};
  rdcarray<rdcstr> protocols;
  RENDERDOC_GetSupportedDeviceProtocols(&protocols);
  QJsonArray protocolNames;
  for(const rdcstr &protocol : protocols) protocolNames.append(FromRdcStr(protocol));
  report[QStringLiteral("deviceProtocols")] = protocolNames;
  QString sdk;
  const SDObject *sdkSetting = RENDERDOC_GetConfigSetting("Android.SDKDirPath");
  if(sdkSetting) sdk = FromRdcStr(sdkSetting->AsString());
#if defined(RENDERDOC_PLATFORM_WIN32)
  const QString adbName = QStringLiteral("adb.exe");
#else
  const QString adbName = QStringLiteral("adb");
#endif
  const QString configuredAdb = sdk.isEmpty() ? QString() : QDir(sdk).filePath(QStringLiteral("platform-tools/") + adbName);
  const QString pathAdb = QStandardPaths::findExecutable(adbName);
  report[QStringLiteral("adb")] = QJsonObject{{QStringLiteral("configuredSDKPath"), sdk},
      {QStringLiteral("configuredExecutable"), configuredAdb},
      {QStringLiteral("configuredExecutableExists"), !configuredAdb.isEmpty() && QFileInfo(configuredAdb).isFile()},
      {QStringLiteral("pathExecutable"), pathAdb},
      {QStringLiteral("protocolAvailable"), protocolNames.contains(QStringLiteral("adb")) || protocolNames.contains(QStringLiteral("android"))},
      {QStringLiteral("devices"), QJsonValue(QJsonValue::Null)},
      {QStringLiteral("deviceCoverage"), QStringLiteral("not probed: enumeration may start adb or alter device forwarding")}};
  QJsonArray limitations = report.value(QStringLiteral("limitations")).toArray();
  limitations.append(QStringLiteral("No adb process, device enumeration, installation, forwarding, environment mutation or target connection was performed. File presence does not establish adb readiness or device connectivity."));
  if(!m_Ctx) limitations.append(QStringLiteral("No native CaptureContext is bound; configuration is unavailable, not observed disabled."));
  report[QStringLiteral("limitations")] = limitations;
  const QString fmt = format == QStringLiteral("markdown") ? format : QStringLiteral("json");
  QString content = QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Indented));
  if(fmt == QStringLiteral("markdown"))
    content = QStringLiteral("# Native RenderDoc Environment Diagnosis\n\nObserved runtime, configuration and application session; device readiness is not inferred.\n\n```json\n") + content + QStringLiteral("```\n");
  return ToolResult::Success(QJsonObject{{QStringLiteral("report"), report},
      {QStringLiteral("format"), fmt}, {QStringLiteral("content"), content}}, content);
}

ToolResult QueryAdapter::PipelineSnapshot(const QStringList &fields)
{
  const CaptureSnapshot snapshot = Snapshot();
  if(!snapshot.hasCapture)
    return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));

  // The three sub-queries each marshal onto the UI thread themselves, so this
  // composes them without an extra hop.
  QJsonObject j;

  ToolResult api = ApiProperties(fields);
  if(!api.ok)
    return api;
  j[QStringLiteral("api")] = api.structured;

  ToolResult frame = FrameInfo();
  if(frame.ok)
    j[QStringLiteral("frame")] = frame.structured;

  ToolResult mods = Modifications();
  if(mods.ok)
    j[QStringLiteral("modifications")] = mods.structured;

  // The capture/selection snapshot is always included so a client can relate the
  // pipeline data back to a specific capture and event.
  j[QStringLiteral("snapshot")] = SnapshotToJson(Snapshot());

  return ToolResult::Success(j);
}

// ---- P0-6 replay dispatch ------------------------------------------------

ToolResult QueryAdapter::DispatchBlock(const char *toolName, CallContext &call,
                                       const std::function<ToolResult(IReplayController &)> &fn)
{
  IReplayManager *replayManager = NULL;
  const ToolResult ready = ReadOnUIThread<ToolResult>([this, &replayManager]() {
    if(m_Ctx == NULL)
      return ToolResult::Failure(Error(ErrorCode::InternalError, "no capture context"));
    if(!m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));
    replayManager = &m_Ctx->Replay();
    return ToolResult::Success(QJsonObject());
  });
  if(!ready.ok) return ready;

  if(call.IsCancelled())
    return ToolResult::Failure(Error(ErrorCode::Cancelled, "tool call cancelled"));

  if(call.IsExpired())
    return ToolResult::Failure(Error(ErrorCode::Timeout, "tool call deadline exceeded"));

  // MCP inspection tools may select one or many arbitrary events while gathering
  // pipeline facts. ReplayController::SetFrameEvent() also updates every live
  // replay output, including TextureViewer's output. Preserve the event selected
  // by qrenderdoc before dispatch and restore it before releasing the replay
  // thread, otherwise a read-only MCP request can leave TextureViewer presenting
  // resource contents from the final inspected event.
  const uint32_t restoreEvent = Snapshot().currentEvent;

  IReplayManager &replay = *replayManager;

  // Without a capture access handle the replay thread isn't ready for the switch
  // to a new capture; surface this distinctly from a plain "no capture".
  if(replay.GetCaptureAccess() == NULL)
    return ToolResult::Failure(Error(ErrorCode::ReplayUnavailable,
        "the replay thread is not ready to answer queries"));

  //
  // IMPORTANT (P0-6): ReplayManager::PushInvoke() deletes the invoke and returns
  // without ever calling the callback when the replay thread isn't running. So we
  // cannot rely on the callback to know whether our lambda ran - we track it
  // ourselves via `invoked`, and if it never ran we report ReplayUnavailable
  // rather than pretending the (default) result is real data.
  //
  ToolResult result = ToolResult::Failure(
      Error(ErrorCode::ReplayUnavailable, "the replay thread did not run the query"));
  bool invoked = false;

  try
  {
    replay.BlockInvoke([&](IReplayController *r) {
      invoked = true;

      if(r == NULL)
      {
        result = ToolResult::Failure(
            Error(ErrorCode::ReplayUnavailable, "the replay controller is not available"));
        return;
      }

      if(call.IsCancelled())
      {
        result = ToolResult::Failure(Error(ErrorCode::Cancelled, "tool call cancelled"));
        return;
      }

      if(call.IsExpired())
      {
        result = ToolResult::Failure(Error(ErrorCode::Timeout, "tool call deadline exceeded"));
        return;
      }

      try
      {
        result = fn(*r);
      }
      catch(const std::exception &e)
      {
        result = ToolResult::Failure(Error(ErrorCode::InternalError,
            QStringLiteral("replay query threw: %1").arg(QString::fromUtf8(e.what()))));
      }
      catch(...)
      {
        result = ToolResult::Failure(
            Error(ErrorCode::InternalError, "replay query threw an unknown exception"));
      }

      // Do this after the query even when it reports a tool-level error. Queries
      // such as pass graph construction can select hundreds of events, and the
      // currently displayed TextureViewer resource must remain evaluated at the
      // qrenderdoc UI's selected event rather than at the last inspected event.
      if(restoreEvent != 0)
      {
        try
        {
          r->SetFrameEvent(restoreEvent, false);
        }
        catch(...)
        {
          // The primary query result is still useful if a capture closes or
          // replay becomes unavailable while this read-only request completes.
          // Do not replace it with a restoration failure.
        }
      }
    });
  }
  catch(const std::exception &e)
  {
    return ToolResult::Failure(Error(ErrorCode::InternalError,
        QStringLiteral("failed to dispatch replay query: %1").arg(QString::fromUtf8(e.what()))));
  }

  if(call.IsExpired())
    return ToolResult::Failure(Error(ErrorCode::Timeout, "tool call deadline exceeded"));

  if(!invoked)
  {
    return ToolResult::Failure(Error(ErrorCode::ReplayUnavailable,
        QStringLiteral("the replay thread rejected the '%1' query")
            .arg(QString::fromUtf8(toolName ? toolName : "unknown"))));
  }

  return result;
}

ToolResult QueryAdapter::DispatchBlock(
    const char *toolName, const QString &capturePath, CallContext &call,
    const std::function<ToolResult(IReplayController &)> &fn)
{
  if(capturePath.trimmed().isEmpty())
    return ToolResult::Failure(Error(ErrorCode::InvalidParams, "capture path is empty"));
  ToolResult result = ToolResult::Failure(Error(ErrorCode::ReplayUnavailable,
                                                "independent replay was not invoked"));
  // No ReplayManager or UI state is borrowed. All controller calls, including
  // creation and destruction, run on this worker. Joining preserves callback
  // lifetime even when a driver exceeds the soft deadline.
  std::thread worker([&]() {
    static std::mutex independentReplay;
    std::lock_guard<std::mutex> lock(independentReplay);
    struct OwnedReplay
    {
      ICaptureFile *file = NULL;
      IReplayController *controller = NULL;
      ~OwnedReplay()
      {
        if(controller) controller->Shutdown();
        if(file) file->Shutdown();
      }
    } owned;
    try
    {
      if(call.IsCancelled() || call.IsExpired())
      {
        result = ToolResult::Failure(Error(call.IsCancelled() ? ErrorCode::Cancelled : ErrorCode::Timeout,
                                           "independent replay cancelled or deadline exceeded"));
        return;
      }
      owned.file = RENDERDOC_OpenCaptureFile();
      if(!owned.file)
      {
        result = ToolResult::Failure(Error(ErrorCode::ReplayUnavailable,
            QStringLiteral("RENDERDOC_OpenCaptureFile returned no capture file"),
            QJsonObject{{QStringLiteral("path"), capturePath},
                        {QStringLiteral("operation"), QStringLiteral("RENDERDOC_OpenCaptureFile")}}));
        return;
      }
      const ResultDetails opened = owned.file->OpenFile(rdcstr(capturePath.toUtf8().constData()), rdcstr(), NULL);
      if(!opened.OK())
      {
        result = ToolResult::Failure(Error(ErrorCode::ReplayUnavailable,
            QStringLiteral("OpenFile failed: %1").arg(FromRdcStr(opened.Message())),
            QJsonObject{{QStringLiteral("path"), capturePath},
                        {QStringLiteral("operation"), QStringLiteral("OpenFile")},
                        {QStringLiteral("resultCode"), int(opened.code)}}));
        return;
      }
      if(owned.file->LocalReplaySupport() != ReplaySupport::Supported)
      {
        result = ToolResult::Failure(Error(ErrorCode::Unsupported,
            QStringLiteral("local replay is unsupported for %1").arg(capturePath)));
        return;
      }
      const auto replay = owned.file->OpenCapture(ReplayOptions(), NULL);
      owned.controller = replay.second;
      if(!replay.first.OK() || !owned.controller)
      {
        result = ToolResult::Failure(Error(ErrorCode::ReplayUnavailable,
            QStringLiteral("OpenCapture failed: %1").arg(FromRdcStr(replay.first.Message())),
            QJsonObject{{QStringLiteral("path"), capturePath},
                        {QStringLiteral("operation"), QStringLiteral("OpenCapture")},
                        {QStringLiteral("resultCode"), int(replay.first.code)}}));
        return;
      }
      if(!call.IsCancelled() && !call.IsExpired())
        result = fn(*owned.controller);
    }
    catch(const std::exception &e)
    {
      result = ToolResult::Failure(Error(ErrorCode::InternalError,
          QStringLiteral("%1 independent replay threw: %2")
              .arg(QString::fromUtf8(toolName)).arg(QString::fromUtf8(e.what()))));
    }
    catch(...)
    {
      result = ToolResult::Failure(Error(ErrorCode::InternalError,
                                          "independent replay threw an unknown exception"));
    }
  });
  worker.join();
  if(call.IsCancelled())
    return ToolResult::Failure(Error(ErrorCode::Cancelled, "independent replay cancelled"));
  if(call.IsExpired())
    return ToolResult::Failure(Error(ErrorCode::Timeout, "independent replay deadline exceeded"));
  return result;
}

ToolResult QueryAdapter::ReadComparisonSnapshot(const QString &path, bool fetchTimings,
                                                CallContext &call, ComparisonSnapshot &out)
{
  ComparisonSnapshot snapshot;
  ToolResult result = DispatchBlock("capture comparison", path, call,
      [&](IReplayController &replay) -> ToolResult {
    CanonicalCapture canonical;
    canonical.capturePath = path;
    canonical.api = detail::GraphicsAPIName(replay.GetAPIProperties().pipelineType);
    canonical.frameNumber = replay.GetFrameInfo().frameNumber;
    canonical.unifyKey = UnifyCaptureKey(canonical.api, path);
    rdcarray<EventNode> nodes;
    FlattenEvents(replay.GetRootActions(), replay.GetStructuredFile(), nodes);
    BuildCanonicalEvents(nodes, canonical);
    QHash<quint64, TextureDescription> textures;
    QHash<quint64, BufferDescription> buffers;
    for(const auto &t : replay.GetTextures()) textures.insert(ResIdValue(t.resourceId), t);
    for(const auto &b : replay.GetBuffers()) buffers.insert(ResIdValue(b.resourceId), b);
    for(const ResourceDescription &resource : replay.GetResources())
    {
      if(call.IsCancelled() || call.IsExpired())
        return ToolResult::Failure(Error(call.IsCancelled() ? ErrorCode::Cancelled : ErrorCode::Timeout,
                                          "snapshot collection interrupted"));
      ResourceMemoryRow row;
      row.resourceId = resource.resourceId;
      row.name = resource.autogeneratedName ? QString() : FromRdcStr(resource.name);
      row.type = detail::ResourceTypeName(resource.type);
      const quint64 id = ResIdValue(resource.resourceId);
      bool known = false;
      if(textures.contains(id))
      {
        const TextureDescription &t = textures[id];
        row.byteSize = t.byteSize;
        row.width = t.width; row.height = t.height; row.depth = t.depth;
        row.mips = t.mips; row.arraySize = t.arraysize;
        row.format = detail::ResourceFormatString(t.format);
        row.textureType = detail::TextureTypeName(t.type);
        known = true;
      }
      else if(buffers.contains(id))
      {
        row.byteSize = buffers[id].length;
        known = true;
      }
      snapshot.resources.push_back(row);
      snapshot.bytesKnown.push_back(known);
    }
    BuildCanonicalResources(snapshot.resources, canonical);
    snapshot.regression = BuildRegressionSnapshot(canonical);
    // Canonical occurrence ordinals are useful display keys, not identity proof.
    // Use marker path + normalized label; duplicate labels stay ambiguous.
    for(RegressionEventEntry &event : snapshot.regression.events)
      event.key = QString::number(int(event.kind)) + QStringLiteral("|") + event.label;
    for(int i = 0; i < snapshot.regression.resources.size(); ++i)
    {
      RegressionResourceEntry &entry = snapshot.regression.resources[i];
      entry.key = snapshot.resources[i].name.isEmpty() ? QString() :
          entry.type + QStringLiteral("|") + NormalizeName(snapshot.resources[i].name);
      entry.confidence = Confidence::Low;
    }
    snapshot.timingStatus = QStringLiteral("not requested");
    if(fetchTimings)
    {
      snapshot.timingStatus = QStringLiteral("EventGPUDuration unsupported");
      for(GPUCounter counter : replay.EnumerateCounters())
      {
        if(counter != GPUCounter::EventGPUDuration) continue;
        snapshot.timingStatus = QStringLiteral("no valid EventGPUDuration samples");
        for(const CounterResult &sample : replay.FetchCounters({GPUCounter::EventGPUDuration}))
          if(sample.counter == GPUCounter::EventGPUDuration && std::isfinite(sample.value.d * 1000000.0) &&
             sample.value.d >= 0.0 && canonical.events.contains(sample.eventId))
            snapshot.timings.insert(sample.eventId, sample.value.d * 1000000.0);
        if(!snapshot.timings.isEmpty()) snapshot.timingStatus = QStringLiteral("measured");
        break;
      }
      ApplyEventTimings(snapshot.regression, snapshot.timings);
      snapshot.regression.timingAvailable = !snapshot.timings.isEmpty();
    }
    return ToolResult::Success(QJsonObject());
  });
  if(result.ok) out = snapshot;
  return result;
}

ToolResult QueryAdapter::DispatchBlockVoid(const char *toolName, CallContext &call,
                                           const std::function<void(IReplayController &)> &fn,
                                           const QJsonObject &structured)
{
  return DispatchBlock(toolName, call, [&](IReplayController &r) -> ToolResult {
    fn(r);
    return ToolResult::Success(structured);
  });
}

bool QueryAdapter::ReplayAvailable() const
{
  if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded() || m_Ctx->IsCaptureLoading())
    return false;

  IReplayManager &replay = m_Ctx->Replay();
  if(replay.GetCaptureAccess() == NULL)
    return false;

  bool ok = false;
  replay.BlockInvoke([&](IReplayController *r) { ok = (r != NULL); });
  return ok;
}

// ---- P1-2 frame analysis -------------------------------------------------

namespace
{
// Action *bit* names, emitted as a "|"-joined string so it matches the flag
// strings the plugin produced (e.g. "Drawcall|Instanced"). The numeric value is
// emitted alongside it so clients can also bit-test.
QString FlagsToString(ActionFlags flags)
{
  struct Entry
  {
    ActionFlags bit;
    const char *name;
  };
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
  return names.join(QStringLiteral("|"));
}

// Depth-first search for an action id inside the root action list. Replay only
// exposes the roots, so the tree has to be walked to reach a leaf event.
const ActionDescription *HotFindAction(const rdcarray<ActionDescription> &actions, uint32_t eventId)
{
  for(const ActionDescription &a : actions)
  {
    if(a.eventId == eventId)
      return &a;

    if(!a.children.empty())
    {
      const ActionDescription *found = HotFindAction(a.children, eventId);
      if(found != NULL)
        return found;
    }
  }
  return NULL;
}

// Lower-case stage tag ("vs", "ps", "cs", ...) so the output lines up with the
// plugin's stage keys rather than the native "Vertex"/"Pixel" spelling.
QString HotStageName(ShaderStage stage)
{
  switch(stage)
  {
    case ShaderStage::Vertex: return QStringLiteral("vs");
    case ShaderStage::Hull: return QStringLiteral("hs");
    case ShaderStage::Domain: return QStringLiteral("ds");
    case ShaderStage::Geometry: return QStringLiteral("gs");
    case ShaderStage::Pixel: return QStringLiteral("ps");
    case ShaderStage::Compute: return QStringLiteral("cs");
    case ShaderStage::Task: return QStringLiteral("ts");
    case ShaderStage::Mesh: return QStringLiteral("ms");
    case ShaderStage::RayGen: return QStringLiteral("raygen");
    case ShaderStage::Intersection: return QStringLiteral("intersection");
    case ShaderStage::AnyHit: return QStringLiteral("anyhit");
    case ShaderStage::ClosestHit: return QStringLiteral("closesthit");
    case ShaderStage::Miss: return QStringLiteral("miss");
    case ShaderStage::Callable: return QStringLiteral("callable");
    default: return QString();
  }
}

// Minimal texture facts needed to describe a render target.
struct HotTexInfo
{
  QString name;
  QString format;
  uint32_t width = 0;
  uint32_t height = 0;
};

QString TexName(ResourceId id, const QHash<quint64, QString> &names)
{
  const QString n = names.value(ResIdValue(id));
  return n.isEmpty() ? ResourceIdToString(id) : n;
}
}    // namespace

ToolResult QueryAdapter::AnalyzeFrame(const FrameAnalysisOptions &options, const QStringList &fields)
{
  return ReadOnUIThread<ToolResult>([this, options, fields]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

    const CaptureSnapshot snap = Snapshot();

    FrameDescription frame = m_Ctx->FrameInfo();

    FrameAnalysisOptions opts = options;
    if(opts.rdocVersion.isEmpty())
      opts.rdocVersion = QString::fromLatin1(FULL_VERSION_STRING);

    FrameAnalysis analysis;
    BuildFrameAnalysis(snap, m_Ctx->CurRootActions(), m_Ctx->GetStructuredFile(), m_Ctx->GetResources(),
                       m_Ctx->GetTextures(), m_Ctx->GetBuffers(), analysis, opts, &frame);

    QJsonObject j = FrameAnalysisToJson(analysis);

    // The analysis body is field-filterable, but the relating snapshot is always
    // present so a client can tell which capture/frame the result belongs to.
    if(!fields.isEmpty())
    {
      auto want = [&](const char *n) { return fields.contains(QString::fromLatin1(n)); };

      QJsonObject filtered;
      for(auto it = j.constBegin(); it != j.constEnd(); ++it)
      {
        const QString key = it.key();
        if(key == QStringLiteral("snapshot") || want(key.toUtf8().constData()))
          filtered[key] = it.value();
      }
      j = filtered;
    }

    j[QStringLiteral("snapshot")] = SnapshotToJson(Snapshot());
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::FrameSummary(const QStringList &fields)
{
  return ReadOnUIThread<ToolResult>([this, fields]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

    const SDFile &sdfile = m_Ctx->GetStructuredFile();
    const rdcarray<ActionDescription> &roots = m_Ctx->CurRootActions();

    auto want = [&](const char *n) {
      return fields.isEmpty() || fields.contains(QString::fromLatin1(n));
    };

    // Flatten the action tree once into a pre-order list; both the leaf-only and
    // the whole-tree counts below are derived from this single walk.
    rdcarray<EventNode> events;
    FlattenEvents(roots, sdfile, events);

    QJsonObject j;

    // Top-level passes: one entry per root marker/action, with its child count.
    if(want("topLevelPasses"))
    {
      QJsonArray passes;
      for(const ActionDescription &a : roots)
      {
        QJsonObject p;
        p[QStringLiteral("eventId")] = double(a.eventId);
        p[QStringLiteral("name")] = FromRdcStr(a.GetName(sdfile));
        p[QStringLiteral("flags")] = FlagsToString(a.flags);
        p[QStringLiteral("childCount")] = double((uint32_t)a.children.size());
        passes.push_back(p);
      }
      j[QStringLiteral("topLevelPasses")] = passes;
    }

    // Leaf-only effect counts, matching the plugin's frame summary: containers
    // and markers are not counted, only the concrete effects at the leaves
    // (nodes with no children).
    if(want("leafCounts"))
    {
      rdcarray<EventNode> leaves;
      for(const EventNode &n : events)
      {
        if(n.childCount == 0)
          leaves.push_back(n);
      }

      const EventCounts counts = CountEffects(leaves);

      QJsonObject leaf;
      leaf[QStringLiteral("total")] = double(counts.total);
      leaf[QStringLiteral("draws")] = double(counts.draws);
      leaf[QStringLiteral("dispatches")] = double(counts.dispatches);
      leaf[QStringLiteral("clears")] = double(counts.clears);
      j[QStringLiteral("leafCounts")] = leaf;
    }

    // Frame statistics, computed natively rather than shelling out to a bridge.
    if(want("statistics"))
    {
      FrameDescription frame = m_Ctx->FrameInfo();
      MCP::FrameStatistics stats;
      BuildFrameStatistics(frame, stats);
      j[QStringLiteral("statistics")] = FrameStatisticsToJson(stats);
    }

    if(want("counts"))
    {
      // Whole-tree effect counts (not just leaves) are useful denominators for
      // clients that compare against the leaf counts.
      j[QStringLiteral("counts")] = EventCountsToJson(CountEffects(events));
    }

    j[QStringLiteral("snapshot")] = SnapshotToJson(Snapshot());
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::HotEvents(uint32_t topCount, CallContext &call, const QStringList &fields)
{
  if(topCount == 0)
    topCount = 10;

  return DispatchBlock("renderdoc_getHotEvents", call,
                       [topCount, fields](IReplayController &replay) -> ToolResult {
    auto want = [&](const char *n) {
      return fields.isEmpty() || fields.contains(QString::fromLatin1(n));
    };

    // EventGPUDuration is the only supported counter for per-event timing; when
    // an API does not support it the report is marked unsupported instead of
    // silently returning empty timings.
    bool supported = false;
    for(const GPUCounter &c : replay.EnumerateCounters())
    {
      if(c == GPUCounter::EventGPUDuration)
      {
        supported = true;
        break;
      }
    }

    if(!supported)
    {
      QJsonObject j;
      j[QStringLiteral("supported")] = false;
      j[QStringLiteral("counter")] = QStringLiteral("EventGPUDuration");
      j[QStringLiteral("reason")] = QStringLiteral(
          "this capture's API does not expose the EventGPUDuration counter");
      return ToolResult::Success(j);
    }

    const rdcarray<CounterResult> results =
        replay.FetchCounters(rdcarray<GPUCounter>{GPUCounter::EventGPUDuration});

    const double kSecondsToMicro = 1000000.0;

    double totalUs = 0.0;
    rdcarray<EventTiming> timings;
    for(const CounterResult &r : results)
    {
      // A negative value means "not measured for this event"; it must not be
      // treated as a real (near-zero) duration.
      if(r.value.d < 0.0)
        continue;

      const double micro = r.value.d * kSecondsToMicro;
      totalUs += micro;

      EventTiming t;
      t.eventId = r.eventId;
      t.durationUs = micro;
      t.confidence = Confidence::Confirmed;
      timings.push_back(t);
    }

    std::sort(timings.begin(), timings.end(),
              [](const EventTiming &l, const EventTiming &r) { return l.durationUs > r.durationUs; });

    if((int)timings.size() > (int)topCount)
      timings.resize((size_t)topCount);

    QJsonObject j;
    j[QStringLiteral("supported")] = true;
    j[QStringLiteral("counter")] = QStringLiteral("EventGPUDuration");
    j[QStringLiteral("unit")] = QStringLiteral("us");
    j[QStringLiteral("measuredEventCount")] = double((uint32_t)results.size());
    j[QStringLiteral("totalDurationUs")] = totalUs;

    if(want("events"))
    {
      QJsonArray arr;
      for(const EventTiming &t : timings)
      {
        const double share = totalUs > 0.0 ? (t.durationUs / totalUs) : 0.0;

        QJsonObject e;
        e[QStringLiteral("eventId")] = double(t.eventId);
        e[QStringLiteral("durationUs")] = t.durationUs;
        e[QStringLiteral("durationMs")] = t.durationUs / 1000.0;
        e[QStringLiteral("share")] = share;
        e[QStringLiteral("confidence")] = ConfidenceName(t.confidence);
        e[QStringLiteral("evidence")] = EvidenceToJson(TimingEvidence(t.eventId, t.durationUs));
        arr.push_back(e);
      }
      j[QStringLiteral("events")] = arr;
    }

    if(want("hottest"))
    {
      if(timings.empty())
      {
        j[QStringLiteral("hottest")] = QJsonValue::Null;
      }
      else
      {
        const EventTiming &t = timings[0];
        QJsonObject e;
        e[QStringLiteral("eventId")] = double(t.eventId);
        e[QStringLiteral("durationUs")] = t.durationUs;
        e[QStringLiteral("durationMs")] = t.durationUs / 1000.0;
        e[QStringLiteral("share")] = totalUs > 0.0 ? (t.durationUs / totalUs) : 0.0;
        j[QStringLiteral("hottest")] = e;
      }
    }

    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::AnalyzeHotEvent(uint32_t eventId, bool includeShaderInfo, bool includeMeshData,
                                         CallContext &call, const QStringList &fields)
{
  if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
    return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
  if(m_Ctx->IsCaptureLoading())
    return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

  // Resource names and texture facts are materialised on the UI thread once and
  // then captured by value, because the replay-thread lambda below must not touch
  // ICaptureContext directly.
  const QHash<quint64, QString> resourceNames =
      ReadOnUIThread<QHash<quint64, QString>>([this]() -> QHash<quint64, QString> {
    QHash<quint64, QString> names;
    for(const ResourceDescription &r : m_Ctx->GetResources())
    {
      const QString name = FromRdcStr(m_Ctx->GetResourceName(r.resourceId));
      if(!name.isEmpty())
        names.insert(ResIdValue(r.resourceId), name);
    }
    return names;
  });

  const QHash<quint64, HotTexInfo> textures =
      ReadOnUIThread<QHash<quint64, HotTexInfo>>([this]() -> QHash<quint64, HotTexInfo> {
    QHash<quint64, HotTexInfo> map;
    for(const TextureDescription &t : m_Ctx->GetTextures())
    {
      HotTexInfo info;
      info.name = FromRdcStr(m_Ctx->GetResourceName(t.resourceId));
      info.format = detail::ResourceFormatString(t.format);
      info.width = t.width;
      info.height = t.height;
      map.insert(ResIdValue(t.resourceId), info);
    }
    return map;
  });

  return DispatchBlock(
      "renderdoc_analyzeHotEvent", call,
      [eventId, includeShaderInfo, includeMeshData, resourceNames, textures, fields](
          IReplayController &replay) -> ToolResult {
        auto want = [&](const char *n) {
          return fields.isEmpty() || fields.contains(QString::fromLatin1(n));
        };

        const rdcarray<ActionDescription> &roots = replay.GetRootActions();

        QJsonObject j;
        j[QStringLiteral("available")] = true;
        j[QStringLiteral("eventId")] = double(eventId);

        const ActionDescription *action = HotFindAction(roots, eventId);

        if(want("event"))
        {
          if(action == NULL)
          {
            j[QStringLiteral("event")] = QJsonValue::Null;
          }
          else
          {
            QJsonObject e;
            e[QStringLiteral("eventId")] = double(action->eventId);
            e[QStringLiteral("actionId")] = double(action->actionId);
            e[QStringLiteral("name")] = FromRdcStr(action->GetName(replay.GetStructuredFile()));
            e[QStringLiteral("flags")] = FlagsToString(action->flags);
            e[QStringLiteral("numIndices")] = double(action->numIndices);
            e[QStringLiteral("numInstances")] = double(action->numInstances);
            j[QStringLiteral("event")] = e;
          }
        }

        // ---- duration (facts from the timing counter) ----------------------
        double durationUs = 0.0;
        bool haveDuration = false;
        {
          bool supported = false;
          for(const GPUCounter &c : replay.EnumerateCounters())
          {
            if(c == GPUCounter::EventGPUDuration)
            {
              supported = true;
              break;
            }
          }

          if(supported)
          {
            const rdcarray<CounterResult> results =
                replay.FetchCounters(rdcarray<GPUCounter>{GPUCounter::EventGPUDuration});
            for(const CounterResult &r : results)
            {
              if(r.eventId == eventId && r.value.d >= 0.0)
              {
                durationUs = r.value.d * 1000000.0;
                haveDuration = true;
                break;
              }
            }
          }
        }

        if(haveDuration)
        {
          j[QStringLiteral("durationUs")] = durationUs;
          j[QStringLiteral("durationMs")] = durationUs / 1000.0;
          j[QStringLiteral("durationConfidence")] = ConfidenceName(Confidence::Confirmed);
          j[QStringLiteral("durationEvidence")] =
              EvidenceToJson(TimingEvidence(eventId, durationUs));
        }
        else
        {
          j[QStringLiteral("durationUs")] = QJsonValue::Null;
          j[QStringLiteral("durationMs")] = QJsonValue::Null;
          j[QStringLiteral("durationConfidence")] = ConfidenceName(Confidence::ToVerify);
        }

        // ---- pipeline state + resource/shader derivation -------------------
        const PipeState &pipe = replay.GetPipelineState();
        const APIProperties props = replay.GetAPIProperties();

        QJsonObject pipeline;
        if(want("pipelineState"))
        {
          pipeline[QStringLiteral("api")] = detail::GraphicsAPIName(props.pipelineType);

          QJsonArray shaders;
          for(int s = 0; s < int(ShaderStage::Count); s++)
          {
            const ShaderStage stage = ShaderStage(s);
            const ResourceId shader = pipe.GetShader(stage);
            if(shader == ResourceId())
              continue;

            const QString entry = FromRdcStr(pipe.GetShaderEntryPoint(stage));
            QString name = resourceNames.value(ResIdValue(shader));
            if(name.isEmpty())
              name = entry.isEmpty() ? QStringLiteral("unknown") : entry;

            QJsonObject sh;
            sh[QStringLiteral("stage")] = HotStageName(stage);
            sh[QStringLiteral("name")] = name;
            sh[QStringLiteral("resourceId")] = ResourceIdToString(shader);
            shaders.push_back(sh);
          }
          pipeline[QStringLiteral("shaders")] = shaders;

          QJsonArray targets;
          for(const Descriptor &d : pipe.GetOutputTargets())
          {
            if(d.resource == ResourceId())
              continue;

            const HotTexInfo info = textures.value(ResIdValue(d.resource));
            QJsonObject rt;
            rt[QStringLiteral("resourceId")] = ResourceIdToString(d.resource);
            rt[QStringLiteral("name")] = info.name.isEmpty() ? ResourceIdToString(d.resource) : info.name;
            rt[QStringLiteral("format")] = info.format;
            rt[QStringLiteral("width")] = double(info.width);
            rt[QStringLiteral("height")] = double(info.height);
            targets.push_back(rt);
          }
          pipeline[QStringLiteral("renderTargets")] = targets;

          const Descriptor depth = pipe.GetDepthTarget();
          if(depth.resource != ResourceId())
          {
            const HotTexInfo info = textures.value(ResIdValue(depth.resource));
            QJsonObject dt;
            dt[QStringLiteral("resourceId")] = ResourceIdToString(depth.resource);
            dt[QStringLiteral("name")] = info.name.isEmpty() ? ResourceIdToString(depth.resource) : info.name;
            dt[QStringLiteral("format")] = info.format;
            dt[QStringLiteral("width")] = double(info.width);
            dt[QStringLiteral("height")] = double(info.height);
            pipeline[QStringLiteral("depthTarget")] = dt;
          }
          else
          {
            pipeline[QStringLiteral("depthTarget")] = QJsonValue::Null;
          }

          j[QStringLiteral("pipelineState")] = pipeline;
        }

        // Resource counts come from the descriptors actually accessed at the
        // event, so they are facts (Confirmed) rather than reflection guesses.
        if(want("resourceCounts"))
        {
          uint32_t sampledTextures = 0, storageTextures = 0, buffers = 0, samplers = 0,
                   constantBuffers = 0;

          for(const UsedDescriptor &ud : pipe.GetAllUsedDescriptors(true))
          {
            const Descriptor &d = ud.descriptor;
            if(d.resource == ResourceId())
              continue;

            switch(d.type)
            {
              case DescriptorType::ConstantBuffer: constantBuffers++; break;
              case DescriptorType::Sampler: samplers++; break;
              case DescriptorType::ImageSampler: sampledTextures++; samplers++; break;
              case DescriptorType::Image: sampledTextures++; break;
              case DescriptorType::ReadWriteImage:
              case DescriptorType::ReadWriteTypedBuffer: storageTextures++; break;
              case DescriptorType::Buffer:
              case DescriptorType::TypedBuffer:
              case DescriptorType::ReadWriteBuffer:
              case DescriptorType::AccelerationStructure: buffers++; break;
              default: break;
            }
          }

          QJsonObject counts;
          counts[QStringLiteral("sampledTextures")] = double(sampledTextures);
          counts[QStringLiteral("storageTextures")] = double(storageTextures);
          counts[QStringLiteral("buffers")] = double(buffers);
          counts[QStringLiteral("samplers")] = double(samplers);
          counts[QStringLiteral("constantBuffers")] = double(constantBuffers);
          counts[QStringLiteral("confidence")] = ConfidenceName(Confidence::Confirmed);
          j[QStringLiteral("resourceCounts")] = counts;
        }

        // Per-stage shader summary: what each stage declares, plus its evidence.
        if(includeShaderInfo && want("shaderInfo"))
        {
          QJsonArray info;
          for(int s = 0; s < int(ShaderStage::Count); s++)
          {
            const ShaderStage stage = ShaderStage(s);
            const ResourceId shader = pipe.GetShader(stage);
            if(shader == ResourceId())
              continue;

            const ShaderReflection *refl = pipe.GetShaderReflection(stage);

            // Split the reflection's declared resources into sampled textures,
            // buffers, read/write payloads and samplers, so the per-stage summary
            // matches the plugin's texture/buffer/constant-buffer breakdown.
            uint32_t roTextures = 0, roBuffers = 0, rwCount = 0, cbCount = 0, sampCount = 0;
            if(refl != NULL)
            {
              for(const ShaderResource &r : refl->readOnlyResources)
              {
                if(r.textureType == TextureType::Buffer)
                  roBuffers++;
                else
                  roTextures++;
              }
              rwCount = (uint32_t)refl->readWriteResources.size();
              cbCount = (uint32_t)refl->constantBlocks.size();
              sampCount = (uint32_t)refl->samplers.size();
            }

            const QString entry = FromRdcStr(pipe.GetShaderEntryPoint(stage));
            QString name = resourceNames.value(ResIdValue(shader));
            if(name.isEmpty())
              name = entry.isEmpty() ? QStringLiteral("unknown") : entry;

            QJsonObject sh;
            sh[QStringLiteral("stage")] = HotStageName(stage);
            sh[QStringLiteral("name")] = name;
            sh[QStringLiteral("resourceId")] = ResourceIdToString(shader);
            sh[QStringLiteral("boundTextures")] = double(roTextures);
            sh[QStringLiteral("boundStorageTextures")] = double(rwCount);
            sh[QStringLiteral("boundBuffers")] = double(roBuffers);
            sh[QStringLiteral("boundConstantBuffers")] = double(cbCount);
            sh[QStringLiteral("boundSamplers")] = double(sampCount);
            sh[QStringLiteral("confidence")] =
                ConfidenceName(refl != NULL ? Confidence::Confirmed : Confidence::Inferred);
            sh[QStringLiteral("evidence")] =
                EvidenceToJson(ShaderEvidence(shader, HotStageName(stage), name));
            info.push_back(sh);
          }
          j[QStringLiteral("shaderInfo")] = info;
        }

        // ---- mesh data (optional) ------------------------------------------
        if(includeMeshData && want("meshData"))
        {
          MeshFormat mf = replay.GetPostVSData(0, 0, MeshDataStage::VSIn);

          if(action != NULL)
          {
            if(mf.numIndices == 0)
              mf.numIndices = action->numIndices;
            if(mf.baseVertex == 0)
              mf.baseVertex = action->baseVertex;
            if(mf.indexResourceId == ResourceId() && (action->flags & ActionFlags::Indexed))
            {
              const BoundVBuffer ib = pipe.GetIBuffer();
              mf.indexResourceId = ib.resourceId;
              mf.indexByteOffset = ib.byteOffset;
              mf.indexByteStride = ib.byteStride;
            }
          }

          QJsonObject mesh;
          mesh[QStringLiteral("topology")] = detail::TopologyName(pipe.GetPrimitiveTopology());
          mesh[QStringLiteral("indexCount")] = double(mf.numIndices);

          // Vertex count: the highest referenced index + 1 when indexed, else the
          // number of indices (a non-indexed draw's vertices are drawn in order).
          uint32_t vertexCount = mf.numIndices;
          const bool indexed = mf.indexResourceId != ResourceId() &&
                               mf.indexByteStride != 0 && mf.indexByteStride <= 4;
          if(indexed && mf.numIndices > 0)
          {
            const uint32_t probe = qMin(mf.numIndices, 4096u);
            const bytebuf idx = replay.GetBufferData(mf.indexResourceId, mf.indexByteOffset,
                                                     uint64_t(probe) * uint64_t(mf.indexByteStride));
            const byte *p = idx.data();
            uint32_t maxIdx = 0;
            for(uint32_t i = 0; i < probe; i++)
            {
              uint32_t v = 0;
              if(mf.indexByteStride == 1)
                v = uint32_t(p[i]);
              else if(mf.indexByteStride == 2)
              {
                uint16_t h = 0;
                memcpy(&h, p + uint64_t(i) * 2, 2);
                v = h;
              }
              else
              {
                memcpy(&v, p + uint64_t(i) * 4, 4);
              }
              maxIdx = qMax(maxIdx, v);
            }
            vertexCount = maxIdx + 1u;
          }
          mesh[QStringLiteral("vertexCount")] = double(vertexCount);

          QJsonArray attrs;
          for(const VertexInputAttribute &a : pipe.GetVertexInputs())
          {
            QJsonObject o;
            o[QStringLiteral("name")] = FromRdcStr(a.name);
            o[QStringLiteral("semantic")] = FromRdcStr(a.name);
            o[QStringLiteral("format")] = detail::ResourceFormatString(a.format);
            attrs.push_back(o);
          }
          mesh[QStringLiteral("attributes")] = attrs;

          j[QStringLiteral("meshData")] = mesh;
        }

        if(want("evidence"))
        {
          QJsonArray evidence;
          if(action != NULL)
            evidence.push_back(EvidenceToJson(EventEvidence(eventId, FromRdcStr(action->GetName(
                                                                  replay.GetStructuredFile())))));
          if(haveDuration)
            evidence.push_back(EvidenceToJson(TimingEvidence(eventId, durationUs)));
          j[QStringLiteral("evidence")] = evidence;
        }

        return ToolResult::Success(j);
      });
}

// ---- P1-3 pass graph, producer/consumer tracking and reverse search ------

namespace
{
// Rebuild a ResourceId from its raw numeric value (the inverse of ResIdValue).
ResourceId ResourceIdFromValue(uint64_t v)
{
  ResourceId id;
  memcpy(&id, &v, sizeof(v));
  return id;
}

bool VecHasId(const QVector<quint64> &v, quint64 id)
{
  for(quint64 x : v)
  {
    if(x == id)
      return true;
  }
  return false;
}

void AddUniqueId(QVector<quint64> &v, quint64 id)
{
  if(id != 0 && !VecHasId(v, id))
    v.push_back(id);
}

void AddUniqueShader(QVector<ShaderRef> &v, const ShaderRef &s)
{
  for(const ShaderRef &x : v)
  {
    if(x.id == s.id && x.stage == s.stage)
      return;
  }
  v.push_back(s);
}

// Every leaf (effect) action underneath `action`, in pre-order.
void CollectLeavesRecursive(const ActionDescription &action,
                            rdcarray<const ActionDescription *> &out)
{
  if(action.children.empty())
  {
    out.push_back(&action);
    return;
  }
  for(const ActionDescription &c : action.children)
    CollectLeavesRecursive(c, out);
}

void CollectLeavesInList(const rdcarray<ActionDescription> &list,
                         rdcarray<const ActionDescription *> &out)
{
  for(const ActionDescription &a : list)
    CollectLeavesRecursive(a, out);
}

// First and last leaf event ids of a subtree: its contiguous event range.
uint32_t PassFirstLeaf(const ActionDescription &a)
{
  if(a.children.empty())
    return a.eventId;
  return PassFirstLeaf(a.children[0]);
}

uint32_t PassLastLeaf(const ActionDescription &a)
{
  if(a.children.empty())
    return a.eventId;
  return PassLastLeaf(a.children[a.children.size() - 1]);
}

// Number of leaf actions (effects) in a subtree.
uint32_t CountLeavesRecursive(const ActionDescription &a)
{
  if(a.children.empty())
    return 1;
  uint32_t n = 0;
  for(const ActionDescription &c : a.children)
    n += CountLeavesRecursive(c);
  return n;
}

// Pass id, mirroring the plugin: <name with whitespace -> _>:<start>-<end>.
QString BuildPassId(const QString &name, uint32_t start, uint32_t end)
{
  QString safe = name.isEmpty() ? QStringLiteral("pass") : name;
  safe.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral("_"));
  return QStringLiteral("%1:%2-%3").arg(safe).arg(start).arg(end);
}

// Per-leaf facts gathered from the pipeline state, reused by the reverse index
// and the pass-resource collector.
struct LeafFacts
{
  uint32_t eventId = 0;
  QString name;
  QVector<quint64> renderTargetIds;   // color + depth outputs (writes)
  QVector<quint64> sampledIds;        // read-only + read/write resources
  QVector<quint64> resourceIds;       // everything the event touches
  QVector<ShaderRef> shaders;         // bound shader stages
};

// Read the pipeline state at one leaf event. A failure (SetFrameEvent throwing
// for an event without pipeline data) is swallowed so one bad event does not
// abort the whole scan - the plugin did the same with try/catch per event.
bool ReadLeafFacts(IReplayController &replay, const ActionDescription &a, const SDFile &sdfile,
                   LeafFacts &facts)
{
  facts.eventId = a.eventId;
  facts.name = FromRdcStr(a.GetName(sdfile));

  try
  {
    replay.SetFrameEvent(a.eventId, false);

    const PipeState &pipe = replay.GetPipelineState();

    for(int s = 0; s < int(ShaderStage::Count); s++)
    {
      const ShaderStage stage = ShaderStage(s);
      const ResourceId shader = pipe.GetShader(stage);
      if(shader == ResourceId())
        continue;

      ShaderRef sr;
      sr.id = shader;
      sr.stage = HotStageName(stage);
      sr.name = FromRdcStr(pipe.GetShaderEntryPoint(stage));
      AddUniqueShader(facts.shaders, sr);

      AddUniqueId(facts.resourceIds, ResIdValue(shader));
    }

    for(const Descriptor &d : pipe.GetOutputTargets())
    {
      AddUniqueId(facts.renderTargetIds, ResIdValue(d.resource));
      AddUniqueId(facts.resourceIds, ResIdValue(d.resource));
    }

    const quint64 depth = ResIdValue(pipe.GetDepthTarget().resource);
    AddUniqueId(facts.renderTargetIds, depth);
    AddUniqueId(facts.resourceIds, depth);

    for(int s = 0; s < int(ShaderStage::Count); s++)
    {
      const ShaderStage stage = ShaderStage(s);

      for(const UsedDescriptor &ud : pipe.GetReadOnlyResources(stage, true))
      {
        const quint64 v = ResIdValue(ud.descriptor.resource);
        AddUniqueId(facts.sampledIds, v);
        AddUniqueId(facts.resourceIds, v);
      }
      for(const UsedDescriptor &ud : pipe.GetReadWriteResources(stage, true))
      {
        const quint64 v = ResIdValue(ud.descriptor.resource);
        AddUniqueId(facts.sampledIds, v);
        AddUniqueId(facts.resourceIds, v);
      }
    }
  }
  catch(...)
  {
    return false;
  }

  return true;
}

// Per-event GPU durations in microseconds, empty when the API does not expose
// the EventGPUDuration counter.
QHash<quint32, double> FetchEventTimings(IReplayController &replay)
{
  QHash<quint32, double> timings;

  for(const GPUCounter &c : replay.EnumerateCounters())
  {
    if(c != GPUCounter::EventGPUDuration)
      continue;

    for(const CounterResult &r : replay.FetchCounters(rdcarray<GPUCounter>{GPUCounter::EventGPUDuration}))
    {
      // A negative value means "not measured for this event".
      if(r.value.d >= 0.0)
        timings.insert(r.eventId, r.value.d * 1000000.0);
    }
    break;
  }

  return timings;
}
}    // namespace

ToolResult QueryAdapter::ActionTimings(const ActionTimingOptions &options, CallContext &call)
{
  return DispatchBlock("renderdoc_getActionTimings", call,
      [options](IReplayController &replay) -> ToolResult {
    bool supported = false;
    for(GPUCounter counter : replay.EnumerateCounters())
      supported |= counter == GPUCounter::EventGPUDuration;
    if(!supported)
      return ToolResult::Success(QJsonObject{{QStringLiteral("supported"), false},
          {QStringLiteral("available"), false},
          {QStringLiteral("reason"), QStringLiteral("EventGPUDuration counter is unavailable for this capture")},
          {QStringLiteral("timings"), QJsonArray()}, {QStringLiteral("slowest"), QJsonArray()}});
    return SelectActionTimings(replay.GetRootActions(), replay.GetStructuredFile(),
                               FetchEventTimings(replay), options);
  });
}

ToolResult QueryAdapter::GetPassGraph(bool includeResources, CallContext &call,
                                      const QStringList &fields)
{
  const CaptureSnapshot snap = Snapshot();
  return DispatchBlock("renderdoc_getPassGraph", call,
                       [includeResources, fields, snap](IReplayController &replay) -> ToolResult {
    auto want = [&](const char *n) {
      return fields.isEmpty() || fields.contains(QString::fromLatin1(n));
    };

    const SDFile &sdfile = replay.GetStructuredFile();
    const rdcarray<ActionDescription> &roots = replay.GetRootActions();

    const QHash<quint32, double> timings = FetchEventTimings(replay);

    PassGraph graph;

    // One pass per top-level action, matching the plugin: a top-level marker
    // with children becomes a pass spanning its whole subtree; a lone top-level
    // draw call becomes a one-event pass.
    for(const ActionDescription &root : roots)
    {
      const bool hasChildren = !root.children.empty();

      PassInfo pass;
      pass.name = FromRdcStr(root.GetName(sdfile));
      pass.eventIdStart = hasChildren ? PassFirstLeaf(root) : root.eventId;
      pass.eventIdEnd = hasChildren ? PassLastLeaf(root) : root.eventId;
      pass.id = BuildPassId(pass.name, pass.eventIdStart, pass.eventIdEnd);
      pass.drawCount = hasChildren ? CountLeavesRecursive(root) : 1;
      pass.confidence = Confidence::Inferred;

      rdcarray<const ActionDescription *> leaves;
      CollectLeavesRecursive(root, leaves);

      QVector<quint64> rtIds, sampledIds;
      double durationUs = 0.0;

      for(const ActionDescription *leaf : leaves)
      {
        durationUs += timings.value(leaf->eventId, 0.0);

        if(!includeResources)
          continue;

        LeafFacts facts;
        if(!ReadLeafFacts(replay, *leaf, sdfile, facts))
          continue;

        for(quint64 v : facts.renderTargetIds)
          AddUniqueId(rtIds, v);
        for(quint64 v : facts.sampledIds)
          AddUniqueId(sampledIds, v);
        for(const ShaderRef &sr : facts.shaders)
          AddUniqueShader(pass.shaders, sr);
      }

      pass.durationUs = durationUs;

      if(includeResources)
      {
        for(quint64 v : rtIds)
          pass.renderTargets.push_back(ResourceIdFromValue(v));
        for(quint64 v : sampledIds)
          pass.sampledTextures.push_back(ResourceIdFromValue(v));
      }

      graph.totalDurationUs += durationUs;
      graph.passes.push_back(pass);
    }

    // Producer -> consumer edges: a pass that samples a texture rendered by an
    // earlier pass. The producer map is keyed by raw resource id.
    if(includeResources)
    {
      QHash<quint64, QVector<int>> producers;   // resource id -> pass indices that write it
      for(int i = 0; i < graph.passes.size(); i++)
      {
        for(const ResourceId &rt : graph.passes[i].renderTargets)
          producers[ResIdValue(rt)].push_back(i);
      }

      for(int to = 0; to < graph.passes.size(); to++)
      {
        const PassInfo &toPass = graph.passes[to];
        for(const ResourceId &tex : toPass.sampledTextures)
        {
          const QVector<int> froms = producers.value(ResIdValue(tex));
          for(int from : froms)
          {
            if(from == to)
              continue;

            const PassInfo &fromPass = graph.passes[from];
            PassGraphEdge e;
            e.fromPassId = fromPass.id;
            e.fromPassName = fromPass.name;
            e.toPassId = toPass.id;
            e.toPassName = toPass.name;
            e.resourceId = tex;
            e.usage = QStringLiteral("sampledFromRenderTarget");
            e.confidence = Confidence::Inferred;
            graph.edges.push_back(e);
          }
        }
      }
    }

    QJsonObject j = PassGraphToJson(graph);
    j[QStringLiteral("available")] = true;

    if(!fields.isEmpty())
    {
      QJsonObject filtered;
      for(auto it = j.constBegin(); it != j.constEnd(); ++it)
      {
        if(it.key() == QStringLiteral("snapshot") || want(it.key().toUtf8().constData()))
          filtered[it.key()] = it.value();
      }
      j = filtered;
    }

    j[QStringLiteral("snapshot")] = SnapshotToJson(snap);
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::FindDrawsByShader(const QString &term, CallContext &call,
                                           const QStringList &fields)
{
  const CaptureSnapshot snap = Snapshot();
  // Resource display names are materialised on the UI thread once, then captured
  // by value so the replay lambda never touches ICaptureContext.
  const QHash<quint64, QString> names =
      ReadOnUIThread<QHash<quint64, QString>>([this]() -> QHash<quint64, QString> {
    QHash<quint64, QString> map;
    for(const ResourceDescription &r : m_Ctx->GetResources())
    {
      const QString name = FromRdcStr(m_Ctx->GetResourceName(r.resourceId));
      if(!name.isEmpty())
        map.insert(ResIdValue(r.resourceId), name);
    }
    return map;
  });

  return DispatchBlock("renderdoc_findDrawsByShader", call,
                       [term, names, fields, snap](IReplayController &replay) -> ToolResult {
    const QString needle = term.toLower();

    ReverseSearchResult result;
    result.query = QStringLiteral("shader");
    result.term = term;

    const SDFile &sdfile = replay.GetStructuredFile();
    rdcarray<const ActionDescription *> leaves;
    CollectLeavesInList(replay.GetRootActions(), leaves);

    for(const ActionDescription *leaf : leaves)
    {
      LeafFacts facts;
      if(!ReadLeafFacts(replay, *leaf, sdfile, facts))
        continue;

      result.scannedEventCount++;

      ReverseSearchHit hit;
      hit.eventId = facts.eventId;
      hit.name = facts.name;

      for(const ShaderRef &sr : facts.shaders)
      {
        const QString display = names.value(ResIdValue(sr.id));
        const bool match = (!sr.name.isEmpty() && sr.name.toLower().contains(needle)) ||
                           (!display.isEmpty() && display.toLower().contains(needle));
        if(!match)
          continue;

        ResourceAccess access;
        access.id = sr.id;
        access.name = display.isEmpty() ? sr.name : display;
        access.usage = ResourceUsage::Unknown;
        access.eventId = facts.eventId;
        hit.resources.push_back(access);
      }

      if(!hit.resources.isEmpty())
      {
        hit.confidence = Confidence::Confirmed;
        result.hits.push_back(hit);
      }
    }

    std::sort(result.hits.begin(), result.hits.end(),
              [](const ReverseSearchHit &l, const ReverseSearchHit &r) {
                return l.eventId < r.eventId;
              });

    QJsonObject j = ReverseSearchResultToJson(result);
    if(!fields.isEmpty())
    {
      QJsonObject filtered;
      for(auto it = j.constBegin(); it != j.constEnd(); ++it)
      {
        if(it.key() == QStringLiteral("snapshot") ||
           fields.contains(it.key()))
          filtered[it.key()] = it.value();
      }
      j = filtered;
    }
    j[QStringLiteral("snapshot")] = SnapshotToJson(snap);
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::FindDrawsByTexture(const QString &term, CallContext &call,
                                            const QStringList &fields)
{
  const CaptureSnapshot snap = Snapshot();
  const QHash<quint64, QString> names =
      ReadOnUIThread<QHash<quint64, QString>>([this]() -> QHash<quint64, QString> {
    QHash<quint64, QString> map;
    for(const ResourceDescription &r : m_Ctx->GetResources())
    {
      const QString name = FromRdcStr(m_Ctx->GetResourceName(r.resourceId));
      if(!name.isEmpty())
        map.insert(ResIdValue(r.resourceId), name);
    }
    return map;
  });

  return DispatchBlock("renderdoc_findDrawsByTexture", call,
                       [term, names, fields, snap](IReplayController &replay) -> ToolResult {
    const QString needle = term.toLower();

    ReverseSearchResult result;
    result.query = QStringLiteral("texture");
    result.term = term;

    const SDFile &sdfile = replay.GetStructuredFile();
    rdcarray<const ActionDescription *> leaves;
    CollectLeavesInList(replay.GetRootActions(), leaves);

    for(const ActionDescription *leaf : leaves)
    {
      LeafFacts facts;
      if(!ReadLeafFacts(replay, *leaf, sdfile, facts))
        continue;

      result.scannedEventCount++;

      ReverseSearchHit hit;
      hit.eventId = facts.eventId;
      hit.name = facts.name;

      for(quint64 v : facts.resourceIds)
      {
        const QString display = names.value(v);
        if(display.isEmpty() || !display.toLower().contains(needle))
          continue;

        ResourceAccess access;
        access.id = ResourceIdFromValue(v);
        access.name = display;
        access.usage = ResourceUsage::Sampled;
        access.eventId = facts.eventId;
        hit.resources.push_back(access);
      }

      if(!hit.resources.isEmpty())
      {
        hit.resource = hit.resources[0].id;
        hit.confidence = Confidence::Confirmed;
        result.hits.push_back(hit);
      }
    }

    std::sort(result.hits.begin(), result.hits.end(),
              [](const ReverseSearchHit &l, const ReverseSearchHit &r) {
                return l.eventId < r.eventId;
              });

    QJsonObject j = ReverseSearchResultToJson(result);
    if(!fields.isEmpty())
    {
      QJsonObject filtered;
      for(auto it = j.constBegin(); it != j.constEnd(); ++it)
      {
        if(it.key() == QStringLiteral("snapshot") || fields.contains(it.key()))
          filtered[it.key()] = it.value();
      }
      j = filtered;
    }
    j[QStringLiteral("snapshot")] = SnapshotToJson(snap);
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::FindDrawsByResourceId(ResourceId id, CallContext &call,
                                               const QStringList &fields)
{
  const quint64 want = ResIdValue(id);
  const CaptureSnapshot snap = Snapshot();

  const QHash<quint64, QString> names =
      ReadOnUIThread<QHash<quint64, QString>>([this]() -> QHash<quint64, QString> {
    QHash<quint64, QString> map;
    for(const ResourceDescription &r : m_Ctx->GetResources())
    {
      const QString name = FromRdcStr(m_Ctx->GetResourceName(r.resourceId));
      if(!name.isEmpty())
        map.insert(ResIdValue(r.resourceId), name);
    }
    return map;
  });

  return DispatchBlock("renderdoc_findDrawsByResourceId", call,
                       [want, id, names, fields, snap](IReplayController &replay) -> ToolResult {
    ReverseSearchResult result;
    result.query = QStringLiteral("resource");
    result.term = ResourceIdToString(id);

    const SDFile &sdfile = replay.GetStructuredFile();
    rdcarray<const ActionDescription *> leaves;
    CollectLeavesInList(replay.GetRootActions(), leaves);

    for(const ActionDescription *leaf : leaves)
    {
      LeafFacts facts;
      if(!ReadLeafFacts(replay, *leaf, sdfile, facts))
        continue;

      result.scannedEventCount++;

      if(!VecHasId(facts.resourceIds, want))
        continue;

      ReverseSearchHit hit;
      hit.eventId = facts.eventId;
      hit.name = facts.name;
      hit.resource = id;
      hit.confidence = Confidence::Confirmed;

      ResourceAccess access;
      access.id = id;
      access.name = names.value(want);
      access.eventId = facts.eventId;
      if(VecHasId(facts.renderTargetIds, want))
        access.usage = ResourceUsage::RenderTarget;
      else if(VecHasId(facts.sampledIds, want))
        access.usage = ResourceUsage::Sampled;
      hit.resources.push_back(access);

      result.hits.push_back(hit);
    }

    std::sort(result.hits.begin(), result.hits.end(),
              [](const ReverseSearchHit &l, const ReverseSearchHit &r) {
                return l.eventId < r.eventId;
              });

    QJsonObject j = ReverseSearchResultToJson(result);
    j[QStringLiteral("resourceId")] = ResourceIdToString(id);

    if(!fields.isEmpty())
    {
      QJsonObject filtered;
      for(auto it = j.constBegin(); it != j.constEnd(); ++it)
      {
        if(it.key() == QStringLiteral("snapshot") || fields.contains(it.key()))
          filtered[it.key()] = it.value();
      }
      j = filtered;
    }
    j[QStringLiteral("snapshot")] = SnapshotToJson(snap);
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::TraceResourceUsage(ResourceId id, CallContext &call,
                                            const QStringList &fields, int maxEvents)
{
  return WithCaptureUI(call, [&](ICaptureContext &ctx) -> ToolResult {
  if(!ctx.Config().MCP_AllowSensitiveData)
    return ToolResult::Failure(Error(ErrorCode::Unsupported, "Resource usage requires MCP_AllowSensitiveData"));
  if(!ctx.IsCaptureLoaded())
    return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "No capture is loaded"));
  if(ctx.IsCaptureLoading())
    return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "Capture is loading"));
  // Zero is the internal omission sentinel; the public parser rejects explicit zero.
  if(maxEvents < 0 || maxEvents > 500)
    return ToolResult::Failure(Error(ErrorCode::InvalidParams, "maxEvents must be omitted or between 1 and 500"));
  const quint64 want = ResIdValue(id);
  const CaptureSnapshot snap = Snapshot();

  const QHash<quint64, QString> names =
      ReadOnUIThread<QHash<quint64, QString>>([this]() -> QHash<quint64, QString> {
    QHash<quint64, QString> map;
    for(const ResourceDescription &r : m_Ctx->GetResources())
    {
      const QString name = FromRdcStr(m_Ctx->GetResourceName(r.resourceId));
      if(!name.isEmpty())
        map.insert(ResIdValue(r.resourceId), name);
    }
    return map;
  });

  return DispatchBlock("renderdoc_traceResourceUsage", call,
                       [want, id, names, snap, maxEvents, fields, &call](IReplayController &replay) -> ToolResult {
    const SDFile &sdfile = replay.GetStructuredFile();

    rdcarray<const ActionDescription *> leaves;
    CollectLeavesInList(replay.GetRootActions(), leaves);

    QJsonArray producers;    // events that write the resource
    QJsonArray consumers;    // events that read/sample the resource
    QJsonArray accesses;
    int examined = 0, failed = 0;

    for(const ActionDescription *leaf : leaves)
    {
      if(call.IsCancelled()) return ToolResult::Failure(Error(ErrorCode::Cancelled, "Resource usage cancelled"));
      if(call.IsExpired()) return ToolResult::Failure(Error(ErrorCode::Timeout, "Resource usage deadline exceeded"));
      LeafFacts facts;
      examined++;
      if(!ReadLeafFacts(replay, *leaf, sdfile, facts))
      {
        failed++;
        continue;
      }

      const bool renders = VecHasId(facts.renderTargetIds, want);
      const bool samples = VecHasId(facts.sampledIds, want);
      if(!renders && !samples)
        continue;

      const QString name = names.value(want);
      ResourceAccess access;
      access.id = id;
      access.name = name;
      access.eventId = facts.eventId;
      access.usage = renders ? ResourceUsage::RenderTarget : ResourceUsage::Sampled;
      accesses.append(ResourceAccessToJson(access));

      if(renders)
      {
        QJsonObject e;
        e[QStringLiteral("eventId")] = double(facts.eventId);
        e[QStringLiteral("name")] = facts.name;
        e[QStringLiteral("usage")] = ResourceUsageName(ResourceUsage::RenderTarget);
        e[QStringLiteral("confidence")] = ConfidenceName(Confidence::Confirmed);
        producers.append(e);
      }
      if(samples)
      {
        QJsonObject e;
        e[QStringLiteral("eventId")] = double(facts.eventId);
        e[QStringLiteral("name")] = facts.name;
        e[QStringLiteral("usage")] = ResourceUsageName(ResourceUsage::Sampled);
        e[QStringLiteral("confidence")] = ConfidenceName(Confidence::Confirmed);
        consumers.append(e);
      }
    }

    QJsonObject j;
    j[QStringLiteral("available")] = true;
    j[QStringLiteral("resourceId")] = ResourceIdToString(id);
    j[QStringLiteral("name")] = names.value(want);
    j[QStringLiteral("producerEvents")] = producers;
    j[QStringLiteral("consumerEvents")] = consumers;
    j[QStringLiteral("producerCount")] = producers.size();
    j[QStringLiteral("consumerCount")] = consumers.size();
    j[QStringLiteral("accesses")] = accesses;
    j[QStringLiteral("snapshot")] = SnapshotToJson(snap);
    j[QStringLiteral("coverage")] = QStringLiteral("Leaf-action pipeline binding scan; bindings are not proof of executed GPU access");
    j[QStringLiteral("examinedEvents")] = examined;
    j[QStringLiteral("failedEvents")] = failed;
    j[QStringLiteral("complete")] = failed == 0;
    return ToolResult::Success(FilterFields(LimitResourceUsage(j, maxEvents), fields));
  });
  });
}

//
// ---- P1-4 resource memory audit, lifetimes and heuristics -----------------
//

namespace
{
// The mip count a texture would have if its whole chain were allocated.
uint32_t IdealMipCount(uint32_t w, uint32_t h, uint32_t d)
{
  uint32_t m = w > h ? w : h;
  if(d > m)
    m = d;
  uint32_t count = 1;
  while(m > 1)
  {
    m >>= 1;
    count++;
  }
  return count;
}

// A copy of the resource inventory that is safe to hand to the replay thread:
// the name, type and creation-time size are read once on the UI thread.
struct ResourceLite
{
  quint64 id = 0;
  QString name;
  QString type;
  uint64_t byteSize = 0;
};

QVector<ResourceLite> SnapshotResources(ICaptureContext *ctx)
{
  QVector<ResourceLite> out;

  QHash<quint64, uint64_t> sizes;
  for(const TextureDescription &t : ctx->GetTextures())
    sizes.insert(ResIdValue(t.resourceId), t.byteSize);
  for(const BufferDescription &b : ctx->GetBuffers())
    sizes.insert(ResIdValue(b.resourceId), b.length);

  for(const ResourceDescription &r : ctx->GetResources())
  {
    ResourceLite lite;
    lite.id = ResIdValue(r.resourceId);
    lite.name = FromRdcStr(ctx->GetResourceName(r.resourceId));
    lite.type = detail::ResourceTypeName(r.type);
    lite.byteSize = sizes.value(lite.id, 0);
    out.append(lite);
  }
  return out;
}

// The whole-capture memory audit over the UI-thread resource tables. Returns the
// same `ResourceMemoryAudit` the resourceMemoryAudit tool emits, so the
// investigation report (P1-9) reuses one definition of every field. `cap` bounds
// `largestResources` (0 == unlimited).
ResourceMemoryAudit SnapshotMemoryAudit(ICaptureContext *ctx, int cap)
{
  ResourceMemoryAudit out;

  const rdcarray<TextureDescription> textures = ctx->GetTextures();
  const rdcarray<BufferDescription> buffers = ctx->GetBuffers();
  const rdcarray<ResourceDescription> resources = ctx->GetResources();

  QHash<QString, uint32_t> formatCounts;
  QVector<QString> dimensions;
  QVector<ResourceMemoryRow> rows;

  for(const TextureDescription &t : textures)
  {
    ResourceMemoryRow r;
    r.resourceId = t.resourceId;
    r.name = FromRdcStr(ctx->GetResourceName(t.resourceId));
    r.type = QStringLiteral("Texture");
    r.textureType = detail::TextureTypeName(t.type);
    r.byteSize = t.byteSize;
    r.width = t.width;
    r.height = t.height;
    r.depth = t.depth;
    r.mips = t.mips;
    r.arraySize = t.arraysize;
    r.format = detail::ResourceFormatString(t.format);
    r.usage = detail::TextureUsageString(t.creationFlags);
    rows.append(r);

    out.totalTextureBytes += t.byteSize;
    out.textures.count++;
    formatCounts[r.format] += 1u;
    if(t.mips > 1)
      out.textures.mipLevelsAvailable++;

    if(t.mips >= IdealMipCount(t.width, t.height, t.depth))
      out.textures.fullMipChain++;
    else
      out.textures.missingMipChain++;

    const QString dim = QString::fromUtf8("%1x%2x%3").arg(t.width).arg(t.height).arg(t.depth);
    if(!dimensions.contains(dim))
      dimensions.append(dim);
  }

  for(const BufferDescription &b : buffers)
  {
    ResourceMemoryRow r;
    r.resourceId = b.resourceId;
    r.name = FromRdcStr(ctx->GetResourceName(b.resourceId));
    r.type = QStringLiteral("Buffer");
    r.byteSize = b.length;
    r.usage = detail::BufferUsageString(b.creationFlags);
    rows.append(r);

    out.totalBufferBytes += b.length;
    out.bufferCount++;
  }

  for(const ResourceDescription &res : resources)
  {
    if(res.type == ResourceType::Shader)
      out.shaderCount++;
  }
  out.totalResources = (uint32_t)resources.size();

  std::sort(rows.begin(), rows.end(), [](const ResourceMemoryRow &a, const ResourceMemoryRow &b) {
    return a.byteSize > b.byteSize;
  });
  out.largestResources = rows;
  if(cap > 0 && out.largestResources.size() > cap)
    out.largestResources.resize(cap);

  QVector<QPair<QString, uint32_t>> byFormat;
  for(auto it = formatCounts.constBegin(); it != formatCounts.constEnd(); ++it)
    byFormat.append(qMakePair(it.key(), it.value()));
  std::sort(byFormat.begin(), byFormat.end(),
            [](const QPair<QString, uint32_t> &a, const QPair<QString, uint32_t> &b) {
              return a.second > b.second;
            });
  out.textures.byFormat = byFormat;
  out.textures.dimensions = dimensions;

  out.totalBytes = out.totalTextureBytes + out.totalBufferBytes;
  out.totalMiB = double(out.totalBytes) / (1024.0 * 1024.0);

  // Heuristic remarks. These are explicitly not facts and are labelled as such.
  if(out.textures.missingMipChain > 0)
    out.notes.append(QString::fromUtf8("%1 texture(s) have an incomplete mip chain, which can "
                                       "increase sampling bandwidth and memory residency")
                         .arg(out.textures.missingMipChain));
  if(out.totalBytes > 0 && !out.largestResources.isEmpty())
  {
    const double share =
        double(out.largestResources.first().byteSize) * 100.0 / double(out.totalBytes);
    out.notes.append(
        QString::fromUtf8("The largest single resource accounts for %1% of tracked memory")
            .arg(share, 0, 'f', 1));
  }
  out.notes.append(QStringLiteral(
      "Byte sizes are creation-time estimates; resident memory may differ due to aliasing, "
      "sub-allocation and driver overhead."));

  out.confidence = Confidence::Confirmed;
  return out;
}

// Applies the optional caller-supplied field projection, always keeping the
// `snapshot` sibling so clients can still resolve the capture context.
QJsonObject FilterFields(const QJsonObject &j, const QStringList &fields)
{
  if(fields.isEmpty())
    return j;
  QJsonObject filtered;
  for(auto it = j.constBegin(); it != j.constEnd(); ++it)
  {
    if(it.key() == QStringLiteral("snapshot") || fields.contains(it.key()))
      filtered[it.key()] = it.value();
  }
  return filtered;
}
// Display name for a shader stage object in a pipeline snapshot: prefer the
// bound shader's resource name, fall back to the entry point. Returns an empty
// string when the stage is absent. Used only for the human-readable shader
// change list; the raw JSON is always carried in the field diff.
static QString PipelineStageDisplayName(const QJsonObject &shaders, const QString &stage)
{
  const QJsonObject s = shaders.value(stage).toObject();
  if(s.isEmpty())
    return QString();

  const QJsonObject shader = s.value(QStringLiteral("shader")).toObject();
  const QString name = shader.value(QStringLiteral("name")).toString();
  if(!name.isEmpty() && name != QStringLiteral("ResourceId::0"))
    return name;

  return s.value(QStringLiteral("entryPoint")).toString();
}

// Recursively collect every event id in the action tree. SetFrameEvent() returns
// void, so to reject an unknown event id up front we walk the tree ourselves.
static void CollectEventIdsRecursive(const rdcarray<ActionDescription> &list, QSet<quint32> &out)
{
  for(const ActionDescription &a : list)
  {
    out.insert(a.eventId);
    CollectEventIdsRecursive(a.children, out);
  }
}

// Flatten a snapshot into dotted paths (leaf values only). A scalar at the root
// would land at key "", which never happens for the pipeline snapshot object and
// is dropped defensively.
static void FlattenSnapshotJson(const QJsonValue &value, const QString &prefix,
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
      FlattenSnapshotJson(it.value(), key, out);
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
      FlattenSnapshotJson(a[i], prefix + QString::fromUtf8(".") + QString::number(i), out);
    return;
  }

  if(!prefix.isEmpty())
    out.push_back(qMakePair(prefix, value));
}
}    // namespace

// Renders one snapshot value the way the before/after columns should read. Kept
// file-local (rather than in the model) because it is a presentation detail of
// the diff, not part of the shared analysis vocabulary.
static QString SnapshotValueText(const QJsonValue &v)
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

ToolResult QueryAdapter::PipelineSnapshotAt(uint32_t eventId, CallContext &call,
                                            const QStringList &fields)
{
  if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
    return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
  if(m_Ctx->IsCaptureLoading())
    return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

  const CaptureSnapshot snap = Snapshot();
  return DispatchBlock("renderdoc_getPipelineStateAtEvent", call,
                       [eventId, fields, snap](IReplayController &replay) -> ToolResult {
    QSet<quint32> eventIds;
    CollectEventIdsRecursive(replay.GetRootActions(), eventIds);
    if(!eventIds.contains(eventId))
      return ToolResult::Failure(
          Error(ErrorCode::InvalidParams,
                QString::fromUtf8("no event with id %1 in this capture").arg(eventId)));

    replay.SetFrameEvent(eventId, false);

    const QJsonObject state = PipelineStateToJson(replay);
    if(!state.value(QStringLiteral("loaded")).toBool())
      return ToolResult::Failure(
          Error(ErrorCode::ReplayUnavailable,
                QString::fromUtf8("no pipeline state available at event %1").arg(eventId)));

    QJsonObject out = FilterFields(state, fields);
    out[QStringLiteral("eventId")] = double(eventId);
    out[QStringLiteral("snapshot")] = SnapshotToJson(snap);
    return ToolResult::Success(out);
  });
}

ToolResult QueryAdapter::DiffPipelineState(uint32_t eventIdA, uint32_t eventIdB, CallContext &call,
                                           const QStringList &fields)
{
  if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
    return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
  if(m_Ctx->IsCaptureLoading())
    return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

  const CaptureSnapshot snap = Snapshot();
  return DispatchBlock("renderdoc_diffPipelineState", call,
                       [eventIdA, eventIdB, fields, snap](IReplayController &replay) -> ToolResult {
    QSet<quint32> eventIds;
    CollectEventIdsRecursive(replay.GetRootActions(), eventIds);

    if(!eventIds.contains(eventIdA))
      return ToolResult::Failure(
          Error(ErrorCode::InvalidParams,
                QString::fromUtf8("no event with id %1 in this capture").arg(eventIdA)));
    if(!eventIds.contains(eventIdB))
      return ToolResult::Failure(
          Error(ErrorCode::InvalidParams,
                QString::fromUtf8("no event with id %1 in this capture").arg(eventIdB)));

    replay.SetFrameEvent(eventIdA, false);
    const QJsonObject stateA = PipelineStateToJson(replay);

    replay.SetFrameEvent(eventIdB, false);
    const QJsonObject stateB = PipelineStateToJson(replay);

    const bool loadedA = stateA.value(QStringLiteral("loaded")).toBool();
    const bool loadedB = stateB.value(QStringLiteral("loaded")).toBool();
    if(!loadedA || !loadedB)
    {
      const uint32_t badEvent = !loadedA ? eventIdA : eventIdB;
      return ToolResult::Failure(
          Error(ErrorCode::ReplayUnavailable,
                QString::fromUtf8("no pipeline state available at event %1").arg(badEvent)));
    }

    PipelineDiffResult diff;
    diff.available = true;
    diff.eventIdA = eventIdA;
    diff.eventIdB = eventIdB;
    diff.inspectedStages = SnapshotStagesFromPipelineJson(stateA, stateB);
    diff.confidence = Confidence::Confirmed;
    diff.limitation = QString::fromUtf8(
        "Both events are resolved in the same capture, so every reported difference is a fact "
        "read from replay. Fields absent from an API's pipeline state are simply not compared.");

    QVector<QPair<QString, QJsonValue>> flatA;
    QVector<QPair<QString, QJsonValue>> flatB;
    FlattenSnapshotJson(stateA, QString(), flatA);
    FlattenSnapshotJson(stateB, QString(), flatB);

    QHash<QString, QJsonValue> mapA;
    for(const QPair<QString, QJsonValue> &p : flatA)
      mapA.insert(p.first, p.second);

    QSet<QString> seenA;
    for(const QPair<QString, QJsonValue> &p : flatB)
    {
      seenA.insert(p.first);
      const QJsonValue av = mapA.value(p.first);
      if(av == p.second)
        continue;

      PipelineFieldChange c;
      c.path = p.first;
      const int dot = c.path.indexOf(QLatin1Char('.'));
      c.group = dot > 0 ? c.path.left(dot) : c.path;
      c.before = SnapshotValueText(av);
      c.after = SnapshotValueText(p.second);
      c.beforeValue = av;
      c.afterValue = p.second;
      diff.changes.push_back(c);
    }

    // Fields present only in A (the value went away in B).
    for(const QPair<QString, QJsonValue> &p : flatA)
    {
      if(seenA.contains(p.first))
        continue;

      PipelineFieldChange c;
      c.path = p.first;
      const int dot = c.path.indexOf(QLatin1Char('.'));
      c.group = dot > 0 ? c.path.left(dot) : c.path;
      c.before = SnapshotValueText(p.second);
      c.after = QString::fromUtf8("none");
      c.beforeValue = p.second;
      c.afterValue = QJsonValue();
      diff.changes.push_back(c);
    }

    // Shader-stage bindings, denormalised for clients that think in programs.
    const QJsonObject shadersA = stateA.value(QStringLiteral("shaders")).toObject();
    const QJsonObject shadersB = stateB.value(QStringLiteral("shaders")).toObject();
    for(const QString &stage : diff.inspectedStages)
    {
      const QString nameA = PipelineStageDisplayName(shadersA, stage);
      const QString nameB = PipelineStageDisplayName(shadersB, stage);
      if(nameA == nameB)
        continue;

      PipelineShaderChange s;
      s.stage = stage;
      s.before = nameA;
      s.after = nameB;
      diff.shaderChanges.push_back(s);
    }

    diff.hasChanges = !diff.changes.isEmpty();

    QJsonObject out = FilterFields(PipelineDiffResultToJson(diff), fields);

    // The full snapshots are opt-in: they are large and most callers only want
    // the change list. Asking for the `snapshots` field adds them.
    if(fields.isEmpty() || fields.contains(QStringLiteral("snapshots")))
    {
      QJsonObject snaps;
      snaps[QStringLiteral("eventA")] = stateA;
      snaps[QStringLiteral("eventB")] = stateB;
      out[QStringLiteral("snapshots")] = snaps;
    }

    out[QStringLiteral("snapshot")] = SnapshotToJson(snap);
    return ToolResult::Success(out);
  });
}

ToolResult QueryAdapter::ResourceMemoryAudit(int limit, CallContext &call,
                                             const QStringList &fields)
{
  Q_UNUSED(call);

  const int cap = limit > 0 ? limit : 64;

  const MCP::ResourceMemoryAudit audit =
      ReadOnUIThread<MCP::ResourceMemoryAudit>([this, cap]() -> MCP::ResourceMemoryAudit {
    return SnapshotMemoryAudit(m_Ctx, cap);
  });

  QJsonObject j = FilterFields(ResourceMemoryAuditToJson(audit), fields);
  j[QStringLiteral("snapshot")] = SnapshotToJson(Snapshot());
  return ToolResult::Success(j);
}

ToolResult QueryAdapter::ResourceLifetime(int limit, CallContext &call,
                                          const QStringList &fields, ResourceId selectedResource)
{
  const int cap = limit > 0 ? limit : 100;

  const CaptureSnapshot snap = Snapshot();
  const QVector<ResourceLite> resources =
      ReadOnUIThread<QVector<ResourceLite>>([this]() -> QVector<ResourceLite> {
    return SnapshotResources(m_Ctx);
  });

  const quint64 selected = ResIdValue(selectedResource);
  if(selected != 0)
  {
    bool found = false;
    for(const ResourceLite &res : resources) if(res.id == selected) found = true;
    if(!found) return ToolResult::Failure(Error(ErrorCode::InvalidParams, "ResourceId not found in capture"));
  }
  return DispatchBlock("renderdoc_resourceLifetime", call,
                       [resources, cap, fields, snap, selected, &call](IReplayController &replay) -> ToolResult {
    const SDFile &sdfile = replay.GetStructuredFile();

    rdcarray<const ActionDescription *> leaves;
    CollectLeavesInList(replay.GetRootActions(), leaves);

    struct Agg
    {
      uint32_t first = 0;
      uint32_t last = 0;
      uint32_t count = 0;
      QVector<ResourceAccess> accesses;
    };
    QHash<quint64, Agg> aggs;

    for(const ActionDescription *leaf : leaves)
    {
      if(call.IsCancelled()) return ToolResult::Failure(Error(ErrorCode::Cancelled, "Resource lifetime cancelled"));
      if(call.IsExpired()) return ToolResult::Failure(Error(ErrorCode::Timeout, "Resource lifetime deadline exceeded"));
      LeafFacts facts;
      if(!ReadLeafFacts(replay, *leaf, sdfile, facts))
        continue;

      for(quint64 idv : facts.resourceIds)
      {
        Agg &a = aggs[idv];
        if(a.count == 0 || facts.eventId < a.first)
          a.first = facts.eventId;
        if(facts.eventId > a.last)
          a.last = facts.eventId;
        a.count++;

        ResourceAccess acc;
        acc.id = ResourceIdFromValue(idv);
        acc.name = facts.name;
        acc.eventId = facts.eventId;
        acc.usage = VecHasId(facts.renderTargetIds, idv)
                        ? ResourceUsage::RenderTarget
                        : (VecHasId(facts.sampledIds, idv) ? ResourceUsage::Sampled
                                                           : ResourceUsage::Unknown);
        a.accesses.append(acc);
      }
    }

    QVector<ResourceLifetimeRow> rows;
    rows.reserve(resources.size());
    for(const ResourceLite &res : resources)
    {
      if(selected != 0 && res.id != selected) continue;
      ResourceLifetimeRow row;
      row.resourceId = ResourceIdFromValue(res.id);
      row.name = res.name;
      row.type = res.type;
      row.byteSize = res.byteSize;

      const Agg a = aggs.value(res.id);
      row.observedEventCount = a.count;
      row.firstEventId = a.first;
      row.lastEventId = a.last;
      row.accesses = a.accesses;
      row.confidence = Confidence::Confirmed;
      rows.append(row);
    }

    std::sort(rows.begin(), rows.end(),
              [](const ResourceLifetimeRow &a, const ResourceLifetimeRow &b) {
                if(a.observedEventCount != b.observedEventCount)
                  return a.observedEventCount > b.observedEventCount;
                return a.byteSize > b.byteSize;
              });

    ResourceLifetimeResult result;
    result.available = true;
    result.scannedEventCount = (uint32_t)leaves.size();
    result.truncated = cap > 0 && rows.size() > cap;
    if(cap > 0 && rows.size() > cap)
      rows.resize(cap);
    result.rows = rows;

    QJsonObject j = FilterFields(ResourceLifetimeToJson(result), fields);
    j[QStringLiteral("resourceId")] = selected != 0 ? QJsonValue(ResourceIdToString(ResourceIdFromValue(selected))) : QJsonValue(QJsonValue::Null);
    j[QStringLiteral("limitation")] = QStringLiteral("First/last events describe observed pipeline-bound scope, not allocation/destruction lifetime. Indirect, copy and out-of-frame access may be absent.");
    j[QStringLiteral("snapshot")] = SnapshotToJson(snap);
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::FindUnusedResources(int limit, CallContext &call,
                                             const QStringList &fields)
{
  const int cap = limit > 0 ? limit : 100;

  const CaptureSnapshot snap = Snapshot();
  const QVector<ResourceLite> resources =
      ReadOnUIThread<QVector<ResourceLite>>([this]() -> QVector<ResourceLite> {
    return SnapshotResources(m_Ctx);
  });

  return DispatchBlock("renderdoc_findUnusedResources", call,
                       [resources, cap, fields, snap](IReplayController &replay) -> ToolResult {
    const SDFile &sdfile = replay.GetStructuredFile();

    rdcarray<const ActionDescription *> leaves;
    CollectLeavesInList(replay.GetRootActions(), leaves);

    QHash<quint64, bool> touched;
    for(const ActionDescription *leaf : leaves)
    {
      LeafFacts facts;
      if(!ReadLeafFacts(replay, *leaf, sdfile, facts))
        continue;
      for(quint64 idv : facts.resourceIds)
        touched.insert(idv, true);
    }

    QVector<UnusedResource> candidates;
    for(const ResourceLite &res : resources)
    {
      if(res.type == QStringLiteral("Shader") || touched.contains(res.id))
        continue;
      UnusedResource u;
      u.resourceId = ResourceIdFromValue(res.id);
      u.name = res.name;
      u.type = res.type;
      u.byteSize = res.byteSize;
      candidates.append(u);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const UnusedResource &a, const UnusedResource &b) {
                return a.byteSize > b.byteSize;
              });

    UnusedResourceResult result;
    result.available = true;
    result.scannedEventCount = (uint32_t)leaves.size();
    result.candidates = candidates;
    if(cap > 0 && result.candidates.size() > cap)
      result.candidates.resize(cap);
    result.confidence = Confidence::Low;
    result.limitation = QString::fromUtf8(
        "Only resources referenced by an event's bound pipeline (render targets, sampled and "
        "read-write resources) are observed. Resources used via unbound descriptors, indirect "
        "access, or outside the captured frame are reported as unused incorrectly.");

    QJsonObject j = FilterFields(UnusedResourceResultToJson(result), fields);
    j[QStringLiteral("snapshot")] = SnapshotToJson(snap);
    return ToolResult::Success(j);
  });
}

ToolResult QueryAdapter::FindResourceLeaks(int limit, CallContext &call,
                                           const QStringList &fields)
{
  const int cap = limit > 0 ? limit : 100;

  const CaptureSnapshot snap = Snapshot();
  if(!snap.hasCapture)
    return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
  const QVector<ResourceLite> resources =
      ReadOnUIThread<QVector<ResourceLite>>([this]() -> QVector<ResourceLite> {
    return SnapshotResources(m_Ctx);
  });

  return DispatchBlock("renderdoc_findResourceLeaks", call,
                       [resources, cap, fields, snap](IReplayController &replay) -> ToolResult {
    const SDFile &sdfile = replay.GetStructuredFile();

    rdcarray<const ActionDescription *> leaves;
    CollectLeavesInList(replay.GetRootActions(), leaves);

    struct Agg
    {
      uint32_t first = 0;
      uint32_t last = 0;
      uint32_t count = 0;
    };
    QHash<quint64, Agg> aggs;

    for(const ActionDescription *leaf : leaves)
    {
      LeafFacts facts;
      if(!ReadLeafFacts(replay, *leaf, sdfile, facts))
        continue;
      for(quint64 idv : facts.resourceIds)
      {
        Agg &a = aggs[idv];
        if(a.count == 0 || facts.eventId < a.first)
          a.first = facts.eventId;
        if(facts.eventId > a.last)
          a.last = facts.eventId;
        a.count++;
      }
    }

    QVector<LeakCandidate> candidates;
    for(const ResourceLite &res : resources)
    {
      if(res.type == QStringLiteral("Shader"))
        continue;

      const Agg a = aggs.value(res.id);
      LeakCandidate c;
      c.resourceId = ResourceIdFromValue(res.id);
      c.name = res.name;
      c.type = res.type;
      c.byteSize = res.byteSize;
      c.createdEventId = a.first;
      c.lastEventId = a.last;
      c.observedEventCount = a.count;

      if(a.count == 0)
        c.reason = QString::fromUtf8("Never bound to any pipeline event in the captured frame");
      else if(a.count == 1)
        c.reason =
            QString::fromUtf8("Bound to exactly one event (single-use) and still resident at the "
                              "end of the frame");
      else
        continue;

      c.confidence = Confidence::Low;
      candidates.append(c);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const LeakCandidate &a, const LeakCandidate &b) {
                return a.byteSize > b.byteSize;
              });

    LeakResult result;
    result.available = true;
    result.scannedEventCount = (uint32_t)leaves.size();
    result.candidates = candidates;
    if(cap > 0 && result.candidates.size() > cap)
      result.candidates.resize(cap);
    result.confidence = Confidence::Low;
    result.limitation = QString::fromUtf8(
        "A single captured frame cannot show resource destruction, so a leak cannot be proven. "
        "The candidates are resources never bound, or bound only once, during this frame; a "
        "confident verdict requires comparing across frames or captures.");

    QJsonObject j = FilterFields(LeakResultToJson(result), fields);
    j[QStringLiteral("snapshot")] = SnapshotToJson(snap);
    return ToolResult::Success(j);
  });
}

//
// ---- P1-6 structured performance report ----------------------------------
//
// Shared report builder used by both the report and the export tool. It runs on
// the replay thread (inside a DispatchBlock lambda) and only reads data that was
// captured by value on the UI thread, so it never touches ICaptureContext.
static PerformanceReport BuildPerformanceReport(IReplayController &replay,
                                                const CaptureSnapshot &snap,
                                                const QVector<ResourceLite> &resources, int cap,
                                                bool includePipeline, bool includeMesh)
{
  const SDFile &sdfile = replay.GetStructuredFile();

  // Timings first: without them nothing about the ranking is a fact.
  const QHash<quint32, double> timings = FetchEventTimings(replay);

  QHash<quint64, QString> resourceNames;
  for(const ResourceLite &r : resources)
    resourceNames.insert(r.id, r.name);

  // Pass attribution: every leaf event maps to the nearest enclosing marker (or
  // the implicit "Frame" root). Containers name the pass their children belong
  // to; leaves inherit it. This mirrors the plugin's aggregate() walk.
  QHash<quint32, QPair<QString, QString> > passForEvent;

  std::function<void(const ActionDescription &, const QString &, const QString &)> attribute =
      [&](const ActionDescription &a, const QString &passId, const QString &passName) {
        if(a.children.empty())
        {
          const QString id = passId.isEmpty() ? BuildPassId(QStringLiteral("Frame"), a.eventId,
                                                            a.eventId)
                                              : passId;
          const QString name = passName.isEmpty() ? QStringLiteral("Frame") : passName;
          passForEvent.insert(a.eventId, qMakePair(id, name));
          return;
        }

        const QString name = FromRdcStr(a.GetName(sdfile));
        const QString id = BuildPassId(name, PassFirstLeaf(a), PassLastLeaf(a));

        for(const ActionDescription &c : a.children)
          attribute(c, id, name);
      };

  for(const ActionDescription &root : replay.GetRootActions())
    attribute(root, QString(), QString());

  struct PassAgg
  {
    uint32_t first = 0;
    uint32_t last = 0;
    uint32_t count = 0;
    uint32_t timed = 0;
    double total = 0.0;
  };

  QHash<QString, PassAgg> passAgg;
  QHash<QString, QString> passNames;

  struct Row
  {
    const ActionDescription *action = NULL;
    uint32_t eventId = 0;
    QString name;
    QString passId;
    QString passName;
    double us = 0.0;
  };

  rdcarray<const ActionDescription *> leaves;
  CollectLeavesInList(replay.GetRootActions(), leaves);

  QVector<Row> rows;
  double totalUs = 0.0;

  for(const ActionDescription *leaf : leaves)
  {
    const QPair<QString, QString> pass = passForEvent.value(
        leaf->eventId, qMakePair(QStringLiteral("Frame"), QStringLiteral("Frame")));

    PassAgg &agg = passAgg[pass.first];
    passNames.insert(pass.first, pass.second);
    if(agg.count == 0)
      agg.first = leaf->eventId;
    agg.last = leaf->eventId;
    agg.count++;

    const double us = timings.value(leaf->eventId, 0.0);
    if(us <= 0.0)
      continue;

    agg.total += us;
    agg.timed++;
    totalUs += us;

    Row row;
    row.action = leaf;
    row.eventId = leaf->eventId;
    row.name = FromRdcStr(leaf->GetName(sdfile));
    row.passId = pass.first;
    row.passName = pass.second;
    row.us = us;
    rows.push_back(row);
  }

  PerformanceReport report;
  report.available = true;
  report.timingAvailable = !timings.isEmpty();
  report.capturePath = snap.filename;
  report.api = snap.api;
  report.frameNumber = snap.frameNumber;
  report.totalTimedEvents = (uint32_t)rows.size();
  report.totalDurationUs = totalUs;
  report.hotEventLimit = (uint32_t)cap;

  // Passes, hottest first. A pass is Confirmed only when every contained leaf had
  // a measurement; otherwise its total is a partial sum and therefore Inferred.
  QVector<PerformanceHotPass> passes;
  for(auto it = passAgg.constBegin(); it != passAgg.constEnd(); ++it)
  {
    PerformanceHotPass p;
    p.passId = it.key();
    p.name = passNames.value(it.key(), it.key());
    p.firstEventId = it.value().first;
    p.lastEventId = it.value().last;
    p.eventCount = it.value().count;
    p.durationUs = it.value().total;
    p.confidence =
        (it.value().timed == it.value().count) ? Confidence::Confirmed : Confidence::Inferred;
    passes.push_back(p);
  }

  std::sort(passes.begin(), passes.end(),
            [](const PerformanceHotPass &a, const PerformanceHotPass &b) {
              return a.durationUs > b.durationUs;
            });
  if(cap > 0 && passes.size() > cap)
    passes.resize(cap);
  report.hottestPasses = passes;

  // Hot events, hottest first, each with its evidence anchors.
  std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.us > b.us; });

  const int eventCount = (cap > 0 && rows.size() > cap) ? cap : rows.size();
  for(int i = 0; i < eventCount; i++)
  {
    const Row &row = rows[i];

    PerformanceHotEvent e;
    e.eventId = row.eventId;
    e.name = row.name;
    e.passId = row.passId;
    e.passName = row.passName;
    e.durationUs = row.us;
    e.share = totalUs > 0.0 ? (row.us / totalUs) : 0.0;
    e.confidence = Confidence::Confirmed;
    e.evidence.push_back(TimingEvidence(row.eventId, row.us, row.name));

    if(includePipeline && row.action != NULL)
    {
      LeafFacts facts;
      if(ReadLeafFacts(replay, *row.action, sdfile, facts))
      {
        e.pipelineAvailable = true;

        const int shaderCount = (int)facts.shaders.size();
        const int shaderCap = shaderCount > 8 ? 8 : shaderCount;
        for(int s = 0; s < shaderCap; s++)
          e.evidence.push_back(
              ShaderEvidence(facts.shaders[s].id, facts.shaders[s].stage, facts.shaders[s].name));

        int resourceCount = 0;
        for(quint64 idv : facts.resourceIds)
        {
          if(idv == 0)
            continue;
          if(resourceCount >= 12)
            break;
          e.evidence.push_back(ResourceEvidence(ResourceIdFromValue(idv), resourceNames.value(idv)));
          resourceCount++;
        }
      }
    }

    if(includeMesh && row.action != NULL && row.action->numIndices > 0)
      e.meshAvailable = true;

    report.hottestEvents.push_back(e);
  }

  // Provenance-split conclusions: without timings nothing about the ranking is a
  // fact, so everything moves to toVerify.
  if(report.timingAvailable)
  {
    report.confidence = Confidence::Confirmed;
    report.conclusions.confirmed.push_back(QString::fromUtf8(
        "Hotspot ranking is based on captured GPU timing values (EventGPUDuration)."));
    report.conclusions.toVerify.push_back(QString::fromUtf8(
        "Inspect pipeline, shader and resource bindings for the top event ids."));

    bool anyPartial = false;
    for(const PerformanceHotPass &p : report.hottestPasses)
    {
      if(p.confidence != Confidence::Confirmed)
      {
        anyPartial = true;
        break;
      }
    }
    if(anyPartial)
      report.conclusions.inferred.push_back(QString::fromUtf8(
          "Some passes contain leaf events without GPU timings, so their totals are partial."));
  }
  else
  {
    report.confidence = Confidence::ToVerify;
    report.limitation = QString::fromUtf8(
        "This capture's API does not expose the EventGPUDuration counter, so no hotspot ranking "
        "could be produced. Collect GPU timings before drawing performance conclusions.");
    report.conclusions.toVerify.push_back(
        QString::fromUtf8("Collect GPU timings before making performance conclusions."));
  }

  report.evidence.push_back(CaptureEvidence(report.capturePath));
  if(report.timingAvailable)
  {
    EvidenceRef timingAnchor;
    timingAnchor.kind = QStringLiteral("timing");
    timingAnchor.durationUs = totalUs;
    timingAnchor.label =
        QStringLiteral("EventGPUDuration over %1 events").arg(report.totalTimedEvents);
    timingAnchor.confidence = Confidence::Confirmed;
    report.evidence.push_back(timingAnchor);
  }

  return report;
}

ToolResult QueryAdapter::GeneratePerformanceReport(int limit, bool includePipeline, bool includeMesh,
                                                  CallContext &call, const QStringList &fields,
                                                  const QString &format)
{
  if(!format.isEmpty() && format != QStringLiteral("json") && format != QStringLiteral("markdown"))
    return ToolResult::Failure(Error(ErrorCode::InvalidParams, "format must be json or markdown"));
  const ToolResult ready = ReadOnUIThread<ToolResult>([this]() {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));
    return ToolResult::Success(QJsonObject());
  });
  if(!ready.ok) return ready;

  const int cap = limit > 200 ? 200 : (limit > 0 ? limit : 20);

  const CaptureSnapshot snap = Snapshot();
  const QVector<ResourceLite> resources =
      ReadOnUIThread<QVector<ResourceLite>>([this]() -> QVector<ResourceLite> {
    return SnapshotResources(m_Ctx);
  });

  return DispatchBlock(
      "renderdoc_generatePerformanceReport", call,
      [cap, includePipeline, includeMesh, snap, resources, fields, format](IReplayController &replay)
          -> ToolResult {
        const PerformanceReport report =
            BuildPerformanceReport(replay, snap, resources, cap, includePipeline, includeMesh);

        const QJsonObject reportJson = PerformanceReportToJson(report);
        QJsonObject j = FilterFields(reportJson, fields);
        // Keep the legacy top-level report keys. Rendering always uses the full report,
        // never a fields projection which could silently create an empty output file.
        if(!format.isEmpty())
        {
          j[QStringLiteral("format")] = format;
          j[QStringLiteral("report")] = reportJson;
          j[QStringLiteral("content")] = format == QStringLiteral("json")
              ? QString::fromUtf8(QJsonDocument(reportJson).toJson(QJsonDocument::Indented))
              : PerformanceReportToMarkdown(report);
        }
        j[QStringLiteral("snapshot")] = SnapshotToJson(snap);
        return ToolResult::Success(j);
      });
}

ToolResult QueryAdapter::ExportPerformanceReport(const QString &format, int limit, CallContext &call,
                                                 const QStringList &fields)
{
  if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
    return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
  if(m_Ctx->IsCaptureLoading())
    return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

  const QString fmt =
      format.compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0 ? QStringLiteral("json")
                                                                      : QStringLiteral("markdown");
  const int cap = limit > 200 ? 200 : (limit > 0 ? limit : 20);

  const CaptureSnapshot snap = Snapshot();
  const QVector<ResourceLite> resources =
      ReadOnUIThread<QVector<ResourceLite>>([this]() -> QVector<ResourceLite> {
    return SnapshotResources(m_Ctx);
  });

  return DispatchBlock("renderdoc_exportPerformanceReport", call,
                       [fmt, cap, snap, resources, fields](IReplayController &replay) -> ToolResult {
    const PerformanceReport report =
        BuildPerformanceReport(replay, snap, resources, cap, true, true);

    const QJsonObject reportJson = PerformanceReportToJson(report);

    QJsonObject out;
    out[QStringLiteral("format")] = fmt;
    out[QStringLiteral("report")] = reportJson;
    out[QStringLiteral("content")] =
        (fmt == QStringLiteral("json"))
            ? QString::fromUtf8(QJsonDocument(reportJson).toJson(QJsonDocument::Indented))
            : PerformanceReportToMarkdown(report);

    QJsonObject j = FilterFields(out, fields);
    j[QStringLiteral("snapshot")] = SnapshotToJson(snap);
    return ToolResult::Success(j);
  });
}

// P1-9: compose the whole-investigation report. The bookmark half (P1-8) and the
// resource-memory half (P1-4) are plain UI-thread snapshot reads, the performance
// half (P1-6) is built on the replay thread, and everything is assembled into one
// InvestigationReport. Like ExportPerformanceReport this never touches the disk:
// the rendered text comes back in-band as `content` so the dialog owns the file
// side effect and a headless client can just read the JSON.
ToolResult QueryAdapter::BuildInvestigationReport(const QString &format, int limit,
                                                  bool includeResources, CallContext &call,
                                                  const QStringList &fields)
{
  const CaptureSnapshot snap = Snapshot();
  if(!snap.hasCapture)
    return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
  if(snap.loading)
    return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

  const QString fmt =
      format.compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0 ? QStringLiteral("json")
                                                                      : QStringLiteral("markdown");
  const int cap = limit > 200 ? 200 : (limit > 0 ? limit : 20);
  const QString generatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

  // P1-8 bookmarks. This degrades gracefully (available == false with a reason)
  // when the capture is not open locally, it never fails the whole report.
  const BookmarkSidecar sidecar = ReadBookmarkSidecar();

  // P1-4 resource audit. Opt-out because the largest-resource list can be large;
  // when skipped the report records why rather than implying zero resources.
  MCP::ResourceMemoryAudit audit;
  if(includeResources)
  {
    audit = ReadOnUIThread<MCP::ResourceMemoryAudit>([this, cap]() -> MCP::ResourceMemoryAudit {
      return SnapshotMemoryAudit(m_Ctx, cap);
    });
  }

  const QVector<ResourceLite> resources =
      ReadOnUIThread<QVector<ResourceLite>>([this]() -> QVector<ResourceLite> {
    return SnapshotResources(m_Ctx);
  });

  return DispatchBlock(
      "renderdoc_buildInvestigationReport", call,
      [fmt, cap, generatedAt, snap, sidecar, audit, includeResources, resources, fields](
          IReplayController &replay) -> ToolResult {
        InvestigationReport report;
        report.available = true;
        report.generatedAt = generatedAt;
        report.capturePath = snap.filename;
        report.api = snap.api;
        report.frameNumber = snap.frameNumber;
        report.currentEvent = snap.currentEvent;

        report.bookmarks = sidecar.items;
        report.bookmarksAvailable = sidecar.available;
        report.bookmarkLimitation = sidecar.limitation;

        // P1-6 performance half. Pipeline evidence is attached so every hotspot can
        // be followed up; mesh availability is only advertised.
        report.performance = BuildPerformanceReport(replay, snap, resources, cap, true, true);

        // P1-4 resource half.
        report.resources = audit;
        if(!includeResources)
          report.limitations.append(QStringLiteral(
              "The resource-memory audit was not requested, so resource sizes are omitted."));

        // Report-level anchors: the capture plus one timing anchor per hotspot. The
        // per-event shader/resource evidence already travels on
        // performance.hottestEvents[].evidence, so it is not duplicated here.
        report.evidence.push_back(CaptureEvidence(report.capturePath));
        for(const PerformanceHotEvent &e : report.performance.hottestEvents)
        {
          if(!e.evidence.isEmpty())
            report.evidence.push_back(e.evidence.first());
        }

        report.confidence =
            report.performance.timingAvailable ? Confidence::Confirmed : Confidence::ToVerify;

        const QJsonObject reportJson = InvestigationReportToJson(report);

        QJsonObject out;
        out[QStringLiteral("format")] = fmt;
        out[QStringLiteral("report")] = reportJson;
        out[QStringLiteral("content")] =
            (fmt == QStringLiteral("json"))
                ? QString::fromUtf8(QJsonDocument(reportJson).toJson(QJsonDocument::Indented))
                : InvestigationReportToMarkdown(report);

        QJsonObject j = FilterFields(out, fields);
        j[QStringLiteral("snapshot")] = SnapshotToJson(snap);
        return ToolResult::Success(j);
      });
}

// P1-7: apply the combined advanced filter set over the action tree and package
// the survivors as an AI context block. Timings are fetched so rows can carry
// GPU duration when the capture supports EventGPUDuration.
static EventBrowserContext BuildEventBrowserContextImpl(IReplayController &replay,
                                                    const CaptureSnapshot &snap,
                                                    const EventBrowserFilterOptions &options,
                                                    int cap)
{
  EventBrowserContext ctx;
  ctx.capturePath = snap.filename;
  ctx.api = snap.api;
  ctx.frameNumber = snap.frameNumber;
  ctx.filters = options;

  const QHash<quint32, double> timings = FetchEventTimings(replay);

  const SDFile &sdfile = replay.GetStructuredFile();

  QVector<EventBrowserContextRow> rows;
  EventBrowserFilterStats stats;
  FilterEventBrowserActions(replay.GetRootActions(), sdfile, options, timings, rows, stats);

  ctx.totalEvents = stats.totalEvents;
  ctx.matchedEvents = stats.matchedEvents;

  const int limit = cap > 0 ? cap : 500;
  if(rows.size() > limit)
  {
    rows.resize(limit);
    ctx.truncated = true;
  }
  ctx.rows = rows;

  if(timings.isEmpty())
  {
    // Without GPU timings the ordering cannot be a fact, so we say so rather than
    // pretending the surviving list is a ranking.
    ctx.confidence = Confidence::ToVerify;
    ctx.limitation = QStringLiteral(
        "EventGPUDuration is unavailable in this capture, so rows carry no duration and "
        "sortByDuration has no effect.");
  }
  else
  {
    ctx.confidence = Confidence::Confirmed;
  }

  ctx.aiContext = EventBrowserContextToText(ctx);
  return ctx;
}

ToolResult QueryAdapter::BuildEventBrowserContext(const EventBrowserFilterOptions &options, int cap,
                                                  CallContext &call, const QStringList &fields)
{
  const ToolResult ready = ReadOnUIThread<ToolResult>([this]() {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));
    return ToolResult::Success(QJsonObject());
  });
  if(!ready.ok) return ready;

  const int clampedCap = cap > 2000 ? 2000 : (cap > 0 ? cap : 500);

  const CaptureSnapshot snap = Snapshot();

  return DispatchBlock("renderdoc_buildEventBrowserContext", call,
                       [options, clampedCap, snap, fields](IReplayController &replay) -> ToolResult {
    const EventBrowserContext ctx =
        BuildEventBrowserContextImpl(replay, snap, options, clampedCap);

    QJsonObject j = FilterFields(EventBrowserContextToJson(ctx), fields);
    j[QStringLiteral("snapshot")] = SnapshotToJson(snap);
    return ToolResult::Success(j);
  });
}

}    // namespace MCP
