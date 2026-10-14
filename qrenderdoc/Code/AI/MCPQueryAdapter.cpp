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

#include <QJsonArray>
#include <QWidget>
#include <exception>
#include <string.h>

#include "Code/Interface/QRDInterface.h"
#include "Code/QRDUtils.h"

// Provides the ToStr<> specialisations for ResourceType, TextureType, CompType,
// ResourceFormatType, GraphicsAPI and GPUVendor. This is included exactly once
// per translation unit that needs enum stringification, matching QRDUtils.cpp.
#include "renderdoc_tostr.inl"

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

// The remaining enum-to-name helpers are trivial wrappers around the ToStr<>
// specialisations that this translation unit (and only this one) pulls in via
// renderdoc_tostr.inl. They live here so tool code never has to reference the
// ToStr<> template directly and therefore never has to include the .inl a
// second time (which would duplicate the non-inline specialisations at link
// time). See the module-boundary note in MCPToolRegistry.h.
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

QueryAdapter::QueryAdapter(ICaptureContext *ctx) : m_Ctx(ctx)
{
}

template <typename T>
T QueryAdapter::ReadOnUIThread(const std::function<T()> &fn) const
{
  if(m_Ctx == NULL)
    return fn();

  IMainWindow *main = m_Ctx->GetMainWindow();
  QWidget *w = main ? main->Widget() : NULL;

  // Nothing to marshal onto (headless/unit-test context) or already on the UI
  // thread: just run it.
  if(w == NULL || GUIInvoke::onUIThread())
    return fn();

  T ret = T();
  GUIInvoke::blockcall(w, [&]() { ret = fn(); });
  return ret;
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
      const FrameStatistics &stats = frame.stats;
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
    const FrameStatistics &st = f.stats;

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

ToolResult QueryAdapter::RootActions(uint32_t eventId, int maxCount, const QStringList &fields)
{
  return ReadOnUIThread<ToolResult>([this, eventId, maxCount, fields]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

    const SDFile &sdfile = m_Ctx->GetStructuredFile();
    QJsonArray arr;
    int emitted = 0;

    const rdcarray<ActionDescription> &roots = m_Ctx->CurRootActions();
    for(const ActionDescription &a : roots)
    {
      if(eventId != 0 && a.eventId != eventId)
        continue;
      if(maxCount > 0 && emitted >= maxCount)
        break;
      arr.append(ActionToJson(a, sdfile, fields, 0));
      emitted++;
    }

    QJsonObject j;
    j[QStringLiteral("actions")] = arr;
    j[QStringLiteral("count")] = emitted;
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

ToolResult QueryAdapter::Resources(const QString &typeFilter, int maxCount, const QStringList &fields)
{
  return ReadOnUIThread<ToolResult>([this, typeFilter, maxCount, fields]() -> ToolResult {
    if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
      return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));
    if(m_Ctx->IsCaptureLoading())
      return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

    const rdcarray<ResourceDescription> &resources = m_Ctx->GetResources();
    QJsonArray arr;
    int emitted = 0;
    for(const ResourceDescription &r : resources)
    {
      const QString typeName = detail::ResourceTypeName(r.type);
      if(!typeFilter.isEmpty() && typeName.compare(typeFilter, Qt::CaseInsensitive) != 0)
        continue;
      if(maxCount > 0 && emitted >= maxCount)
        break;

      QJsonObject j;
      j[QStringLiteral("resourceId")] = ResourceIdToString(r.resourceId);
      j[QStringLiteral("id")] = double(0);    // reserved: numeric id is in the string form
      j[QStringLiteral("name")] = FromRdcStr(m_Ctx->GetResourceName(r.resourceId));
      j[QStringLiteral("type")] = typeName;
      j[QStringLiteral("autogeneratedName")] = r.autogeneratedName;
      arr.append(j);
      emitted++;
    }

    QJsonObject j;
    j[QStringLiteral("resources")] = arr;
    j[QStringLiteral("count")] = emitted;
    j[QStringLiteral("total")] = (int)resources.size();
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
        j[QStringLiteral("msaaSamples")] = t.msSamp > 1 ? t.msSamp : 1;
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
    j[QStringLiteral("msaaSamples")] = t->msSamp > 1 ? t->msSamp : 1;
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

ToolResult QueryAdapter::PipelineSnapshot(const QStringList &fields)
{
  if(m_Ctx == NULL || !m_Ctx->IsCaptureLoaded())
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
  if(m_Ctx == NULL)
    return ToolResult::Failure(Error(ErrorCode::InternalError, "no capture context"));

  if(!m_Ctx->IsCaptureLoaded())
    return ToolResult::Failure(Error(ErrorCode::NoCaptureLoaded, "no capture is loaded"));

  if(m_Ctx->IsCaptureLoading())
    return ToolResult::Failure(Error(ErrorCode::CaptureBusy, "capture is still loading"));

  if(call.IsCancelled())
    return ToolResult::Failure(Error(ErrorCode::Cancelled, "tool call cancelled"));

  IReplayManager &replay = m_Ctx->Replay();

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
    });
  }
  catch(const std::exception &e)
  {
    return ToolResult::Failure(Error(ErrorCode::InternalError,
        QStringLiteral("failed to dispatch replay query: %1").arg(QString::fromUtf8(e.what()))));
  }

  if(!invoked)
  {
    return ToolResult::Failure(Error(ErrorCode::ReplayUnavailable,
        QStringLiteral("the replay thread rejected the '%1' query")
            .arg(QString::fromUtf8(toolName ? toolName : "unknown"))));
  }

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

}    // namespace MCP
