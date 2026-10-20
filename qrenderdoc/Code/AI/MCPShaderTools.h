/* Native shader MCP contracts. Capture state is accessed only by QueryAdapter. */
#pragma once
#include "MCPQueryAdapter.h"

namespace MCP
{
struct ShaderEditRequest
{
  ResourceId resource;
  ShaderStage stage = ShaderStage::Count;
  ShaderEncoding encoding = ShaderEncoding::Unknown;
  QString entryPoint;
  int entryFileIndex = 0;
  rdcstrpairs files;
  ShaderCompileFlags flags;
};

// Parse once at the owning boundary; compilation never reads host include files.
ToolResult ParseShaderEditRequest(const QJsonObject &args, ShaderEditRequest &request);
ToolResult PrepareShaderEditSource(const ShaderEditRequest &request, QByteArray &source,
                                   CallContext &call);
bool ParseShaderStage(const QString &name, ShaderStage &stage);
QJsonObject ShaderReflectionToJson(const ShaderReflection &reflection, bool includeSource);
QJsonObject ShaderVariableToJson(const ShaderVariable &variable);
void RegisterShaderTools(ToolRegistry &registry, QueryAdapter &adapter);
}
