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

#include "MCPTools.h"
#include "MCPResourceTools.h"
#include "MCPCompatibilityTools.h"
#include "MCPComparisonTools.h"
#include "MCPDiagnostics.h"
#include "MCPExternalTools.h"
#include "MCPSourceMapping.h"

#include <QByteArray>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QStringList>
#include <string.h>
#include "renderdoc_replay.h"
#include "Code/AI/MCPAnalysisModel.h"

// Full ICaptureContext is needed so the sensitive-data gate can read the live
// MCP_AllowSensitiveData setting through the capture context. MCPQueryAdapter.h
// only forward declares it, so pull in the real interface here.
#include "Code/Interface/QRDInterface.h"

// NOTE: this translation unit deliberately does NOT include "renderdoc_tostr.inl".
// That file defines non-inline template<> ToStr<> specialisations and is already
// included by exactly one qrenderdoc TU (MCPQueryAdapter.cpp). Including it again
// would emit duplicate symbols at link time. All enum -> name formatting goes
// through MCP::detail::*Name(), declared in MCPTools.h and implemented in
// MCPQueryAdapter.cpp.

namespace MCP
{

QJsonObject ResourceRef(ResourceId id)
{
  uint64_t value = 0;
  memcpy(&value, &id, sizeof(value));
  return QJsonObject{{QStringLiteral("id"), double(value)},
                     {QStringLiteral("name"), ResourceIdToString(id)},
                     {QStringLiteral("exactId"), QString::number(qulonglong(value))}};
}

namespace
{
////////////////////////////////////////////////////////////////////////////////
// Small helpers
////////////////////////////////////////////////////////////////////////////////

QString ArgStr(const QJsonObject &args, const char *key, const QString &dflt = QString())
{
  const QJsonValue v = args.value(QString::fromUtf8(key));
  if(v.isString())
    return v.toString();
  return dflt;
}

qint64 ArgU64(const QJsonObject &args, const char *key, qint64 dflt = 0)
{
  const QJsonValue v = args.value(QString::fromUtf8(key));
  if(v.isDouble())
  {
    const double d = v.toDouble();
    if(d < 0.0)
      return dflt;
    return qint64(d);
  }
  if(v.isString())
  {
    bool ok = false;
    const qint64 r = v.toString().toLongLong(&ok);
    if(ok && r >= 0)
      return r;
  }
  return dflt;
}

uint32_t ArgU32(const QJsonObject &args, const char *key, uint32_t dflt = 0)
{
  const qint64 v = ArgU64(args, key, qint64(dflt));
  if(v < 0 || v > qint64(0xffffffff))
    return dflt;
  return uint32_t(v);
}

QStringList ArgFields(const QJsonObject &args)
{
  QStringList out;
  const QJsonValue v = args.value(QStringLiteral("fields"));
  if(!v.isArray())
    return out;
  for(const QJsonValue &e : v.toArray())
  {
    if(e.isString())
      out.push_back(e.toString());
  }
  return out;
}

// Boolean arguments must be explicit: a missing key or a non-boolean value
// (e.g. the string "true") falls back to `dflt` rather than being coerced, so
// the JSON Schema validation and the handler agree on what was requested.
bool ArgBool(const QJsonObject &args, const char *key, bool dflt)
{
  const QJsonValue v = args.value(QString::fromUtf8(key));
  if(v.isBool())
    return v.toBool();
  return dflt;
}

// Resource ids are opaque 8-byte values (resourceid.h keeps the payload private),
// so the numeric value is recovered with memcpy - exactly what the native UI does
// when it renders a resource id.
uint64_t ResIdValue(ResourceId id)
{
  uint64_t v = 0;
  memcpy(&v, &id, sizeof(uint64_t));
  return v;
}

ResourceId ResIdFromU64(uint64_t v)
{
  ResourceId id;
  memcpy(&id, &v, sizeof(uint64_t));
  return id;
}

ResourceId ArgResourceId(const QJsonObject &args, const char *key)
{
  const qint64 v = ArgU64(args, key, 0);
  if(v == 0)
    return ResourceId();
  return ResIdFromU64(uint64_t(v));
}

QJsonValue JsonNum(double d) { return QJsonValue(d); }
QJsonValue JsonNum(float f) { return QJsonValue(double(f)); }

QJsonObject MakeError(ErrorCode code, const QString &message, const QJsonObject &data = QJsonObject())
{
  return Error(code, message, data).toJson();
}

ToolResult FailWithCapture(ErrorCode code, const char *message)
{
  return ToolResult::Failure(Error(code, message));
}

// Sensitive-data boundary (P0-8). Tools that hand out bulk capture contents -
// raw buffer/texture bytes, decoded mesh vertex data, or shader reflection blobs -
// are gated behind Config().MCP_AllowSensitiveData. The setting is read live from
// the capture context on every call, so flipping it in the settings dialog takes
// effect immediately without restarting the server. This keeps the "no
// exfiltration by a local client" guarantee the config documents.
bool SensitiveAllowed(CallContext &call)
{
  return call.ctx != NULL && call.ctx->Config().MCP_AllowSensitiveData;
}

ToolResult SensitiveDenied(const char *toolName)
{
  QJsonObject data;
  data[QStringLiteral("setting")] = QStringLiteral("MCP_AllowSensitiveData");
  data[QStringLiteral("tool")] = QString::fromUtf8(toolName);
  return ToolResult::Failure(
      Error(ErrorCode::Unsupported,
            QObject::tr("This tool returns bulk capture contents, which are disabled by default. "
                        "Enable 'MCP_AllowSensitiveData' in qrenderdoc's settings to allow it."),
            data));
}

// Adds the current capture context to a successful payload so a client can always
// tell which capture (and which event) the result refers to.
ToolResult WithCapture(QueryAdapter &adapter, const ToolResult &r)
{
  if(!r.ok)
    return r;

  QJsonObject out = r.structured;
  out.insert(QStringLiteral("capture"), QueryAdapter::SnapshotToJson(adapter.Snapshot()));
  return ToolResult::Success(out, QString());
}

const int kMaxListDefault = 256;
const int kMaxListHard = 4096;
const uint32_t kMaxVertexHard = 4u * 1024u * 1024u;
const uint64_t kMaxBufferBytes = 65536;

int ClampList(int requested)
{
  if(requested <= 0)
    return kMaxListDefault;
  return requested > kMaxListHard ? kMaxListHard : requested;
}

////////////////////////////////////////////////////////////////////////////////
// Structured-data (API inspector) helpers
////////////////////////////////////////////////////////////////////////////////

QString SdLeafToString(const SDObject *o)
{
  if(o == NULL)
    return QStringLiteral("null");

  if(o->IsNULL())
    return QStringLiteral("NULL");

  switch(o->type.basetype)
  {
    case SDBasic::String: return QStringLiteral("\"") + QString::fromUtf8(o->AsString().c_str()) + QStringLiteral("\"");
    case SDBasic::UnsignedInteger: return QString::number(o->AsUInt64());
    case SDBasic::SignedInteger: return QString::number(o->AsInt64());
    case SDBasic::Float: return QString::number(o->AsDouble());
    case SDBasic::Boolean: return o->AsBool() ? QStringLiteral("true") : QStringLiteral("false");
    case SDBasic::Character:
      return QStringLiteral("'") + QChar(ushort(o->AsUInt64() & 0xff)) + QStringLiteral("'");
    case SDBasic::Enum:
      // Enum values are stored stringized, matching the API Inspector.
      return QString::fromUtf8(o->AsString().c_str());
    case SDBasic::Resource: return ResourceIdToString(o->AsResourceId());
    case SDBasic::Array: return QStringLiteral("[") + QString::number(qulonglong(o->NumChildren())) + QStringLiteral("]");
    case SDBasic::Struct: return QStringLiteral("{") + QString::number(qulonglong(o->NumChildren())) + QStringLiteral("}");
    case SDBasic::Buffer: return QStringLiteral("<buffer>");
    default: break;
  }
  return QStringLiteral("?");
}

// Compact "name=value, name=value" preview of a chunk's top-level parameters.
QString ChunkParamsSummary(const SDObject &chunk)
{
  const size_t n = chunk.NumChildren();
  const size_t maxShow = 6;

  QString out;
  for(size_t i = 0; i < n && i < maxShow; i++)
  {
    const SDObject *c = chunk.GetChild(i);
    if(c == NULL)
      continue;

    if(!out.isEmpty())
      out += QStringLiteral(", ");

    const char *nm = c->name.c_str();
    if(nm != NULL && *nm != 0)
    {
      out += QString::fromUtf8(nm);
      out += QStringLiteral("=");
    }
    out += SdLeafToString(c);
  }

  if(n > maxShow)
    out += QStringLiteral(", ...");

  return out;
}

////////////////////////////////////////////////////////////////////////////////
// Vertex format decoding
////////////////////////////////////////////////////////////////////////////////

double DecodeComponent(const byte *p, const ResourceFormat &f)
{
  const uint8_t w = f.compByteWidth;

  switch(f.compType)
  {
    case CompType::Float:
    {
      if(w == 2)
      {
        uint16_t h = 0;
        memcpy(&h, p, 2);
        // rdhalf::storage is private - go through the public set()/operator float().
        rdhalf hv;
        hv.set(h);
        return double(float(hv));
      }
      if(w == 4)
      {
        float v = 0.0f;
        memcpy(&v, p, 4);
        return double(v);
      }
      if(w == 8)
      {
        double v = 0.0;
        memcpy(&v, p, 8);
        return v;
      }
      break;
    }
    case CompType::Depth:
    {
      if(w == 4)
      {
        float v = 0.0f;
        memcpy(&v, p, 4);
        return double(v);
      }
      break;
    }
    case CompType::UNorm:
    {
      if(w == 1)
        return double(p[0]) / 255.0;
      if(w == 2)
      {
        uint16_t v = 0;
        memcpy(&v, p, 2);
        return double(v) / 65535.0;
      }
      if(w == 4)
      {
        uint32_t v = 0;
        memcpy(&v, p, 4);
        return double(v) / 4294967295.0;
      }
      break;
    }
    case CompType::SNorm:
    {
      if(w == 1)
      {
        int8_t v = 0;
        memcpy(&v, p, 1);
        return double(qMax(-1.0, double(v) / 127.0));
      }
      if(w == 2)
      {
        int16_t v = 0;
        memcpy(&v, p, 2);
        return double(qMax(-1.0, double(v) / 32767.0));
      }
      if(w == 4)
      {
        int32_t v = 0;
        memcpy(&v, p, 4);
        return double(qMax(-1.0, double(v) / 2147483647.0));
      }
      break;
    }
    case CompType::UInt:
    case CompType::UScaled:
    {
      if(w == 1)
        return double(p[0]);
      if(w == 2)
      {
        uint16_t v = 0;
        memcpy(&v, p, 2);
        return double(v);
      }
      if(w == 4)
      {
        uint32_t v = 0;
        memcpy(&v, p, 4);
        return double(v);
      }
      if(w == 8)
      {
        uint64_t v = 0;
        memcpy(&v, p, 8);
        return double(v);
      }
      break;
    }
    case CompType::SInt:
    case CompType::SScaled:
    {
      if(w == 1)
      {
        int8_t v = 0;
        memcpy(&v, p, 1);
        return double(v);
      }
      if(w == 2)
      {
        int16_t v = 0;
        memcpy(&v, p, 2);
        return double(v);
      }
      if(w == 4)
      {
        int32_t v = 0;
        memcpy(&v, p, 4);
        return double(v);
      }
      if(w == 8)
      {
        int64_t v = 0;
        memcpy(&v, p, 8);
        return double(v);
      }
      break;
    }
    default: break;
  }

  return 0.0;
}

QJsonObject FormatToJson(const ResourceFormat &f)
{
  QJsonObject o;
  o[QStringLiteral("compType")] = int(uint32_t(f.compType));
  o[QStringLiteral("compCount")] = int(f.compCount);
  o[QStringLiteral("compByteWidth")] = int(f.compByteWidth);
  o[QStringLiteral("type")] = int(uint32_t(f.type));
  return o;
}

////////////////////////////////////////////////////////////////////////////////
// Pipeline state serialisation
////////////////////////////////////////////////////////////////////////////////

QJsonObject ViewportToJson(const Viewport &v)
{
  QJsonObject o;
  o[QStringLiteral("enabled")] = v.enabled;
  o[QStringLiteral("x")] = JsonNum(v.x);
  o[QStringLiteral("y")] = JsonNum(v.y);
  o[QStringLiteral("width")] = JsonNum(v.width);
  o[QStringLiteral("height")] = JsonNum(v.height);
  o[QStringLiteral("minDepth")] = JsonNum(v.minDepth);
  o[QStringLiteral("maxDepth")] = JsonNum(v.maxDepth);
  return o;
}

QJsonObject ScissorToJson(const Scissor &s)
{
  QJsonObject o;
  o[QStringLiteral("enabled")] = s.enabled;
  o[QStringLiteral("x")] = int(s.x);
  o[QStringLiteral("y")] = int(s.y);
  o[QStringLiteral("width")] = int(s.width);
  o[QStringLiteral("height")] = int(s.height);
  return o;
}

QJsonObject StencilFaceToJson(const StencilFace &f)
{
  QJsonObject o;
  o[QStringLiteral("failOperation")] = detail::StencilOperationName(f.failOperation);
  o[QStringLiteral("depthFailOperation")] = detail::StencilOperationName(f.depthFailOperation);
  o[QStringLiteral("passOperation")] = detail::StencilOperationName(f.passOperation);
  o[QStringLiteral("function")] = detail::CompareFunctionName(f.function);
  o[QStringLiteral("reference")] = double(f.reference);
  o[QStringLiteral("compareMask")] = double(f.compareMask);
  o[QStringLiteral("writeMask")] = double(f.writeMask);
  return o;
}

QJsonObject ColorBlendToJson(const ColorBlend &b)
{
  QJsonObject o;
  o[QStringLiteral("enabled")] = b.enabled;
  o[QStringLiteral("logicOperationEnabled")] = b.logicOperationEnabled;
  o[QStringLiteral("logicOperation")] = detail::LogicOperationName(b.logicOperation);
  o[QStringLiteral("writeMask")] = int(b.writeMask);

  QJsonObject cb;
  cb[QStringLiteral("source")] = detail::BlendMultiplierName(b.colorBlend.source);
  cb[QStringLiteral("destination")] = detail::BlendMultiplierName(b.colorBlend.destination);
  cb[QStringLiteral("operation")] = detail::BlendOperationName(b.colorBlend.operation);
  o[QStringLiteral("colorBlend")] = cb;

  QJsonObject ab;
  ab[QStringLiteral("source")] = detail::BlendMultiplierName(b.alphaBlend.source);
  ab[QStringLiteral("destination")] = detail::BlendMultiplierName(b.alphaBlend.destination);
  ab[QStringLiteral("operation")] = detail::BlendOperationName(b.alphaBlend.operation);
  o[QStringLiteral("alphaBlend")] = ab;

  return o;
}

QJsonObject DescriptorToJson(const Descriptor &d)
{
  QJsonObject o;
  o[QStringLiteral("type")] = int(uint32_t(d.type));
  o[QStringLiteral("resource")] = ResourceRef(d.resource);
  if(d.secondary != ResourceId())
    o[QStringLiteral("secondary")] = ResourceRef(d.secondary);
  o[QStringLiteral("format")] = FormatToJson(d.format);
  o[QStringLiteral("byteOffset")] = double(d.byteOffset);
  o[QStringLiteral("byteSize")] = double(d.byteSize);
  o[QStringLiteral("elementByteSize")] = double(d.elementByteSize);
  o[QStringLiteral("firstSlice")] = double(d.firstSlice);
  o[QStringLiteral("numSlices")] = double(d.numSlices);
  o[QStringLiteral("firstMip")] = double(d.firstMip);
  o[QStringLiteral("numMips")] = double(d.numMips);
  return o;
}

QJsonObject BoundVBufferToJson(const BoundVBuffer &vb)
{
  QJsonObject o;
  o[QStringLiteral("resource")] = ResourceRef(vb.resourceId);
  o[QStringLiteral("byteOffset")] = double(vb.byteOffset);
  o[QStringLiteral("byteStride")] = double(vb.byteStride);
  o[QStringLiteral("byteSize")] = double(vb.byteSize);
  return o;
}

QJsonObject VertexInputToJson(const VertexInputAttribute &a)
{
  QJsonObject o;
  o[QStringLiteral("name")] = QString::fromUtf8(a.name.c_str());
  o[QStringLiteral("vertexBuffer")] = int(a.vertexBuffer);
  o[QStringLiteral("byteOffset")] = double(a.byteOffset);
  o[QStringLiteral("perInstance")] = a.perInstance;
  o[QStringLiteral("instanceRate")] = int(a.instanceRate);
  o[QStringLiteral("format")] = FormatToJson(a.format);
  o[QStringLiteral("genericEnabled")] = a.genericEnabled;
  return o;
}

// Serialises the state that is available through the API-agnostic PipeState. The
// API-specific structures (D3D11Pipe::State etc.) are intentionally not touched
// here: PipeState already resolves them, so one code path covers every API.
QJsonObject PipelineStateToJsonImpl(IReplayController &replay)
{
  QJsonObject out;
  const PipeState &pipe = replay.GetPipelineState();

  if(!pipe.IsCaptureLoaded())
  {
    out[QStringLiteral("loaded")] = false;
    return out;
  }

  out[QStringLiteral("loaded")] = true;
  out[QStringLiteral("multiviewBroadcastCount")] = double(pipe.MultiviewBroadcastCount());
  out[QStringLiteral("supportsBarriers")] = pipe.SupportsBarriers();
  out[QStringLiteral("rasterizedStream")] = int(pipe.GetRasterizedStream());
  out[QStringLiteral("topology")] = detail::TopologyName(pipe.GetPrimitiveTopology());

  // Viewports/scissors are a contiguous enabled prefix in every API we support, so
  // we probe a bounded number and stop at the first disabled entry.
  {
    QJsonArray vps;
    for(uint32_t i = 0; i < 16; i++)
    {
      const Viewport vp = pipe.GetViewport(i);
      if(!vp.enabled && i > 0)
        break;
      vps.push_back(ViewportToJson(vp));
      if(!vp.enabled)
        break;
    }
    out[QStringLiteral("viewports")] = vps;
  }

  {
    QJsonArray scs;
    for(uint32_t i = 0; i < 16; i++)
    {
      const Scissor sc = pipe.GetScissor(i);
      if(!sc.enabled && i > 0)
        break;
      scs.push_back(ScissorToJson(sc));
      if(!sc.enabled)
        break;
    }
    out[QStringLiteral("scissors")] = scs;
  }

  {
    const RasterState rs = pipe.GetRasterState();
    QJsonObject r;
    r[QStringLiteral("frontCCW")] = rs.frontCCW;
    r[QStringLiteral("fillMode")] = detail::FillModeName(rs.fillMode);
    r[QStringLiteral("cullMode")] = detail::CullModeName(rs.cullMode);
    out[QStringLiteral("rasterizer")] = r;
  }

  {
    const DepthTestState ds = pipe.GetDepthTestState();
    QJsonObject d;
    d[QStringLiteral("depthEnable")] = ds.depthEnable;
    d[QStringLiteral("depthFunction")] = detail::CompareFunctionName(ds.depthFunction);
    d[QStringLiteral("depthWrites")] = ds.depthWrites;
    d[QStringLiteral("depthBounds")] = ds.depthBounds;
    d[QStringLiteral("minDepthBounds")] = double(ds.minDepthBounds);
    d[QStringLiteral("maxDepthBounds")] = double(ds.maxDepthBounds);
    out[QStringLiteral("depthTest")] = d;
  }

  {
    const rdcpair<StencilFace, StencilFace> faces = pipe.GetStencilFaces();
    QJsonObject s;
    s[QStringLiteral("front")] = StencilFaceToJson(faces.first);
    s[QStringLiteral("back")] = StencilFaceToJson(faces.second);
    out[QStringLiteral("stencil")] = s;
  }

  {
    QJsonArray blends;
    for(const ColorBlend &b : pipe.GetColorBlends())
      blends.push_back(ColorBlendToJson(b));
    out[QStringLiteral("colorBlends")] = blends;

    const rdcfixedarray<float, 4> bf = pipe.GetBlendFactor();
    QJsonArray factor;
    for(int i = 0; i < 4; i++)
      factor.push_back(JsonNum(bf[i]));
    out[QStringLiteral("blendFactor")] = factor;
  }

  {
    QJsonArray targets;
    for(const Descriptor &d : pipe.GetOutputTargets())
      targets.push_back(DescriptorToJson(d));
    out[QStringLiteral("outputTargets")] = targets;
    out[QStringLiteral("depthTarget")] = DescriptorToJson(pipe.GetDepthTarget());
  }

  {
    QJsonArray vbs;
    for(const BoundVBuffer &vb : pipe.GetVBuffers())
      vbs.push_back(BoundVBufferToJson(vb));
    out[QStringLiteral("vertexBuffers")] = vbs;
    out[QStringLiteral("indexBuffer")] = BoundVBufferToJson(pipe.GetIBuffer());

    QJsonArray inputs;
    for(const VertexInputAttribute &a : pipe.GetVertexInputs())
      inputs.push_back(VertexInputToJson(a));
    out[QStringLiteral("vertexInputs")] = inputs;
  }

  {
    QJsonObject shaders;
    for(int i = 0; i < int(ShaderStage::Count); i++)
    {
      const ShaderStage stage = ShaderStage(i);
      const ResourceId shader = pipe.GetShader(stage);
      if(shader == ResourceId())
        continue;

      QJsonObject s;
      s[QStringLiteral("shader")] = ResourceRef(shader);
      s[QStringLiteral("entryPoint")] = QString::fromUtf8(pipe.GetShaderEntryPoint(stage).c_str());

      const ShaderReflection *refl = pipe.GetShaderReflection(stage);
      if(refl != NULL)
      {
        QJsonArray cbuffers;
        for(const ConstantBlock &cb : refl->constantBlocks)
        {
          QJsonObject c;
          c[QStringLiteral("name")] = QString::fromUtf8(cb.name.c_str());
          c[QStringLiteral("byteSize")] = double(cb.byteSize);
          c[QStringLiteral("bindPoint")] = double(cb.fixedBindNumber);
          c[QStringLiteral("bindSet")] = double(cb.fixedBindSetOrSpace);
          c[QStringLiteral("bindArraySize")] = double(cb.bindArraySize);
          c[QStringLiteral("variableCount")] = double(cb.variables.size());
          c[QStringLiteral("bufferBacked")] = cb.bufferBacked;
          cbuffers.push_back(c);
        }
        s[QStringLiteral("constantBlocks")] = cbuffers;

        QJsonArray ro;
        for(const ShaderResource &r : refl->readOnlyResources)
        {
          QJsonObject e;
          e[QStringLiteral("name")] = QString::fromUtf8(r.name.c_str());
          e[QStringLiteral("textureType")] = detail::TextureTypeName(r.textureType);
          e[QStringLiteral("bindPoint")] = double(r.fixedBindNumber);
          e[QStringLiteral("bindSet")] = double(r.fixedBindSetOrSpace);
          ro.push_back(e);
        }
        s[QStringLiteral("readOnlyResources")] = ro;

        QJsonArray rw;
        for(const ShaderResource &r : refl->readWriteResources)
        {
          QJsonObject e;
          e[QStringLiteral("name")] = QString::fromUtf8(r.name.c_str());
          e[QStringLiteral("textureType")] = detail::TextureTypeName(r.textureType);
          e[QStringLiteral("bindPoint")] = double(r.fixedBindNumber);
          e[QStringLiteral("bindSet")] = double(r.fixedBindSetOrSpace);
          rw.push_back(e);
        }
        s[QStringLiteral("readWriteResources")] = rw;

        QJsonArray samplers;
        for(const ShaderSampler &sm : refl->samplers)
        {
          QJsonObject e;
          e[QStringLiteral("name")] = QString::fromUtf8(sm.name.c_str());
          e[QStringLiteral("bindPoint")] = double(sm.fixedBindNumber);
          e[QStringLiteral("bindSet")] = double(sm.fixedBindSetOrSpace);
          samplers.push_back(e);
        }
        s[QStringLiteral("samplers")] = samplers;
      }

      shaders[detail::ShaderStageName(stage)] = s;
    }
    out[QStringLiteral("shaders")] = shaders;
  }

  return out;
}

////////////////////////////////////////////////////////////////////////////////
// Post-VS / input-assembly mesh data
////////////////////////////////////////////////////////////////////////////////

MeshDataStage ParseMeshStage(const QString &s)
{
  // Accepts both the snake_case spelling clients tend to send and the canonical
  // names, mirroring the extension's parseMeshStage().
  const QString v = s.trimmed().toLower();
  if(v == QStringLiteral("vsout") || v == QStringLiteral("vs_out"))
    return MeshDataStage::VSOut;
  if(v == QStringLiteral("gsout") || v == QStringLiteral("gs_out"))
    return MeshDataStage::GSOut;
  if(v == QStringLiteral("taskout") || v == QStringLiteral("ampout"))
    return MeshDataStage::TaskOut;
  if(v == QStringLiteral("meshout"))
    return MeshDataStage::MeshOut;
  return MeshDataStage::VSIn;
}

QString MeshStageName(MeshDataStage s)
{
  switch(s)
  {
    case MeshDataStage::VSIn: return QStringLiteral("vsin");
    case MeshDataStage::VSOut: return QStringLiteral("vsout");
    case MeshDataStage::GSOut: return QStringLiteral("gsout");
    case MeshDataStage::TaskOut: return QStringLiteral("taskout");
    case MeshDataStage::MeshOut: return QStringLiteral("meshout");
    default: break;
  }
  return QStringLiteral("unknown");
}

QJsonObject MeshFormatToJson(const MeshFormat &mf)
{
  QJsonObject o;
  o[QStringLiteral("topology")] = detail::TopologyName(mf.topology);
  o[QStringLiteral("numIndices")] = double(mf.numIndices);
  o[QStringLiteral("baseVertex")] = int(mf.baseVertex);
  o[QStringLiteral("instanced")] = mf.instanced;
  o[QStringLiteral("instStepRate")] = double(mf.instStepRate);
  o[QStringLiteral("restartIndex")] = double(mf.restartIndex);
  o[QStringLiteral("allowRestart")] = mf.allowRestart;
  o[QStringLiteral("status")] = QString::fromUtf8(mf.status.c_str());
  o[QStringLiteral("indexBuffer")] = ResourceRef(mf.indexResourceId);
  o[QStringLiteral("indexByteOffset")] = double(mf.indexByteOffset);
  o[QStringLiteral("indexByteStride")] = double(mf.indexByteStride);
  o[QStringLiteral("vertexBuffer")] = ResourceRef(mf.vertexResourceId);
  o[QStringLiteral("vertexByteOffset")] = double(mf.vertexByteOffset);
  o[QStringLiteral("vertexByteStride")] = double(mf.vertexByteStride);
  o[QStringLiteral("format")] = FormatToJson(mf.format);
  return o;
}

// Reads one index out of a raw index buffer. byteStride must be 1, 2 or 4.
uint32_t ReadIndexValue(const bytebuf &idxBuf, uint32_t byteStride, uint64_t i)
{
  const uint64_t off = i * uint64_t(byteStride);
  if(off + uint64_t(byteStride) > uint64_t(idxBuf.size()))
    return 0;

  const uint8_t *p = idxBuf.data() + off;
  if(byteStride == 1)
    return uint32_t(p[0]);
  if(byteStride == 2)
  {
    uint16_t v = 0;
    memcpy(&v, p, 2);
    return uint32_t(v);
  }
  if(byteStride == 4)
  {
    uint32_t v = 0;
    memcpy(&v, p, 4);
    return v;
  }
  return 0;
}

// Depth-first search of the action tree for the action with a given event id.
// The public IReplayController interface exposes the root actions but no
// arbitrary-EID lookup, so we mirror the search the native replay proxy uses.
const ActionDescription *FindActionByEID(const rdcarray<ActionDescription> &actions, uint32_t eid)
{
  for(const ActionDescription &a : actions)
  {
    if(a.eventId == eid)
      return &a;

    const ActionDescription *found = FindActionByEID(a.children, eid);
    if(found != NULL)
      return found;
  }

  return NULL;
}

}    // namespace

QJsonObject PipelineStateToJson(IReplayController &replay)
{
  return PipelineStateToJsonImpl(replay);
}

////////////////////////////////////////////////////////////////////////////////
// Registration
////////////////////////////////////////////////////////////////////////////////

static void AddSchemaProperty(QJsonObject &props, const char *name, const char *type,
                              const char *description)
{
  QJsonObject p;
  p[QStringLiteral("type")] = QString::fromUtf8(type);
  if(description != NULL)
    p[QStringLiteral("description")] = QString::fromUtf8(description);
  props[QString::fromUtf8(name)] = p;
}

static QJsonObject MakeSchema(const QJsonObject &properties)
{
  QJsonObject schema;
  schema[QStringLiteral("type")] = QStringLiteral("object");
  schema[QStringLiteral("properties")] = properties;
  schema[QStringLiteral("additionalProperties")] = true;
  return schema;
}

int RegisterCoreTools(ToolRegistry &registry, QueryAdapter &adapter)
{
  // ---------------------------------------------------------------------------
  // Session / selection / status
  // ---------------------------------------------------------------------------

  {
    QJsonObject props;
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getCaptureInfo"), QStringLiteral("Get Capture Info"),
                       QStringLiteral("Return the details of the capture currently open in qrenderdoc: "
                                      "filename, graphics API, frame number, resource/action counts, "
                                      "replay availability and any UI modifications."),
                       ToolEffects::ReadOnly, ToolRequirements::None, MakeSchema(props)},
        [&adapter](const QJsonObject &, CallContext &) -> ToolResult {
          return WithCapture(adapter, adapter.ApiProperties());
        });
  }

  {
    QJsonObject props;

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getReplayStatus"), QStringLiteral("Get Replay Status"),
                       QStringLiteral("Report whether a capture is loaded and whether the replay "
                                      "controller is currently usable, including the remote host if "
                                      "replay is running remotely."),
                       ToolEffects::ReadOnly, ToolRequirements::None, MakeSchema(props)},
        [&adapter](const QJsonObject &, CallContext &) -> ToolResult {
          return WithCapture(adapter, adapter.ApiProperties());
        });
  }

  {
    QJsonObject props;

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getSelectionContext"),
                       QStringLiteral("Get Selection Context"),
                       QStringLiteral("Return the currently selected event and the events adjacent to "
                                      "it, so a client can resolve 'the current draw call' or "
                                      "'the current event' without extra round trips."),
                       ToolEffects::ReadOnly, ToolRequirements::None, MakeSchema(props)},
        [&adapter](const QJsonObject &, CallContext &) -> ToolResult {
          return WithCapture(adapter, adapter.EventInfo(0));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "fields", "array",
                      "Optional list of top-level keys to return: topLevelPasses, leafCounts, "
                      "statistics, counts.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getFrameSummary"), QStringLiteral("Get Frame Summary"),
                       QStringLiteral("Return a native frame summary: the top-level pass list "
                                      "(eventId/name/flags/childCount per root action), leaf-only "
                                      "effect counts (draws/dispatches/clears) and the frame "
                                      "statistics. Snapshot-based, so it works even when replay is "
                                      "unavailable."),
                       ToolEffects::ReadOnly, ToolRequirements::LoadedCapture, MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &) -> ToolResult {
          return adapter.FrameSummary(ArgFields(args));
        });
  }

  // ---------------------------------------------------------------------------
  // Frame analysis (P1-2)
  // ---------------------------------------------------------------------------

  {
    QJsonObject props;
    AddSchemaProperty(props, "driver", "string",
                      "Optional driver label to stamp on the report (APIProperties does not "
                      "expose it).");
    AddSchemaProperty(props, "rdocVersion", "string",
                      "Optional RenderDoc version label; defaults to this build's version.");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_analyzeFrame"),
                       QStringLiteral("Analyze Frame"),
                       QStringLiteral("Run a whole-frame analysis: API/driver labels, frame "
                                      "statistics, whole-tree effect counts, resource inventory "
                                      "(textures, buffers, shaders, large textures and their "
                                      "memory) and heuristic potential issues, each tagged with "
                                      "evidence and a confidence. Snapshot-based, so it does not "
                                      "need the replay thread."),
                       ToolEffects::ReadOnly, ToolRequirements::LoadedCapture, MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &) -> ToolResult {
          FrameAnalysisOptions options;
          options.driver = ArgStr(args, "driver");
          options.rdocVersion = ArgStr(args, "rdocVersion");
          return adapter.AnalyzeFrame(options, ArgFields(args));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "top", "integer",
                      "Number of hottest events to return (default 10).");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getHotEvents"), QStringLiteral("Get Hot Events"),
                       QStringLiteral("Rank the frame's events by GPU duration (the "
                                      "EventGPUDuration counter) and return the hottest events "
                                      "with their per-frame share and timing evidence. The result "
                                      "is marked unsupported rather than zero-filled when the "
                                      "capture API does not expose the counter. Requires replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          const uint32_t top = ArgU32(args, "top", 10);
          return adapter.HotEvents(top, call, ArgFields(args));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "eventId", "integer", "The event id to analyse.");
    AddSchemaProperty(props, "includeShaderInfo", "boolean",
                      "Include the per-stage shader reflection summary (default false).");
    AddSchemaProperty(props, "includeMeshData", "boolean",
                      "Include the mesh/post-VS data summary (default false).");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_analyzeHotEvent"),
                       QStringLiteral("Analyze Hot Event"),
                       QStringLiteral("Multi-evidence analysis of one event: the event itself, its "
                                      "GPU duration, the pipeline state (shaders and render/depth "
                                      "targets), resource counts, and optionally the per-stage "
                                      "shader info and mesh data. Every conclusion carries an "
                                      "evidence reference and a confidence so clients can separate "
                                      "facts from inference. Requires replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          const uint32_t eventId = ArgU32(args, "eventId");
          const bool shaderInfo = args.value(QStringLiteral("includeShaderInfo")).toBool(false);
          const bool meshData = args.value(QStringLiteral("includeMeshData")).toBool(false);
          return adapter.AnalyzeHotEvent(eventId, shaderInfo, meshData, call, ArgFields(args));
        });
  }

  // ---------------------------------------------------------------------------
  // Pass graph and reverse search (P1-3)
  // ---------------------------------------------------------------------------

  {
    QJsonObject props;
    AddSchemaProperty(props, "includeResources", "boolean",
                      "Collect each pass's render target and sampled texture resources "
                      "(default true).");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getPassGraph"),
                       QStringLiteral("Get Pass Graph"),
                       QStringLiteral("Build the frame's pass graph: one pass per top-level marker "
                                      "with its event range, draw count, duration and (optionally) "
                                      "its render target and sampled texture resources, plus the "
                                      "edges where a later pass samples a texture an earlier pass "
                                      "rendered. Requires replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          const bool includeResources = args.value(QStringLiteral("includeResources")).toBool(true);
          return adapter.GetPassGraph(includeResources, call, ArgFields(args));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "shaderName", "string",
                      "Case-insensitive substring of a shader entry point name or a shader "
                      "resource display name to match.");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_findDrawsByShader"),
                       QStringLiteral("Find Draws By Shader"),
                       QStringLiteral("Reverse-search the frame for every draw/dispatch that uses a "
                                      "shader whose entry point name or resource name contains the "
                                      "query (case-insensitive). Returns the matching event ids in "
                                      "ascending order. Requires replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          const QString term = ArgStr(args, "shaderName");
          if(term.isEmpty())
            return FailWithCapture(ErrorCode::InvalidToolInput,
                                   "shaderName is required and must be a non-empty string.");
          return adapter.FindDrawsByShader(term, call, ArgFields(args));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "textureName", "string",
                      "Case-insensitive substring of a texture/buffer resource name to match.");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_findDrawsByTexture"),
                       QStringLiteral("Find Draws By Texture"),
                       QStringLiteral("Reverse-search the frame for every draw/dispatch that samples "
                                      "a texture or buffer whose resource name contains the query "
                                      "(case-insensitive). Returns the matching event ids in "
                                      "ascending order. Requires replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          const QString term = ArgStr(args, "textureName");
          if(term.isEmpty())
            return FailWithCapture(ErrorCode::InvalidToolInput,
                                   "textureName is required and must be a non-empty string.");
          return adapter.FindDrawsByTexture(term, call, ArgFields(args));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "resourceId", "string",
                      "The resource id (as produced by the resource tools) to match exactly.");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_findDrawsByResourceId"),
                       QStringLiteral("Find Draws By Resource Id"),
                       QStringLiteral("Reverse-search the frame for every draw/dispatch that binds "
                                      "the given resource id, reporting whether it was used as a "
                                      "render target or sampled. Returns the matching event ids in "
                                      "ascending order. Requires replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          const ResourceId id = ArgResourceId(args, "resourceId");
          if(ResIdValue(id) == 0)
            return FailWithCapture(ErrorCode::InvalidToolInput,
                                   "resourceId is required and must be a valid resource id.");
          return adapter.FindDrawsByResourceId(id, call, ArgFields(args));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "resourceId", "string",
                      "The resource id to trace through the frame.");
    QJsonObject resourceId = props.value(QStringLiteral("resourceId")).toObject();
    resourceId[QStringLiteral("type")] = QJsonArray{QStringLiteral("string"), QStringLiteral("integer")};
    props[QStringLiteral("resourceId")] = resourceId;
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");
    AddSchemaProperty(props, "maxEvents", "integer", "Maximum returned events per category (maximum 500); omission preserves the full native result. Scan coverage is unchanged.");
    props[QStringLiteral("maxEvents")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
        {QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 500}};

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_traceResourceUsage"),
                       QStringLiteral("Trace Resource Usage"),
                       QStringLiteral("Trace one resource through the frame: the events that "
                                      "produce it (render/write) and the events that consume it "
                                      "(sample/read), each with usage and confidence evidence. "
                                      "Requires replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          ResourceId id;
          ToolResult parsed = ParseResourceSelector(args, id, true);
          if(!parsed.ok) return parsed;
          int64_t maxEvents = 0;
          if(!ReadResourceInteger(args, "maxEvents", 1, 500, maxEvents))
            return FailWithCapture(ErrorCode::InvalidParams, "Invalid maxEvents");
          return adapter.TraceResourceUsage(id, call, ArgFields(args), int(maxEvents));
        });
  }

  // ---------------------------------------------------------------------------
  // Resource memory audit, lifetimes, unused resources and leak heuristics (P1-4)
  // ---------------------------------------------------------------------------

  {
    QJsonObject props;
    AddSchemaProperty(props, "limit", "number",
                      "Maximum number of largest resources to return (default 64).");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_resourceMemoryAudit"),
                       QStringLiteral("Resource Memory Audit"),
                       QStringLiteral("Audit creation-time resource memory: total tracked bytes, "
                                      "texture and buffer subtotals, the largest resources and a "
                                      "texture format/mip histogram. Facts are Confirmed; the "
                                      "notes list heuristic observations."),
                       ToolEffects::ReadOnly, ToolRequirements::LoadedCapture, MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          return adapter.ResourceMemoryAudit((int)ArgU32(args, "limit", 64), call, ArgFields(args));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "limit", "number",
                      "Maximum number of resource rows to return (default 100).");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_resourceLifetime"),
                       QStringLiteral("Resource Lifetime"),
                       QStringLiteral("Report the observed lifetime of each resource in the frame: "
                                      "the events that produced/consumed it, the first and last "
                                      "such event and the touch count. Requires replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          return adapter.ResourceLifetime((int)ArgU32(args, "limit", 100), call, ArgFields(args));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "limit", "number",
                      "Maximum number of candidates to return (default 100).");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_findUnusedResources"),
                       QStringLiteral("Find Unused Resources"),
                       QStringLiteral("Heuristically identify resources that no bound pipeline "
                                      "event touched in the frame. Results carry low confidence "
                                      "and an explicit limitation. Requires replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          return adapter.FindUnusedResources((int)ArgU32(args, "limit", 100), call,
                                             ArgFields(args));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "limit", "number",
                      "Maximum number of candidates to return (default 100).");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");
    AddSchemaProperty(props, "baselinePath", "string", "Baseline capture path (requires candidatePath).");
    AddSchemaProperty(props, "candidatePath", "string", "Candidate capture path (requires baselinePath).");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_findResourceLeaks"),
                       QStringLiteral("Find Suspected Resource Leaks"),
                       QStringLiteral("Heuristically rank resources in the loaded capture, or compare "
                                      "baselinePath/candidatePath using independent read-only replay. "
                                      "Cross-capture names are low-confidence candidates, never leak proof. "
                                      "Without paths, the legacy branch requires a loaded capture/replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::None,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          if(args.contains(QStringLiteral("baselinePath")) ||
             args.contains(QStringLiteral("candidatePath")))
            return CompareCapturePaths(adapter, args, call, ComparisonKind::ResourceLeaks);
          return adapter.FindResourceLeaks((int)ArgU32(args, "limit", 100), call, ArgFields(args));
        });
  }

  // ---------------------------------------------------------------------------
  // Structured pipeline snapshot per event and same-capture pipeline diff (P1-5)
  // ---------------------------------------------------------------------------

  {
    QJsonObject props;
    AddSchemaProperty(props, "eventId", "number",
                      "The event id whose pipeline state to serialise.");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getPipelineStateAtEvent"),
                       QStringLiteral("Get Pipeline State At Event"),
                       QStringLiteral("Select one event and return its structured pipeline state "
                                      "(same shape as renderdoc_getPipelineState, but for any "
                                      "event, not just the current one). Requires replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          return adapter.PipelineSnapshotAt(ArgU32(args, "eventId", 0), call, ArgFields(args));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "eventIdA", "number", "The baseline event id.");
    AddSchemaProperty(props, "eventIdB", "number", "The event id to compare against the baseline.");
    AddSchemaProperty(props, "fields", "array",
                      "Optional list of top-level keys to return. Add \"snapshots\" to also "
                      "receive both full pipeline snapshots, not just the change list.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_diffPipelineState"),
                       QStringLiteral("Diff Pipeline State"),
                       QStringLiteral("Compare the structured pipeline state of two events in the "
                                      "same capture. Returns the differing dotted field paths with "
                                      "before/after values plus a denormalised shader-stage binding "
                                      "list. Every difference is a fact read from replay. Requires "
                                      "replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          const uint32_t eventIdA = ArgU32(args, "eventIdA", 0);
          const uint32_t eventIdB = ArgU32(args, "eventIdB", 0);
          if(eventIdA == 0 || eventIdB == 0)
            return FailWithCapture(ErrorCode::InvalidToolInput,
                                   "eventIdA and eventIdB are required and must be non-zero.");
          return adapter.DiffPipelineState(eventIdA, eventIdB, call, ArgFields(args));
        });
  }

  // ---------------------------------------------------------------------------
  // Structured performance report generation and export (P1-6). Every row is
  // anchored to timing/shader/resource evidence; the report never asserts a
  // hot-spot conclusion without the EventGPUDuration measurement behind it.
  // ---------------------------------------------------------------------------

  {
    QJsonObject props;
    AddSchemaProperty(props, "limit", "number",
                      "Maximum number of hottest passes and hottest events to return. Default 20, "
                      "hard maximum 200.");
    AddSchemaProperty(props, "format", "string", "Report format: json (default) or markdown.");
    props[QStringLiteral("format")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("enum"), QJsonArray{QStringLiteral("json"), QStringLiteral("markdown")}}};
    AddSchemaProperty(props, "outputPath", "string", "Optional atomic no-overwrite document destination; parent must exist.");
    AddSchemaProperty(props, "confirm", "boolean", "Confirm the report tool's possible file side effect.");
    AddSchemaProperty(props, "includePipeline", "boolean",
                      "Attach shader and resource evidence read from the pipeline state of each "
                      "hottest event. Default true.");
    AddSchemaProperty(props, "includeMesh", "boolean",
                      "Advertise the mesh-data follow-up for hottest events that have an index "
                      "buffer. Default true.");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_generatePerformanceReport"),
                       QStringLiteral("Generate Performance Report"),
                       QStringLiteral("Aggregate captured GPU time per pass and rank the hottest "
                                      "leaf events. Each hot pass and hot event is anchored to "
                                      "timing, shader and resource evidence so the conclusion can be "
                                      "traced back to a concrete event id. When EventGPUDuration is "
                                      "unavailable the ranking is returned with confidence "
                                      "\"toVerify\" and a stated limitation rather than being "
                                      "presented as fact. Returns JSON/Markdown content in-band and optionally "
                                      "writes outputPath atomically without overwriting. Requires replay."),
                       ToolEffects::SideEffect,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          if(args.contains(QStringLiteral("confirm")) && !ArgBool(args, "confirm", false))
            return ToolResult::Failure(Error(ErrorCode::SideEffectNotConfirmed, "confirm=false refuses report generation"));
          const QString format = ArgStr(args, "format", QStringLiteral("json"));
          if(format != QStringLiteral("json") && format != QStringLiteral("markdown"))
            return ToolResult::Failure(Error(ErrorCode::InvalidParams, "format must be json or markdown"));
          return CompatibilityDocument(adapter.GeneratePerformanceReport((int)ArgU32(args, "limit", 20),
                                                   ArgBool(args, "includePipeline", true),
                                                   ArgBool(args, "includeMesh", true), call,
                                                   ArgFields(args), format), args, call);
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "format", "string",
                      "Rendering format: \"markdown\" (default) or \"json\".");
    AddSchemaProperty(props, "limit", "number",
                      "Maximum number of hottest passes and hottest events to include. Default 20, "
                      "hard maximum 200.");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_exportPerformanceReport"),
                       QStringLiteral("Export Performance Report"),
                       QStringLiteral("Render the structured performance report as a JSON or "
                                      "Markdown document. The rendered text is returned in-band as "
                                      "\"content\" next to the structured report; the tool performs "
                                      "no file write, so the caller decides whether and where to "
                                      "persist it. Requires replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          return adapter.ExportPerformanceReport(ArgStr(args, "format", QStringLiteral("markdown")),
                                                 (int)ArgU32(args, "limit", 20), call,
                                                 ArgFields(args));
        });
  }

  // ---------------------------------------------------------------------------
  // Whole-investigation report (P1-9). One artefact that composes the P1-8
  // bookmarks, the P1-6 performance report and the P1-4 resource-memory audit
  // into a single JSON or Markdown document. Read-only: it never writes to disk,
  // so the rendered text comes back in-band and the caller owns persistence.
  // ---------------------------------------------------------------------------
  {
    QJsonObject props;
    AddSchemaProperty(props, "format", "string",
                      "Rendering format: \"markdown\" (default) or \"json\".");
    AddSchemaProperty(props, "limit", "number",
                      "Maximum number of hotspot and audit rows to include. Default 20, hard "
                      "maximum 200.");
    AddSchemaProperty(props, "includeResources", "boolean",
                      "Attach the resource-memory audit (largest resources, format breakdown). "
                      "Default true; set false for a compact report.");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_buildInvestigationReport"),
                       QStringLiteral("Build Investigation Report"),
                       QStringLiteral("Compose ONE investigation report from the current capture: "
                                      "capture metadata plus the investigation bookmarks, the "
                                      "performance hotspot report and the resource-memory audit. "
                                      "Each section that cannot be a fact (unavailable bookmarks, "
                                      "missing GPU timings, estimate-only resource sizes) is "
                                      "reported with a stated limitation instead of an invented "
                                      "value. Rendered as JSON or Markdown and returned in-band as "
                                      "\"content\"; the tool performs no file write. Requires "
                                      "replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          return adapter.BuildInvestigationReport(
              ArgStr(args, "format", QStringLiteral("markdown")), (int)ArgU32(args, "limit", 20),
              ArgBool(args, "includeResources", true), call, ArgFields(args));
        });
  }

  // ---------------------------------------------------------------------------
  // Event Browser advanced filtering and AI context (P1-7). Applies the plugin's
  // combined filter set over the action tree and returns a flattened, capped view
  // plus a prose context block. The native event tree and its expression filter
  // box are left untouched - this only augments them with a machine-readable,
  // programmatic query a client can drive.
  // ---------------------------------------------------------------------------
  {
    QJsonObject props;
    AddSchemaProperty(props, "filter", "string", "Case-insensitive name substring after advanced tree filtering.");
    AddSchemaProperty(props, "markerFilter", "string",
                      "Case-insensitive substring matched against action names. Once a marker "
                      "matches, all of its descendants qualify.");
    AddSchemaProperty(props, "excludeMarkers", "boolean", "Drop marker groups. Default false.");
    AddSchemaProperty(props, "onlyDrawCalls", "boolean",
                      "Keep only real draw/dispatch/clear effects. Default false.");
    AddSchemaProperty(props, "excludeDebugMarkers", "boolean",
                      "Drop debug/annotation marker groups. Default false.");
    AddSchemaProperty(props, "excludeEmptyOperations", "boolean",
                      "Drop draws/dispatches with no indices and no instances. Default false.");
    AddSchemaProperty(props, "sortByDuration", "boolean",
                      "Order matching events by GPU duration, hottest first. Has no effect when "
                      "EventGPUDuration is unavailable. Default false.");
    for(const char *key : {"eventIdMin", "eventIdMax"})
    {
      AddSchemaProperty(props, key, "integer", "Inclusive event-id bound; omission means no bound, including an explicit zero constrains the range.");
      QJsonObject property = props.value(QString::fromLatin1(key)).toObject();
      property[QStringLiteral("minimum")] = 0;
      property[QStringLiteral("maximum")] = double(UINT32_MAX);
      props[QString::fromLatin1(key)] = property;
    }
    AddSchemaProperty(props, "limit", "number",
                      "Maximum number of events to return. Default 500, hard maximum 2000.");
    AddSchemaProperty(props, "fields", "array", "Optional list of top-level keys to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_buildEventBrowserContext"),
                       QStringLiteral("Build Event Browser AI Context"),
                       QStringLiteral("Apply the combined advanced filter set (marker scope, "
                                      "debug/empty-operation exclusion, event-id bounds, draw-only) "
                                      "over the capture's action tree and return the surviving "
                                      "events as a flat, capped list plus an `aiContext` prose "
                                      "block. Does not modify or replace the native event tree. "
                                      "Requires replay."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          EventBrowserFilterOptions options;
          const ToolResult parsed = ParseEventBrowserFilters(args, options);
          if(!parsed.ok) return parsed;
          return adapter.BuildEventBrowserContext(options, (int)ArgU32(args, "limit", 500), call,
                                                  ArgFields(args));
        });
  }

  {
    QJsonObject props;

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getSessionState"),
                       QStringLiteral("Get RenderDoc Session State"),
                       QStringLiteral("Return the full session state: capture info, replay status, "
                                      "modifications made in the UI and the list of bookmarks."),
                       ToolEffects::ReadOnly, ToolRequirements::None, MakeSchema(props)},
        [&adapter](const QJsonObject &, CallContext &) -> ToolResult {
          ToolResult snap = adapter.ApiProperties();
          ToolResult frame = adapter.FrameInfo();
          ToolResult mods = adapter.Modifications();
          ToolResult bookmarks = adapter.Bookmarks();

          QJsonObject out;
          out[QStringLiteral("capture")] = QueryAdapter::SnapshotToJson(adapter.Snapshot());
          // Application/target ownership is separate from the MCP HTTP session.
          out[QStringLiteral("applicationSession")] = adapter.ApplicationSessionState();
          if(snap.ok)
            out[QStringLiteral("api")] = snap.structured;
          if(frame.ok)
            out[QStringLiteral("frame")] = frame.structured;
          if(mods.ok)
            out[QStringLiteral("modifications")] = mods.structured;
          if(bookmarks.ok)
            out[QStringLiteral("bookmarks")] = bookmarks.structured;

          return ToolResult::Success(out, QString());
        });
  }

  // ---------------------------------------------------------------------------
  // Actions / events
  // ---------------------------------------------------------------------------

  {
    QJsonObject props;
    AddSchemaProperty(props, "eventId", "integer",
                      "Optional top-level event id to restrict the result to one subtree.");
    AddSchemaProperty(props, "maxCount", "integer", "Maximum number of post-filter siblings to return.");
    AddSchemaProperty(props, "filter", "string", "Case-insensitive action-name substring, applied before advanced filtering.");
    AddSchemaProperty(props, "markerFilter", "string", "Case-insensitive subtree name scope.");
    AddSchemaProperty(props, "excludeMarkers", "boolean", "Drop marker/group nodes and promote matching children.");
    AddSchemaProperty(props, "onlyDrawCalls", "boolean", "Keep draw, dispatch, mesh-dispatch and clear operations.");
    for(const char *key : {"eventIdMin", "eventIdMax"})
      props[QString::fromLatin1(key)] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
          {QStringLiteral("minimum"), 0}, {QStringLiteral("maximum"), double(UINT32_MAX)}};
    AddSchemaProperty(props, "fields", "array", "Optional list of keys per action to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getDrawCalls"), QStringLiteral("Get Draw Calls"),
                       QStringLiteral("Return the root action tree (markers, draw calls and "
                                      "dispatches) with event ids, index/instance counts and child "
                                      "counts. Use eventId to drill into a single subtree."),
                       ToolEffects::ReadOnly, ToolRequirements::LoadedCapture, MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &) -> ToolResult {
          EventBrowserFilterOptions options;
          const ToolResult parsed = ParseEventBrowserFilters(args, options);
          if(!parsed.ok) return parsed;
          return adapter.RootActions(ArgU32(args, "eventId"), ClampList(int(ArgU64(args, "maxCount"))),
                                     ArgFields(args), options);
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "eventId", "integer", "The event id to describe.");
    AddSchemaProperty(props, "fields", "array", "Optional list of keys to return.");
    props[QStringLiteral("required")] = QJsonArray{QStringLiteral("eventId")};

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getEventDetails"), QStringLiteral("Get Event Details"),
                       QStringLiteral("Return the action at a specific event id: its name, flags, "
                                      "index/instance counts, children and - when available - the "
                                      "bound resources at that event."),
                       ToolEffects::ReadOnly, ToolRequirements::LoadedCapture, MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &) -> ToolResult {
          const uint32_t eid = ArgU32(args, "eventId");
          if(eid == 0)
          {
            // No explicit event id: fall back to the current UI selection.
            return adapter.EventInfo(0);
          }
          return adapter.Action(eid, ArgFields(args));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "eventId", "integer", "The event id whose API chunks to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getEventChunks"), QStringLiteral("Get Event Chunks"),
                       QStringLiteral("Return the ordered list of API chunks (calls) that make up an "
                                      "event, together with a compact summary of each chunk's "
                                      "top-level parameters - the machine readable form of the API "
                                      "Inspector."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          const uint32_t eid = ArgU32(args, "eventId");

          return adapter.DispatchBlock(
              "renderdoc_getEventChunks", call,
              [eid](IReplayController &replay) -> ToolResult {
                const SDFile &sdfile = replay.GetStructuredFile();

                const ActionDescription *action = FindActionByEID(replay.GetRootActions(), eid);

                QJsonArray chunks;
                if(action != NULL)
                {
                  for(const APIEvent &ev : action->events)
                  {
                    QJsonObject c;
                    c[QStringLiteral("eventId")] = double(ev.eventId);

                    const SDChunk *chunk = NULL;
                    if(ev.chunkIndex < sdfile.chunks.size())
                      chunk = sdfile.chunks[ev.chunkIndex];

                    if(chunk != NULL)
                    {
                      const char *nm = chunk->name.c_str();
                      c[QStringLiteral("name")] = nm != NULL ? QString::fromUtf8(nm) : QString();
                      c[QStringLiteral("params")] = ChunkParamsSummary(*chunk);
                      c[QStringLiteral("chunkId")] = double(chunk->metadata.chunkID);
                      c[QStringLiteral("durationMicro")] = double(chunk->metadata.durationMicro);
                    }
                    else
                    {
                      c[QStringLiteral("name")] = QStringLiteral("(unknown)");
                      c[QStringLiteral("params")] = QString();
                    }

                    chunks.push_back(c);
                  }
                }

                QJsonObject out;
                out[QStringLiteral("eventId")] = double(eid);
                out[QStringLiteral("found")] = action != NULL;
                out[QStringLiteral("chunks")] = chunks;
                return ToolResult::Success(out, QString());
              });
        });
  }

  // ---------------------------------------------------------------------------
  // Resources
  // ---------------------------------------------------------------------------

  {
    QJsonObject props;
    AddSchemaProperty(props, "type", "string",
                      "Optional resource category filter, e.g. Texture, Buffer, Shader.");
    AddSchemaProperty(props, "maxCount", "integer", "Legacy budget: 0 means 256, positive values capped at 4096; cannot conflict with limit.");
    AddSchemaProperty(props, "limit", "integer", "Page size (default and maximum 500).");
    AddSchemaProperty(props, "offset", "integer", "Offset in the filtered resource inventory (default 0).");
    props[QStringLiteral("limit")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
        {QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 500}};
    props[QStringLiteral("offset")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
        {QStringLiteral("minimum"), 0}, {QStringLiteral("maximum"), INT32_MAX}};
    AddSchemaProperty(props, "fields", "array", "Optional list of keys per resource to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getResources"), QStringLiteral("Get Resources"),
                       QStringLiteral("List the resources in the capture, optionally filtered by "
                                      "category. Each entry carries the resource id, name, type and "
                                      "size where known."),
                       ToolEffects::ReadOnly, ToolRequirements::LoadedCapture, MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          return adapter.ResourceTool(QStringLiteral("renderdoc_getResources"), args, call);
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "resourceId", "integer", "The resource id to look up.");
    props[QStringLiteral("required")] = QJsonArray{QStringLiteral("resourceId")};

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getResourceDetail"),
                       QStringLiteral("Get Resource Detail"),
                       QStringLiteral("Return the full description of a single resource: type, name, "
                                      "dimensions/format for textures and length/usage for buffers."),
                       ToolEffects::ReadOnly, ToolRequirements::LoadedCapture, MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &) -> ToolResult {
          const ResourceId id = ArgResourceId(args, "resourceId");
          if(id == ResourceId())
            return FailWithCapture(ErrorCode::InvalidToolInput, "resourceId is required");
          return adapter.Resource(id);
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "nameFilter", "string", "Optional case-insensitive substring filter.");
    AddSchemaProperty(props, "maxCount", "integer", "Maximum number of textures to return.");
    AddSchemaProperty(props, "fields", "array", "Optional list of keys per texture to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getTextures"), QStringLiteral("Get Textures"),
                       QStringLiteral("List the textures in the capture with their dimensions, mip "
                                      "and array/sample counts, format and usage flags."),
                       ToolEffects::ReadOnly, ToolRequirements::LoadedCapture, MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &) -> ToolResult {
          return adapter.Textures(ArgStr(args, "nameFilter"), ClampList(int(ArgU64(args, "maxCount"))),
                                  ArgFields(args));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "resourceId", "integer", "Legacy texture resource id.");
    QJsonObject resourceId = props.value(QStringLiteral("resourceId")).toObject();
    resourceId[QStringLiteral("type")] = QJsonArray{QStringLiteral("string"), QStringLiteral("integer")};
    props[QStringLiteral("resourceId")] = resourceId;
    AddSchemaProperty(props, "textureId", "string", "Optional exact texture id; omission lists textures. Conflicting aliases are rejected.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getTextureInfo"), QStringLiteral("Get Texture Info"),
                       QStringLiteral("Return texture descriptions, optionally selecting one exact texture id, "
                                      "including dimensions, format, mip levels and usage."),
                       ToolEffects::ReadOnly, ToolRequirements::LoadedCapture, MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          return adapter.ResourceTool(QStringLiteral("renderdoc_getTextureInfo"), args, call);
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "resourceId", "integer", "Legacy texture resource id; resourceId or textureId is required.");
    AddSchemaProperty(props, "textureId", "string", "Exact texture id, alias of resourceId; conflicts are rejected.");
    AddSchemaProperty(props, "eventId", "integer", "Event to read; explicit 0 means end of frame. Raw omission preserves current event.");
    AddSchemaProperty(props, "channelExtract", "integer", "PNG only: -1=RGB with source alpha discarded, 0=R, 1=G, 2=B, 3=A grayscale. Raw extraction is Unsupported.");
    AddSchemaProperty(props, "outputFormat", "string", "raw (default, unchanged native bytes) or png (opaque display preview, whole PNG; no ASTC/volume conversion).");
    QJsonObject resourceId = props.value(QStringLiteral("resourceId")).toObject();
    resourceId[QStringLiteral("type")] = QJsonArray{QStringLiteral("string"), QStringLiteral("integer")};
    props[QStringLiteral("resourceId")] = resourceId;
    QJsonObject encoding = props.value(QStringLiteral("outputFormat")).toObject();
    encoding[QStringLiteral("enum")] = QJsonArray{QStringLiteral("raw"), QStringLiteral("png")};
    props[QStringLiteral("outputFormat")] = encoding;
    QJsonObject channel = props.value(QStringLiteral("channelExtract")).toObject();
    channel[QStringLiteral("minimum")] = -1;
    channel[QStringLiteral("maximum")] = 3;
    props[QStringLiteral("channelExtract")] = channel;
    AddSchemaProperty(props, "mip", "integer", "Mip level to read (default 0).");
    AddSchemaProperty(props, "slice", "integer", "Array slice to read (default 0).");
    AddSchemaProperty(props, "sample", "integer", "Sample index to read (default 0).");
    AddSchemaProperty(props, "maxBytes", "integer",
                      "Upper bound on returned bytes (default and maximum 65536).");
    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getTextureData"), QStringLiteral("Get Texture Data"),
                       QStringLiteral("Read one texture subresource as original raw bytes (default, "
                                      "bounded by maxBytes), or explicitly request a whole opaque PNG "
                                      "display preview with optional grayscale channel extraction."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          return adapter.WithCaptureUI(call, [&](ICaptureContext &ctx) -> ToolResult {
          if(!ctx.Config().MCP_AllowSensitiveData) return SensitiveDenied("renderdoc_getTextureData");
          TextureReadRequest request;
          ToolResult parsed = ParseTextureReadRequest(args, request);
          if(!parsed.ok) return parsed;
          const ResourceId id = request.id;
          const Subresource sub = request.sub;
          const uint64_t maxBytes = request.maxBytes;
          const uint32_t current = adapter.Snapshot().currentEvent;
          return adapter.DispatchBlock("renderdoc_getTextureData", call,
                                       [id, sub, maxBytes, request, current, &call](IReplayController &replay) -> ToolResult {
                                          uint32_t event = current;
                                          if(request.selectEvent)
                                          {
                                            ToolResult selected = SelectResourceEvent(replay.GetRootActions(), request.eventId, true,
                                                [&](uint32_t eid) { replay.SetFrameEvent(eid, false); }, event);
                                            if(!selected.ok) return selected;
                                          }
                                          if(request.png)
                                          {
                                            ToolResult png = ReadTexturePNG(replay, request, call);
                                            if(png.ok)
                                            {
                                              png.structured[QStringLiteral("eventId")] = double(event);
                                              return ToolResult::Success(png.structured);
                                            }
                                            return png;
                                          }
                                         const TextureDescription *tex = NULL;
                                         for(const TextureDescription &t : replay.GetTextures())
                                         {
                                           if(t.resourceId == id)
                                           {
                                             tex = &t;
                                             break;
                                           }
                                         }

                                         if(tex == NULL)
                                         {
                                           return ToolResult::Failure(
                                               Error(ErrorCode::InvalidParams,
                                                     QObject::tr("Texture not found in capture")));
                                         }

                                         // Preserve native raw subresource semantics (including volume slices and sample resolve).
                                         const bytebuf data = replay.GetTextureData(id, sub);
                                         if(data.empty())
                                         {
                                           return ToolResult::Failure(
                                               Error(ErrorCode::InternalError,
                                                     QObject::tr("Failed to read texture data")));
                                         }

                                         const uint64_t total = uint64_t(data.size());
                                         const uint64_t take = qMin(total, maxBytes);

                                         const QByteArray b64 = QByteArray::fromRawData((const char *)data.data(), int(take)).toBase64();

                                         QJsonObject out;
                                         out[QStringLiteral("resourceId")] = ResourceRef(id);
                                         out[QStringLiteral("outputFormat")] = QStringLiteral("raw");
                                         out[QStringLiteral("eventId")] = double(event);
                                         out[QStringLiteral("channelExtract")] = -1;
                                         out[QStringLiteral("mip")] = double(sub.mip);
                                         out[QStringLiteral("slice")] = double(sub.slice);
                                         out[QStringLiteral("sample")] = double(sub.sample);
                                         out[QStringLiteral("width")] = double(tex->width);
                                         out[QStringLiteral("height")] = double(tex->height);
                                         out[QStringLiteral("depth")] = double(tex->depth);
                                         out[QStringLiteral("format")] = FormatToJson(tex->format);
                                         out[QStringLiteral("totalBytes")] = double(total);
                                         out[QStringLiteral("returnedBytes")] = double(take);
                                         out[QStringLiteral("truncated")] = take < total;
                                         out[QStringLiteral("base64")] = QString::fromLatin1(b64);
                                         return ToolResult::Success(out, QString());
                                       });
         });
       });
 }

 {
   QJsonObject props;
   AddSchemaProperty(props, "nameFilter", "string", "Optional case-insensitive substring filter.");
   AddSchemaProperty(props, "maxCount", "integer", "Maximum number of buffers to return.");
    AddSchemaProperty(props, "fields", "array", "Optional list of keys per buffer to return.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getBuffers"), QStringLiteral("Get Buffers"),
                       QStringLiteral("List the buffers in the capture with length, structure size "
                                      "and usage flags, plus an aggregate breakdown by usage."),
                       ToolEffects::ReadOnly, ToolRequirements::LoadedCapture, MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &) -> ToolResult {
          return adapter.Buffers(ArgStr(args, "nameFilter"), ClampList(int(ArgU64(args, "maxCount"))),
                                 ArgFields(args));
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "resourceId", "integer", "The buffer resource id.");
    props[QStringLiteral("required")] = QJsonArray{QStringLiteral("resourceId")};

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getBufferDetail"),
                       QStringLiteral("Get Buffer Detail"),
                       QStringLiteral("Return the full description of a single buffer: length, "
                                      "structure byte stride and usage flags."),
                       ToolEffects::ReadOnly, ToolRequirements::LoadedCapture, MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &) -> ToolResult {
          const ResourceId id = ArgResourceId(args, "resourceId");
          if(id == ResourceId())
            return FailWithCapture(ErrorCode::InvalidToolInput, "resourceId is required");
          return adapter.Buffer(id);
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "resourceId", "integer", "The buffer resource id.");
    AddSchemaProperty(props, "offset", "integer", "Byte offset to start reading from (default 0).");
    AddSchemaProperty(props, "len", "integer",
                       "Number of bytes to read (default 4096, capped at 65536).");
    AddSchemaProperty(props, "eventId", "integer", "Positive event selects replay before reading; 0/omission preserves current event (native bridge behavior).");
    QJsonObject resourceId = props.value(QStringLiteral("resourceId")).toObject();
    resourceId[QStringLiteral("type")] = QJsonArray{QStringLiteral("string"), QStringLiteral("integer")};
    props[QStringLiteral("resourceId")] = resourceId;
    props[QStringLiteral("required")] = QJsonArray{QStringLiteral("resourceId")};

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getBufferContents"),
                       QStringLiteral("Get Buffer Contents"),
                       QStringLiteral("Read raw bytes from a buffer and return them base64-encoded. "
                                      "The read length is capped to keep responses bounded."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          return adapter.WithCaptureUI(call, [&](ICaptureContext &ctx) -> ToolResult {
          if(!ctx.Config().MCP_AllowSensitiveData) return SensitiveDenied("renderdoc_getBufferContents");
          ResourceId id;
          if(!ParseResourceToolId(args.value(QStringLiteral("resourceId")), id))
            return FailWithCapture(ErrorCode::InvalidParams, "Invalid resourceId");
          int64_t offsetArg = 0, lenArg = 4096, eventArg = 0;
          if(!ReadResourceInteger(args, "offset", 0, 9007199254740991LL, offsetArg) ||
             !ReadResourceInteger(args, "len", 1, 65536, lenArg) ||
             !ReadResourceInteger(args, "eventId", 0, UINT32_MAX, eventArg))
            return FailWithCapture(ErrorCode::InvalidParams, "Invalid buffer offset/length/event");
          const uint64_t offset = uint64_t(offsetArg), len = uint64_t(lenArg);
          const uint32_t current = adapter.Snapshot().currentEvent;
          return adapter.DispatchBlock(
              "renderdoc_getBufferContents", call,
              [id, offset, len, eventArg, current](IReplayController &replay) -> ToolResult {
                uint32_t event = 0;
                ToolResult selected = SelectResourceEvent(replay.GetRootActions(), uint32_t(eventArg), false,
                    [&](uint32_t eid) { replay.SetFrameEvent(eid, false); }, event);
                if(!selected.ok) return selected;
                if(!event) event = current;
                const bytebuf data = replay.GetBufferData(id, offset, len);
                if(data.empty())
                {
                  return ToolResult::Failure(
                      Error(ErrorCode::InternalError,
                            QObject::tr("Failed to read buffer data - the buffer may be empty or "
                                        "the resource id may not exist")));
                }

                const QByteArray b64 =
                    QByteArray::fromRawData((const char *)data.data(), int(data.size())).toBase64();

                QJsonObject out;
                out[QStringLiteral("resourceId")] = ResourceRef(id);
                out[QStringLiteral("eventId")] = double(event);
                out[QStringLiteral("offset")] = double(offset);
                out[QStringLiteral("len")] = double(data.size());
                out[QStringLiteral("base64")] = QString::fromLatin1(b64);
                return ToolResult::Success(out, QString());
              });
          });
        });
  }

  // ---------------------------------------------------------------------------
  // Timings
  // ---------------------------------------------------------------------------

  {
    QJsonObject props;
    AddSchemaProperty(props, "eventId", "integer",
                      "Optional event id to restrict the result to a single action subtree.");
    AddSchemaProperty(props, "top", "integer",
                      "Maximum number of slowest events to return (default 50).");

    props[QStringLiteral("eventIds")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
          {QStringLiteral("minimum"), 0}, {QStringLiteral("maximum"), double(UINT32_MAX)}}}};
    AddSchemaProperty(props, "markerFilter", "string", "Case-insensitive ancestor marker scope.");
    props[QStringLiteral("excludeMarkers")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}};
    AddSchemaProperty(props, "onlyDrawCalls", "boolean", "Keep timed leaf draw/dispatch/mesh-dispatch/clear operations.");
    props[QStringLiteral("limit")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
        {QStringLiteral("minimum"), 0}, {QStringLiteral("maximum"), double(UINT32_MAX)},
        {QStringLiteral("description"), QStringLiteral("Default 200; implementation clamps to 1..2000 (including 0 -> 1).")}};

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getActionTimings"),
                       QStringLiteral("Get Action Timings"),
                       QStringLiteral("Return per-event GPU durations in microseconds, derived from "
                                      "the EventGPUDuration performance counter, plus the slowest "
                                      "events in descending order. Events without timing support are "
                                      "omitted rather than reported as zero."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          ActionTimingOptions options;
          const ToolResult parsed = ParseActionTimingOptions(args, options);
          if(!parsed.ok) return parsed;
          return adapter.ActionTimings(options, call);
        });
  }

  {
    QJsonObject props;

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_enumerateCounters"),
                       QStringLiteral("Enumerate Counters"),
                       QStringLiteral("List the performance counters this capture exposes, with "
                                      "their human readable name, category, result type, unit and "
                                      "description."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &, CallContext &call) -> ToolResult {
          return adapter.DispatchBlock("renderdoc_enumerateCounters", call,
                                       [](IReplayController &replay) -> ToolResult {
                                         QJsonArray counters;
                                         for(GPUCounter c : replay.EnumerateCounters())
                                         {
                                           const CounterDescription desc = replay.DescribeCounter(c);
                                           QJsonObject o;
                                           o[QStringLiteral("id")] = double(uint32_t(c));
                                           o[QStringLiteral("name")] =
                                               QString::fromUtf8(desc.name.c_str());
                                           o[QStringLiteral("category")] =
                                               QString::fromUtf8(desc.category.c_str());
                                           o[QStringLiteral("description")] =
                                               QString::fromUtf8(desc.description.c_str());
                                           o[QStringLiteral("resultType")] =
                                               int(uint32_t(desc.resultType));
                                           o[QStringLiteral("resultByteWidth")] =
                                               double(desc.resultByteWidth);
                                           o[QStringLiteral("unit")] = int(uint32_t(desc.unit));
                                           counters.push_back(o);
                                         }

                                         QJsonObject out;
                                         out[QStringLiteral("counters")] = counters;
                                         return ToolResult::Success(out, QString());
                                       });
        });
  }

  // ---------------------------------------------------------------------------
  // Pipeline
  // ---------------------------------------------------------------------------

  {
    QJsonObject props;
    AddSchemaProperty(props, "eventId", "integer", "Positive event selects replay; 0/omission preserves current event.");
    AddSchemaProperty(props, "includePipelineState", "boolean",
                      "Also include the live replay pipeline state (default true).");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getPipelineState"),
                       QStringLiteral("Get Pipeline State"),
                       QStringLiteral("Return the pipeline state at the current event: viewports, "
                                      "scissors, rasterizer, depth/stencil, blend, output targets, "
                                      "vertex/index buffers, vertex inputs and per-stage shader "
                                      "bindings with their reflection."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          return adapter.WithCaptureUI(call, [&](ICaptureContext &ctx) -> ToolResult {
          if(!ctx.Config().MCP_AllowSensitiveData) return SensitiveDenied("renderdoc_getPipelineState");
          int64_t eventArg = 0;
          if(!ReadResourceInteger(args, "eventId", 0, UINT32_MAX, eventArg))
            return FailWithCapture(ErrorCode::InvalidParams, "Invalid eventId");
          const uint32_t current = adapter.Snapshot().currentEvent;
          const bool includeState = args.value(QStringLiteral("includePipelineState")).toBool(true);

          ToolResult result = adapter.DispatchBlock(
              "renderdoc_getPipelineState", call,
              [includeState, eventArg, current](IReplayController &replay) -> ToolResult {
                uint32_t event = 0;
                ToolResult selected = SelectResourceEvent(replay.GetRootActions(), uint32_t(eventArg), false,
                    [&](uint32_t eid) { replay.SetFrameEvent(eid, false); }, event);
                if(!selected.ok) return selected;
                QJsonObject out;
                out[QStringLiteral("eventId")] = double(event ? event : current);

                // The capture-load-time fields the UI already has, without
                // touching the replay thread for anything else.
                const ActionDescription *action = NULL;
                const rdcarray<ActionDescription> &roots = replay.GetRootActions();

                QJsonObject counts;
                counts[QStringLiteral("rootActions")] = double(roots.size());
                out[QStringLiteral("counts")] = counts;

                if(includeState)
                  out[QStringLiteral("state")] = PipelineStateToJson(replay);

                (void)action;
                return ToolResult::Success(out, QString());
              });

          if(!result.ok)
            return result;

          return WithCapture(adapter, result);
          });
        });
  }

  // ---------------------------------------------------------------------------
  // Shaders
  // ---------------------------------------------------------------------------

  {
    QJsonObject props;
    AddSchemaProperty(props, "resourceId", "integer", "The shader resource id.");
    props[QStringLiteral("required")] = QJsonArray{QStringLiteral("resourceId")};

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getShaderEntryPoints"),
                       QStringLiteral("Get Shader Entry Points"),
                       QStringLiteral("List the entry points available in a shader, with the shader "
                                      "stage of each. Returns an empty list for ids that are not "
                                      "shaders."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          const ResourceId id = ArgResourceId(args, "resourceId");
          if(id == ResourceId())
            return FailWithCapture(ErrorCode::InvalidToolInput, "resourceId is required");

          return adapter.DispatchBlock("renderdoc_getShaderEntryPoints", call,
                                       [id](IReplayController &replay) -> ToolResult {
                                         QJsonArray entries;
                                         for(const ShaderEntryPoint &e : replay.GetShaderEntryPoints(id))
                                         {
                                           QJsonObject o;
                                           o[QStringLiteral("name")] =
                                               QString::fromUtf8(e.name.c_str());
                                           o[QStringLiteral("stage")] =
                                               detail::ShaderStageName(e.stage);
                                           entries.push_back(o);
                                         }

                                         QJsonObject out;
                                         out[QStringLiteral("resourceId")] = ResourceRef(id);
                                         out[QStringLiteral("entryPoints")] = entries;
                                         return ToolResult::Success(out, QString());
                                       });
        });
  }

  {
    QJsonObject props;
    AddSchemaProperty(props, "resourceId", "integer", "Shader resource id, required without eventId.");
    AddSchemaProperty(props, "pipeline", "integer", "Optional pipeline; validated against event binding.");
    for(const QString &key : {QStringLiteral("resourceId"), QStringLiteral("pipeline")})
    {
      QJsonObject property = props.value(key).toObject();
      property[QStringLiteral("type")] = QJsonArray{QStringLiteral("string"), QStringLiteral("integer")};
      props[key] = property;
    }
    AddSchemaProperty(props, "entryPoint", "string", "Optional entry point to reflect.");
    AddSchemaProperty(props, "eventId", "integer", "Event whose shader bindings to inspect.");
    QJsonObject event = props.value(QStringLiteral("eventId")).toObject();
    event[QStringLiteral("minimum")] = 1;
    event[QStringLiteral("maximum")] = double(UINT32_MAX);
    props[QStringLiteral("eventId")] = event;
    AddSchemaProperty(props, "stage", "string", "Optional shader stage, including fragment/pixel aliases.");
    AddSchemaProperty(props, "includeSource", "boolean", "Include source/disassembly; default true with stage.");
    AddSchemaProperty(props, "includeConstantBuffers", "boolean", "Decode constants; defaults true for event queries, requires eventId.");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getShaderInfo"), QStringLiteral("Get Shader Info"),
                       QStringLiteral("Return the reflection for a shader: shader stage, encoding, "
                                      "input/output signatures, constant blocks, samplers and the "
                                      "read-only/read-write resource bindings."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::None,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          return adapter.ShaderTool(QStringLiteral("renderdoc_getShaderInfo"), args, call);
        });
  }

  // ---------------------------------------------------------------------------
  // Mesh data
  // ---------------------------------------------------------------------------

  {
    QJsonObject props;
    AddSchemaProperty(props, "eventId", "integer",
                      "Event id of the draw whose mesh data to decode; 0 uses the current event.");
    AddSchemaProperty(props, "stage", "string",
                      "One of vsin, vsout, gsout, taskout, meshout (default vsin).");
    AddSchemaProperty(props, "instance", "integer", "Instance index (default 0).");
    AddSchemaProperty(props, "view", "integer", "Multiview view index (default 0).");
    AddSchemaProperty(props, "maxVertices", "integer",
                      "Maximum vertices to decode; 0 means all (capped at 4M).");

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getMeshData"),
                       QStringLiteral("Get Mesh Data"),
                       QStringLiteral("Decode vertex and index data for the mesh at an event, "
                                      "exactly as the native Mesh Viewer does: the topology, the "
                                      "index buffer, the bound vertex attributes and, for "
                                      "post-transform stages, the generated vertex positions."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController,
                       MakeSchema(props)},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          if(!SensitiveAllowed(call))
            return SensitiveDenied("renderdoc_getMeshData");

          const uint32_t eventId = ArgU32(args, "eventId");
          const MeshDataStage stage = ParseMeshStage(ArgStr(args, "stage", QStringLiteral("vsin")));
          const uint32_t instance = ArgU32(args, "instance");
          const uint32_t view = ArgU32(args, "view");
          const uint32_t maxVerts = ArgU32(args, "maxVertices");

          return adapter.DispatchBlock(
              "renderdoc_getMeshData", call,
              [eventId, stage, instance, view, maxVerts](IReplayController &replay) -> ToolResult {
                MeshFormat mf = replay.GetPostVSData(instance, view, stage);

                const ActionDescription *action = NULL;
                if(stage == MeshDataStage::VSIn)
                {
                  action = FindActionByEID(replay.GetRootActions(), eventId);

                  if(action != NULL)
                  {
                    // For the input-assembly stage the draw's own counts are
                    // authoritative - GetPostVSData(VSIn) can return zero on APIs
                    // that don't expose a post-IA stream.
                    if(mf.numIndices == 0)
                      mf.numIndices = action->numIndices;
                    if(mf.baseVertex == 0)
                      mf.baseVertex = action->baseVertex;
                    if(mf.indexResourceId == ResourceId() && (action->flags & ActionFlags::Indexed))
                    {
                      const BoundVBuffer ib = replay.GetPipelineState().GetIBuffer();
                      mf.indexResourceId = ib.resourceId;
                      mf.indexByteOffset = ib.byteOffset;
                      mf.indexByteStride = ib.byteStride;
                    }
                  }
                }

                QJsonObject out;
                out[QStringLiteral("eventId")] = double(eventId);
                out[QStringLiteral("stage")] = MeshStageName(stage);
                out[QStringLiteral("instance")] = double(instance);
                out[QStringLiteral("view")] = double(view);
                out[QStringLiteral("meshFormat")] = MeshFormatToJson(mf);

                // Bound vertex attributes (VSIn only): decode every attribute the
                // app bound, matching the native Mesh Viewer.
                QJsonArray attrs;
                if(stage == MeshDataStage::VSIn)
                {
                  for(const VertexInputAttribute &a : replay.GetPipelineState().GetVertexInputs())
                  {
                    const BoundVBuffer vb = (a.vertexBuffer >= 0 &&
                                             size_t(a.vertexBuffer) <
                                                 replay.GetPipelineState().GetVBuffers().size())
                                                ? replay.GetPipelineState().GetVBuffers()[a.vertexBuffer]
                                                : BoundVBuffer();
                    QJsonObject o;
                    o[QStringLiteral("name")] = QString::fromUtf8(a.name.c_str());
                    o[QStringLiteral("vertexBuffer")] = ResourceRef(vb.resourceId);
                    o[QStringLiteral("byteOffset")] = double(a.byteOffset);
                    o[QStringLiteral("byteStride")] = double(vb.byteStride);
                    o[QStringLiteral("perInstance")] = a.perInstance;
                    o[QStringLiteral("instanceRate")] = int(a.instanceRate);
                    o[QStringLiteral("format")] = FormatToJson(a.format);
                    o[QStringLiteral("genericEnabled")] = a.genericEnabled;
                    attrs.push_back(o);
                  }
                }

                // Fall back to the single position stream MeshFormat describes, which
                // is all that is available for post-transform stages.
                if(attrs.isEmpty())
                {
                  QJsonObject o;
                  o[QStringLiteral("name")] = stage == MeshDataStage::VSIn
                                                  ? QStringLiteral("POSITION")
                                                  : QStringLiteral("gl_Position");
                  o[QStringLiteral("vertexBuffer")] = ResourceRef(mf.vertexResourceId);
                  o[QStringLiteral("byteOffset")] = double(mf.vertexByteOffset);
                  o[QStringLiteral("byteStride")] = double(mf.vertexByteStride);
                  o[QStringLiteral("perInstance")] = false;
                  o[QStringLiteral("instanceRate")] = 0;
                  o[QStringLiteral("format")] = FormatToJson(mf.format);
                  o[QStringLiteral("genericEnabled")] = false;
                  attrs.push_back(o);
                }

                out[QStringLiteral("attributes")] = attrs;

                const uint32_t total = mf.numIndices;
                if(total == 0)
                {
                  out[QStringLiteral("rows")] = QJsonArray();
                  out[QStringLiteral("totalIndices")] = 0;
                  out[QStringLiteral("returnedIndices")] = 0;
                  return ToolResult::Success(out, QString());
                }

                const uint32_t effectiveMax = maxVerts == 0 ? kMaxVertexHard
                                                            : qMin(maxVerts, kMaxVertexHard);
                const uint32_t count = qMin(total, effectiveMax);

                // Read the index buffer, or synthesise an implicit 0..n sequence for
                // non-indexed draws.
                bytebuf idxBuf;
                const bool indexed = mf.indexResourceId != ResourceId() &&
                                     mf.indexByteStride != 0 && mf.indexByteStride <= 4;
                if(indexed)
                {
                  idxBuf = replay.GetBufferData(mf.indexResourceId, mf.indexByteOffset,
                                                uint64_t(count) * uint64_t(mf.indexByteStride));
                }

                // Read each vertex buffer once for as much of it as we need.
                struct VBRead
                {
                  ResourceId id;
                  QByteArray data;
                  uint64_t offset = 0;
                  uint32_t stride = 0;
                };
                QVector<VBRead> vbReads;

                auto vbFor = [&](ResourceId id, uint32_t vbStride, uint64_t vbOffset) -> const VBRead * {
                  if(id == ResourceId() || vbStride == 0)
                    return NULL;

                  for(const VBRead &r : vbReads)
                  {
                    if(r.id == id && r.stride == vbStride && r.offset == vbOffset)
                      return &r;
                  }

                  VBRead r;
                  r.id = id;
                  r.offset = vbOffset;
                  r.stride = vbStride;
                  const bytebuf raw = replay.GetBufferData(id, vbOffset,
                                                           uint64_t(vbStride) * uint64_t(count + 1));
                  r.data = QByteArray((const char *)raw.data(), int(raw.size()));
                  vbReads.push_back(r);
                  return &vbReads.last();
                };

                // Cache vertex-buffer reads keyed by the attribute's binding.
                QJsonArray rows;
                for(uint32_t i = 0; i < count; i++)
                {
                  // Resolve the vertex index: real index (plus base vertex) when
                  // indexed, otherwise the implicit sequential index.
                  uint32_t vtxIdx = i;
                  if(indexed)
                    vtxIdx = ReadIndexValue(idxBuf, mf.indexByteStride, uint64_t(i));

                  if(mf.baseVertex > 0)
                    vtxIdx += uint32_t(mf.baseVertex);

                  QJsonArray row;
                  for(const QJsonValue &av : attrs)
                  {
                    const QJsonObject a = av.toObject();
                    const QJsonObject fmt = a[QStringLiteral("format")].toObject();
                    const QJsonObject vbo = a[QStringLiteral("vertexBuffer")].toObject();

                    ResourceFormat f;
                    f.compType = CompType(a[QStringLiteral("format")].toObject()[QStringLiteral("compType")].toInt());
                    f.compCount = uint8_t(fmt[QStringLiteral("compCount")].toInt());
                    f.compByteWidth = uint8_t(fmt[QStringLiteral("compByteWidth")].toInt());
                    f.type = ResourceFormatType(fmt[QStringLiteral("type")].toInt());

                    const uint64_t vbIdNum = uint64_t(vbo[QStringLiteral("id")].toDouble());
                    const ResourceId vbId = ResIdFromU64(vbIdNum);
                    const uint32_t vbStride = uint32_t(a[QStringLiteral("byteStride")].toDouble());
                    const uint64_t attrOff = uint64_t(a[QStringLiteral("byteOffset")].toDouble());

                    const VBRead *vb = vbFor(vbId, vbStride, 0);
                    if(vb == NULL || vbStride == 0)
                    {
                      row.push_back(QJsonValue::Null);
                      continue;
                    }

                    // Per-instance attributes read from a fixed row, matching the
                    // way the mesh viewer binds instance data.
                    const uint32_t rowIndex = a[QStringLiteral("perInstance")].toBool() ? 0 : vtxIdx;
                    const uint64_t off = uint64_t(rowIndex) * vbStride + attrOff;
                    const uint64_t need = off + uint64_t(f.compByteWidth) * f.compCount;

                    if(need > uint64_t(vb->data.size()))
                    {
                      row.push_back(QJsonValue::Null);
                      continue;
                    }

                    QJsonArray comps;
                    const byte *p = (const byte *)vb->data.constData() + off;
                    for(uint8_t c = 0; c < f.compCount && c < 4; c++)
                      comps.push_back(JsonNum(DecodeComponent(p + uint64_t(c) * f.compByteWidth, f)));

                    row.push_back(comps);
                  }

                  rows.push_back(row);
                }

                out[QStringLiteral("totalIndices")] = double(total);
                out[QStringLiteral("returnedIndices")] = double(count);
                out[QStringLiteral("rows")] = rows;
                return ToolResult::Success(out, QString());
              });
        });
  }

  // ---------------------------------------------------------------------------
  // Environment diagnostics (P2-5)
  // ---------------------------------------------------------------------------
  {
    QJsonObject props;
    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getEnvironmentDiagnostics"),
                       QStringLiteral("Get Environment Diagnostics"),
                       QStringLiteral("Report MCP availability, sensitive-data policy, project source mapping "
                                      "support, and Mali Offline Compiler configuration with actionable "
                                      "recovery advice. The tool never launches an external executable."),
                       ToolEffects::ReadOnly, ToolRequirements::None, MakeSchema(props)},
        [](const QJsonObject &, CallContext &call) -> ToolResult {
          if(call.ctx == NULL)
            return ToolResult::Failure(
                Error(ErrorCode::InternalError, QObject::tr("Capture context is unavailable.")));

          EnvironmentDiagnosticInput input;
          input.serverEnabled = call.ctx->Config().MCP_Enabled;
          input.serverPort = call.ctx->Config().MCP_Port;
          input.sensitiveDataAllowed = call.ctx->Config().MCP_AllowSensitiveData;
#if defined(RENDERDOC_PLATFORM_WIN32)
          input.platform = QStringLiteral("windows");
#elif defined(RENDERDOC_PLATFORM_APPLE)
          input.platform = QStringLiteral("macos");
#else
          input.platform = QStringLiteral("linux");
#endif
          const rdcstr &configuredMaliPath = call.ctx->Config().MCP_MaliCompilerPath;
          input.maliConfiguredPath =
              QString::fromUtf8(configuredMaliPath.c_str(), int(configuredMaliPath.size()));
          input.maliResolvedExecutable = ResolveMaliExecutablePath(input.maliConfiguredPath);
          input.maliExecutableExists = !input.maliResolvedExecutable.isEmpty() &&
                                       QFileInfo::exists(input.maliResolvedExecutable);
          input.supportsMali = input.platform == QStringLiteral("windows") ||
                               input.platform == QStringLiteral("linux");
          return ToolResult::Success(
              EnvironmentDiagnosticReportToJson(BuildEnvironmentDiagnosticReport(input)), QString());
        });
  }

  // ---------------------------------------------------------------------------
  // Project source mapping (P2-4)
  // ---------------------------------------------------------------------------
  {
    QJsonObject props;
    AddSchemaProperty(props, "sourceReference", "string",
                      "Recorded shader/source file reference to resolve against local project files.");
    AddSchemaProperty(props, "projectRoots", "array",
                      "Project roots to recursively index for local source files.");
    AddSchemaProperty(props, "includePaths", "array",
                      "Additional INCLUDE/dependency paths to recursively index.");
    AddSchemaProperty(props, "extensions", "array",
                      "Optional source extensions without requiring a leading dot.");
    AddSchemaProperty(props, "maxFiles", "integer",
                      "Maximum files to index (default 20000) to bound local filesystem work.");
    AddSchemaProperty(props, "fuzzyThreshold", "number",
                      "Minimum filename similarity for a fuzzy candidate, in the range 0 to 1.");
    AddSchemaProperty(props, "overrides", "array",
                      "Manual mappings with sourceReference and localPath fields; an existing localPath wins.");
    QJsonObject schema = MakeSchema(props);
    schema[QStringLiteral("required")] = QJsonArray{QStringLiteral("sourceReference")};

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_mapProjectSource"),
                       QStringLiteral("Map Capture Source To Local Project File"),
                       QStringLiteral("Recursively index explicitly supplied local project roots and INCLUDE paths, "
                                      "then resolve a capture/debug source reference using exact, relative, filename, "
                                      "suffix and fuzzy matching. Returns confidence, alternatives and ambiguity instead "
                                      "of guessing between equally ranked files."),
                       ToolEffects::ReadOnly, ToolRequirements::None, schema},
        [](const QJsonObject &args, CallContext &) -> ToolResult {
          SourceMappingOptions options;
          for(const QJsonValue &value : args.value(QStringLiteral("projectRoots")).toArray())
            if(value.isString())
              options.projectRoots.push_back(value.toString());
          for(const QJsonValue &value : args.value(QStringLiteral("includePaths")).toArray())
            if(value.isString())
              options.includePaths.push_back(value.toString());
          const QJsonArray extensions = args.value(QStringLiteral("extensions")).toArray();
          if(!extensions.isEmpty())
          {
            options.extensions.clear();
            for(const QJsonValue &value : extensions)
              if(value.isString())
                options.extensions.push_back(value.toString());
          }
          options.maxFiles = int(ArgU64(args, "maxFiles", uint32_t(options.maxFiles)));
          options.fuzzyThreshold = args.value(QStringLiteral("fuzzyThreshold")).toDouble(options.fuzzyThreshold);
          options.fuzzyThreshold = std::max(0.0, std::min(1.0, options.fuzzyThreshold));

          QVector<SourceMappingOverride> overrides;
          for(const QJsonValue &value : args.value(QStringLiteral("overrides")).toArray())
          {
            const QJsonObject mapping = value.toObject();
            if(!mapping.isEmpty())
              overrides.push_back({mapping.value(QStringLiteral("sourceReference")).toString(),
                                   mapping.value(QStringLiteral("localPath")).toString()});
          }

          const SourceMappingIndex index = IndexSourceMappingPaths(options);
          const SourceMappingResult result =
              ResolveSourceMapping(index, ArgStr(args, "sourceReference"), overrides);
          QJsonObject out = SourceMappingResultToJson(result);
          out[QStringLiteral("index")] = SourceMappingIndexToJson(index);
          return ToolResult::Success(out, QString());
        });
  }

  RegisterCompatibilityTools(registry, adapter, true);
  return registry.Count();
}

}    // namespace MCP
