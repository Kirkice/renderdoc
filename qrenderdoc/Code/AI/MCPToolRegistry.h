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

#include <QAtomicInt>
#include <QJsonObject>
#include <QSharedPointer>
#include <QMap>
#include <QString>
#include <functional>
#include "MCPTypes.h"

struct ICaptureContext;

namespace MCP
{

// Validates a JSON value against a subset of JSON Schema. We deliberately avoid
// pulling in a third party schema library: the tool schemas are authored by us and
// only use a small, well understood vocabulary (type, required, properties, items,
// enum, minimum/maximum, minLength, pattern). Anything outside that vocabulary is
// ignored rather than treated as an error, so schemas stay forward compatible.
class JsonSchema
{
public:
  // Returns true if `value` conforms. On failure `errorPath` describes where.
  static bool Validate(const QJsonObject &schema, const QJsonValue &value, QString &errorPath);

private:
  static bool ValidateType(const QString &type, const QJsonValue &value);
  static bool ValidateValue(const QJsonValue &schemaVal, const QJsonValue &value,
                            const QString &path, QString &errorPath);
};

// The runtime context passed to every tool invocation. This is how a tool reaches
// the capture without knowing anything about HTTP, sessions or the UI.
struct CallContext
{
  ICaptureContext *ctx = NULL;
  // Legacy non-owning cancellation flag, retained for direct/unit-test callers.
  // Transport callers must use cancellationState so that a disconnect cannot
  // leave replay work observing a dangling stack pointer.
  const volatile bool *cancelled = NULL;
  QSharedPointer<QAtomicInt> cancellationState;
  // Soft deadline in milliseconds since epoch, or 0 for "no deadline".
  qint64 deadlineMs = 0;

  bool IsCancelled() const
  {
    return (cancelled != NULL && *cancelled) ||
           (!cancellationState.isNull() && cancellationState->loadAcquire() != 0);
  }
  bool IsExpired() const;
};

// A single registered tool: metadata plus its handler.
struct RegisteredTool
{
  ToolDefinition definition;
  std::function<ToolResult(const QJsonObject &args, CallContext &call)> handler;
};

// Owns the set of tools exposed over MCP. Registration happens once at startup;
// lookup and invocation are read-only afterwards and safe to call concurrently as
// long as registration has finished.
class ToolRegistry
{
public:
  ToolRegistry() {}
  ~ToolRegistry() {}

  void Add(const ToolDefinition &def,
           std::function<ToolResult(const QJsonObject &, CallContext &)> handler);

  bool Contains(const QString &name) const;
  const RegisteredTool *Find(const QString &name) const;

  // Ordered list of tool definitions for tools/list. Stable ordering by name so
  // clients see a deterministic list between runs.
  QList<ToolDefinition> Definitions() const;

  int Count() const { return m_Tools.size(); }

  // Validate arguments against the tool schema then invoke the handler. Never
  // throws: schema failures and handler failures both come back as a failed
  // ToolResult with a structured Error.
  ToolResult Call(const QString &name, const QJsonObject &args, CallContext &call) const;

private:
  QMap<QString, RegisteredTool> m_Tools;
};

}    // namespace MCP
