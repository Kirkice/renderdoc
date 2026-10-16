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

// Module boundary
// ----------------
// Everything under qrenderdoc/Code/AI/ implements the native AI / MCP surface that
// was previously only available through the external VS Code extension. The module
// is deliberately layered so that each layer has a single responsibility and can be
// tested in isolation:
//
//   MCPServer              - HTTP transport, session bookkeeping, lifecycle
//     -> MCPToolRegistry   - tool metadata, JSON Schema validation, result wrapping
//        -> MCPQueryAdapter- CaptureContext snapshot + ReplayManager dispatch
//
// No layer talks to the VS Code extension, the out-of-process native bridge, or any
// other external process. Everything is resolved through the in-process
// ICaptureContext / IReplayController inside qrenderdoc, dispatched via
// ReplayManager::AsyncInvoke() / BlockInvoke().

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <functional>

namespace MCP
{
// The MCP protocol revision this build implements. Clients negotiate down by
// sending their own revision in the initialize handshake; we echo the negotiated
// value back and reject anything newer than what we understand.
//
// DSH's bundled MCP SDK 1.29 initializes using 2025-11-25. The qrenderdoc
// Streamable HTTP surface used here (initialize, notifications, tools/list,
// and tools/call) remains compatible with that revision.
static const char *const kLatestProtocolVersion = "2025-11-25";
static const char *const kServerName = "qrenderdoc";
static const char *const kDefaultHost = "127.0.0.1";
static const int kDefaultPort = 38967;

// JSON-RPC 2.0 error codes, plus MCP/qrenderdoc specific extensions. The extension
// codes are namespaced in the -32000..-32099 range reserved for implementations
// by the JSON-RPC specification.
enum class ErrorCode : int
{
  // JSON-RPC standard
  ParseError = -32700,
  InvalidRequest = -32600,
  MethodNotFound = -32601,
  InvalidParams = -32602,
  InternalError = -32603,

  // qrenderdoc extensions
  NoCaptureLoaded = -32001,
  CaptureBusy = -32002,
  ReplayUnavailable = -32003,
  InvalidToolInput = -32004,
  SideEffectNotConfirmed = -32005,
  ProtocolVersionUnsupported = -32006,
  Cancelled = -32007,
  Timeout = -32008,
  Unsupported = -32009,
};

// A structured, machine readable error. Every failure that crosses the MCP
// boundary is expressed as one of these so that clients never have to parse free
// text. `data` carries whatever optional evidence helps a client recover.
struct Error
{
  // Explicit flag so that a default-constructed Error is not mistaken for a real
  // failure, and so an empty message never silently means "success".
  bool hasError = false;
  ErrorCode code = ErrorCode::InternalError;
  QString message;
  QJsonObject data;

  Error() {}
  Error(ErrorCode c, const QString &m, const QJsonObject &d = QJsonObject())
      : hasError(true), code(c), message(m), data(d)
  {
  }
  // Convenience overload so call sites can pass a plain UTF-8 literal without
  // relying on Qt's const char * -> QString conversion, which qrenderdoc disables
  // via QT_NO_CAST_FROM_ASCII.
  Error(ErrorCode c, const char *m)
      : hasError(true), code(c), message(QString::fromUtf8(m))
  {
  }

  bool isError() const { return hasError; }

  // Serialise into a JSON-RPC error object.
  QJsonObject toJson() const;

  static Error fromJson(const QJsonObject &obj);
};

// Canonical string name for an error code, used in the `error.data.qrenderdocCode`
// field so clients can branch on a stable identifier rather than the numeric code.
QString ErrorCodeName(ErrorCode code);

// How the server may treat a tool. Read-only tools never mutate capture state and
// can be retried freely; side-effecting tools require explicit confirmation and are
// never auto-retried.
enum class ToolEffects
{
  ReadOnly,
  SideEffect,
};

// Whether a tool depends on the ReplayManager thread being free (i.e. it issues
// IReplayController calls) and whether it requires a loaded capture at all.
enum class ToolRequirements : uint32_t
{
  None = 0x0,
  LoadedCapture = 0x1,
  ReplayController = 0x2,
};

inline ToolRequirements operator|(ToolRequirements a, ToolRequirements b)
{
  return ToolRequirements(uint32_t(a) | uint32_t(b));
}
inline bool operator&(ToolRequirements a, ToolRequirements b)
{
  return (uint32_t(a) & uint32_t(b)) != 0;
}

// Static description of a tool as advertised to clients via tools/list.
struct ToolDefinition
{
  QString name;
  QString title;
  QString description;
  ToolEffects effects = ToolEffects::ReadOnly;
  ToolRequirements requirements = ToolRequirements::None;
  // JSON Schema for the tool input. Kept as a raw QJsonObject so the schema is
  // authored once and validated without an intermediate representation.
  QJsonObject inputSchema;
};

// The outcome of a single tool call. Either `error` is set, or `structured` /
// `text` carry the result. Keeping a structured payload alongside a text rendering
// means clients that understand the schema get the data, and everyone else still
// gets something readable.
struct ToolResult
{
  bool ok = true;
  Error error;
  QJsonObject structured;
  QString text;

  static ToolResult Success(const QJsonObject &structuredContent, const QString &textContent = QString());
  static ToolResult Failure(const Error &e);
};

// The negotiated session state. A session is created on the MCP initialize
// handshake and torn down when the client disconnects or the server stops.
struct SessionInfo
{
  QString id;
  QString protocolVersion;
  QString clientName;
  QString clientVersion;
  qint64 createdAtMs = 0;
  qint64 lastSeenMs = 0;
};

// Server lifecycle status, surfaced to the UI and to diagnostics.
struct ServerStatus
{
  bool enabled = false;
  bool running = false;
  QString host;
  int port = 0;
  QString endpoint;
  int activeSessions = 0;
  int registeredTools = 0;
  QString lastError;
};

}    // namespace MCP
