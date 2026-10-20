/* Native resource tools. All replay access is inside QueryAdapter::DispatchBlock. */
#include "MCPResourceTools.h"
#include "Code/Interface/QRDInterface.h"
#include "MCPTools.h"
#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>
#include <limits>
#include <memory>


namespace MCP
{
namespace
{
ToolResult Fail(ErrorCode code, const char *message)
{
  return ToolResult::Failure(Error(code, message));
}
QJsonObject Type(const char *type)
{
  return QJsonObject{{QStringLiteral("type"), QString::fromUtf8(type)}};
}
QJsonObject Integer(double min, double max)
{
  QJsonObject type = Type("integer");
  type[QStringLiteral("minimum")] = min;
  type[QStringLiteral("maximum")] = max;
  return type;
}
QString Str(const rdcstr &s) { return QString::fromUtf8(s.c_str()); }
const ActionDescription *FindAction(const rdcarray<ActionDescription> &actions, uint32_t eventId)
{
  for(const ActionDescription &action : actions)
  {
    if(action.eventId == eventId) return &action;
    const ActionDescription *found = FindAction(action.children, eventId);
    if(found) return found;
  }
  return NULL;
}
bool ReadInteger(const QJsonObject &args, const char *name, int64_t min, int64_t max,
                 int64_t &out)
{
  const QJsonValue value = args.value(QString::fromUtf8(name));
  if(value.isUndefined()) return true;
  const double d = value.toDouble();
  if(!value.isDouble() || !std::isfinite(d) || std::floor(d) != d || d < double(min) || d > double(max))
    return false;
  out = int64_t(d);
  return true;
}
struct OutputDeleter
{
  void operator()(IReplayOutput *output) const { if(output) output->Shutdown(); }
};
QJsonObject ResourceMetadata(IReplayController &replay, ResourceId id)
{
  QJsonObject out{{QStringLiteral("resourceId"), id == ResourceId() ? QJsonValue(QJsonValue::Null) : QJsonValue(ResourceIdToString(id))}};
  for(const ResourceDescription &resource : replay.GetResources())
    if(resource.resourceId == id) { out[QStringLiteral("resourceName")] = Str(resource.name); break; }
  for(const TextureDescription &texture : replay.GetTextures())
    if(texture.resourceId == id)
    {
      out[QStringLiteral("format")] = detail::ResourceFormatString(texture.format);
      out[QStringLiteral("width")] = double(texture.width);
      out[QStringLiteral("height")] = double(texture.height);
      out[QStringLiteral("byteSize")] = QString::number(qulonglong(texture.byteSize));
      break;
    }
  for(const BufferDescription &buffer : replay.GetBuffers())
    if(buffer.resourceId == id) { out[QStringLiteral("byteSize")] = QString::number(qulonglong(buffer.length)); break; }
  return out;
}
QJsonObject BoundDescriptor(IReplayController &replay, const UsedDescriptor &used,
                            const ShaderReflection *reflection)
{
  QJsonObject entry = ResourceMetadata(replay, used.descriptor.resource);
  entry[QStringLiteral("slot")] = QJsonValue(QJsonValue::Null);
  entry[QStringLiteral("space")] = QJsonValue(QJsonValue::Null);
  entry[QStringLiteral("reflectionIndex")] = int(used.access.index);
  entry[QStringLiteral("arrayElement")] = double(used.access.arrayElement);
  entry[QStringLiteral("staticallyUnused")] = used.access.staticallyUnused;
  entry[QStringLiteral("descriptorStore")] = ResourceIdToString(used.access.descriptorStore);
  entry[QStringLiteral("descriptorByteOffset")] = double(used.access.byteOffset);
  entry[QStringLiteral("byteOffset")] = QString::number(qulonglong(used.descriptor.byteOffset));
  entry[QStringLiteral("viewByteSize")] = QString::number(qulonglong(used.descriptor.byteSize));
  const uint32_t index = used.access.index;
  const DescriptorCategory category = CategoryForDescriptorType(used.access.type);
  if(reflection && index != DescriptorAccess::NoShaderBinding)
  {
    if(category == DescriptorCategory::ConstantBlock && index < reflection->constantBlocks.size())
    {
      const ConstantBlock &block = reflection->constantBlocks[index];
      entry[QStringLiteral("name")] = Str(block.name);
      entry[QStringLiteral("slot")] = double(block.fixedBindNumber);
      entry[QStringLiteral("space")] = double(block.fixedBindSetOrSpace);
      entry[QStringLiteral("byteSize")] = double(block.byteSize);
      entry[QStringLiteral("variableCount")] = double(block.variables.size());
      entry[QStringLiteral("bufferBacked")] = block.bufferBacked;
    }
    else if(category == DescriptorCategory::Sampler && index < reflection->samplers.size())
    {
      const ShaderSampler &sampler = reflection->samplers[index];
      entry[QStringLiteral("name")] = Str(sampler.name);
      entry[QStringLiteral("slot")] = double(sampler.fixedBindNumber);
      entry[QStringLiteral("space")] = double(sampler.fixedBindSetOrSpace);
    }
    else
    {
      const rdcarray<ShaderResource> &resources = category == DescriptorCategory::ReadWriteResource ?
          reflection->readWriteResources : reflection->readOnlyResources;
      if(index < resources.size())
      {
        entry[QStringLiteral("name")] = Str(resources[index].name);
        entry[QStringLiteral("slot")] = double(resources[index].fixedBindNumber);
        entry[QStringLiteral("space")] = double(resources[index].fixedBindSetOrSpace);
      }
    }
  }
  if(category == DescriptorCategory::Sampler)
  {
    entry[QStringLiteral("resourceId")] = used.sampler.object == ResourceId() ? QJsonValue(QJsonValue::Null) : QJsonValue(ResourceIdToString(used.sampler.object));
    entry[QStringLiteral("minFilter")] = detail::FilterModeName(used.sampler.filter.minify);
    entry[QStringLiteral("magFilter")] = detail::FilterModeName(used.sampler.filter.magnify);
    entry[QStringLiteral("addressU")] = detail::AddressModeName(used.sampler.addressU);
    entry[QStringLiteral("addressV")] = detail::AddressModeName(used.sampler.addressV);
  }
  return entry;
}
ToolResult BoundResources(IReplayController &replay, uint32_t eventId, bool includeUnused,
                           bool includeConstants)
{
  if(!FindAction(replay.GetRootActions(), eventId))
    return Fail(ErrorCode::InvalidParams, "Action not found for eventId");
  replay.SetFrameEvent(eventId, false);
  const PipeState &pipe = replay.GetPipelineState();
  if(!pipe.IsCaptureLoaded()) return Fail(ErrorCode::ReplayUnavailable, "Pipeline state unavailable");
  // The common pipeline serializer remains the owner of the pipeline projection.
  const QJsonObject pipeline = PipelineStateToJson(replay);
  QJsonArray targets;
  int slot = 0;
  for(const Descriptor &target : pipe.GetOutputTargets())
  {
    if(target.resource != ResourceId())
    {
      QJsonObject entry = ResourceMetadata(replay, target.resource);
      entry[QStringLiteral("slot")] = slot;
      entry[QStringLiteral("name")] = entry.value(QStringLiteral("resourceName"));
      targets.append(entry);
    }
    slot++;
  }
  QJsonValue depth(QJsonValue::Null);
  if(pipe.GetDepthTarget().resource != ResourceId())
  {
    QJsonObject target = ResourceMetadata(replay, pipe.GetDepthTarget().resource);
    target[QStringLiteral("name")] = target.value(QStringLiteral("resourceName"));
    depth = target;
  }
  QJsonArray stages;
  int sampled = 0, storage = 0, buffers = 0, samplers = 0, constants = 0;
  for(int i = 0; i < int(ShaderStage::Count); i++)
  {
    ShaderStage stage = ShaderStage(i);
    if(pipe.GetShader(stage) == ResourceId()) continue;
    const ShaderReflection *reflection = pipe.GetShaderReflection(stage);
    QJsonObject entry{{QStringLiteral("stage"), stage == ShaderStage::Pixel ? QStringLiteral("Fragment") : detail::ShaderStageName(stage)},
                      {QStringLiteral("shaderName"), pipeline.value(QStringLiteral("shaders")).toObject()
                        .value(detail::ShaderStageName(stage)).toObject().value(QStringLiteral("entryPoint"))},
                      {QStringLiteral("shaderId"), ResourceIdToString(pipe.GetShader(stage))}};
    QJsonArray ro, rw, bs, ss, cb;
    auto append = [&](const rdcarray<UsedDescriptor> &descriptors, bool write) {
      for(const UsedDescriptor &used : descriptors)
      {
        if(!includeUnused && used.access.staticallyUnused) continue;
        QJsonObject item = BoundDescriptor(replay, used, reflection);
        if(used.descriptor.textureType == TextureType::Buffer || used.descriptor.textureType == TextureType::Unknown)
        { item[QStringLiteral("access")] = write ? QStringLiteral("rw") : QStringLiteral("read"); bs.append(item); }
        else if(write) rw.append(item);
        else ro.append(item);
      }
    };
    append(pipe.GetReadOnlyResources(stage), false);
    append(pipe.GetReadWriteResources(stage), true);
    for(const UsedDescriptor &used : pipe.GetSamplers(stage))
      if(includeUnused || !used.access.staticallyUnused) ss.append(BoundDescriptor(replay, used, reflection));
    if(includeConstants)
      for(const UsedDescriptor &used : pipe.GetConstantBlocks(stage))
        if(includeUnused || !used.access.staticallyUnused) cb.append(BoundDescriptor(replay, used, reflection));
    entry[QStringLiteral("readOnlyTextures")] = ro;
    entry[QStringLiteral("readWriteTextures")] = rw;
    entry[QStringLiteral("buffers")] = bs;
    entry[QStringLiteral("samplers")] = ss;
    if(includeConstants) entry[QStringLiteral("constantBuffers")] = cb;
    sampled += ro.size(); storage += rw.size(); buffers += bs.size(); samplers += ss.size(); constants += cb.size();
    stages.append(entry);
  }
  return ToolResult::Success(QJsonObject{{QStringLiteral("available"), true},
      {QStringLiteral("eventId"), double(eventId)}, {QStringLiteral("api"), detail::GraphicsAPIName(replay.GetAPIProperties().pipelineType)},
      {QStringLiteral("renderTargets"), targets}, {QStringLiteral("depthTarget"), depth},
      {QStringLiteral("stages"), stages},
      {QStringLiteral("resourceCounts"), QJsonObject{{QStringLiteral("renderTargets"), targets.size()},
        {QStringLiteral("sampledTextures"), sampled}, {QStringLiteral("storageTextures"), storage},
        {QStringLiteral("buffers"), buffers}, {QStringLiteral("samplers"), samplers}, {QStringLiteral("constantBuffers"), constants}}},
      {QStringLiteral("limitation"), QStringLiteral("Bindings and static usage are pipeline evidence, not proof of GPU access on an executed path.")}});
}
}

bool ParseResourceToolId(const QJsonValue &value, ResourceId &id)
{
  QString text;
  if(value.isString()) text = value.toString();
  else if(value.isDouble())
  {
    double d = value.toDouble();
    if(!std::isfinite(d) || d < 1 || d > 9007199254740991.0 || std::floor(d) != d) return false;
    text = QString::number(qulonglong(d));
  }
  else return false;
  if(text.startsWith(QStringLiteral("ResourceId::"))) text = text.mid(12);
  if(!QRegularExpression(QStringLiteral("^[0-9]+$")).match(text).hasMatch()) return false;
  bool ok = false;
  const quint64 number = text.toULongLong(&ok);
  if(!ok || number == 0) return false;
  static_assert(sizeof(id) == sizeof(number), "ResourceId layout");
  memcpy(&id, &number, sizeof(id));
  return true;
}

bool ReadResourceInteger(const QJsonObject &args, const char *name, int64_t min, int64_t max,
                         int64_t &out)
{
  return ReadInteger(args, name, min, max, out);
}

ToolResult ParseResourceSelector(const QJsonObject &args, ResourceId &id, bool required)
{
  ResourceId resource, texture;
  const bool hasResource = args.contains(QStringLiteral("resourceId"));
  const bool hasTexture = args.contains(QStringLiteral("textureId"));
  if((hasResource && !ParseResourceToolId(args.value(QStringLiteral("resourceId")), resource)) ||
     (hasTexture && !ParseResourceToolId(args.value(QStringLiteral("textureId")), texture)) ||
     (hasResource && hasTexture && resource != texture) || (required && !hasResource && !hasTexture))
    return Fail(ErrorCode::InvalidParams, "Invalid or conflicting resourceId/textureId");
  id = hasTexture ? texture : resource;
  return ToolResult::Success(QJsonObject());
}

QJsonObject ResourcePage(const QJsonArray &resources, int limit, int offset)
{
  QJsonArray page;
  for(int i = offset; i < resources.size() && page.size() < limit; i++) page.append(resources[i]);
  const bool truncated = offset < resources.size() && page.size() < resources.size() - offset;
  return QJsonObject{{QStringLiteral("resources"), page}, {QStringLiteral("total"), resources.size()},
      {QStringLiteral("count"), page.size()}, {QStringLiteral("returned"), page.size()},
      {QStringLiteral("offset"), offset}, {QStringLiteral("limit"), limit},
      {QStringLiteral("truncated"), truncated},
      {QStringLiteral("nextOffset"), truncated ? QJsonValue(offset + page.size()) : QJsonValue(QJsonValue::Null)}};
}

QJsonObject LimitResourceUsage(const QJsonObject &usage, int maxEvents)
{
  QJsonObject out = usage;
  bool truncated = false;
  for(const char *key : {"producerEvents", "consumerEvents", "accesses"})
  {
    const QString name = QString::fromLatin1(key);
    const QJsonArray all = usage.value(name).toArray();
    QJsonArray page;
    for(int i = 0; i < all.size() && (maxEvents <= 0 || i < maxEvents); i++) page.append(all[i]);
    out[name] = page;
    out[name + QStringLiteral("Returned")] = page.size();
    out[name + QStringLiteral("Truncated")] = page.size() < all.size();
    truncated |= page.size() < all.size();
  }
  out[QStringLiteral("maxEvents")] = maxEvents;
  out[QStringLiteral("truncated")] = truncated;
  return out;
}

ToolResult SelectResourceEvent(const rdcarray<ActionDescription> &actions, uint32_t requested,
                               bool zeroMeansEnd, const std::function<void(uint32_t)> &select,
                               uint32_t &effective)
{
  uint32_t maximum = 0;
  bool found = false;
  std::function<void(const rdcarray<ActionDescription> &)> visit = [&](const rdcarray<ActionDescription> &list) {
    for(const ActionDescription &action : list)
    {
      maximum = qMax(maximum, action.eventId);
      found |= action.eventId == requested;
      for(const APIEvent &event : action.events) found |= event.eventId == requested;
      visit(action.children);
    }
  };
  visit(actions);
  effective = requested ? requested : (zeroMeansEnd ? maximum : 0);
  if((requested && !found) || (zeroMeansEnd && !effective))
    return Fail(ErrorCode::InvalidParams, "Requested event not found in capture");
  if(effective) select(effective);
  return ToolResult::Success(QJsonObject());
}

ToolResult ParseTextureReadRequest(const QJsonObject &args, TextureReadRequest &request)
{
  TextureReadRequest parsed;
  ToolResult selector = ParseResourceSelector(args, parsed.id, true);
  if(!selector.ok) return selector;
  int64_t mip = 0, slice = 0, sample = 0, event = 0, channel = -1, maxBytes = 65536;
  if(!ReadInteger(args, "mip", 0, UINT32_MAX, mip) ||
     !ReadInteger(args, "slice", 0, UINT32_MAX, slice) ||
     !ReadInteger(args, "sample", 0, UINT32_MAX, sample) ||
     !ReadInteger(args, "eventId", 0, UINT32_MAX, event) ||
     !ReadInteger(args, "channelExtract", -1, 3, channel) ||
     !ReadInteger(args, "maxBytes", 1, 65536, maxBytes))
    return Fail(ErrorCode::InvalidParams, "Invalid texture subresource/event/channel/byte limit");
  const QJsonValue encoding = args.value(QStringLiteral("outputFormat"));
  if(!encoding.isUndefined() && (!encoding.isString() ||
      (encoding.toString() != QStringLiteral("raw") && encoding.toString() != QStringLiteral("png"))))
    return Fail(ErrorCode::InvalidParams, "outputFormat must be raw or png");
  parsed.png = encoding.toString() == QStringLiteral("png");
  // Raw data retains its original format and all channels. Never silently discard extraction.
  if(!parsed.png && channel >= 0)
    return Fail(ErrorCode::Unsupported, "channelExtract requires outputFormat=png; raw texture bytes are not converted");
  if(parsed.png && args.contains(QStringLiteral("maxBytes")))
    return Fail(ErrorCode::Unsupported, "maxBytes bounds raw bytes only; PNG is returned whole");
  parsed.sub = {uint32_t(mip), uint32_t(slice), uint32_t(sample)};
  parsed.eventId = uint32_t(event);
  parsed.selectEvent = args.contains(QStringLiteral("eventId")) || parsed.png;
  parsed.channelExtract = int(channel);
  parsed.maxBytes = uint64_t(maxBytes);
  request = parsed;
  return ToolResult::Success(QJsonObject());
}

ToolResult ReadTexturePNG(IReplayController &replay, const TextureReadRequest &request, CallContext &call)
{
  const TextureDescription *texture = NULL;
  for(const TextureDescription &candidate : replay.GetTextures())
    if(candidate.resourceId == request.id) { texture = &candidate; break; }
  if(!texture) return Fail(ErrorCode::InvalidParams, "Texture not found in capture");
  if(request.sub.mip >= texture->mips || request.sub.slice >= qMax(1U, texture->arraysize) ||
     request.sub.sample >= qMax(1U, texture->msSamp))
    return Fail(ErrorCode::InvalidParams, "Texture subresource is out of range");
  // Unlike the extension we have no software ASTC decompressor. A black GPU image is not evidence.
  if(texture->format.type == ResourceFormatType::ASTC || texture->depth > 1)
    return Fail(ErrorCode::Unsupported, "PNG preview does not support ASTC software decoding or volume textures; use raw");
  const uint32_t width = qMax(1U, texture->width >> qMin(request.sub.mip, 31U));
  const uint32_t height = qMax(1U, texture->height >> qMin(request.sub.mip, 31U));
  if(width > uint32_t(INT32_MAX / 3) || height > uint32_t(INT32_MAX) ||
     uint64_t(width) * height > uint64_t(INT32_MAX) / 4)
    return Fail(ErrorCode::InvalidParams, "PNG dimensions exceed Qt image representation");
  if(call.IsCancelled()) return Fail(ErrorCode::Cancelled, "Texture preview cancelled");
  if(call.IsExpired()) return Fail(ErrorCode::Timeout, "Texture preview deadline exceeded");
  std::unique_ptr<IReplayOutput, OutputDeleter> output(replay.CreateOutput(
      CreateHeadlessWindowingData(int32_t(width), int32_t(height)), ReplayOutputType::Texture));
  if(!output) return Fail(ErrorCode::Unsupported, "Replay backend cannot create a headless texture preview");
  TextureDisplay display = {};
  display.resourceId = request.id;
  display.subresource = request.sub;
  display.typeCast = CompType::Typeless;
  display.red = request.channelExtract < 0 || request.channelExtract == 0;
  display.green = request.channelExtract < 0 || request.channelExtract == 1;
  display.blue = request.channelExtract < 0 || request.channelExtract == 2;
  display.alpha = request.channelExtract == 3;
  if(texture->format.compType == CompType::Depth && request.channelExtract < 0)
  { display.red = true; display.green = display.blue = display.alpha = false; }
  display.rangeMin = 0.0f; display.rangeMax = 1.0f;
  display.hdrMultiplier = -1.0f;
  display.scale = 1.0f;
  display.backgroundColor = FloatVector(0, 0, 0, 0);
  display.overlay = DebugOverlay::NoOverlay;
  // RGB display deliberately discards source alpha; alpha extraction is grayscale.
  output->SetTextureDisplay(display);
  output->Display();
  const rdcpair<int32_t, int32_t> dimensions = output->GetDimensions();
  if(dimensions.first != int32_t(width) || dimensions.second != int32_t(height))
    return Fail(ErrorCode::ReplayUnavailable, "PNG replay output dimensions differ from texture mip");
  ToolResult result = EncodePreviewPNG(output->ReadbackOutputTexture(), int(width), int(height), call);
  if(result.ok)
  {
    result.structured[QStringLiteral("resourceId")] = ResourceIdToString(request.id);
    result.structured[QStringLiteral("texFormat")] = detail::ResourceFormatString(texture->format);
    result.structured[QStringLiteral("mip")] = double(request.sub.mip);
    result.structured[QStringLiteral("slice")] = double(request.sub.slice);
    result.structured[QStringLiteral("sample")] = double(request.sub.sample);
    result.structured[QStringLiteral("channelExtract")] = request.channelExtract;
    result.structured[QStringLiteral("outputFormat")] = QStringLiteral("png");
    result.structured[QStringLiteral("alphaSemantics")] = QStringLiteral("opaque RGB preview; extracted channel is grayscale");
  }
  return result;
}

ToolResult ParseDrawPreviewRequest(const QJsonObject &args, DrawPreviewRequest &request)
{
  DrawPreviewRequest parsed;
  int64_t event = 0, channel = -1, width = 0, height = 0;
  if(!args.contains(QStringLiteral("eventId")) || !ReadInteger(args, "eventId", 0, UINT32_MAX, event) ||
     !ReadInteger(args, "channelExtract", -1, 3, channel) ||
     !ReadInteger(args, "width", 1, INT32_MAX / 3, width) || !ReadInteger(args, "height", 1, INT32_MAX, height))
    return Fail(ErrorCode::InvalidParams, "Invalid event/channel/size parameter");
  parsed.eventId = uint32_t(event); parsed.channelExtract = int(channel);
  parsed.width = int(width); parsed.height = int(height);
  if(args.contains(QStringLiteral("overlayMode")) && !args.value(QStringLiteral("overlayMode")).isString())
    return Fail(ErrorCode::InvalidParams, "overlayMode must be a string");
  parsed.overlayMode = args.value(QStringLiteral("overlayMode")).toString(QStringLiteral("none")).toLower();
  if(parsed.overlayMode.isEmpty()) parsed.overlayMode = QStringLiteral("none");
  const char *names[] = {"none", "drawcall", "wireframe", "depth", "stencil", "backfacecull", "viewportscissor", "nan"};
  const DebugOverlay values[] = {DebugOverlay::NoOverlay, DebugOverlay::Drawcall, DebugOverlay::Wireframe,
      DebugOverlay::Depth, DebugOverlay::Stencil, DebugOverlay::BackfaceCull, DebugOverlay::ViewportScissor, DebugOverlay::NaN};
  bool found = false;
  for(int i = 0; i < 8; i++) if(parsed.overlayMode == QString::fromLatin1(names[i])) { parsed.overlay = values[i]; found = true; }
  if(!found) return Fail(ErrorCode::InvalidParams, "Unsupported overlayMode");
  for(const char *key : {"baseGammaEnabled", "gamma"})
    if(args.contains(QString::fromLatin1(key)))
    {
      if(!args.value(QString::fromLatin1(key)).isBool()) return Fail(ErrorCode::InvalidParams, "gamma must be boolean");
      parsed.baseGammaEnabled = args.value(QString::fromLatin1(key)).toBool();
    }
  if(args.contains(QStringLiteral("gamma")) && args.contains(QStringLiteral("baseGammaEnabled")) &&
     args.value(QStringLiteral("gamma")) != args.value(QStringLiteral("baseGammaEnabled")))
    return Fail(ErrorCode::InvalidParams, "Conflicting gamma switches");
  if(args.contains(QStringLiteral("label")))
  {
    if(!args.value(QStringLiteral("label")).isString()) return Fail(ErrorCode::InvalidParams, "label must be a string");
    parsed.label = args.value(QStringLiteral("label")).toString();
  }
  if(args.contains(QStringLiteral("resourceId")) && !ParseResourceToolId(args.value(QStringLiteral("resourceId")), parsed.resourceId))
    return Fail(ErrorCode::InvalidParams, "Invalid resourceId");
  if(args.contains(QStringLiteral("overlayResourceId")) && !ParseResourceToolId(args.value(QStringLiteral("overlayResourceId")), parsed.overlayResourceId))
    return Fail(ErrorCode::InvalidParams, "Invalid overlayResourceId");
  if(parsed.overlayResourceId != ResourceId() && parsed.overlay == DebugOverlay::NoOverlay)
    return Fail(ErrorCode::InvalidParams, "overlayResourceId requires an overlay mode");
  request = parsed;
  return ToolResult::Success(QJsonObject());
}

ToolResult EncodePreviewPNG(const bytebuf &rgb, int width, int height, CallContext &call)
{
  if(call.IsCancelled()) return Fail(ErrorCode::Cancelled, "Preview cancelled");
  if(call.IsExpired()) return Fail(ErrorCode::Timeout, "Preview deadline exceeded");
  const uint64_t pixels = width > 0 && height > 0 ? uint64_t(width) * uint64_t(height) : 0;
  // Qt5 image and QByteArray sizes are signed int; reserve representation expansion before allocation.
  if(!pixels || uint64_t(width) * 3 > INT32_MAX || pixels > uint64_t(INT32_MAX) / 4)
    return Fail(ErrorCode::InvalidParams, "Preview dimensions exceed Qt image representation");
  if(uint64_t(rgb.size()) != pixels * 3)
    return Fail(ErrorCode::ReplayUnavailable, "Output readback is not the expected tightly packed RGB image");
  QImage image(rgb.data(), width, height, width * 3, QImage::Format_RGB888);
  QByteArray png;
  QBuffer buffer(&png);
  if(image.isNull() || !buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG") || png.isEmpty())
    return Fail(ErrorCode::InternalError, "PNG encoder failed");
  if(uint64_t(png.size()) > uint64_t(INT32_MAX) / 4 * 3)
    return Fail(ErrorCode::InternalError, "PNG base64 exceeds Qt representation");
  if(call.IsCancelled()) return Fail(ErrorCode::Cancelled, "Preview cancelled");
  if(call.IsExpired()) return Fail(ErrorCode::Timeout, "Preview deadline exceeded");
  return ToolResult::Success(QJsonObject{{QStringLiteral("available"), true}, {QStringLiteral("format"), QStringLiteral("png")},
      {QStringLiteral("mimeType"), QStringLiteral("image/png")}, {QStringLiteral("width"), width},
      {QStringLiteral("height"), height}, {QStringLiteral("base64"), QString::fromLatin1(png.toBase64())}});
}

ToolResult CompositePreviewRGB(bytebuf &base, const bytebuf &overlay, const bytebuf &alpha)
{
  if(base.empty() || base.size() % 3 || overlay.size() != base.size() || alpha.size() != base.size())
    return Fail(ErrorCode::ReplayUnavailable, "Overlay RGB/alpha readback size mismatch");
  for(size_t i = 0; i < base.size(); i += 3)
    for(size_t c = 0; c < 3; c++)
      base[i + c] = byte((uint32_t(base[i + c]) * (255 - alpha[i]) +
                          uint32_t(overlay[i + c]) * alpha[i] + 127) / 255);
  return ToolResult::Success(QJsonObject());
}

ToolResult ReadDrawPreview(IReplayController &replay, const DrawPreviewRequest &request, CallContext &call)
{
  uint32_t event = request.eventId;
  if(!event)
  {
    std::function<void(const rdcarray<ActionDescription> &)> maximum = [&](const rdcarray<ActionDescription> &actions) {
      for(const ActionDescription &action : actions) { event = qMax(event, action.eventId); maximum(action.children); }
    };
    maximum(replay.GetRootActions());
  }
  const ActionDescription *action = FindAction(replay.GetRootActions(), event);
  if(!action) return Fail(ErrorCode::InvalidParams, "Action not found for preview event");
  replay.SetFrameEvent(event, false);
  ResourceId resource = request.resourceId;
  if(resource == ResourceId())
  {
    for(ResourceId output : action->outputs) if(output != ResourceId()) { resource = output; break; }
    if(resource == ResourceId()) resource = action->depthOut;
    if(resource == ResourceId()) resource = action->copyDestination;
  }
  if(resource == ResourceId())
    for(const TextureDescription &candidate : replay.GetTextures())
      if((candidate.creationFlags & TextureCategory::SwapBuffer) != TextureCategory::NoFlags)
      { resource = candidate.resourceId; break; }
  if(resource == ResourceId()) return Fail(ErrorCode::Unsupported, "Event has no preview texture; select resourceId explicitly");
  if(request.overlay != DebugOverlay::NoOverlay &&
     (action->flags & (ActionFlags::Drawcall | ActionFlags::MeshDispatch)) == ActionFlags::NoFlags)
    return Fail(ErrorCode::Unsupported, "Overlay requires a draw event");
  // overlayResourceId selects the overlay producer, never the base image.
  ResourceId displayResource = resource;
  ResourceId overlaySource = request.overlayResourceId == ResourceId() ? resource : request.overlayResourceId;
  const TextureDescription *texture = NULL;
  bool baseExists = false;
  for(const TextureDescription &candidate : replay.GetTextures())
  {
    if(candidate.resourceId == resource) baseExists = true;
    if(candidate.resourceId == displayResource) texture = &candidate;
  }
  if(request.overlay == DebugOverlay::NaN && overlaySource != resource)
    return Fail(ErrorCode::Unsupported, "NaN inline overlay must use the base resource");
  if(!baseExists || !texture || !texture->width || !texture->height)
    return Fail(ErrorCode::InvalidParams, "Requested preview/overlay texture not found");
  const TextureDescription *overlayTexture = NULL;
  for(const TextureDescription &candidate : replay.GetTextures())
    if(candidate.resourceId == overlaySource) overlayTexture = &candidate;
  if(request.overlay != DebugOverlay::NoOverlay && request.overlay != DebugOverlay::NaN)
  {
    auto capable = [](const TextureDescription *t) {
      return t && (t->creationFlags & (TextureCategory::ColorTarget | TextureCategory::SwapBuffer)) != TextureCategory::NoFlags;
    };
    if(!capable(overlayTexture) && request.overlayResourceId == ResourceId())
      for(const Descriptor &target : replay.GetPipelineState().GetOutputTargets())
        for(const TextureDescription &candidate : replay.GetTextures())
          if(candidate.resourceId == target.resource && capable(&candidate))
          { overlaySource = target.resource; overlayTexture = &candidate; break; }
    if(!capable(overlayTexture)) return Fail(ErrorCode::Unsupported, "Overlay requires a color render target");
    if(overlayTexture->width != texture->width || overlayTexture->height != texture->height)
      return Fail(ErrorCode::Unsupported, "Overlay source dimensions differ from base; viewport remapping is not supported");
  }
  const ResourceFormat textureFormat = texture->format;
  uint64_t width = request.width ? uint64_t(request.width) : uint64_t(texture->width);
  uint64_t height = request.height ? uint64_t(request.height) : uint64_t(texture->height);
  if(!width || !height || width > uint64_t(INT32_MAX) / 3 || height > INT32_MAX || width * height > uint64_t(INT32_MAX) / 4)
    return Fail(ErrorCode::InvalidParams, "Preview dimensions exceed Qt image representation");
  if(call.IsCancelled()) return Fail(ErrorCode::Cancelled, "Preview cancelled");
  if(call.IsExpired()) return Fail(ErrorCode::Timeout, "Preview deadline exceeded");
  std::unique_ptr<IReplayOutput, OutputDeleter> output(replay.CreateOutput(
      CreateHeadlessWindowingData(int32_t(width), int32_t(height)), ReplayOutputType::Texture));
  if(!output) return Fail(ErrorCode::ReplayUnavailable, "CreateOutput failed");
  TextureDisplay display = {};
  display.resourceId = displayResource;
  display.typeCast = CompType::Typeless;
  display.red = request.channelExtract < 0 || request.channelExtract == 0;
  display.green = request.channelExtract < 0 || request.channelExtract == 1;
  display.blue = request.channelExtract < 0 || request.channelExtract == 2;
  display.alpha = request.channelExtract == 3;
  display.hdrMultiplier = -1.0f;
  bool gammaAvailable = true;
  bool gammaEnabled = request.baseGammaEnabled;
  if((texture->creationFlags & TextureCategory::SwapBuffer) != TextureCategory::NoFlags)
  {
    gammaAvailable = false;
    gammaEnabled = !(texture->format.compByteWidth == 2 && texture->format.type == ResourceFormatType::Regular);
  }
  else if(texture->format.SRGBCorrected()) { gammaAvailable = false; gammaEnabled = false; }
  display.linearDisplayAsGamma = gammaEnabled;
  display.rangeMin = 0.0f; display.rangeMax = 1.0f;
  display.subresource = {0, 0, texture->msSamp > 1 && texture->format.compType != CompType::Depth ? TextureDisplay::ResolveSamples : 0u};
  display.overlay = request.overlay == DebugOverlay::NaN ? request.overlay : DebugOverlay::NoOverlay;
  if(texture->format.compType == CompType::Depth && request.channelExtract < 0)
  { display.red = true; display.green = display.blue = display.alpha = false; }
  display.backgroundColor = FloatVector(0, 0, 0, 0);
  display.scale = qMin(float(width) / texture->width, float(height) / texture->height);
  display.xOffset = (float(width) - texture->width * display.scale) * 0.5f;
  display.yOffset = (float(height) - texture->height * display.scale) * 0.5f;
  output->SetTextureDisplay(display);
  output->Display();
  const rdcpair<int32_t, int32_t> dimensions = output->GetDimensions();
  if(dimensions.first != int32_t(width) || dimensions.second != int32_t(height))
    return Fail(ErrorCode::ReplayUnavailable, "Replay output dimensions differ from requested size");
  bytebuf rgb = output->ReadbackOutputTexture();
  if(request.overlay != DebugOverlay::NoOverlay && request.overlay != DebugOverlay::NaN)
  {
    std::unique_ptr<IReplayOutput, OutputDeleter> overlayOutput(replay.CreateOutput(
        CreateHeadlessWindowingData(int32_t(width), int32_t(height)), ReplayOutputType::Texture));
    if(!overlayOutput) return Fail(ErrorCode::ReplayUnavailable, "CreateOutput failed for overlay");
    TextureDisplay overlayDisplay = display;
    overlayDisplay.resourceId = overlaySource;
    overlayDisplay.overlay = request.overlay;
    overlayDisplay.red = overlayDisplay.green = overlayDisplay.blue = true;
    overlayDisplay.alpha = false;
    overlayDisplay.linearDisplayAsGamma = false;
    overlayOutput->SetTextureDisplay(overlayDisplay);
    overlayOutput->Display();
    const ResourceId overlayId = overlayOutput->GetDebugOverlayTexID();
    if(overlayId == ResourceId()) return Fail(ErrorCode::ReplayUnavailable, "Replay did not produce the requested debug overlay");
    overlayDisplay.resourceId = overlayId;
    overlayDisplay.overlay = DebugOverlay::NoOverlay;
    overlayDisplay.subresource = {0, 0, 0};
    overlayOutput->SetTextureDisplay(overlayDisplay);
    overlayOutput->Display();
    bytebuf overlayRGB = overlayOutput->ReadbackOutputTexture();
    overlayDisplay.red = overlayDisplay.green = overlayDisplay.blue = false;
    overlayDisplay.alpha = true;
    overlayOutput->SetTextureDisplay(overlayDisplay);
    overlayOutput->Display();
    bytebuf alphaRGB = overlayOutput->ReadbackOutputTexture();
    if(rgb.size() != width * height * 3 || overlayRGB.size() != rgb.size() || alphaRGB.size() != rgb.size())
      return Fail(ErrorCode::ReplayUnavailable, "Overlay readback dimensions differ from base");
    ToolResult composite = CompositePreviewRGB(rgb, overlayRGB, alphaRGB);
    if(!composite.ok) return composite;
  }
  ToolResult result = EncodePreviewPNG(rgb, int(width), int(height), call);
  if(!result.ok) return result;
  result.structured[QStringLiteral("eventId")] = double(event);
  result.structured[QStringLiteral("resourceId")] = ResourceIdToString(resource);
  result.structured[QStringLiteral("displayResourceId")] = ResourceIdToString(displayResource);
  result.structured[QStringLiteral("overlayResourceId")] = request.overlay == DebugOverlay::NoOverlay ? QJsonValue(QJsonValue::Null) : QJsonValue(ResourceIdToString(overlaySource));
  result.structured[QStringLiteral("texFormat")] = detail::ResourceFormatString(textureFormat);
  result.structured[QStringLiteral("baseGammaAvailable")] = gammaAvailable;
  result.structured[QStringLiteral("overlayMode")] = request.overlayMode;
  result.structured[QStringLiteral("channelExtract")] = request.channelExtract;
  result.structured[QStringLiteral("baseGammaEnabled")] = gammaEnabled;
  result.structured[QStringLiteral("label")] = request.label;
  return result;
}

ToolResult QueryAdapter::ResourceTool(const QString &name, const QJsonObject &args, CallContext &call)
{
  ToolResult result = WithCaptureUI(call, [&](ICaptureContext &ctx) -> ToolResult {
    if(!ctx.Config().MCP_AllowSensitiveData)
      return ToolResult::Failure(Error(ErrorCode::Unsupported,
          QStringLiteral("Resource inspection requires MCP_AllowSensitiveData"),
          QJsonObject{{QStringLiteral("setting"), QStringLiteral("MCP_AllowSensitiveData")}, {QStringLiteral("tool"), name}}));
    if(!ctx.IsCaptureLoaded()) return Fail(ErrorCode::NoCaptureLoaded, "No capture is loaded");
    if(ctx.IsCaptureLoading()) return Fail(ErrorCode::CaptureBusy, "Capture is loading");
    if(name == QStringLiteral("renderdoc_getResources"))
    {
      int64_t limit = 256, offset = 0, legacy = 0;
      if(!ReadInteger(args, "limit", 1, 500, limit) ||
         !ReadInteger(args, "offset", 0, INT32_MAX, offset) ||
         !ReadInteger(args, "maxCount", 0, INT32_MAX, legacy))
        return Fail(ErrorCode::InvalidParams, "Invalid resource limit/offset/maxCount");
      // Keep the native legacy budget (default 256, cap 4096) independently of limit's cap.
      if(args.contains(QStringLiteral("maxCount")))
      {
        const int64_t legacyLimit = legacy > 0 ? qMin<int64_t>(legacy, 4096) : 256;
        if(args.contains(QStringLiteral("limit")) && legacyLimit != limit)
          return Fail(ErrorCode::InvalidParams, "Conflicting limit and maxCount");
        if(!args.contains(QStringLiteral("limit"))) limit = legacyLimit;
      }
      if(args.contains(QStringLiteral("type")) && !args.value(QStringLiteral("type")).isString())
        return Fail(ErrorCode::InvalidParams, "type must be a string");
      QStringList fields;
      for(const QJsonValue &field : args.value(QStringLiteral("fields")).toArray()) fields.append(field.toString());
      return Resources(args.value(QStringLiteral("type")).toString(), int(limit), fields, int(offset));
    }
    if(name == QStringLiteral("renderdoc_getTextureInfo"))
    {
      ResourceId id;
      ToolResult parsed = ParseResourceSelector(args, id, false);
      if(!parsed.ok) return parsed;
      return id == ResourceId() ? Textures() : Texture(id);
    }
    if(name == QStringLiteral("renderdoc_getResourceLifetime"))
    {
      ResourceId id;
      if(args.contains(QStringLiteral("resourceId")) && !ParseResourceToolId(args.value(QStringLiteral("resourceId")), id))
        return Fail(ErrorCode::InvalidParams, "Invalid resourceId");
      int64_t limit = 100;
      if(!ReadInteger(args, "limit", 1, 500, limit)) return Fail(ErrorCode::InvalidParams, "Invalid limit");
      ToolResult result = ResourceLifetime(int(limit), call, QStringList(), id);
      if(result.ok && id != ResourceId())
      {
        ToolResult usage = TraceResourceUsage(id, call);
        if(!usage.ok) return usage;
        result.structured[QStringLiteral("usageEvidence")] = usage.structured;
      }
      return result;
    }
    if(name == QStringLiteral("renderdoc_getBoundResources"))
    {
      int64_t event = 0;
      if(!args.contains(QStringLiteral("eventId")) || !ReadInteger(args, "eventId", 1, UINT32_MAX, event))
        return Fail(ErrorCode::InvalidParams, "Invalid eventId");
      for(const char *key : {"includeUnused", "includeConstantBuffers"})
        if(args.contains(QString::fromLatin1(key)) && !args.value(QString::fromLatin1(key)).isBool())
          return Fail(ErrorCode::InvalidParams, "Binding filters must be boolean");
      const CaptureSnapshot snapshot = Snapshot();
      return DispatchBlock("renderdoc_getBoundResources", call, [&](IReplayController &replay) {
        ToolResult result = BoundResources(replay, uint32_t(event), args.value(QStringLiteral("includeUnused")).toBool(true),
                                          args.value(QStringLiteral("includeConstantBuffers")).toBool(true));
        if(result.ok) result.structured[QStringLiteral("snapshot")] = SnapshotToJson(snapshot);
        return result;
      });
    }
    if(name != QStringLiteral("renderdoc_getCurrentDrawPreview"))
      return Fail(ErrorCode::InvalidParams, "Unknown resource tool");
    DrawPreviewRequest request;
    ToolResult parsed = ParseDrawPreviewRequest(args, request);
    if(!parsed.ok) return parsed;
    const CaptureSnapshot snapshot = Snapshot();
    return DispatchBlock("renderdoc_getCurrentDrawPreview", call, [&](IReplayController &replay) {
      ToolResult result = ReadDrawPreview(replay, request, call);
      if(result.ok) result.structured[QStringLiteral("snapshot")] = SnapshotToJson(snapshot);
      return result;
    });
  });
  // Structured metadata added after encoding must also be present in MCP text content.
  if(result.ok) result.text = QString::fromUtf8(QJsonDocument(result.structured).toJson(QJsonDocument::Compact));
  return result;
}

void RegisterResourceTools(ToolRegistry &registry, QueryAdapter &adapter)
{
  const QJsonObject id = Type("string");
  const QJsonObject bound{{QStringLiteral("eventId"), Integer(1, UINT32_MAX)},
      {QStringLiteral("includeUnused"), Type("boolean")}, {QStringLiteral("includeConstantBuffers"), Type("boolean")}};
  const QJsonObject preview{{QStringLiteral("eventId"), Integer(0, UINT32_MAX)},
      {QStringLiteral("channelExtract"), Integer(-1, 3)}, {QStringLiteral("overlayMode"), Type("string")},
      {QStringLiteral("resourceId"), id}, {QStringLiteral("overlayResourceId"), id},
      {QStringLiteral("baseGammaEnabled"), Type("boolean")}, {QStringLiteral("gamma"), Type("boolean")},
      {QStringLiteral("width"), Integer(1, INT32_MAX / 3)}, {QStringLiteral("height"), Integer(1, INT32_MAX)},
      {QStringLiteral("label"), Type("string")}};
  const QJsonObject lifetime{{QStringLiteral("resourceId"), id}, {QStringLiteral("limit"), Integer(1, 500)}};
  struct Tool { const char *name; QJsonObject properties; QJsonArray required; const char *description; };
  for(const Tool &tool : {Tool{"renderdoc_getBoundResources", bound, QJsonArray{QStringLiteral("eventId")}, "Inspect normalized pipeline bindings with static-use filtering."},
      Tool{"renderdoc_getCurrentDrawPreview", preview, QJsonArray{QStringLiteral("eventId")}, "Render a headless output and encode its RGB readback as PNG; temporary outputs are destroyed."},
      Tool{"renderdoc_getResourceLifetime", lifetime, QJsonArray(), "Inspect observed resource event scope, optionally selecting resourceId; not allocation lifetime proof."}})
  {
    const QString name = QString::fromLatin1(tool.name);
    registry.Add(ToolDefinition{name, name, QString::fromLatin1(tool.description), ToolEffects::ReadOnly,
        ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
        QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), tool.properties},
          {QStringLiteral("required"), tool.required}, {QStringLiteral("additionalProperties"), false}}},
        [&adapter, name](const QJsonObject &args, CallContext &call) { return adapter.ResourceTool(name, args, call); });
  }
}
}
