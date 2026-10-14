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

#include "MCPToolRegistry.h"

#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <exception>
#include "Code/QRDUtils.h"

namespace MCP
{

bool CallContext::IsExpired() const
{
  if(deadlineMs <= 0)
    return false;

  return QDateTime::currentMSecsSinceEpoch() > deadlineMs;
}

////////////////////////////////////////////////////////////////////////////////
// JsonSchema
////////////////////////////////////////////////////////////////////////////////

bool JsonSchema::ValidateType(const QString &type, const QJsonValue &value)
{
  if(type == QStringLiteral("object"))
    return value.isObject();
  if(type == QStringLiteral("array"))
    return value.isArray();
  if(type == QStringLiteral("string"))
    return value.isString();
  if(type == QStringLiteral("boolean"))
    return value.isBool();
  if(type == QStringLiteral("null"))
    return value.isNull();
  if(type == QStringLiteral("number"))
    return value.isDouble();
  if(type == QStringLiteral("integer"))
  {
    if(!value.isDouble())
      return false;
    const double d = value.toDouble();
    return d == double(qint64(d));
  }

  // Unknown types are treated as unconstrained.
  return true;
}

bool JsonSchema::ValidateValue(const QJsonValue &schemaVal, const QJsonValue &value,
                               const QString &path, QString &errorPath)
{
  if(!schemaVal.isObject())
  {
    // Boolean schemas: true matches anything, false matches nothing.
    if(schemaVal.isBool() && !schemaVal.toBool())
    {
      errorPath = path;
      return false;
    }
    return true;
  }

  const QJsonObject schema = schemaVal.toObject();

  // type
  const QJsonValue typeVal = schema[QStringLiteral("type")];
  if(typeVal.isString())
  {
    if(!ValidateType(typeVal.toString(), value))
    {
      errorPath = path + QStringLiteral(" (expected ") + typeVal.toString() + QStringLiteral(")");
      return false;
    }
  }
  else if(typeVal.isArray())
  {
    bool anyMatch = false;
    for(const QJsonValue &t : typeVal.toArray())
    {
      if(ValidateType(t.toString(), value))
      {
        anyMatch = true;
        break;
      }
    }
    if(!anyMatch)
    {
      errorPath = path + QStringLiteral(" (expected one of the allowed types)");
      return false;
    }
  }

  // enum
  const QJsonValue enumVal = schema[QStringLiteral("enum")];
  if(enumVal.isArray())
  {
    bool found = false;
    for(const QJsonValue &candidate : enumVal.toArray())
    {
      if(candidate == value)
      {
        found = true;
        break;
      }
    }
    if(!found)
    {
      errorPath = path + QStringLiteral(" (not one of the allowed values)");
      return false;
    }
  }

  // numeric bounds
  if(value.isDouble())
  {
    const double d = value.toDouble();
    if(schema.contains(QStringLiteral("minimum")) && d < schema[QStringLiteral("minimum")].toDouble())
    {
      errorPath = path + QStringLiteral(" (below minimum)");
      return false;
    }
    if(schema.contains(QStringLiteral("maximum")) && d > schema[QStringLiteral("maximum")].toDouble())
    {
      errorPath = path + QStringLiteral(" (above maximum)");
      return false;
    }
  }

  // string constraints
  if(value.isString())
  {
    const QString s = value.toString();
    if(schema.contains(QStringLiteral("minLength")) &&
       s.size() < schema[QStringLiteral("minLength")].toInt())
    {
      errorPath = path + QStringLiteral(" (too short)");
      return false;
    }
    if(schema.contains(QStringLiteral("maxLength")) &&
       s.size() > schema[QStringLiteral("maxLength")].toInt())
    {
      errorPath = path + QStringLiteral(" (too long)");
      return false;
    }
    if(schema.contains(QStringLiteral("pattern")))
    {
      QRegularExpression re(schema[QStringLiteral("pattern")].toString());
      if(re.isValid() && !re.match(s).hasMatch())
      {
        errorPath = path + QStringLiteral(" (does not match pattern)");
        return false;
      }
    }
  }

  // array items
  if(value.isArray() && schema.contains(QStringLiteral("items")))
  {
    const QJsonArray arr = value.toArray();
    const QJsonValue itemsSchema = schema[QStringLiteral("items")];
    for(int i = 0; i < arr.size(); i++)
    {
      if(!ValidateValue(itemsSchema, arr[i], path + QStringLiteral("[%1]").arg(i), errorPath))
        return false;
    }
  }

  // object properties / required / additionalProperties
  if(value.isObject())
  {
    const QJsonObject obj = value.toObject();
    const QJsonObject props = schema[QStringLiteral("properties")].toObject();

    const QJsonValue required = schema[QStringLiteral("required")];
    if(required.isArray())
    {
      for(const QJsonValue &r : required.toArray())
      {
        const QString key = r.toString();
        if(!obj.contains(key))
        {
          errorPath = path.isEmpty() ? key : path + QStringLiteral(".") + key;
          errorPath += QStringLiteral(" (required)");
          return false;
        }
      }
    }

    for(auto it = obj.begin(); it != obj.end(); ++it)
    {
      const QString childPath = path.isEmpty() ? it.key() : path + QStringLiteral(".") + it.key();
      if(props.contains(it.key()))
      {
        if(!ValidateValue(props[it.key()], it.value(), childPath, errorPath))
          return false;
      }
      else if(schema.contains(QStringLiteral("additionalProperties")))
      {
        if(!ValidateValue(schema[QStringLiteral("additionalProperties")], it.value(), childPath,
                          errorPath))
          return false;
      }
    }
  }

  return true;
}

bool JsonSchema::Validate(const QJsonObject &schema, const QJsonValue &value, QString &errorPath)
{
  errorPath.clear();
  return ValidateValue(schema, value, QString(), errorPath);
}

////////////////////////////////////////////////////////////////////////////////
// ToolRegistry
////////////////////////////////////////////////////////////////////////////////

void ToolRegistry::Add(const ToolDefinition &def,
                       std::function<ToolResult(const QJsonObject &, CallContext &)> handler)
{
  RegisteredTool tool;
  tool.definition = def;
  tool.handler = handler;
  m_Tools[def.name] = tool;
}

bool ToolRegistry::Contains(const QString &name) const
{
  return m_Tools.contains(name);
}

const RegisteredTool *ToolRegistry::Find(const QString &name) const
{
  auto it = m_Tools.find(name);
  if(it == m_Tools.end())
    return NULL;
  return &it.value();
}

QList<ToolDefinition> ToolRegistry::Definitions() const
{
  QList<ToolDefinition> defs;
  for(auto it = m_Tools.begin(); it != m_Tools.end(); ++it)
    defs.push_back(it.value().definition);
  return defs;
}

ToolResult ToolRegistry::Call(const QString &name, const QJsonObject &args, CallContext &call) const
{
  const RegisteredTool *tool = Find(name);

  if(tool == NULL)
  {
    Error e(ErrorCode::MethodNotFound, QStringLiteral("Unknown tool: %1").arg(name));
    return ToolResult::Failure(e);
  }

  // Cancellation and deadline are checked before doing any work, and the context
  // is handed to the handler so long running tools can poll them.
  if(call.IsCancelled())
    return ToolResult::Failure(Error(ErrorCode::Cancelled, QStringLiteral("Request cancelled")));

  if(call.IsExpired())
    return ToolResult::Failure(Error(ErrorCode::Timeout, QStringLiteral("Request deadline passed")));

  QString errorPath;
  if(!JsonSchema::Validate(tool->definition.inputSchema, args, errorPath))
  {
    QJsonObject data;
    data[QStringLiteral("field")] = errorPath;
    data[QStringLiteral("tool")] = name;
    Error e(ErrorCode::InvalidToolInput,
            QStringLiteral("The tool input failed schema validation."), data);
    return ToolResult::Failure(e);
  }

  if(!tool->handler)
    return ToolResult::Failure(
        Error(ErrorCode::InternalError, QStringLiteral("Tool has no handler: %1").arg(name)));

  // Any exception escaping a handler is contained here so a single misbehaving
  // tool can never take down the server thread.
  try
  {
    return tool->handler(args, call);
  }
  catch(const std::exception &ex)
  {
    return ToolResult::Failure(Error(ErrorCode::InternalError,
                                     QStringLiteral("Tool %1 threw: %2")
                                         .arg(name)
                                         .arg(QString::fromUtf8(ex.what()))));
  }
  catch(...)
  {
    return ToolResult::Failure(
        Error(ErrorCode::InternalError, QStringLiteral("Tool %1 threw an unknown exception").arg(name)));
  }
}

}    // namespace MCP
