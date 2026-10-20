/* Native shader boundary tests; these do not claim real-driver compilation coverage. */
#include "Code/QRDUtils.h"
#if ENABLE_UNIT_TESTS
#include <QDateTime>
#include <QJsonArray>
#include "3rdparty/catch/catch.hpp"
#include "MCPExternalTools.h"
#include "MCPSessionTools.h"
#include "MCPShaderTools.h"
#include "MCPTools.h"

namespace
{
QJsonObject Edit()
{
  return QJsonObject{
      {QStringLiteral("resourceId"), QStringLiteral("18446744073709551615")},
      {QStringLiteral("shaderStage"), int(ShaderStage::Pixel)},
      {QStringLiteral("sourceEncoding"), int(ShaderEncoding::HLSL)},
      {QStringLiteral("entryPoint"), QStringLiteral("main")},
      {QStringLiteral("files"),
       QJsonArray{
           QJsonObject{{QStringLiteral("filename"), QStringLiteral("main.hlsl")},
                       {QStringLiteral("contents"),
                        QStringLiteral(
                            "#include \"common.hlsl\"\nfloat4 main():SV_Target{return value;}")}},
           QJsonObject{{QStringLiteral("filename"), QStringLiteral("common.hlsl")},
                       {QStringLiteral("contents"), QStringLiteral("float4 value=1;")}}}}};
}
}
TEST_CASE("Native shader catalog preserves session tools and both info contracts", "[mcp][shader]")
{
  MCP::QueryAdapter adapter(NULL);
  MCP::ToolRegistry registry;
  MCP::RegisterCoreTools(registry, adapter);
  MCP::RegisterExternalToolTools(registry, adapter);
  MCP::RegisterSessionTools(registry, adapter);
  REQUIRE(registry.Count() == 59);
  MCP::RegisterShaderTools(registry, adapter);
  REQUIRE(registry.Count() == 66);
  MCP::RegisterShaderTools(registry, adapter);
  REQUIRE(registry.Count() == 66);
  for(const char *name :
      {"renderdoc_validateShaderEdit", "renderdoc_applyShaderEdit", "renderdoc_findShaderVariants",
       "renderdoc_compareShaders", "renderdoc_getShaderCompileDiagnostics",
       "renderdoc_getShaderSource", "renderdoc_findProjectImplementation"})
    REQUIRE(registry.Find(QString::fromUtf8(name)) != NULL);
  const MCP::RegisteredTool *info = registry.Find(QStringLiteral("renderdoc_getShaderInfo"));
  REQUIRE(info != NULL);
  QString error;
  const QJsonObject legacy{{QStringLiteral("resourceId"), QStringLiteral("18446744073709551615")},
                           {QStringLiteral("pipeline"), 2},
                           {QStringLiteral("entryPoint"), QStringLiteral("main")}};
  const QJsonObject event{{QStringLiteral("eventId"), 10},
                          {QStringLiteral("stage"), QStringLiteral("fragment")},
                          {QStringLiteral("includeSource"), false},
                          {QStringLiteral("includeConstantBuffers"), false}};
  REQUIRE(MCP::JsonSchema::Validate(info->definition.inputSchema, legacy, error));
  REQUIRE(MCP::JsonSchema::Validate(info->definition.inputSchema, event, error));
  QJsonObject mixed = legacy;
  for(auto it = event.begin(); it != event.end(); ++it)
    mixed[it.key()] = it.value();
  REQUIRE(MCP::JsonSchema::Validate(info->definition.inputSchema, mixed, error));
  REQUIRE(int(registry.Find(QStringLiteral("renderdoc_applyShaderEdit"))->definition.effects) ==
          int(MCP::ToolEffects::SideEffect));
  REQUIRE(int(registry.Find(QStringLiteral("renderdoc_validateShaderEdit"))->definition.effects) ==
          int(MCP::ToolEffects::ReadOnly));
  MCP::CallContext call;
  REQUIRE_FALSE(registry.Call(QStringLiteral("renderdoc_getShaderInfo"), QJsonObject(), call).ok);
  REQUIRE_FALSE(registry.Call(QStringLiteral("renderdoc_getShaderSource"), QJsonObject(), call).ok);
  REQUIRE_FALSE(registry
                    .Call(QStringLiteral("renderdoc_compareShaders"),
                          QJsonObject{{QStringLiteral("leftEventId"), 1}}, call)
                    .ok);
  REQUIRE_FALSE(registry
                    .Call(QStringLiteral("renderdoc_getShaderInfo"),
                          QJsonObject{{QStringLiteral("eventId"), -1}}, call)
                    .ok);
}
TEST_CASE("Native shader edit parser preserves ids and virtual sources", "[mcp][shader]")
{
  MCP::ShaderEditRequest request;
  QJsonObject args = Edit();
  REQUIRE(MCP::ParseShaderEditRequest(args, request).ok);
  REQUIRE(request.files.size() == 2);
  REQUIRE(int(request.stage) == int(ShaderStage::Pixel));
  REQUIRE(int(request.encoding) == int(ShaderEncoding::HLSL));
  MCP::CallContext call;
  QByteArray source;
  REQUIRE(MCP::PrepareShaderEditSource(request, source, call).ok);
  REQUIRE(source.contains("float4 value=1;"));
  REQUIRE_FALSE(source.contains("#include"));
  args[QStringLiteral("resourceId")] = 9007199254740992.0;
  REQUIRE_FALSE(MCP::ParseShaderEditRequest(args, request).ok);
  args = Edit();
  args[QStringLiteral("entryFileIndex")] = 2;
  REQUIRE_FALSE(MCP::ParseShaderEditRequest(args, request).ok);
  args = Edit();
  args[QStringLiteral("sourceEncoding")] = int(ShaderEncoding::SPIRV);
  REQUIRE_FALSE(MCP::ParseShaderEditRequest(args, request).ok);
  args = Edit();
  args[QStringLiteral("compileFlags")] = QJsonArray{QJsonObject{{QStringLiteral("name"), 1}}};
  REQUIRE_FALSE(MCP::ParseShaderEditRequest(args, request).ok);
}
TEST_CASE("Native virtual includes reject missing conditional cyclic sources and interruption",
          "[mcp][shader]")
{
  MCP::ShaderEditRequest request;
  REQUIRE(MCP::ParseShaderEditRequest(Edit(), request).ok);
  MCP::CallContext call;
  QByteArray source;
  request.files[0].second = "#pragma once\n#include \"common.hlsl\"";
  request.files[1].second = "#include \"main.hlsl\"";
  REQUIRE(MCP::PrepareShaderEditSource(request, source, call).ok);
  REQUIRE(MCP::ParseShaderEditRequest(Edit(), request).ok);
  request.files[1].second = "#include \"main.hlsl\"";
  REQUIRE_FALSE(MCP::PrepareShaderEditSource(request, source, call).ok);
  request.files[1].second = "#include \"absent.hlsl\"";
  REQUIRE_FALSE(MCP::PrepareShaderEditSource(request, source, call).ok);
  request.files[1].second = "#if 1\n#include \"absent.hlsl\"\n#endif";
  REQUIRE_FALSE(MCP::PrepareShaderEditSource(request, source, call).ok);
  request.files[1].second = "/*\n#include \"absent.hlsl\"\n*/\nfloat value=1;";
  REQUIRE(MCP::PrepareShaderEditSource(request, source, call).ok);
  REQUIRE(source.contains("float value=1;"));
  request.files[1].second = "#if 0\n#pragma once\n#endif";
  REQUIRE_FALSE(MCP::PrepareShaderEditSource(request, source, call).ok);
  request.files[1].second = "float value=1;";
  call.cancellationState.reset(new QAtomicInt(1));
  REQUIRE_FALSE(MCP::PrepareShaderEditSource(request, source, call).ok);
  call.cancellationState.clear();
  call.deadlineMs = QDateTime::currentMSecsSinceEpoch() - 1;
  REQUIRE_FALSE(MCP::PrepareShaderEditSource(request, source, call).ok);
}
TEST_CASE("Native shader source projection and constant scalar widths", "[mcp][shader]")
{
  ShaderReflection reflection;
  ShaderSourceFile file;
  file.filename = "shader.hlsl";
  file.contents = "sensitive source";
  reflection.debugInfo.files.push_back(file);
  REQUIRE_FALSE(MCP::ShaderReflectionToJson(reflection, false)
                    .value(QStringLiteral("sourceFiles"))
                    .toArray()[0]
                    .toObject()
                    .contains(QStringLiteral("contents")));
  REQUIRE(MCP::ShaderReflectionToJson(reflection, true)
              .value(QStringLiteral("sourceFiles"))
              .toArray()[0]
              .toObject()
              .value(QStringLiteral("contents"))
              .toString()
              .toUtf8() == QByteArray("sensitive source"));
  ShaderVariable variable;
  variable.rows = 1;
  variable.columns = 2;
  variable.type = VarType::SShort;
  variable.value.s16v[0] = -123;
  variable.value.s16v[1] = 456;
  QJsonArray values = MCP::ShaderVariableToJson(variable).value(QStringLiteral("value")).toArray();
  REQUIRE(values[0].toInt() == -123);
  REQUIRE(values[1].toInt() == 456);
  variable.type = VarType::UByte;
  variable.value.u8v[0] = 255;
  variable.value.u8v[1] = 3;
  values = MCP::ShaderVariableToJson(variable).value(QStringLiteral("value")).toArray();
  REQUIRE(values[0].toInt() == 255);
  REQUIRE(values[1].toInt() == 3);
  variable.type = VarType::ULong;
  variable.value.u64v[0] = ~uint64_t(0);
  REQUIRE(
      MCP::ShaderVariableToJson(variable).value(QStringLiteral("value")).toArray()[0].toString().toUtf8() ==
      QByteArray("18446744073709551615"));
  ShaderStage stage = ShaderStage::Count;
  REQUIRE(MCP::ParseShaderStage(QStringLiteral("fragment"), stage));
  REQUIRE(int(stage) == int(ShaderStage::Pixel));
  REQUIRE(MCP::ParseShaderStage(QStringLiteral("CS"), stage));
  REQUIRE(int(stage) == int(ShaderStage::Compute));
  for(int i = int(ShaderStage::RayGen); i < int(ShaderStage::Count); i++)
  {
    const QString names[] = {QStringLiteral("raygen"), QStringLiteral("intersection"),
                             QStringLiteral("anyhit"), QStringLiteral("closesthit"),
                             QStringLiteral("miss"),   QStringLiteral("callable")};
    REQUIRE(MCP::ParseShaderStage(names[i - int(ShaderStage::RayGen)], stage));
    REQUIRE(int(stage) == i);
  }
  REQUIRE_FALSE(MCP::ParseShaderStage(QStringLiteral("unknown"), stage));
  MCP::QueryAdapter adapter(NULL);
  MCP::CallContext call;
  MCP::ToolResult rayComparison =
      adapter.ShaderTool(QStringLiteral("renderdoc_compareShaders"),
                         QJsonObject{{QStringLiteral("leftEventId"), 1},
                                     {QStringLiteral("rightEventId"), 2},
                                     {QStringLiteral("stage"), QStringLiteral("raygen")}},
                         call);
  REQUIRE_FALSE(rayComparison.ok);
  REQUIRE(int(rayComparison.error.code) == int(MCP::ErrorCode::Unsupported));
}
#endif
