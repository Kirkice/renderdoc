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

// P0-9 Tests
// ----------
// Unit and smoke tests for the native AI/MCP module. They exercise each layer in
// isolation and then together:
//
//   * JsonSchema         - the input validation vocabulary.
//   * ToolRegistry       - registration, lookup, schema gating, cancellation,
//                          deadlines, exception containment and result wrapping.
//   * HttpServer         - JSON-RPC structural checks, the initialize handshake
//                          and version negotiation, session gating and every
//                          dispatch method, all through the socket-free
//                          HandleJsonRpc() test hook.
//   * QueryAdapter       - the "no capture" and "replay unavailable" paths, i.e.
//                          what a tool sees before a capture is loaded.
//   * End to end         - the real registered tools answered through the full
//                          HTTP + JSON-RPC + registry + adapter stack.
//   * Loopback transport - a real POST over 127.0.0.1 through a QTcpSocket, to
//                          prove the socket plumbing (parse, route, frame, send)
//                          works and not just the dispatch function.
//
// Everything here is deliberately deterministic and requires no capture file, so
// it can run in CI. Live capture switching and real IReplayController queries are
// covered by the integration checks in the wrap-up item.

#include "Code/QRDUtils.h"

#if ENABLE_UNIT_TESTS

#include <functional>
#include <memory>
#include <stdexcept>

#include <QAtomicInt>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include "3rdparty/catch/catch.hpp"

#include "Code/AI/MCPAnalysisModel.h"
#include "Code/AI/MCPDiagnostics.h"
#include "Code/AI/MCPExternalTools.h"
#include "Code/AI/MCPCaptureMatch.h"
#include "Code/AI/MCPHttpServer.h"
#include "Code/AI/MCPQueryAdapter.h"
#include "Code/AI/MCPRegressionDiff.h"
#include "Code/AI/MCPSourceMapping.h"
#include "Code/AI/MCPToolRegistry.h"
#include "Code/AI/MCPTools.h"
#include "Code/AI/MCPTypes.h"

// Catch's project-wide fallback stringifier delegates to RenderDoc's ToStr(),
// which intentionally has no specialisations for MCP-private enums or QString.
// Keep test-only formatters local to this translation unit so assertion diagnostics
// remain useful without creating global DoStringise<> definitions or ODR hazards.
namespace Catch
{
template <>
struct StringMaker<QString>
{
  static std::string convert(const QString &value)
  {
    const QByteArray utf8 = value.toUtf8();
    return std::string(utf8.constBegin(), utf8.constEnd());
  }
};

#define MCP_CATCH_STRING_MAKER(Type, Formatter)        \
  template <>                                          \
  struct StringMaker<MCP::Type>                         \
  {                                                     \
    static std::string convert(MCP::Type value)         \
    {                                                   \
      return StringMaker<QString>::convert(MCP::Formatter(value)); \
    }                                                   \
  }

MCP_CATCH_STRING_MAKER(ErrorCode, ErrorCodeName);
MCP_CATCH_STRING_MAKER(Confidence, ConfidenceName);
MCP_CATCH_STRING_MAKER(EventCanonicalKind, EventCanonicalKindName);
MCP_CATCH_STRING_MAKER(CaptureMatchConfidence, CaptureMatchConfidenceName);
MCP_CATCH_STRING_MAKER(RegressionVerdict, RegressionVerdictName);
MCP_CATCH_STRING_MAKER(MaliRunStatus, MaliRunStatusName);
MCP_CATCH_STRING_MAKER(SourceMatchKind, SourceMatchKindName);
MCP_CATCH_STRING_MAKER(DiagnosticSeverity, DiagnosticSeverityName);

#undef MCP_CATCH_STRING_MAKER
}

namespace
{
QJsonObject ParseJsonObject(const QString &json)
{
  return QJsonDocument::fromJson(json.toUtf8()).object();
}

QJsonObject ErrorObject(const QJsonObject &resp)
{
  return resp.value(QStringLiteral("error")).toObject();
}

MCP::ToolDefinition MakeTool(const QString &name, const QJsonObject &schema)
{
  MCP::ToolDefinition def;
  def.name = name;
  def.title = name;
  def.description = QStringLiteral("synthetic test tool");
  def.effects = MCP::ToolEffects::ReadOnly;
  def.requirements = MCP::ToolRequirements::None;
  def.inputSchema = schema;
  return def;
}

QJsonObject ObjectSchema()
{
  QJsonObject schema;
  schema[QStringLiteral("type")] = QStringLiteral("object");
  return schema;
}

// A minimal initialize message, used by the session/protocol tests.
QString InitializeBody(const QString &protocol = QString::fromUtf8(MCP::kLatestProtocolVersion))
{
  return QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
                        "{\"protocolVersion\":\"%1\",\"clientInfo\":{\"name\":\"unit\","
                        "\"version\":\"1\"}}}")
      .arg(protocol);
}

struct RpcResult
{
  QString json;
  QString session;
  int status = 200;
};

RpcResult Rpc(MCP::HttpServer &server, const QString &body, const QString &session = QString())
{
  RpcResult r;
  r.json = server.HandleJsonRpc(body, session, &r.session, &r.status);
  return r;
}

// ---- small networking helpers for the loopback smoke test ------------------

bool WaitFor(const std::function<bool()> &pred, int timeoutMs)
{
  QElapsedTimer t;
  t.start();
  while(!pred())
  {
    if(t.elapsed() > timeoutMs)
      return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(1);
  }
  return true;
}

// Read exactly one complete HTTP response (headers + Content-Length body),
// pumping the event loop so the server's socket notifiers fire.
QByteArray ReadHttpResponse(QTcpSocket &sock, int timeoutMs)
{
  QByteArray buf;
  QElapsedTimer t;
  t.start();

  for(;;)
  {
    if(sock.bytesAvailable() > 0)
      buf += sock.readAll();

    const int headerEnd = buf.indexOf("\r\n\r\n");
    if(headerEnd >= 0)
    {
      int contentLength = 0;
      const QList<QByteArray> lines = buf.left(headerEnd).split('\n');
      for(const QByteArray &line : lines)
      {
        const int colon = line.indexOf(':');
        if(colon < 0)
          continue;
        if(line.left(colon).trimmed().toLower() == "content-length")
          contentLength = line.mid(colon + 1).trimmed().toInt();
      }

      if(buf.size() - (headerEnd + 4) >= contentLength)
        return buf;
    }

    if(t.elapsed() > timeoutMs)
      return buf;

    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(1);
  }
}

QByteArray HttpResponseBody(const QByteArray &response)
{
  const int headerEnd = response.indexOf("\r\n\r\n");
  if(headerEnd < 0)
    return QByteArray();
  return response.mid(headerEnd + 4);
}

QByteArray HttpResponseHeader(const QByteArray &response, const QByteArray &name)
{
  const int headerEnd = response.indexOf("\r\n\r\n");
  const QByteArray head = headerEnd >= 0 ? response.left(headerEnd) : response;
  const QList<QByteArray> lines = head.split('\n');
  for(const QByteArray &line : lines)
  {
    const int colon = line.indexOf(':');
    if(colon < 0)
      continue;
    if(line.left(colon).trimmed().toLower() == name)
      return line.mid(colon + 1).trimmed();
  }
  return QByteArray();
}

QByteArray HttpRoundTrip(QTcpSocket &sock, const QByteArray &body, const QByteArray &sessionId,
                         const QByteArray &token, int timeoutMs,
                         const QByteArray &path = QByteArray("/mcp"),
                         const QByteArray &contentType = QByteArray("application/json"))
{
  QByteArray req;
  req += "POST ";
  req += path;
  req += " HTTP/1.1\r\n";
  req += "Host: 127.0.0.1\r\n";
  req += "Content-Type: ";
  req += contentType;
  req += "\r\n";
  req += "Accept: application/json\r\n";
  if(!token.isEmpty())
  {
    req += "Authorization: Bearer ";
    req += token;
    req += "\r\n";
  }
  if(!sessionId.isEmpty())
  {
    req += "Mcp-Session-Id: ";
    req += sessionId;
    req += "\r\n";
  }
  req += "Content-Length: ";
  req += QByteArray::number(body.size());
  req += "\r\n\r\n";
  req += body;

  sock.write(req);
  sock.flush();

  return ReadHttpResponse(sock, timeoutMs);
}
}    // namespace

////////////////////////////////////////////////////////////////////////////////
// JsonSchema
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("MCP JSON schema validation", "[mcp][schema]")
{
  QString err;

  // ---- type + required -----------------------------------------------------
  {
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");
    QJsonObject props;
    QJsonObject idSchema;
    idSchema[QStringLiteral("type")] = QStringLiteral("integer");
    props[QStringLiteral("id")] = idSchema;
    schema[QStringLiteral("properties")] = props;
    QJsonArray required;
    required.append(QStringLiteral("id"));
    schema[QStringLiteral("required")] = required;

    QJsonObject good;
    good[QStringLiteral("id")] = 7;
    CHECK(MCP::JsonSchema::Validate(schema, good, err));

    QJsonObject missing;
    CHECK_FALSE(MCP::JsonSchema::Validate(schema, missing, err));
    CHECK(err.contains(QStringLiteral("required")));
    CHECK(err.contains(QStringLiteral("id")));
  }

  // ---- wrong primitive type ------------------------------------------------
  {
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");
    CHECK_FALSE(MCP::JsonSchema::Validate(schema, QJsonArray(), err));
    CHECK_FALSE(MCP::JsonSchema::Validate(schema, QStringLiteral("nope"), err));
    CHECK(MCP::JsonSchema::Validate(schema, QJsonObject(), err));
  }

  // ---- integer vs number ---------------------------------------------------
  {
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("integer");

    CHECK(MCP::JsonSchema::Validate(schema, 3, err));
    CHECK(MCP::JsonSchema::Validate(schema, 3.0, err));
    CHECK_FALSE(MCP::JsonSchema::Validate(schema, 3.5, err));
    CHECK_FALSE(MCP::JsonSchema::Validate(schema, QStringLiteral("3"), err));
  }

  // ---- enum ----------------------------------------------------------------
  {
    QJsonObject schema;
    QJsonArray allowed;
    allowed.append(QStringLiteral("a"));
    allowed.append(QStringLiteral("b"));
    schema[QStringLiteral("enum")] = allowed;

    CHECK(MCP::JsonSchema::Validate(schema, QStringLiteral("a"), err));
    CHECK_FALSE(MCP::JsonSchema::Validate(schema, QStringLiteral("c"), err));
    CHECK(err.contains(QStringLiteral("allowed values")));
  }

  // ---- numeric bounds ------------------------------------------------------
  {
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("number");
    schema[QStringLiteral("minimum")] = 0;
    schema[QStringLiteral("maximum")] = 10;

    CHECK(MCP::JsonSchema::Validate(schema, 5, err));
    CHECK_FALSE(MCP::JsonSchema::Validate(schema, -1, err));
    CHECK(err.contains(QStringLiteral("minimum")));
    CHECK_FALSE(MCP::JsonSchema::Validate(schema, 11, err));
    CHECK(err.contains(QStringLiteral("maximum")));
  }

  // ---- string constraints --------------------------------------------------
  {
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("string");
    schema[QStringLiteral("minLength")] = 2;
    schema[QStringLiteral("maxLength")] = 4;
    schema[QStringLiteral("pattern")] = QStringLiteral("^[a-z]+$");

    CHECK(MCP::JsonSchema::Validate(schema, QStringLiteral("abc"), err));
    CHECK_FALSE(MCP::JsonSchema::Validate(schema, QStringLiteral("a"), err));
    CHECK(err.contains(QStringLiteral("too short")));
    CHECK_FALSE(MCP::JsonSchema::Validate(schema, QStringLiteral("abcde"), err));
    CHECK(err.contains(QStringLiteral("too long")));
    CHECK_FALSE(MCP::JsonSchema::Validate(schema, QStringLiteral("ABC"), err));
    CHECK(err.contains(QStringLiteral("pattern")));
  }

  // ---- array items ---------------------------------------------------------
  {
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("array");
    QJsonObject items;
    items[QStringLiteral("type")] = QStringLiteral("integer");
    schema[QStringLiteral("items")] = items;

    QJsonArray good;
    good.append(1);
    good.append(2);
    CHECK(MCP::JsonSchema::Validate(schema, good, err));

    QJsonArray bad;
    bad.append(1);
    bad.append(QStringLiteral("x"));
    CHECK_FALSE(MCP::JsonSchema::Validate(schema, bad, err));
    CHECK(err.contains(QStringLiteral("[1]")));
  }

  // ---- additionalProperties: false rejects unknown keys --------------------
  {
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");
    QJsonObject props;
    QJsonObject aSchema;
    aSchema[QStringLiteral("type")] = QStringLiteral("integer");
    props[QStringLiteral("a")] = aSchema;
    schema[QStringLiteral("properties")] = props;
    schema[QStringLiteral("additionalProperties")] = false;

    QJsonObject good;
    good[QStringLiteral("a")] = 1;
    CHECK(MCP::JsonSchema::Validate(schema, good, err));

    QJsonObject extra;
    extra[QStringLiteral("a")] = 1;
    extra[QStringLiteral("b")] = 2;
    CHECK_FALSE(MCP::JsonSchema::Validate(schema, extra, err));
    CHECK(err.contains(QStringLiteral("b")));
  }
}

////////////////////////////////////////////////////////////////////////////////
// ToolRegistry
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("MCP tool registry call semantics", "[mcp][registry]")
{
  MCP::ToolRegistry registry;

  // A plain tool that echoes a required integer argument.
  QJsonObject echoSchema;
  echoSchema[QStringLiteral("type")] = QStringLiteral("object");
  QJsonObject props;
  QJsonObject valueSchema;
  valueSchema[QStringLiteral("type")] = QStringLiteral("integer");
  props[QStringLiteral("value")] = valueSchema;
  echoSchema[QStringLiteral("properties")] = props;
  QJsonArray required;
  required.append(QStringLiteral("value"));
  echoSchema[QStringLiteral("required")] = required;

  registry.Add(MakeTool(QStringLiteral("t_echo"), echoSchema),
               [](const QJsonObject &args, MCP::CallContext &) -> MCP::ToolResult {
                 QJsonObject out;
                 out[QStringLiteral("value")] = args.value(QStringLiteral("value")).toInt();
                 return MCP::ToolResult::Success(out);
               });

  registry.Add(MakeTool(QStringLiteral("t_alpha"), ObjectSchema()),
               [](const QJsonObject &, MCP::CallContext &) -> MCP::ToolResult {
                 return MCP::ToolResult::Success(QJsonObject());
               });

  registry.Add(MakeTool(QStringLiteral("t_throw"), ObjectSchema()),
               [](const QJsonObject &, MCP::CallContext &) -> MCP::ToolResult {
                 throw std::runtime_error("boom");
               });

  registry.Add(MakeTool(QStringLiteral("t_nohandler"), ObjectSchema()),
               std::function<MCP::ToolResult(const QJsonObject &, MCP::CallContext &)>());

  CHECK(registry.Count() == 4);
  CHECK(registry.Contains(QStringLiteral("t_echo")));
  CHECK(registry.Find(QStringLiteral("t_echo")) != NULL);
  CHECK(registry.Find(QStringLiteral("missing")) == NULL);

  // Definitions() is ordered by name for a deterministic tools/list.
  {
    const QList<MCP::ToolDefinition> defs = registry.Definitions();
    REQUIRE(defs.size() == 4);
    CHECK(defs[0].name == QStringLiteral("t_alpha"));
    CHECK(defs[1].name == QStringLiteral("t_echo"));
    CHECK(defs[2].name == QStringLiteral("t_nohandler"));
    CHECK(defs[3].name == QStringLiteral("t_throw"));
  }

  // ---- unknown tool --------------------------------------------------------
  {
    MCP::CallContext call;
    MCP::ToolResult r = registry.Call(QStringLiteral("nope"), QJsonObject(), call);
    CHECK_FALSE(r.ok);
    CHECK(r.error.code == MCP::ErrorCode::MethodNotFound);
  }

  // ---- happy path ----------------------------------------------------------
  {
    QJsonObject args;
    args[QStringLiteral("value")] = 42;
    MCP::CallContext call;
    MCP::ToolResult r = registry.Call(QStringLiteral("t_echo"), args, call);
    REQUIRE(r.ok);
    CHECK(r.structured.value(QStringLiteral("value")).toInt() == 42);
  }

  // ---- schema validation failure ------------------------------------------
  {
    MCP::CallContext call;
    MCP::ToolResult r = registry.Call(QStringLiteral("t_echo"), QJsonObject(), call);
    CHECK_FALSE(r.ok);
    CHECK(r.error.code == MCP::ErrorCode::InvalidToolInput);
    CHECK(r.error.data.value(QStringLiteral("tool")).toString() == QStringLiteral("t_echo"));
    CHECK(r.error.data.value(QStringLiteral("field")).toString().contains(QStringLiteral("value")));
  }

  // ---- cancellation is checked before any work ----------------------------
  {
    volatile bool cancelledFlag = true;
    MCP::CallContext call;
    call.cancelled = &cancelledFlag;
    MCP::ToolResult r = registry.Call(QStringLiteral("t_alpha"), QJsonObject(), call);
    CHECK_FALSE(r.ok);
    CHECK(r.error.code == MCP::ErrorCode::Cancelled);
  }

  // ---- deadline ------------------------------------------------------------
  {
    MCP::CallContext call;
    call.deadlineMs = QDateTime::currentMSecsSinceEpoch() - 1000;
    MCP::ToolResult r = registry.Call(QStringLiteral("t_alpha"), QJsonObject(), call);
    CHECK_FALSE(r.ok);
    CHECK(r.error.code == MCP::ErrorCode::Timeout);
  }

  // ---- exceptions are contained -------------------------------------------
  {
    MCP::CallContext call;
    MCP::ToolResult r = registry.Call(QStringLiteral("t_throw"), QJsonObject(), call);
    CHECK_FALSE(r.ok);
    CHECK(r.error.code == MCP::ErrorCode::InternalError);
    CHECK(r.error.message.contains(QStringLiteral("threw")));
    CHECK(r.error.message.contains(QStringLiteral("boom")));
  }

  // ---- missing handler -----------------------------------------------------
  {
    MCP::CallContext call;
    MCP::ToolResult r = registry.Call(QStringLiteral("t_nohandler"), QJsonObject(), call);
    CHECK_FALSE(r.ok);
    CHECK(r.error.code == MCP::ErrorCode::InternalError);
    CHECK(r.error.message.contains(QStringLiteral("no handler")));
  }
}

////////////////////////////////////////////////////////////////////////////////
// Types
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("MCP error and result serialisation", "[mcp][types]")
{
  // ---- error round trip ----------------------------------------------------
  {
    QJsonObject data;
    data[QStringLiteral("field")] = QStringLiteral("x");
    MCP::Error e(MCP::ErrorCode::InvalidToolInput, QStringLiteral("bad"), data);
    CHECK(e.isError());

    const QJsonObject j = e.toJson();
    CHECK(j.value(QStringLiteral("code")).toInt() == int(MCP::ErrorCode::InvalidToolInput));
    CHECK(j.value(QStringLiteral("message")).toString() == QStringLiteral("bad"));
    CHECK(j.value(QStringLiteral("data")).toObject().value(QStringLiteral("field")).toString() ==
          QStringLiteral("x"));
    CHECK(j.value(QStringLiteral("data")).toObject().value(QStringLiteral("qrenderdocCode")).toString() ==
          QStringLiteral("INVALID_TOOL_INPUT"));

    const MCP::Error round = MCP::Error::fromJson(j);
    CHECK(round.isError());
    CHECK(round.code == MCP::ErrorCode::InvalidToolInput);
    CHECK(round.data.contains(QStringLiteral("field")));
    CHECK_FALSE(round.data.contains(QStringLiteral("qrenderdocCode")));
  }

  // ---- default-constructed error is not a failure --------------------------
  {
    MCP::Error e;
    CHECK_FALSE(e.isError());
  }

  // ---- error code names ----------------------------------------------------
  {
    CHECK(MCP::ErrorCodeName(MCP::ErrorCode::ParseError) == QStringLiteral("PARSE_ERROR"));
    CHECK(MCP::ErrorCodeName(MCP::ErrorCode::NoCaptureLoaded) == QStringLiteral("NO_CAPTURE_LOADED"));
    CHECK(MCP::ErrorCodeName(MCP::ErrorCode::Timeout) == QStringLiteral("TIMEOUT"));
  }

  // ---- result wrapping -----------------------------------------------------
  {
    QJsonObject structured;
    structured[QStringLiteral("k")] = 1;

    MCP::ToolResult ok = MCP::ToolResult::Success(structured);
    CHECK(ok.ok);
    CHECK_FALSE(ok.text.isEmpty());
    CHECK(ParseJsonObject(ok.text).value(QStringLiteral("k")).toInt() == 1);

    MCP::ToolResult custom = MCP::ToolResult::Success(structured, QStringLiteral("hello"));
    CHECK(custom.text == QStringLiteral("hello"));

    MCP::ToolResult fail =
        MCP::ToolResult::Failure(MCP::Error(MCP::ErrorCode::Cancelled, QStringLiteral("nope")));
    CHECK_FALSE(fail.ok);
    CHECK(fail.text == QStringLiteral("nope"));
    CHECK(fail.error.code == MCP::ErrorCode::Cancelled);
  }
}

////////////////////////////////////////////////////////////////////////////////
// HttpServer: JSON-RPC structural handling and the initialize handshake
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("MCP JSON-RPC structural handling", "[mcp][protocol]")
{
  MCP::ToolRegistry registry;
  MCP::HttpServer server;
  server.SetRegistry(&registry);

  // ---- invalid JSON --------------------------------------------------------
  {
    RpcResult r = Rpc(server, QStringLiteral("{not json"));
    CHECK(ErrorObject(ParseJsonObject(r.json)).value(QStringLiteral("code")).toInt() ==
          int(MCP::ErrorCode::ParseError));
  }

  // ---- non-object JSON -----------------------------------------------------
  {
    RpcResult r = Rpc(server, QStringLiteral("[1,2,3]"));
    CHECK(ErrorObject(ParseJsonObject(r.json)).value(QStringLiteral("code")).toInt() ==
          int(MCP::ErrorCode::ParseError));
  }

  // ---- wrong JSON-RPC version ---------------------------------------------
  {
    RpcResult r = Rpc(server, QStringLiteral("{\"jsonrpc\":\"1.0\",\"id\":9,\"method\":\"ping\"}"));
    CHECK(ErrorObject(ParseJsonObject(r.json)).value(QStringLiteral("code")).toInt() ==
          int(MCP::ErrorCode::InvalidRequest));
  }
}

TEST_CASE("MCP initialize handshake and version negotiation", "[mcp][protocol]")
{
  MCP::ToolRegistry registry;
  MCP::HttpServer server;
  server.SetRegistry(&registry);

  // ---- successful initialize creates a session -----------------------------
  {
    RpcResult r = Rpc(server, InitializeBody());
    CHECK(r.status == 200);
    CHECK_FALSE(r.session.isEmpty());

    const QJsonObject result = ParseJsonObject(r.json).value(QStringLiteral("result")).toObject();
    CHECK(result.value(QStringLiteral("protocolVersion")).toString() ==
          QString::fromUtf8(MCP::kLatestProtocolVersion));
    CHECK(result.value(QStringLiteral("serverInfo")).toObject().value(QStringLiteral("name")).toString() ==
          QString::fromUtf8(MCP::kServerName));
    CHECK(result.value(QStringLiteral("capabilities"))
              .toObject()
              .value(QStringLiteral("tools"))
              .toObject()
              .value(QStringLiteral("listChanged"))
              .toBool() == false);
    CHECK(result.contains(QStringLiteral("instructions")));

    CHECK(server.ActiveSessions() >= 1);
  }

  // ---- unsupported protocol version is rejected up front -------------------
  {
    RpcResult r = Rpc(server, InitializeBody(QStringLiteral("1999-01-01")));
    const QJsonObject error = ErrorObject(ParseJsonObject(r.json));
    CHECK(error.value(QStringLiteral("code")).toInt() ==
          int(MCP::ErrorCode::ProtocolVersionUnsupported));

    const QJsonObject data = error.value(QStringLiteral("data")).toObject();
    CHECK(data.value(QStringLiteral("supported")).toString() ==
          QString::fromUtf8(MCP::kLatestProtocolVersion));
    CHECK(data.value(QStringLiteral("requested")).toString() == QStringLiteral("1999-01-01"));
    CHECK(data.value(QStringLiteral("qrenderdocCode")).toString() ==
          QStringLiteral("PROTOCOL_VERSION_UNSUPPORTED"));
  }

  // ---- initialize as a notification still creates the session --------------
  {
    const QString json = server.HandleJsonRpc(
        QStringLiteral("{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"params\":"
                       "{\"protocolVersion\":\"%1\"}}")
            .arg(QString::fromUtf8(MCP::kLatestProtocolVersion)),
        QString(), NULL, NULL);
    CHECK(json.isEmpty());
  }
}

TEST_CASE("MCP session gating and method dispatch", "[mcp][protocol]")
{
  MCP::ToolRegistry registry;
  MCP::HttpServer server;
  server.SetRegistry(&registry);

  // ---- a session-gated method without a session gets a 404 ----------------
  {
    RpcResult r = Rpc(server, QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}"));
    CHECK(r.status == 404);

    const QJsonObject error = ErrorObject(ParseJsonObject(r.json));
    CHECK(error.value(QStringLiteral("code")).toInt() == int(MCP::ErrorCode::InvalidRequest));
    CHECK(error.value(QStringLiteral("data")).toObject().value(QStringLiteral("header")).toString() ==
          QStringLiteral("Mcp-Session-Id"));
  }

  // Establish a session for the remaining checks.
  QString session;
  {
    const QString json = server.HandleJsonRpc(InitializeBody(), QString(), &session, NULL);
    CHECK_FALSE(json.isEmpty());
    REQUIRE_FALSE(session.isEmpty());
  }

  // ---- ping ----------------------------------------------------------------
  {
    RpcResult r = Rpc(server, QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}"),
                      session);
    const QJsonObject resp = ParseJsonObject(r.json);
    CHECK(resp.contains(QStringLiteral("result")));
    CHECK_FALSE(resp.contains(QStringLiteral("error")));
  }

  // ---- tools/list is empty without a registry-attached tool set ------------
  {
    RpcResult r = Rpc(server, QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\"}"),
                      session);
    const QJsonObject result = ParseJsonObject(r.json).value(QStringLiteral("result")).toObject();
    CHECK(result.value(QStringLiteral("tools")).isArray());
    CHECK(result.value(QStringLiteral("tools")).toArray().size() == registry.Count());
  }

  // ---- unknown method ------------------------------------------------------
  {
    RpcResult r = Rpc(server,
                      QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"no/such/method\"}"),
                      session);
    CHECK(ErrorObject(ParseJsonObject(r.json)).value(QStringLiteral("code")).toInt() ==
          int(MCP::ErrorCode::MethodNotFound));
  }

  // ---- tools/call with a missing tool name --------------------------------
  {
    RpcResult r = Rpc(server,
                      QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
                                     "\"params\":{\"arguments\":{}}}"),
                      session);
    CHECK(ErrorObject(ParseJsonObject(r.json)).value(QStringLiteral("code")).toInt() ==
          int(MCP::ErrorCode::InvalidParams));
  }

  // ---- tools/call for an unregistered tool is a tool-level error ----------
  {
    RpcResult r = Rpc(server,
                      QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
                                     "\"params\":{\"name\":\"does_not_exist\",\"arguments\":{}}}"),
                      session);
    const QJsonObject result = ParseJsonObject(r.json).value(QStringLiteral("result")).toObject();
    CHECK(result.value(QStringLiteral("isError")).toBool() == true);
    CHECK(result.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("code")).toInt() ==
          int(MCP::ErrorCode::MethodNotFound));
    CHECK(result.value(QStringLiteral("content")).toArray().size() == 1);
  }

  // ---- a notification is accepted with no reply body ----------------------
  {
    const QString json = server.HandleJsonRpc(
        QStringLiteral("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"), session,
        NULL, NULL);
    CHECK(json.isEmpty());
  }
}

////////////////////////////////////////////////////////////////////////////////
// QueryAdapter: no capture / replay unavailable
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("MCP query adapter without a capture", "[mcp][capture]")
{
  MCP::QueryAdapter adapter(NULL);

  const MCP::CaptureSnapshot snap = adapter.Snapshot();
  CHECK(snap.hasCapture == false);
  CHECK(snap.loading == false);
  CHECK(snap.filename.isEmpty());
  CHECK(snap.fatalError.isEmpty());

  const QJsonObject snapJson = MCP::QueryAdapter::SnapshotToJson(snap);
  CHECK(snapJson.value(QStringLiteral("hasCapture")).toBool() == false);
  // Fields that only make sense with a capture are omitted entirely.
  CHECK_FALSE(snapJson.contains(QStringLiteral("filename")));

  // Snapshot reads must be idempotent: a client that "switches capture" by
  // re-querying before one is loaded must never crash or see stale state.
  CHECK(adapter.Snapshot().hasCapture == false);

  // ---- capture-dependent snapshot reads fail with NO_CAPTURE_LOADED -------
  const MCP::ToolResult props = adapter.ApiProperties();
  CHECK_FALSE(props.ok);
  CHECK(props.error.code == MCP::ErrorCode::NoCaptureLoaded);

  const MCP::ToolResult frame = adapter.FrameInfo();
  CHECK_FALSE(frame.ok);
  CHECK(frame.error.code == MCP::ErrorCode::NoCaptureLoaded);

  const MCP::ToolResult resources = adapter.Resources();
  CHECK_FALSE(resources.ok);
  CHECK(resources.error.code == MCP::ErrorCode::NoCaptureLoaded);

  // ---- replay dispatch fails distinctly and never runs the callback -------
  bool callbackRan = false;
  MCP::CallContext call;
  const MCP::ToolResult dispatched = adapter.DispatchBlock(
      "test", call, [&](IReplayController &) -> MCP::ToolResult {
        callbackRan = true;
        return MCP::ToolResult::Success(QJsonObject());
      });

  CHECK_FALSE(callbackRan);
  CHECK_FALSE(dispatched.ok);
  CHECK(dispatched.error.code == MCP::ErrorCode::InternalError);
  CHECK(dispatched.error.message.contains(QStringLiteral("no capture context")));

  CHECK(adapter.ReplayAvailable() == false);
}

////////////////////////////////////////////////////////////////////////////////
// End to end: the real registered tools through the full protocol stack
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("MCP core tools end-to-end without a capture", "[mcp][smoke]")
{
  MCP::ToolRegistry registry;
  MCP::QueryAdapter adapter(NULL);
  MCP::RegisterCoreTools(registry, adapter);
  MCP::RegisterExternalToolTools(registry, adapter);
  const int count = registry.Count();

  REQUIRE(count > 0);
  CHECK(registry.Contains(QStringLiteral("renderdoc_getCaptureInfo")));
  CHECK(registry.Contains(QStringLiteral("renderdoc_getReplayStatus")));
  CHECK(registry.Contains(QStringLiteral("renderdoc_getDrawCalls")));
  CHECK(registry.Contains(QStringLiteral("renderdoc_getMaliToolStatus")));
  CHECK(registry.Contains(QStringLiteral("renderdoc_analyzeMaliShader")));
  CHECK(registry.Contains(QStringLiteral("renderdoc_getEnvironmentDiagnostics")));

  MCP::HttpServer server;
  server.SetRegistry(&registry);

  QString session;
  server.HandleJsonRpc(InitializeBody(), QString(), &session, NULL);
  REQUIRE_FALSE(session.isEmpty());

  // ---- tools/list advertises every registered tool ------------------------
  {
    RpcResult r = Rpc(server, QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}"),
                      session);
    const QJsonArray tools =
        ParseJsonObject(r.json).value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
    CHECK(tools.size() == count);

    bool foundCaptureInfo = false;
    for(const QJsonValue &t : tools)
    {
      if(t.toObject().value(QStringLiteral("name")).toString() ==
         QStringLiteral("renderdoc_getCaptureInfo"))
        foundCaptureInfo = true;
      // Every advertised tool must carry a schema object.
      CHECK(t.toObject().value(QStringLiteral("inputSchema")).isObject());
    }
    CHECK(foundCaptureInfo);
  }

  // ---- diagnostics needs only the live configuration context, never replay -
  {
    RpcResult r = Rpc(server,
                      QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
                                     "\"params\":{\"name\":\"renderdoc_getEnvironmentDiagnostics\","
                                     "\"arguments\":{}}}"),
                      session);
    const QJsonObject result = ParseJsonObject(r.json).value(QStringLiteral("result")).toObject();
    CHECK(result.value(QStringLiteral("isError")).toBool() == true);
    const QJsonObject sc = result.value(QStringLiteral("structuredContent")).toObject();
    CHECK(sc.value(QStringLiteral("code")).toInt() == int(MCP::ErrorCode::InternalError));
  }

  // ---- a capture-dependent tool fails cleanly with NO_CAPTURE_LOADED ------
  {
    RpcResult r = Rpc(server,
                      QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                                     "\"params\":{\"name\":\"renderdoc_getCaptureInfo\","
                                     "\"arguments\":{}}}"),
                      session);

    // Tool failures ride in a successful JSON-RPC result, per the MCP convention.
    const QJsonObject result = ParseJsonObject(r.json).value(QStringLiteral("result")).toObject();
    CHECK(result.value(QStringLiteral("isError")).toBool() == true);
    CHECK(result.value(QStringLiteral("content")).toArray().size() == 1);

    const QJsonObject sc = result.value(QStringLiteral("structuredContent")).toObject();
    CHECK(sc.value(QStringLiteral("code")).toInt() == int(MCP::ErrorCode::NoCaptureLoaded));
    CHECK(sc.value(QStringLiteral("data")).toObject().value(QStringLiteral("qrenderdocCode")).toString() ==
          QStringLiteral("NO_CAPTURE_LOADED"));
  }

  // ---- an unregistered tool name is rejected at the tool level ------------
  {
    RpcResult r = Rpc(server,
                      QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                                     "\"params\":{\"name\":\"renderdoc_not_a_tool\","
                                     "\"arguments\":{}}}"),
                      session);
    const QJsonObject result = ParseJsonObject(r.json).value(QStringLiteral("result")).toObject();
    CHECK(result.value(QStringLiteral("isError")).toBool() == true);
    CHECK(result.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("code")).toInt() ==
          int(MCP::ErrorCode::MethodNotFound));
  }
}

////////////////////////////////////////////////////////////////////////////////
// Loopback transport: a real HTTP POST over 127.0.0.1
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("MCP loopback transport smoke test", "[mcp][network]")
{
  // The unit-test harness runs Catch before qrenderdoc constructs its application
  // object, so there is no event dispatcher yet. Socket notifiers and event
  // delivery need one, so create a temporary QCoreApplication for this case only
  // and tear it down afterwards - leaving it alive would collide with the
  // application object qrenderdoc creates after the tests.
  std::unique_ptr<QCoreApplication> owned;
  if(QCoreApplication::instance() == NULL)
  {
    static int argc = 1;
    static char arg0[] = "qrenderdoc";
    static char *argv[] = {arg0, NULL};
    owned.reset(new QCoreApplication(argc, argv));
  }
  REQUIRE(QCoreApplication::instance() != NULL);
  GUIInvoke::init();

  MCP::ToolRegistry registry;
  MCP::QueryAdapter adapter(NULL);
  MCP::RegisterCoreTools(registry, adapter);
  MCP::RegisterExternalToolTools(registry, adapter);

  MCP::HttpServer server;
  server.SetRegistry(&registry);

  // Port 0 lets the OS pick a free ephemeral port, avoiding collisions.
  QString err;
  REQUIRE(server.Start(0, false, err));
  REQUIRE(server.IsRunning());
  const quint16 port = server.Port();
  REQUIRE(port != 0);

  QTcpSocket sock;
  sock.connectToHost(QHostAddress::LocalHost, port);
  REQUIRE(WaitFor([&]() { return sock.state() == QAbstractSocket::ConnectedState; }, 5000));

  // ---- initialize over the wire -------------------------------------------
  const QByteArray initBody = QByteArray(
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
      "{\"protocolVersion\":\"2025-06-18\",\"clientInfo\":{\"name\":\"unit\",\"version\":\"1\"}}}");

  const QByteArray token = server.AccessToken().toUtf8();
  REQUIRE_FALSE(token.isEmpty());
  CHECK_FALSE(server.RequiresToken());

  const QByteArray initResp = HttpRoundTrip(sock, initBody, QByteArray(), QByteArray(), 5000);
  REQUIRE_FALSE(initResp.isEmpty());
  CHECK(initResp.startsWith("HTTP/1.1 200"));

  const QByteArray sessionHeader = HttpResponseHeader(initResp, "mcp-session-id");
  REQUIRE_FALSE(sessionHeader.isEmpty());

  const QJsonObject initJson = ParseJsonObject(QString::fromUtf8(HttpResponseBody(initResp)));
  const QJsonObject initResult = initJson.value(QStringLiteral("result")).toObject();
  CHECK(initResult.value(QStringLiteral("protocolVersion")).toString() ==
        QString::fromUtf8(MCP::kLatestProtocolVersion));
  CHECK(initResult.value(QStringLiteral("serverInfo")).toObject().value(QStringLiteral("name")).toString() ==
        QString::fromUtf8(MCP::kServerName));

  // ---- a second request on the same keep-alive connection -----------------
  const QByteArray listBody = QByteArray("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
  const QByteArray listResp = HttpRoundTrip(sock, listBody, sessionHeader, QByteArray(), 5000);
  REQUIRE_FALSE(listResp.isEmpty());
  CHECK(listResp.startsWith("HTTP/1.1 200"));

  const QJsonObject listJson = ParseJsonObject(QString::fromUtf8(HttpResponseBody(listResp)));
  const QJsonArray tools =
      listJson.value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
  CHECK(tools.size() == registry.Count());
  REQUIRE_FALSE(tools.isEmpty());
  CHECK(tools[0].toObject().value(QStringLiteral("annotations")).toObject().contains(
      QStringLiteral("readOnlyHint")));

  // Path and media-type validation apply even when the default local server does
  // not require a bearer token.
  QTcpSocket rejected;
  rejected.connectToHost(QHostAddress::LocalHost, port);
  REQUIRE(WaitFor([&]() { return rejected.state() == QAbstractSocket::ConnectedState; }, 5000));

  const QByteArray noAuth = HttpRoundTrip(rejected, initBody, QByteArray(), QByteArray(), 5000);
  CHECK(noAuth.startsWith("HTTP/1.1 200"));

  const QByteArray wrongPath =
      HttpRoundTrip(rejected, initBody, QByteArray(), token, 5000, QByteArray("/other"));
  CHECK(wrongPath.startsWith("HTTP/1.1 404"));

  const QByteArray wrongContentType = HttpRoundTrip(rejected, initBody, QByteArray(), token, 5000,
                                                    QByteArray("/mcp"), QByteArray("text/plain"));
  CHECK(wrongContentType.startsWith("HTTP/1.1 415"));

  rejected.disconnectFromHost();
  sock.disconnectFromHost();
  server.Stop();
  CHECK_FALSE(server.IsRunning());

  // Enabling the explicit safer mode restores bearer-token enforcement.
  REQUIRE(server.Start(0, true, err));
  CHECK(server.RequiresToken());

  QTcpSocket secured;
  secured.connectToHost(QHostAddress::LocalHost, server.Port());
  REQUIRE(WaitFor([&]() { return secured.state() == QAbstractSocket::ConnectedState; }, 5000));
  const QByteArray securedNoAuth =
      HttpRoundTrip(secured, initBody, QByteArray(), QByteArray(), 5000);
  CHECK(securedNoAuth.startsWith("HTTP/1.1 401"));

  const QByteArray securedToken = server.AccessToken().toUtf8();
  const QByteArray securedInit =
      HttpRoundTrip(secured, initBody, QByteArray(), securedToken, 5000);
  CHECK(securedInit.startsWith("HTTP/1.1 200"));
  secured.disconnectFromHost();
  server.Stop();
}

TEST_CASE("MCP loopback tool calls are asynchronous and cancellable", "[mcp][network]")
{
  // The async completion path uses GUIInvoke to return socket ownership to the
  // application's event thread. The normal qrenderdoc startup initialises this,
  // while the unit-test harness needs to do so explicitly.
  std::unique_ptr<QCoreApplication> owned;
  if(QCoreApplication::instance() == NULL)
  {
    static int argc = 1;
    static char arg0[] = "qrenderdoc";
    static char *argv[] = {arg0, NULL};
    owned.reset(new QCoreApplication(argc, argv));
  }
  REQUIRE(QCoreApplication::instance() != NULL);
  GUIInvoke::init();

  MCP::ToolRegistry registry;
  QAtomicInt started(0);
  QAtomicInt cancellationObserved(0);

  MCP::ToolDefinition immediate = MakeTool(QStringLiteral("test_immediate"), ObjectSchema());
  registry.Add(immediate, [](const QJsonObject &, MCP::CallContext &) {
    QJsonObject result;
    result[QStringLiteral("value")] = QStringLiteral("completed off socket callback");
    return MCP::ToolResult::Success(result);
  });

  MCP::ToolDefinition blocking = MakeTool(QStringLiteral("test_wait_for_cancel"), ObjectSchema());
  registry.Add(blocking, [&started, &cancellationObserved](const QJsonObject &, MCP::CallContext &call) {
    started.storeRelease(1);
    while(!call.IsCancelled() && !call.IsExpired())
      QThread::msleep(1);

    if(call.IsCancelled())
    {
      cancellationObserved.storeRelease(1);
      return MCP::ToolResult::Failure(
          MCP::Error(MCP::ErrorCode::Cancelled, QStringLiteral("client disconnected")));
    }

    return MCP::ToolResult::Failure(
        MCP::Error(MCP::ErrorCode::Timeout, QStringLiteral("test deadline elapsed")));
  });

  MCP::HttpServer server;
  server.SetRegistry(&registry);
  QString err;
  REQUIRE(server.Start(0, false, err));

  const QByteArray token = server.AccessToken().toUtf8();
  REQUIRE_FALSE(token.isEmpty());

  // A normal tools/call reaches the worker and is delivered back through the
  // UI/socket thread as a regular MCP response.
  QTcpSocket completed;
  completed.connectToHost(QHostAddress::LocalHost, server.Port());
  REQUIRE(WaitFor([&]() { return completed.state() == QAbstractSocket::ConnectedState; }, 5000));
  const QByteArray initResp = HttpRoundTrip(completed, InitializeBody().toUtf8(), QByteArray(), token, 5000);
  const QByteArray session = HttpResponseHeader(initResp, "mcp-session-id");
  REQUIRE_FALSE(session.isEmpty());

  const QByteArray immediateBody = QByteArray(
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{"
      "\"name\":\"test_immediate\",\"arguments\":{}}}");
  const QByteArray immediateResp = HttpRoundTrip(completed, immediateBody, session, token, 5000);
  REQUIRE(immediateResp.startsWith("HTTP/1.1 200"));
  const QJsonObject immediateResult =
      ParseJsonObject(QString::fromUtf8(HttpResponseBody(immediateResp))).value(QStringLiteral("result")).toObject();
  CHECK_FALSE(immediateResult.value(QStringLiteral("isError")).toBool());
  CHECK(immediateResult.value(QStringLiteral("structuredContent"))
            .toObject()
            .value(QStringLiteral("value"))
            .toString() == QStringLiteral("completed off socket callback"));

  // Disconnect during an in-flight request. The tool sees the durable atomic
  // cancellation state, while the server drops its late response safely.
  QTcpSocket cancelled;
  cancelled.connectToHost(QHostAddress::LocalHost, server.Port());
  REQUIRE(WaitFor([&]() { return cancelled.state() == QAbstractSocket::ConnectedState; }, 5000));
  const QByteArray cancelInit = HttpRoundTrip(cancelled, InitializeBody().toUtf8(), QByteArray(), token, 5000);
  const QByteArray cancelSession = HttpResponseHeader(cancelInit, "mcp-session-id");
  REQUIRE_FALSE(cancelSession.isEmpty());

  const QByteArray blockingBody = QByteArray(
      "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{"
      "\"name\":\"test_wait_for_cancel\",\"arguments\":{}}}");
  QByteArray request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                       "Accept: application/json\r\nAuthorization: Bearer ";
  request += token;
  request += "\r\nMcp-Session-Id: ";
  request += cancelSession;
  request += "\r\nContent-Length: ";
  request += QByteArray::number(blockingBody.size());
  request += "\r\n\r\n";
  request += blockingBody;
  cancelled.write(request);
  cancelled.flush();

  REQUIRE(WaitFor([&]() { return started.loadAcquire() != 0; }, 5000));
  cancelled.abort();
  REQUIRE(WaitFor([&]() { return cancellationObserved.loadAcquire() != 0; }, 5000));

  // Server shutdown uses the same durable cancellation state, then joins the
  // bounded worker pool before returning. This models MainWindow teardown,
  // where the registry and capture context are destroyed immediately after Stop.
  started.storeRelease(0);
  cancellationObserved.storeRelease(0);
  QTcpSocket stopping;
  stopping.connectToHost(QHostAddress::LocalHost, server.Port());
  REQUIRE(WaitFor([&]() { return stopping.state() == QAbstractSocket::ConnectedState; }, 5000));
  const QByteArray stopInit = HttpRoundTrip(stopping, InitializeBody().toUtf8(), QByteArray(), token, 5000);
  const QByteArray stopSession = HttpResponseHeader(stopInit, "mcp-session-id");
  REQUIRE_FALSE(stopSession.isEmpty());
  HttpRoundTrip(stopping, blockingBody, stopSession, token, 1);
  REQUIRE(WaitFor([&]() { return started.loadAcquire() != 0; }, 5000));
  server.Stop();
  CHECK(cancellationObserved.loadAcquire() != 0);
  CHECK_FALSE(server.IsRunning());

  completed.disconnectFromHost();
}

TEST_CASE("MCP analysis domain model", "[mcp][analysis]")
{
  using namespace MCP;

  // ---- evidence + confidence ---------------------------------------------
  CHECK(ConfidenceName(Confidence::Confirmed) == QStringLiteral("confirmed"));
  CHECK(ConfidenceName(Confidence::Inferred) == QStringLiteral("inferred"));
  CHECK(ConfidenceName(Confidence::ToVerify) == QStringLiteral("toVerify"));

  {
    const EvidenceRef ev = EventEvidence(42, QStringLiteral("Draw(3)"));
    CHECK(ev.kind == QStringLiteral("event"));
    CHECK(ev.eventId == 42u);
    CHECK(ev.confidence == Confidence::Confirmed);

    const QJsonObject j = EvidenceToJson(ev);
    CHECK(j.value(QStringLiteral("kind")).toString() == QStringLiteral("event"));
    CHECK(j.value(QStringLiteral("eventId")).toInt() == 42);
    // Fields irrelevant to an event evidence must not be serialised.
    CHECK_FALSE(j.contains(QStringLiteral("resourceValue")));
    CHECK_FALSE(j.contains(QStringLiteral("durationUs")));
  }

  {
    const EvidenceRef ev = TimingEvidence(7, 1234.5, QStringLiteral("PS main"));
    CHECK(ev.kind == QStringLiteral("timing"));
    CHECK(ev.eventId == 7u);
    CHECK(ev.durationUs == 1234.5);

    const QJsonObject j = EvidenceToJson(ev);
    CHECK(j.value(QStringLiteral("kind")).toString() == QStringLiteral("timing"));
    CHECK(j.value(QStringLiteral("durationUs")).toDouble() == 1234.5);
  }

  // ---- flags classification ------------------------------------------------
  CHECK(IsRealEffect(ActionFlags::Drawcall));
  CHECK(IsRealEffect(ActionFlags::Dispatch));
  CHECK(IsRealEffect(ActionFlags::MeshDispatch));
  CHECK(IsRealEffect(ActionFlags::Clear));
  CHECK_FALSE(IsRealEffect(ActionFlags::PushMarker));
  CHECK_FALSE(IsRealEffect(ActionFlags::PopMarker));
  CHECK_FALSE(IsRealEffect(ActionFlags::SetMarker));
  CHECK_FALSE(IsRealEffect(ActionFlags::NoFlags));

  // ---- flatten + count ----------------------------------------------------
  ActionDescription root;
  root.eventId = 10;
  root.actionId = 10;
  root.flags = ActionFlags::PushMarker;
  root.customName = rdcstr("Frame");

  ActionDescription draw1;
  draw1.eventId = 11;
  draw1.actionId = 11;
  draw1.flags = ActionFlags::Drawcall;
  draw1.numIndices = 3;
  draw1.numInstances = 1;

  ActionDescription dispatch1;
  dispatch1.eventId = 12;
  dispatch1.actionId = 12;
  dispatch1.flags = ActionFlags::Dispatch;

  ActionDescription clear1;
  clear1.eventId = 13;
  clear1.actionId = 13;
  clear1.flags = ActionFlags::Clear;

  root.children.push_back(draw1);
  root.children.push_back(dispatch1);
  root.children.push_back(clear1);

  CHECK(IsMarkerGroup(root));
  CHECK_FALSE(IsMarkerGroup(draw1));

  rdcarray<ActionDescription> roots;
  roots.push_back(root);

  SDFile sdfile;   // empty structured file: GetName falls back to customName

  rdcarray<EventNode> events;
  FlattenEvents(roots, sdfile, events);

  // Pre-order: root, then its three leaves.
  REQUIRE(events.size() == 4u);
  CHECK(events[0].eventId == 10u);
  CHECK(events[0].parentEventId == 0u);
  CHECK(events[0].depth == 0u);
  CHECK(events[0].childCount == 3u);
  CHECK(events[0].name == QStringLiteral("Frame"));

  CHECK(events[1].eventId == 11u);
  CHECK(events[1].parentEventId == 10u);
  CHECK(events[1].depth == 1u);
  CHECK(events[1].childCount == 0u);
  CHECK(events[1].numIndices == 3u);

  const EventCounts whole = CountEffects(events);
  CHECK(whole.total == 4u);
  CHECK(whole.draws == 1u);
  CHECK(whole.dispatches == 1u);
  CHECK(whole.clears == 1u);
  CHECK(whole.markers == 1u);

  // Leaf-only counting matches the FrameSummary contract: the container marker
  // has childCount != 0 and is therefore excluded from leaf totals.
  rdcarray<EventNode> leaves;
  for(const EventNode &n : events)
  {
    if(n.childCount == 0)
      leaves.push_back(n);
  }
  const EventCounts leafCounts = CountEffects(leaves);
  CHECK(leafCounts.total == 3u);
  CHECK(leafCounts.draws == 1u);
  CHECK(leafCounts.dispatches == 1u);
  CHECK(leafCounts.clears == 1u);
  CHECK(leafCounts.markers == 0u);

  {
    const QJsonObject j = EventCountsToJson(leafCounts);
    CHECK(j.value(QStringLiteral("total")).toInt() == 3);
    CHECK(j.value(QStringLiteral("draws")).toInt() == 1);
    CHECK(j.value(QStringLiteral("markers")).toInt() == 0);

    const QJsonObject nj = EventNodeToJson(events[1]);
    CHECK(nj.value(QStringLiteral("eventId")).toInt() == 11);
    CHECK(nj.value(QStringLiteral("depth")).toInt() == 1);
    // childCount is only emitted when non-zero, keeping leaf nodes compact.
    CHECK_FALSE(nj.contains(QStringLiteral("childCount")));
    CHECK(EventNodeToJson(events[0]).value(QStringLiteral("childCount")).toInt() == 3);
  }

  // ---- statistics serialisation -------------------------------------------
  {
    MCP::FrameStatistics stats;
    stats.frameNumber = 5;
    stats.uncompressedFileSize = 1000;
    stats.compressedFileSize = 250;

    const QJsonObject j = FrameStatisticsToJson(stats);
    CHECK(j.value(QStringLiteral("frameNumber")).toInt() == 5);
    CHECK(j.value(QStringLiteral("uncompressedFileSize")).toDouble() == 1000.0);
    CHECK(j.value(QStringLiteral("compressedFileSize")).toDouble() == 250.0);
  }

  // ---- frame analysis + pass graph JSON shape -----------------------------
  {
    FrameAnalysis analysis;
    analysis.api = QStringLiteral("Vulkan");
    analysis.counts = leafCounts;

    const QJsonObject j = FrameAnalysisToJson(analysis);
    CHECK(j.value(QStringLiteral("api")).toString() == QStringLiteral("Vulkan"));
    REQUIRE(j.contains(QStringLiteral("counts")));
    CHECK(j.value(QStringLiteral("counts")).toObject().value(QStringLiteral("total")).toInt() == 3);
    REQUIRE(j.contains(QStringLiteral("statistics")));
    CHECK(j.value(QStringLiteral("resources")).toObject().value(QStringLiteral("textureCount")).toInt() == 0);
  }

  {
    PassGraph graph;
    PassInfo pass;
    pass.id = QStringLiteral("pass-0");
    pass.name = QStringLiteral("GBuffer");
    pass.eventIdStart = 10;
    pass.eventIdEnd = 20;
    pass.drawCount = 4;
    graph.passes.push_back(pass);

    const QJsonObject j = PassGraphToJson(graph);
    REQUIRE(j.contains(QStringLiteral("passes")));
    const QJsonArray passes = j.value(QStringLiteral("passes")).toArray();
    REQUIRE(passes.size() == 1);
    CHECK(passes[0].toObject().value(QStringLiteral("id")).toString() == QStringLiteral("pass-0"));
    CHECK(passes[0].toObject().value(QStringLiteral("drawCount")).toInt() == 4);
  }
}

TEST_CASE("MCP pass graph and reverse search model", "[mcp][analysis]")
{
  using namespace MCP;

  // ---- resource access + reverse search hit shape ------------------------
  {
    ResourceAccess access;
    access.eventId = 11;
    access.name = QStringLiteral("GBufferAlbedo");
    access.usage = MCP::ResourceUsage::RenderTarget;
    access.confidence = Confidence::Confirmed;

    const QJsonObject j = ResourceAccessToJson(access);
    CHECK(j.value(QStringLiteral("eventId")).toInt() == 11);
    CHECK(j.value(QStringLiteral("name")).toString() == QStringLiteral("GBufferAlbedo"));
    CHECK(j.value(QStringLiteral("usage")).toString() == QStringLiteral("renderTarget"));
    CHECK(j.value(QStringLiteral("confidence")).toString() == QStringLiteral("confirmed"));
  }

  {
    ReverseSearchResult result;
    result.query = QStringLiteral("shader");
    result.term = QStringLiteral("ps_main");
    result.scannedEventCount = 4;

    ReverseSearchHit hit;
    hit.eventId = 22;
    hit.name = QStringLiteral("Draw(3)");
    hit.confidence = Confidence::Confirmed;

    ResourceAccess access;
    access.eventId = 22;
    access.name = QStringLiteral("ps_main");
    access.usage = MCP::ResourceUsage::Sampled;
    hit.resources.push_back(access);

    result.hits.push_back(hit);

    const QJsonObject j = ReverseSearchResultToJson(result);
    CHECK(j.value(QStringLiteral("query")).toString() == QStringLiteral("shader"));
    CHECK(j.value(QStringLiteral("term")).toString() == QStringLiteral("ps_main"));
    CHECK(j.value(QStringLiteral("scannedEventCount")).toInt() == 4);
    CHECK(j.value(QStringLiteral("truncated")).toBool() == false);
    CHECK(j.value(QStringLiteral("matchCount")).toInt() == 1);

    const QJsonArray eventIds = j.value(QStringLiteral("eventIds")).toArray();
    REQUIRE(eventIds.size() == 1);
    CHECK(eventIds[0].toInt() == 22);

    const QJsonArray hits = j.value(QStringLiteral("hits")).toArray();
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].toObject().value(QStringLiteral("eventId")).toInt() == 22);
    const QJsonArray res = hits[0].toObject().value(QStringLiteral("resources")).toArray();
    REQUIRE(res.size() == 1);
    CHECK(res[0].toObject().value(QStringLiteral("usage")).toString() ==
          QStringLiteral("sampled"));
  }

  // ---- pass graph result shape -------------------------------------------
  {
    PassGraphResult graph;
    graph.scannedEventCount = 4;
    graph.truncated = true;

    ResourceEdge edge;
    edge.fromEventId = 11;
    edge.toEventId = 30;
    edge.resourceName = QStringLiteral("GBufferAlbedo");
    edge.usage = QStringLiteral("sampledFromRenderTarget");
    edge.confidence = Confidence::Inferred;
    graph.edges.push_back(edge);

    const QJsonObject j = PassGraphResultToJson(graph);
    CHECK(j.value(QStringLiteral("scannedEventCount")).toInt() == 4);
    CHECK(j.value(QStringLiteral("truncated")).toBool() == true);
    CHECK(j.value(QStringLiteral("edgeCount")).toInt() == 1);

    const QJsonArray edges = j.value(QStringLiteral("edges")).toArray();
    REQUIRE(edges.size() == 1);
    const QJsonObject e = edges[0].toObject();
    CHECK(e.value(QStringLiteral("fromEventId")).toInt() == 11);
    CHECK(e.value(QStringLiteral("toEventId")).toInt() == 30);
    CHECK(e.value(QStringLiteral("resourceName")).toString() == QStringLiteral("GBufferAlbedo"));
    CHECK(e.value(QStringLiteral("usage")).toString() ==
          QStringLiteral("sampledFromRenderTarget"));
    CHECK(e.value(QStringLiteral("confidence")).toString() == QStringLiteral("inferred"));
  }
}

TEST_CASE("MCP resource audit, lifetime, unused and leak model", "[mcp][analysis]")
{
  using namespace MCP;

  // ---- memory audit: facts are confirmed, notes are heuristic -------------
  {
    ResourceMemoryAudit audit;
    audit.totalResources = 3;
    audit.totalBytes = 3u * 1024u * 1024u;
    audit.totalMiB = 3.0;
    audit.textureCount = 1;
    audit.bufferCount = 1;
    audit.shaderCount = 1;
    audit.totalTextureBytes = 2u * 1024u * 1024u;
    audit.totalBufferBytes = 1024u * 1024u;

    ResourceMemoryRow row;
    row.name = QStringLiteral("GBufferAlbedo");
    row.type = QStringLiteral("Texture");
    row.textureType = QStringLiteral("2D");
    row.byteSize = 2u * 1024u * 1024u;
    row.width = 1920;
    row.height = 1080;
    row.mips = 1;
    row.format = QStringLiteral("R8G8B8A8_UNORM");
    audit.largestResources.push_back(row);

    audit.textures.count = 1;
    audit.textures.byFormat.push_back(qMakePair(QStringLiteral("R8G8B8A8_UNORM"), (uint32_t)1));
    audit.textures.mipLevelsAvailable = 0;
    audit.textures.missingMipChain = 1;
    audit.textures.dimensions.push_back(QStringLiteral("1920x1080x1"));
    audit.notes.push_back(QStringLiteral("heuristic note"));
    audit.confidence = Confidence::Confirmed;

    const QJsonObject j = ResourceMemoryAuditToJson(audit);
    CHECK(j.value(QStringLiteral("totalResources")).toInt() == 3);
    CHECK(j.value(QStringLiteral("totalBytes")).toDouble() == double(3u * 1024u * 1024u));
    CHECK(j.value(QStringLiteral("totalMiB")).toDouble() == 3.0);
    CHECK(j.value(QStringLiteral("confidence")).toString() == QStringLiteral("confirmed"));

    const QJsonArray largest = j.value(QStringLiteral("largestResources")).toArray();
    REQUIRE(largest.size() == 1);
    const QJsonObject r = largest[0].toObject();
    CHECK(r.value(QStringLiteral("name")).toString() == QStringLiteral("GBufferAlbedo"));
    CHECK(r.value(QStringLiteral("width")).toInt() == 1920);
    CHECK(r.value(QStringLiteral("format")).toString() == QStringLiteral("R8G8B8A8_UNORM"));

    const QJsonObject ta = j.value(QStringLiteral("textureAudit")).toObject();
    CHECK(ta.value(QStringLiteral("count")).toInt() == 1);
    CHECK(ta.value(QStringLiteral("missingMipChain")).toInt() == 1);
    CHECK(ta.value(QStringLiteral("byFormat")).toObject().value(
              QStringLiteral("R8G8B8A8_UNORM")).toInt() == 1);
    CHECK(ta.value(QStringLiteral("dimensions")).toArray().size() == 1);

    const QJsonArray notes = j.value(QStringLiteral("notes")).toArray();
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].toString() == QStringLiteral("heuristic note"));
  }

  // ---- lifetime rows -----------------------------------------------------
  {
    ResourceLifetimeResult lifetime;
    lifetime.available = true;
    lifetime.scannedEventCount = 6;

    ResourceLifetimeRow row;
    row.name = QStringLiteral("DepthBuffer");
    row.type = QStringLiteral("Texture");
    row.byteSize = 1024;
    row.observedEventCount = 2;
    row.firstEventId = 11;
    row.lastEventId = 30;
    row.confidence = Confidence::Confirmed;

    ResourceAccess access;
    access.eventId = 11;
    access.name = QStringLiteral("DepthBuffer");
    access.usage = MCP::ResourceUsage::RenderTarget;
    row.accesses.push_back(access);
    lifetime.rows.push_back(row);

    const QJsonObject j = ResourceLifetimeToJson(lifetime);
    CHECK(j.value(QStringLiteral("available")).toBool() == true);
    CHECK(j.value(QStringLiteral("scannedEventCount")).toInt() == 6);
    CHECK(j.value(QStringLiteral("rowCount")).toInt() == 1);

    const QJsonArray rows = j.value(QStringLiteral("rows")).toArray();
    REQUIRE(rows.size() == 1);
    const QJsonObject r = rows[0].toObject();
    CHECK(r.value(QStringLiteral("observedEventCount")).toInt() == 2);
    CHECK(r.value(QStringLiteral("firstEventId")).toInt() == 11);
    CHECK(r.value(QStringLiteral("lastEventId")).toInt() == 30);
    CHECK(r.value(QStringLiteral("accesses")).toArray().size() == 1);
  }

  // ---- unused-resource candidates: low confidence + limitation -----------
  {
    UnusedResourceResult unused;
    unused.available = true;
    unused.scannedEventCount = 6;
    unused.confidence = Confidence::Low;
    unused.limitation = QStringLiteral("only bound resources are observed");

    UnusedResource u;
    u.name = QStringLiteral("OrphanTex");
    u.type = QStringLiteral("Texture");
    u.byteSize = 4096;
    unused.candidates.push_back(u);

    const QJsonObject j = UnusedResourceResultToJson(unused);
    CHECK(j.value(QStringLiteral("available")).toBool() == true);
    CHECK(j.value(QStringLiteral("scannedEventCount")).toInt() == 6);
    CHECK(j.value(QStringLiteral("candidateCount")).toInt() == 1);
    CHECK(j.value(QStringLiteral("confidence")).toString() == QStringLiteral("low"));
    CHECK(j.value(QStringLiteral("limitation")).toString() ==
          QStringLiteral("only bound resources are observed"));

    const QJsonArray candidates = j.value(QStringLiteral("candidates")).toArray();
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0].toObject().value(QStringLiteral("name")).toString() ==
          QStringLiteral("OrphanTex"));
  }

  // ---- leak candidates: reason + low confidence --------------------------
  {
    LeakResult leak;
    leak.available = true;
    leak.scannedEventCount = 6;
    leak.confidence = Confidence::Low;
    leak.limitation = QStringLiteral("single frame cannot prove a leak");

    LeakCandidate c;
    c.name = QStringLiteral("Transient");
    c.type = QStringLiteral("Texture");
    c.byteSize = 8192;
    c.createdEventId = 42;
    c.lastEventId = 42;
    c.observedEventCount = 1;
    c.reason = QStringLiteral("single-use");
    c.confidence = Confidence::Low;
    leak.candidates.push_back(c);

    const QJsonObject j = LeakResultToJson(leak);
    CHECK(j.value(QStringLiteral("candidateCount")).toInt() == 1);
    CHECK(j.value(QStringLiteral("confidence")).toString() == QStringLiteral("low"));
    CHECK(j.value(QStringLiteral("limitation")).toString() ==
          QStringLiteral("single frame cannot prove a leak"));

    const QJsonArray candidates = j.value(QStringLiteral("candidates")).toArray();
    REQUIRE(candidates.size() == 1);
    const QJsonObject o = candidates[0].toObject();
    CHECK(o.value(QStringLiteral("name")).toString() == QStringLiteral("Transient"));
    CHECK(o.value(QStringLiteral("observedEventCount")).toInt() == 1);
    CHECK(o.value(QStringLiteral("reason")).toString() == QStringLiteral("single-use"));
    CHECK(o.value(QStringLiteral("confidence")).toString() == QStringLiteral("low"));
  }
}

TEST_CASE("MCP pipeline snapshot and diff model", "[mcp][analysis]")
{
  using namespace MCP;

  // ---- diff result: field/shader changes serialise deterministically -----
  {
    PipelineDiffResult diff;
    diff.available = true;
    diff.eventIdA = 12;
    diff.eventIdB = 40;
    diff.hasChanges = true;
    diff.inspectedStages = QStringList() << QStringLiteral("vertex") << QStringLiteral("pixel");
    diff.confidence = Confidence::Confirmed;
    diff.limitation = QStringLiteral("snapshots are flattened to dotted paths only");

    PipelineFieldChange field;
    field.path = QStringLiteral("rasterizer.cullMode");
    field.group = QStringLiteral("rasterizer");
    field.before = QStringLiteral("Back");
    field.after = QStringLiteral("None");
    field.beforeValue = QStringLiteral("Back");
    field.afterValue = QStringLiteral("None");
    diff.changes.push_back(field);

    PipelineShaderChange shader;
    shader.stage = QStringLiteral("pixel");
    shader.before = QStringLiteral("ps_main_old");
    shader.after = QStringLiteral("ps_main");
    diff.shaderChanges.push_back(shader);

    const QJsonObject j = PipelineDiffResultToJson(diff);
    CHECK(j.value(QStringLiteral("available")).toBool() == true);
    CHECK(j.value(QStringLiteral("eventIdA")).toInt() == 12);
    CHECK(j.value(QStringLiteral("eventIdB")).toInt() == 40);
    CHECK(j.value(QStringLiteral("hasChanges")).toBool() == true);
    CHECK(j.value(QStringLiteral("changeCount")).toInt() == 1);
    CHECK(j.value(QStringLiteral("confidence")).toString() == QStringLiteral("confirmed"));
    CHECK(j.value(QStringLiteral("limitation")).toString() ==
          QStringLiteral("snapshots are flattened to dotted paths only"));

    const QJsonArray stages = j.value(QStringLiteral("inspectedStages")).toArray();
    REQUIRE(stages.size() == 2);
    CHECK(stages[0].toString() == QStringLiteral("vertex"));
    CHECK(stages[1].toString() == QStringLiteral("pixel"));

    const QJsonArray changes = j.value(QStringLiteral("changes")).toArray();
    REQUIRE(changes.size() == 1);
    const QJsonObject c = changes[0].toObject();
    CHECK(c.value(QStringLiteral("path")).toString() == QStringLiteral("rasterizer.cullMode"));
    CHECK(c.value(QStringLiteral("group")).toString() == QStringLiteral("rasterizer"));
    CHECK(c.value(QStringLiteral("before")).toString() == QStringLiteral("Back"));
    CHECK(c.value(QStringLiteral("after")).toString() == QStringLiteral("None"));
    CHECK(c.value(QStringLiteral("beforeValue")).toString() == QStringLiteral("Back"));
    CHECK(c.value(QStringLiteral("afterValue")).toString() == QStringLiteral("None"));

    const QJsonArray shaderChanges = j.value(QStringLiteral("shaderChanges")).toArray();
    REQUIRE(shaderChanges.size() == 1);
    const QJsonObject s = shaderChanges[0].toObject();
    CHECK(s.value(QStringLiteral("stage")).toString() == QStringLiteral("pixel"));
    CHECK(s.value(QStringLiteral("before")).toString() == QStringLiteral("ps_main_old"));
    CHECK(s.value(QStringLiteral("after")).toString() == QStringLiteral("ps_main"));

    // Human-readable messages lead with the shader stage change, then the
    // non-shader field changes.
    const QJsonArray text = j.value(QStringLiteral("changesText")).toArray();
    REQUIRE(text.size() == 2);
    CHECK(text[0].toString() == QStringLiteral("Shader pixel: ps_main_old -> ps_main"));
    CHECK(text[1].toString() == QStringLiteral("rasterizer.cullMode: Back -> None"));
  }

  // ---- diff with no changes keeps changeCount at zero --------------------
  {
    PipelineDiffResult same;
    same.eventIdA = 12;
    same.eventIdB = 12;
    same.hasChanges = false;
    same.confidence = Confidence::Confirmed;

    const QJsonObject j = PipelineDiffResultToJson(same);
    CHECK(j.value(QStringLiteral("hasChanges")).toBool() == false);
    CHECK(j.value(QStringLiteral("changeCount")).toInt() == 0);
    CHECK(j.value(QStringLiteral("changes")).toArray().isEmpty());
    CHECK(j.value(QStringLiteral("changesText")).toArray().isEmpty());
  }

  // ---- stage enumeration: union of both snapshots in canonical order -----
  {
    QJsonObject shaderA;
    shaderA[QStringLiteral("vertex")] = QJsonObject();
    shaderA[QStringLiteral("pixel")] = QJsonObject();

    QJsonObject shaderB;
    shaderB[QStringLiteral("pixel")] = QJsonObject();
    shaderB[QStringLiteral("compute")] = QJsonObject();

    QJsonObject stateA;
    stateA[QStringLiteral("shaders")] = shaderA;
    QJsonObject stateB;
    stateB[QStringLiteral("shaders")] = shaderB;

    const QStringList stages = SnapshotStagesFromPipelineJson(stateA, stateB);
    REQUIRE(stages.size() == 3);
    CHECK(stages[0] == QStringLiteral("vertex"));
    CHECK(stages[1] == QStringLiteral("pixel"));
    CHECK(stages[2] == QStringLiteral("compute"));

    // A stage present in only one snapshot still enumerates.
    QJsonObject shaderOnly;
    shaderOnly[QStringLiteral("mesh")] = QJsonObject();
    QJsonObject stateC;
    stateC[QStringLiteral("shaders")] = shaderOnly;
    const QStringList meshOnly = SnapshotStagesFromPipelineJson(QJsonObject(), stateC);
    REQUIRE(meshOnly.size() == 1);
    CHECK(meshOnly[0] == QStringLiteral("mesh"));

    // Two snapshots with no shaders object yield no stages.
    CHECK(SnapshotStagesFromPipelineJson(QJsonObject(), QJsonObject()).isEmpty());
  }
}

TEST_CASE("MCP structured performance report model", "[mcp][analysis]")
{
  using namespace MCP;

  // ---- timed report: hot passes/events serialise with evidence -----------
  {
    PerformanceReport report;
    report.available = true;
    report.timingAvailable = true;
    report.capturePath = QStringLiteral("D:/caps/frame.rdc");
    report.api = QStringLiteral("Vulkan");
    report.frameNumber = 3;
    report.totalTimedEvents = 2;
    report.totalDurationUs = 5000.0;
    report.hotEventLimit = 20;
    report.confidence = Confidence::Confirmed;

    PerformanceHotPass pass;
    pass.passId = QStringLiteral("GBuffer@1-2");
    pass.name = QStringLiteral("GBuffer");
    pass.firstEventId = 1;
    pass.lastEventId = 2;
    pass.eventCount = 2;
    pass.durationUs = 4000.0;
    pass.confidence = Confidence::Confirmed;
    report.hottestPasses.push_back(pass);

    PerformanceHotEvent hot;
    hot.eventId = 120;
    hot.name = QStringLiteral("Draw #120");
    hot.passId = QStringLiteral("GBuffer@1-2");
    hot.passName = QStringLiteral("GBuffer");
    hot.durationUs = 1500.0;
    hot.share = 0.3;
    hot.pipelineAvailable = true;
    hot.meshAvailable = true;
    hot.confidence = Confidence::Confirmed;
    hot.evidence.push_back(TimingEvidence(120, 1500.0, QStringLiteral("Draw #120")));
    hot.evidence.push_back(ShaderEvidence(ResourceId(), QStringLiteral("ps"), QStringLiteral("ps_main")));
    report.hottestEvents.push_back(hot);

    report.conclusions.confirmed.push_back(
        QStringLiteral("The ranking is based on captured EventGPUDuration values."));
    report.conclusions.toVerify.push_back(QStringLiteral("Inspect the top event ids."));
    report.evidence.push_back(CaptureEvidence(report.capturePath));
    report.evidence.push_back(
        TimingEvidence(0, report.totalDurationUs, QStringLiteral("EventGPUDuration over 2 events")));

    const QJsonObject j = PerformanceReportToJson(report);
    CHECK(j.value(QStringLiteral("available")).toBool() == true);
    CHECK(j.value(QStringLiteral("timingAvailable")).toBool() == true);
    CHECK(j.value(QStringLiteral("capturePath")).toString() == QStringLiteral("D:/caps/frame.rdc"));
    CHECK(j.value(QStringLiteral("api")).toString() == QStringLiteral("Vulkan"));
    CHECK(j.value(QStringLiteral("frameNumber")).toInt() == 3);
    CHECK(j.value(QStringLiteral("totalTimedEvents")).toInt() == 2);
    CHECK(j.value(QStringLiteral("totalDurationUs")).toDouble() == 5000.0);
    CHECK(j.value(QStringLiteral("totalDurationMs")).toDouble() == 5.0);
    CHECK(j.value(QStringLiteral("hotEventLimit")).toInt() == 20);
    CHECK(j.value(QStringLiteral("confidence")).toString() == QStringLiteral("confirmed"));

    const QJsonArray passes = j.value(QStringLiteral("hottestPasses")).toArray();
    REQUIRE(passes.size() == 1);
    const QJsonObject p = passes[0].toObject();
    CHECK(p.value(QStringLiteral("passId")).toString() == QStringLiteral("GBuffer@1-2"));
    CHECK(p.value(QStringLiteral("name")).toString() == QStringLiteral("GBuffer"));
    CHECK(p.value(QStringLiteral("firstEventId")).toInt() == 1);
    CHECK(p.value(QStringLiteral("lastEventId")).toInt() == 2);
    CHECK(p.value(QStringLiteral("eventCount")).toInt() == 2);
    CHECK(p.value(QStringLiteral("durationUs")).toDouble() == 4000.0);
    CHECK(p.value(QStringLiteral("confidence")).toString() == QStringLiteral("confirmed"));

    const QJsonArray events = j.value(QStringLiteral("hottestEvents")).toArray();
    REQUIRE(events.size() == 1);
    const QJsonObject e = events[0].toObject();
    CHECK(e.value(QStringLiteral("eventId")).toInt() == 120);
    CHECK(e.value(QStringLiteral("share")).toDouble() == 0.3);
    CHECK(e.value(QStringLiteral("pipelineAvailable")).toBool() == true);
    CHECK(e.value(QStringLiteral("meshAvailable")).toBool() == true);

    // Every hot event carries the follow-up tools that can expand it.
    const QJsonObject followUp = e.value(QStringLiteral("followUp")).toObject();
    CHECK(followUp.value(QStringLiteral("pipeline")).toString() ==
          QStringLiteral("renderdoc_getPipelineStateAtEvent"));
    CHECK(followUp.value(QStringLiteral("mesh")).toString() == QStringLiteral("renderdoc_getMeshData"));

    const QJsonArray ev = e.value(QStringLiteral("evidence")).toArray();
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].toObject().value(QStringLiteral("kind")).toString() == QStringLiteral("timing"));
    CHECK(ev[0].toObject().value(QStringLiteral("eventId")).toInt() == 120);
    CHECK(ev[0].toObject().value(QStringLiteral("durationUs")).toDouble() == 1500.0);
    CHECK(ev[1].toObject().value(QStringLiteral("kind")).toString() == QStringLiteral("shader"));
    CHECK(ev[1].toObject().value(QStringLiteral("shaderStage")).toString() == QStringLiteral("ps"));

    const QJsonObject conclusions = j.value(QStringLiteral("conclusions")).toObject();
    CHECK(conclusions.value(QStringLiteral("confirmed")).toArray().size() == 1);
    CHECK(conclusions.value(QStringLiteral("inferred")).toArray().isEmpty());
    CHECK(conclusions.value(QStringLiteral("toVerify")).toArray().size() == 1);

    const QJsonArray anchor = j.value(QStringLiteral("evidence")).toArray();
    REQUIRE(anchor.size() == 2);
    CHECK(anchor[0].toObject().value(QStringLiteral("kind")).toString() == QStringLiteral("capture"));
    CHECK(anchor[1].toObject().value(QStringLiteral("kind")).toString() == QStringLiteral("timing"));
    CHECK(anchor[1].toObject().value(QStringLiteral("durationUs")).toDouble() == 5000.0);
  }

  // ---- no GPU timings: report is downgraded to "toVerify" ----------------
  {
    PerformanceReport report;
    report.available = true;
    report.timingAvailable = false;
    report.capturePath = QStringLiteral("D:/caps/frame.rdc");
    report.totalTimedEvents = 0;
    report.confidence = Confidence::ToVerify;
    report.limitation = QStringLiteral("EventGPUDuration is not available in this capture.");
    report.conclusions.toVerify.push_back(
        QStringLiteral("Collect GPU timings for the frame and re-run the report."));

    const QJsonObject j = PerformanceReportToJson(report);
    CHECK(j.value(QStringLiteral("timingAvailable")).toBool() == false);
    CHECK(j.value(QStringLiteral("confidence")).toString() == QStringLiteral("toVerify"));
    CHECK(j.value(QStringLiteral("limitation")).toString() ==
          QStringLiteral("EventGPUDuration is not available in this capture."));
    CHECK(j.value(QStringLiteral("hottestEvents")).toArray().isEmpty());

    const QJsonObject conclusions = j.value(QStringLiteral("conclusions")).toObject();
    CHECK(conclusions.value(QStringLiteral("confirmed")).toArray().isEmpty());
    CHECK(conclusions.value(QStringLiteral("toVerify")).toArray().size() == 1);
  }

  // ---- markdown export leads with the title and lists every section ------
  {
    PerformanceReport report;
    report.timingAvailable = true;
    report.capturePath = QStringLiteral("D:/caps/frame.rdc");
    report.api = QStringLiteral("Vulkan");
    report.totalTimedEvents = 1;
    report.confidence = Confidence::Confirmed;
    report.limitation = QStringLiteral("pass totals sum leaf timings only");

    PerformanceHotPass pass;
    pass.name = QStringLiteral("GBuffer");
    pass.firstEventId = 1;
    pass.lastEventId = 2;
    pass.durationUs = 4000.0;
    report.hottestPasses.push_back(pass);

    PerformanceHotEvent hot;
    hot.eventId = 120;
    hot.name = QStringLiteral("Draw #120");
    hot.durationUs = 1500.0;
    report.hottestEvents.push_back(hot);

    report.evidence.push_back(CaptureEvidence(report.capturePath));
    report.conclusions.toVerify.push_back(QStringLiteral("Inspect the top event ids."));

    const QString md = PerformanceReportToMarkdown(report);
    CHECK(md.startsWith(QStringLiteral("# RenderDoc Performance Report")));
    CHECK(md.contains(QStringLiteral("## Hottest passes")));
    CHECK(md.contains(QStringLiteral("## Hottest events")));
    CHECK(md.contains(QStringLiteral("## Evidence and limitations")));
    CHECK(md.contains(QStringLiteral("GBuffer")));
    CHECK(md.contains(QStringLiteral("EID 120")));
    CHECK(md.contains(QStringLiteral("Follow-up: Inspect the top event ids.")));
    CHECK(md.contains(report.limitation));
  }
}

TEST_CASE("MCP event browser advanced filtering and AI context model", "[mcp][analysis]")
{
  using namespace MCP;

  // ---- flag/name predicates ----------------------------------------------
  CHECK(ActionFlagNames(ActionFlags::Drawcall | ActionFlags::Indexed) ==
        QStringLiteral("Drawcall|Indexed"));
  CHECK(ActionFlagNames(ActionFlags::NoFlags).isEmpty());

  CHECK(IsRealDrawCall(ActionFlags::Drawcall));
  CHECK(IsRealDrawCall(ActionFlags::Dispatch));
  CHECK(IsRealDrawCall(ActionFlags::MeshDispatch));
  CHECK(IsRealDrawCall(ActionFlags::Clear));
  CHECK_FALSE(IsRealDrawCall(ActionFlags::PushMarker));

  CHECK(IsDebugMarkerName(QStringLiteral("Debug Markers")));
  CHECK(IsDebugMarkerName(QStringLiteral("rdoc pass")));
  CHECK_FALSE(IsDebugMarkerName(QStringLiteral("GBuffer")));

  // ---- action tree --------------------------------------------------------
  ActionDescription root;
  root.eventId = 10;
  root.flags = ActionFlags::PushMarker;
  root.customName = rdcstr("Frame");

  ActionDescription dbg;
  dbg.eventId = 11;
  dbg.flags = ActionFlags::PushMarker;
  dbg.customName = rdcstr("Debug Markers");

  ActionDescription dbgDraw;
  dbgDraw.eventId = 12;
  dbgDraw.flags = ActionFlags::Drawcall | ActionFlags::Indexed;
  dbgDraw.numIndices = 3;
  dbgDraw.numInstances = 1;
  dbgDraw.customName = rdcstr("Draw Debug");
  dbg.children.push_back(dbgDraw);

  ActionDescription pass;
  pass.eventId = 20;
  pass.flags = ActionFlags::PushMarker;
  pass.customName = rdcstr("GBuffer");

  ActionDescription draw;
  draw.eventId = 21;
  draw.flags = ActionFlags::Drawcall;
  draw.numIndices = 6;
  draw.numInstances = 1;
  draw.customName = rdcstr("Draw G");

  ActionDescription empty;
  empty.eventId = 22;
  empty.flags = ActionFlags::Dispatch;
  empty.numIndices = 0;
  empty.numInstances = 0;
  empty.customName = rdcstr("Empty Dispatch");

  pass.children.push_back(draw);
  pass.children.push_back(empty);

  root.children.push_back(dbg);
  root.children.push_back(pass);

  CHECK(IsEmptyOperation(empty));
  CHECK_FALSE(IsEmptyOperation(draw));

  rdcarray<ActionDescription> roots;
  roots.push_back(root);

  SDFile sdfile;   // empty structured file: GetName falls back to customName

  const QHash<quint32, double> timings = {{21u, 5000.0}, {12u, 1000.0}};

  // ---- no filters keeps the whole tree in pre-order ----------------------
  {
    EventBrowserFilterOptions opts;
    QVector<EventBrowserContextRow> rows;
    EventBrowserFilterStats stats;
    FilterEventBrowserActions(roots, sdfile, opts, QHash<quint32, double>(), rows, stats);

    REQUIRE(rows.size() == 6u);
    CHECK(stats.totalEvents == 6u);
    CHECK(stats.matchedEvents == 6u);
    CHECK(rows[0].eventId == 10u);
    CHECK(rows[1].eventId == 11u);
    CHECK(rows[2].eventId == 12u);
    CHECK(rows[3].eventId == 20u);
    CHECK(rows[4].eventId == 21u);
    CHECK(rows[5].eventId == 22u);
    CHECK_FALSE(rows[0].hasDuration);
  }

  // ---- excludeMarkers drops containers but promotes their children -------
  {
    EventBrowserFilterOptions opts;
    opts.excludeMarkers = true;
    QVector<EventBrowserContextRow> rows;
    EventBrowserFilterStats stats;
    FilterEventBrowserActions(roots, sdfile, opts, QHash<quint32, double>(), rows, stats);

    REQUIRE(rows.size() == 3u);
    CHECK(stats.totalEvents == 6u);
    CHECK(rows[0].eventId == 12u);
    CHECK(rows[1].eventId == 21u);
    CHECK(rows[2].eventId == 22u);
  }

  // ---- onlyDrawCalls keeps only real effects -----------------------------
  {
    EventBrowserFilterOptions opts;
    opts.onlyDrawCalls = true;
    QVector<EventBrowserContextRow> rows;
    EventBrowserFilterStats stats;
    FilterEventBrowserActions(roots, sdfile, opts, QHash<quint32, double>(), rows, stats);

    REQUIRE(rows.size() == 3u);
    CHECK(rows[0].eventId == 12u);
    CHECK(rows[1].eventId == 21u);
    CHECK(rows[2].eventId == 22u);
  }

  // ---- excludeEmptyOperations drops the zero-work dispatch ---------------
  {
    EventBrowserFilterOptions opts;
    opts.excludeEmptyOperations = true;
    QVector<EventBrowserContextRow> rows;
    EventBrowserFilterStats stats;
    FilterEventBrowserActions(roots, sdfile, opts, QHash<quint32, double>(), rows, stats);

    REQUIRE(rows.size() == 5u);
    for(const EventBrowserContextRow &r : rows)
      CHECK(r.eventId != 22u);
  }

  // ---- markerFilter scopes to a subtree and keeps its ancestors ----------
  {
    EventBrowserFilterOptions opts;
    opts.markerFilter = QStringLiteral("GBuffer");
    QVector<EventBrowserContextRow> rows;
    EventBrowserFilterStats stats;
    FilterEventBrowserActions(roots, sdfile, opts, QHash<quint32, double>(), rows, stats);

    REQUIRE(rows.size() == 4u);
    CHECK(rows[0].eventId == 10u);   // ancestor kept because a descendant matched
    CHECK(rows[1].eventId == 20u);
    CHECK(rows[2].eventId == 21u);
    CHECK(rows[3].eventId == 22u);
    for(const EventBrowserContextRow &r : rows)
      CHECK(r.eventId != 11u);       // debug subtree excluded
  }

  // ---- excludeDebugMarkers drops the debug group, promotes its draw ------
  {
    EventBrowserFilterOptions opts;
    opts.excludeDebugMarkers = true;
    QVector<EventBrowserContextRow> rows;
    EventBrowserFilterStats stats;
    FilterEventBrowserActions(roots, sdfile, opts, QHash<quint32, double>(), rows, stats);

    REQUIRE(rows.size() == 5u);
    for(const EventBrowserContextRow &r : rows)
      CHECK(r.eventId != 11u);       // only the debug marker group is removed
    CHECK(rows[1].eventId == 12u);   // its draw survives
  }

  // ---- event-id bounds ----------------------------------------------------
  {
    EventBrowserFilterOptions opts;
    opts.eventIdMin = 20;
    QVector<EventBrowserContextRow> rows;
    EventBrowserFilterStats stats;
    FilterEventBrowserActions(roots, sdfile, opts, QHash<quint32, double>(), rows, stats);

    // Event-id bounds apply to every node. The debug draw (12) is below the
    // lower bound too, so only the GBuffer marker and its two effects remain.
    REQUIRE(rows.size() == 3u);
    CHECK(rows[0].eventId == 20u);
    CHECK(rows[1].eventId == 21u);
    CHECK(rows[2].eventId == 22u);
  }

  // ---- sortByDuration orders hottest first, untimed last -----------------
  {
    EventBrowserFilterOptions opts;
    opts.sortByDuration = true;
    QVector<EventBrowserContextRow> rows;
    EventBrowserFilterStats stats;
    FilterEventBrowserActions(roots, sdfile, opts, timings, rows, stats);

    REQUIRE(rows.size() == 6u);
    CHECK(rows[0].eventId == 21u);   // 5000us
    CHECK(rows[0].hasDuration);
    CHECK(rows[0].durationUs == 5000.0);
    CHECK(rows[1].eventId == 12u);   // 1000us
    CHECK_FALSE(rows[2].hasDuration);   // remaining events carry no timing
  }

  // ---- JSON + AI context rendering ---------------------------------------
  {
    EventBrowserContext ctx;
    ctx.capturePath = QStringLiteral("D:/caps/frame.rdc");
    ctx.api = QStringLiteral("Vulkan");
    ctx.frameNumber = 1;
    ctx.filters.onlyDrawCalls = true;
    ctx.filters.markerFilter = QStringLiteral("GBuffer");
    ctx.totalEvents = 6;
    ctx.matchedEvents = 2;
    ctx.confidence = Confidence::Confirmed;

    EventBrowserContextRow r0;
    r0.eventId = 21;
    r0.name = QStringLiteral("Draw G");
    r0.flags = QStringLiteral("Drawcall");
    r0.numIndices = 6;
    r0.numInstances = 1;
    r0.durationUs = 5000.0;
    r0.hasDuration = true;
    ctx.rows.push_back(r0);

    EventBrowserContextRow r1;
    r1.eventId = 22;
    r1.name = QStringLiteral("Empty Dispatch");
    r1.flags = QStringLiteral("Dispatch");
    ctx.rows.push_back(r1);

    const QJsonObject j = EventBrowserContextToJson(ctx);
    CHECK(j.value(QStringLiteral("available")).toBool());
    CHECK(j.value(QStringLiteral("capturePath")).toString() == QStringLiteral("D:/caps/frame.rdc"));
    CHECK(j.value(QStringLiteral("totalEvents")).toInt() == 6);
    CHECK(j.value(QStringLiteral("matchedEvents")).toInt() == 2);
    CHECK(j.value(QStringLiteral("confidence")).toString() == QStringLiteral("confirmed"));
    CHECK_FALSE(j.value(QStringLiteral("truncated")).toBool());

    const QJsonObject filters = j.value(QStringLiteral("filters")).toObject();
    CHECK(filters.value(QStringLiteral("onlyDrawCalls")).toBool());
    CHECK(filters.value(QStringLiteral("markerFilter")).toString() == QStringLiteral("GBuffer"));

    const QJsonArray events = j.value(QStringLiteral("events")).toArray();
    REQUIRE(events.size() == 2);
    CHECK(events[0].toObject().value(QStringLiteral("eventId")).toInt() == 21);
    CHECK(events[0].toObject().value(QStringLiteral("durationUs")).toDouble() == 5000.0);
    CHECK_FALSE(events[1].toObject().contains(QStringLiteral("durationUs")));

    const QString text = EventBrowserContextToText(ctx);
    CHECK(text.startsWith(QStringLiteral("RenderDoc event context: 2 filtered event(s) of 6.")));
    CHECK(text.contains(QStringLiteral("Vulkan")));
    CHECK(text.contains(QStringLiteral("marker=\"GBuffer\"")));
    CHECK(text.contains(QStringLiteral("EID 21")));
    CHECK(text.contains(QStringLiteral("5000.0us")));
    CHECK(text.contains(QStringLiteral("request pipeline/shader/resource evidence")));
  }
}

// P1-8: the extended bookmark model keeps `note` in the native capture bookmark
// so a stock build degrades to a plain bookmark, while the extended half lives in
// a versioned sidecar that a stock build simply never looks at. These checks pin
// the compatibility boundary (defensive parse, capture-wins merge, non-empty-wins
// upsert) rather than any UI behaviour.
TEST_CASE("MCP investigation bookmark model", "[mcp][analysis]")
{
  using namespace MCP;

  // ---- sidecar path derivation -------------------------------------------
  CHECK(BookmarkSidecarPath(QStringLiteral("D:/caps/frame.rdc")) ==
        QStringLiteral("D:/caps/frame.rdc.bookmarks.json"));
  CHECK(BookmarkSidecarPath(QString()).isEmpty());

  // ---- extended-field predicate ------------------------------------------
  // A note-only record is exactly what a plain capture bookmark can hold, so it
  // must NOT be considered "extended" (no sidecar needed for it to survive).
  BookmarkTaggedItem noteOnly;
  noteOnly.note = QStringLiteral("check the depth resolve");
  CHECK_FALSE(BookmarkHasExtendedFields(noteOnly));

  BookmarkTaggedItem titled = noteOnly;
  titled.title = QStringLiteral("Depth resolve suspicion");
  CHECK(BookmarkHasExtendedFields(titled));

  BookmarkTaggedItem shot = noteOnly;
  shot.screenshotPath = QStringLiteral("D:/caps/shots/123.png");
  CHECK(BookmarkHasExtendedFields(shot));

  // ---- serialize/parse round-trip ----------------------------------------
  BookmarkFile file;
  BookmarkTaggedItem a;
  a.id = QStringLiteral("bm-21");
  a.eventId = QStringLiteral("21");
  a.title = QStringLiteral("GBuffer overdraw");
  a.note = QStringLiteral("draw G writes four targets");
  a.conclusion = QStringLiteral("candidate for depth pre-pass");
  a.analysis = QStringLiteral("four MRT writes at 4K");
  a.screenshotPath = QStringLiteral("D:/caps/shots/gbuffer.png");
  a.confidence = Confidence::Inferred;
  file.items.append(a);

  BookmarkTaggedItem b;
  b.id = QStringLiteral("bm-30");
  b.eventId = QStringLiteral("30");
  b.note = QStringLiteral("plain note only");
  b.confidence = Confidence::ToVerify;
  file.items.append(b);

  const QByteArray serialized = SerializeBookmarkFile(file);
  const BookmarkFile parsed = ParseBookmarkFile(serialized);
  CHECK(parsed.version == 1);
  REQUIRE(parsed.items.size() == 2);
  CHECK(parsed.items[0].id == QStringLiteral("bm-21"));
  CHECK(parsed.items[0].eventId == QStringLiteral("21"));
  CHECK(parsed.items[0].title == QStringLiteral("GBuffer overdraw"));
  CHECK(parsed.items[0].note == QStringLiteral("draw G writes four targets"));
  CHECK(parsed.items[0].conclusion == QStringLiteral("candidate for depth pre-pass"));
  CHECK(parsed.items[0].screenshotPath == QStringLiteral("D:/caps/shots/gbuffer.png"));
  CHECK(parsed.items[0].confidence == Confidence::Inferred);
  CHECK(parsed.items[1].confidence == Confidence::ToVerify);
  CHECK(parsed.items[1].analysis.isEmpty());
  // confidence round-trips by name, not by enum ordinal
  CHECK(ConfidenceName(parsed.items[0].confidence) == QStringLiteral("inferred"));

  // ---- defensive parse: never throws, never blocks a capture load ---------
  CHECK(ParseBookmarkFile(QByteArray()).items.isEmpty());
  CHECK(ParseBookmarkFile(QByteArray("{ not json")).items.isEmpty());
  // a JSON array (or any non-object document) is not a sidecar
  CHECK(ParseBookmarkFile(QByteArray("[1,2,3]")).items.isEmpty());
  // an unknown version still loads every record it can understand, and rows that
  // are not objects are dropped rather than producing a malformed record
  const BookmarkFile weird = ParseBookmarkFile(
      QByteArray("{\"version\":99,\"bookmarks\":[{\"id\":\"bm-x\",\"eventId\":\"7\"},42]}"));
  CHECK(weird.version == 99);
  REQUIRE(weird.items.size() == 1);
  CHECK(weird.items[0].id == QStringLiteral("bm-x"));
  CHECK(weird.items[0].eventId == QStringLiteral("7"));
  CHECK(weird.items[0].confidence == Confidence::Confirmed);

  // ---- merge: capture wins for the note, sidecar fills the extended half --
  rdcarray<EventBookmark> bookmarks;
  EventBookmark native0(21);
  native0.text = rdcstr("draw G writes four targets");    // binds sidecar `a` by eid
  bookmarks.push_back(native0);

  EventBookmark native1(40);
  native1.text = rdcstr("renamed in the event browser");    // no sidecar entry
  bookmarks.push_back(native1);

  const QVector<BookmarkTaggedItem> merged = MergeBookmarkSidecar(bookmarks, parsed);
  REQUIRE(merged.size() == 3);    // two capture bookmarks + one sidecar-only record
  CHECK(merged[0].eventId == QStringLiteral("21"));
  CHECK(merged[0].id == QStringLiteral("bm-21"));
  CHECK(merged[0].title == QStringLiteral("GBuffer overdraw"));
  CHECK(merged[0].note == QStringLiteral("draw G writes four targets"));
  CHECK(merged[0].conclusion == QStringLiteral("candidate for depth pre-pass"));
  CHECK(merged[0].confidence == Confidence::Inferred);
  CHECK(merged[1].eventId == QStringLiteral("40"));
  CHECK(merged[1].id == QStringLiteral("bm-40"));
  CHECK(merged[1].note == QStringLiteral("renamed in the event browser"));
  CHECK(merged[1].confidence == Confidence::Confirmed);
  // sidecar-only record (bm-30) is appended rather than dropped
  CHECK(merged[2].id == QStringLiteral("bm-30"));
  CHECK(merged[2].eventId == QStringLiteral("30"));

  // capture text wins over a stale sidecar note
  rdcarray<EventBookmark> wins;
  EventBookmark w(21);
  w.text = rdcstr("text edited in the event browser");
  wins.push_back(w);
  BookmarkFile stale;
  BookmarkTaggedItem s;
  s.id = QStringLiteral("bm-21");
  s.eventId = QStringLiteral("21");
  s.note = QStringLiteral("stale note");
  s.title = QStringLiteral("kept title");
  stale.items.append(s);
  const QVector<BookmarkTaggedItem> wm = MergeBookmarkSidecar(wins, stale);
  REQUIRE(wm.size() == 1);
  CHECK(wm[0].note == QStringLiteral("text edited in the event browser"));
  CHECK(wm[0].title == QStringLiteral("kept title"));

  // a sidecar record with no event id still binds by exact note text
  rdcarray<EventBookmark> byNote;
  EventBookmark nb(50);
  nb.text = rdcstr("note match");
  byNote.push_back(nb);
  BookmarkFile noteSide;
  BookmarkTaggedItem ns;
  ns.id = QStringLiteral("bm-note");
  ns.note = QStringLiteral("note match");
  ns.conclusion = QStringLiteral("bound by note");
  noteSide.items.append(ns);
  const QVector<BookmarkTaggedItem> nm = MergeBookmarkSidecar(byNote, noteSide);
  REQUIRE(nm.size() == 1);
  CHECK(nm[0].id == QStringLiteral("bm-note"));
  CHECK(nm[0].conclusion == QStringLiteral("bound by note"));

  // ---- JSON shape and prose block ----------------------------------------
  BookmarkSidecar sidecar;
  sidecar.available = true;
  sidecar.sidecarPresent = true;
  sidecar.version = 1;
  sidecar.capturePath = QStringLiteral("D:/caps/frame.rdc");
  sidecar.sidecarPath = QStringLiteral("D:/caps/frame.rdc.bookmarks.json");
  sidecar.items = parsed.items;

  const QJsonObject sj = BookmarkSidecarToJson(sidecar);
  CHECK(sj.value(QStringLiteral("available")).toBool());
  CHECK(sj.value(QStringLiteral("sidecarPresent")).toBool());
  CHECK(sj.value(QStringLiteral("version")).toInt() == 1);
  CHECK(sj.value(QStringLiteral("count")).toInt() == 2);
  const QJsonArray sarr = sj.value(QStringLiteral("bookmarks")).toArray();
  REQUIRE(sarr.size() == 2);
  CHECK(sarr[0].toObject().value(QStringLiteral("eventId")).toInt() == 21);
  CHECK(sarr[0].toObject().value(QStringLiteral("confidence")).toString() ==
        QStringLiteral("inferred"));

  const QString text = BookmarkSidecarToText(sidecar);
  CHECK(text.startsWith(QStringLiteral("RenderDoc investigation bookmarks: 2 record(s).")));
  CHECK(text.contains(QStringLiteral("Capture: D:/caps/frame.rdc")));
  CHECK(text.contains(QStringLiteral("Sidecar: D:/caps/frame.rdc.bookmarks.json")));
  CHECK(text.contains(QStringLiteral("[bm-21]")));
  CHECK(text.contains(QStringLiteral("EID 21")));
  CHECK(text.contains(QStringLiteral("\"GBuffer overdraw\"")));
  CHECK(text.contains(QStringLiteral("=> candidate for depth pre-pass")));
  CHECK(text.contains(QStringLiteral("note: draw G writes four targets")));
  CHECK(text.contains(QStringLiteral("analysis: four MRT writes at 4K")));
  CHECK(text.contains(QStringLiteral("screenshot: D:/caps/shots/gbuffer.png")));
  CHECK(text.contains(QStringLiteral("confidence: inferred")));

  // the degraded states explain *why* the extended half is missing
  BookmarkSidecar unloaded;
  CHECK(BookmarkSidecarToText(unloaded)
            .contains(QStringLiteral("capture not open locally")));

  BookmarkSidecar noSidecar;
  noSidecar.available = true;
  CHECK(BookmarkSidecarToText(noSidecar)
            .contains(QStringLiteral("no extended bookmark sidecar yet")));
}

// P1-9: the whole-investigation report model. These cover the composition rules
// (bookmarks + performance + resource audit), the degradation rules (an
// unavailable half becomes a stated limitation rather than a silent zero) and
// the two renderings (JSON shape and Markdown section titles) without needing a
// capture, a replay controller or any Qt widget.
TEST_CASE("MCP investigation report composition and rendering", "[mcp][analysis]")
{
  using namespace MCP;

  // ---- a fully populated report: every section present and factual
  InvestigationReport report;
  report.available = true;
  report.generatedAt = QStringLiteral("2026-01-02T03:04:05Z");
  report.capturePath = QStringLiteral("D:/caps/frame.rdc");
  report.api = QStringLiteral("Vulkan");
  report.frameNumber = 7;
  report.currentEvent = 412;

  BookmarkTaggedItem b0;
  b0.id = QStringLiteral("bm-412");
  b0.eventId = QStringLiteral("412");
  b0.title = QStringLiteral("GBuffer overdraw");
  b0.conclusion = QStringLiteral("candidate for depth pre-pass");
  b0.confidence = Confidence::Inferred;
  report.bookmarks.append(b0);
  report.bookmarksAvailable = true;

  report.performance.available = true;
  report.performance.timingAvailable = true;
  PerformanceHotEvent hot;
  hot.eventId = 412;
  hot.name = QStringLiteral("DrawIndexed(2048)");
  hot.passId = QStringLiteral("GBuffer@410-430");
  hot.durationUs = 1430.0;
  hot.evidence.push_back(TimingEvidence(412, 1430.0, hot.name));
  report.performance.hottestEvents.push_back(hot);

  report.resources.totalResources = 3;
  report.resources.textureCount = 2;
  report.resources.bufferCount = 1;
  report.resources.shaderCount = 0;
  report.resources.totalMiB = 4.0;
  ResourceMemoryRow big;
  big.name = QStringLiteral("Albedo 4096x4096");
  big.type = QStringLiteral("Texture");
  big.byteSize = 4194304;
  report.resources.largestResources.append(big);
  report.resources.notes.append(QStringLiteral("Byte sizes are creation-time estimates."));

  report.evidence.push_back(CaptureEvidence(report.capturePath));
  report.confidence = Confidence::Confirmed;

  // Limitations: the caller-supplied list is kept, the two derived reasons are
  // added once each and never duplicated.
  report.limitations.append(QStringLiteral("Custom caveat."));
  report.limitations.append(QStringLiteral("Resource byte sizes are creation-time estimates."));
  const QStringList merged = InvestigationReportLimitations(report);
  CHECK(merged.contains(QStringLiteral("Custom caveat.")));
  CHECK(merged.count(QStringLiteral("Resource byte sizes are creation-time estimates.")) == 1);

  // ---- JSON shape
  const QJsonObject j = InvestigationReportToJson(report);
  CHECK(j.value(QStringLiteral("available")).toBool());
  CHECK(j.value(QStringLiteral("generatedAt")).toString() == QStringLiteral("2026-01-02T03:04:05Z"));
  CHECK(j.value(QStringLiteral("capturePath")).toString() == QStringLiteral("D:/caps/frame.rdc"));
  CHECK(j.value(QStringLiteral("api")).toString() == QStringLiteral("Vulkan"));
  CHECK(j.value(QStringLiteral("frameNumber")).toInt() == 7);
  CHECK(j.value(QStringLiteral("currentEvent")).toInt() == 412);
  CHECK(j.value(QStringLiteral("bookmarkCount")).toInt() == 1);
  CHECK(j.value(QStringLiteral("bookmarksAvailable")).toBool());
  CHECK_FALSE(j.contains(QStringLiteral("bookmarkLimitation")));
  CHECK(j.value(QStringLiteral("confidence")).toString() == QStringLiteral("confirmed"));
  REQUIRE(j.value(QStringLiteral("bookmarks")).toArray().size() == 1);
  CHECK(j.value(QStringLiteral("bookmarks"))
            .toArray()[0]
            .toObject()
            .value(QStringLiteral("title"))
            .toString() == QStringLiteral("GBuffer overdraw"));
  CHECK(j.value(QStringLiteral("performance")).isObject());
  CHECK(j.value(QStringLiteral("resources")).isObject());
  CHECK(j.value(QStringLiteral("evidence")).toArray().size() >= 1);
  CHECK(j.value(QStringLiteral("limitations")).toArray().size() >= 1);

  // ---- Markdown rendering keeps the section titles the plugin contract uses
  const QString md = InvestigationReportToMarkdown(report);
  CHECK(md.startsWith(QStringLiteral("# RenderDoc Investigation Report")));
  CHECK(md.contains(QStringLiteral("- Generated: 2026-01-02T03:04:05Z")));
  CHECK(md.contains(QStringLiteral("- Capture: D:/caps/frame.rdc")));
  CHECK(md.contains(QStringLiteral("- API: Vulkan")));
  CHECK(md.contains(QStringLiteral("- Frame: 7")));
  CHECK(md.contains(QStringLiteral("- Resources: 3 (4.00 MiB)")));
  CHECK(md.contains(QStringLiteral("## Bookmarks")));
  CHECK(md.contains(QStringLiteral("**GBuffer overdraw** (EID 412) - candidate for depth pre-pass")));
  CHECK(md.contains(QStringLiteral("## Performance hotspots")));
  CHECK(md.contains(QStringLiteral("EID 412: DrawIndexed(2048) - 1430.00 us")));
  CHECK(md.contains(QStringLiteral("## Resource footprint")));
  CHECK(md.contains(QStringLiteral("Albedo 4096x4096 (Texture): 4194304 bytes")));
  CHECK(md.contains(QStringLiteral("## Limitations")));
  CHECK(md.contains(QStringLiteral("- Custom caveat.")));

  // ---- degraded: no bookmarks, no GPU timings, audit skipped. Each missing half
  // becomes an explicit, de-duplicated limitation instead of an implicit zero.
  InvestigationReport degraded;
  degraded.generatedAt = QStringLiteral("2026-01-02T03:04:05Z");
  degraded.capturePath = QStringLiteral("D:/caps/frame.rdc");
  degraded.api = QStringLiteral("OpenGL");
  degraded.bookmarksAvailable = false;
  degraded.bookmarkLimitation = QStringLiteral("capture has no local filename");
  degraded.performance.timingAvailable = false;
  degraded.resources.totalResources = 0;
  degraded.resources.notes.append(QStringLiteral("Byte sizes are creation-time estimates."));
  degraded.confidence = Confidence::ToVerify;

  const QStringList dlim = InvestigationReportLimitations(degraded);
  CHECK(dlim.contains(QStringLiteral("capture has no local filename")));
  CHECK(dlim.contains(QStringLiteral("Resource byte sizes are creation-time estimates.")));
  bool sawTiming = false;
  for(const QString &s : dlim)
  {
    if(s.contains(QStringLiteral("GPU timings are unavailable")))
      sawTiming = true;
  }
  CHECK(sawTiming);

  const QString dmd = InvestigationReportToMarkdown(degraded);
  CHECK(dmd.contains(QStringLiteral("- capture has no local filename")));
  CHECK(dmd.contains(QStringLiteral(
      "- GPU timings unavailable; treat performance conclusions as hypotheses.")));
  CHECK_FALSE(dmd.contains(QStringLiteral("## Bookmarks\n- None")));

  // a report with nothing to say still renders the four sections with sane bodies
  InvestigationReport empty;
  empty.bookmarksAvailable = true;
  empty.performance.timingAvailable = true;
  const QString emd = InvestigationReportToMarkdown(empty);
  CHECK(emd.contains(QStringLiteral("## Bookmarks\n- None")));
  CHECK(emd.contains(QStringLiteral("## Performance hotspots\n- None")));
  CHECK(emd.contains(QStringLiteral("## Limitations\n- None")));
}

// P2-1: cross-capture normalization. These pin the individual normalization
// rules so every later regression diff can be explained in terms of them.
TEST_CASE("MCP cross-capture name normalization", "[mcp][analysis]")
{
  using namespace MCP;

  const NormalizationOptions def;

  CHECK(NormalizeName(QStringLiteral("GBuffer"), def) == QStringLiteral("gbuffer"));
  CHECK(NormalizeName(QStringLiteral("  Albedo   Map  "), def) == QStringLiteral("albedo map"));
  CHECK(NormalizeName(QStringLiteral("Albedo (12)"), def) == QStringLiteral("albedo"));
  CHECK(NormalizeName(QStringLiteral("Albedo[3]"), def) == QStringLiteral("albedo"));
  CHECK(NormalizeName(QStringLiteral("Albedo 7"), def) == QStringLiteral("albedo"));
  CHECK(NormalizeName(QStringLiteral("Shaders/Opaque.hlsl"), def) == QStringLiteral("opaque"));
  CHECK(NormalizeName(QStringLiteral("ShadowMap 4096x4096"), def) ==
        QStringLiteral("shadowmap res"));
  CHECK(NormalizeName(QStringLiteral("Albedo0"), def) == QStringLiteral("albedo#"));
  CHECK(NormalizeName(QString(), def) == QStringLiteral("<anon>"));

  // A hex literal must not be mistaken for a resolution; it is only collapsed by
  // the (explicitly optional) digit-run rule.
  CHECK(NormalizeName(QStringLiteral("Const 0x1a"), def) == QStringLiteral("const #x#a"));
  NormalizationOptions noDigits = def;
  noDigits.stripNumericSuffix = false;
  CHECK(NormalizeName(QStringLiteral("Const 0x1a"), noDigits) == QStringLiteral("const 0x1a"));

  // With every rule off the display name is preserved verbatim.
  NormalizationOptions raw;
  raw.caseInsensitive = false;
  raw.stripExtension = false;
  raw.stripPath = false;
  raw.stripInstanceSuffix = false;
  raw.stripResolutionSuffix = false;
  raw.stripNumericSuffix = false;
  raw.replaceEmptyWithAnon = false;
  CHECK(NormalizeName(QStringLiteral("Albedo (12)"), raw) == QStringLiteral("Albedo (12)"));
  CHECK(NormalizeName(QString(), raw).isEmpty());

  // Event ids: leading zeros are presentation only, ranges survive intact.
  CHECK(NormalizeEventKey(QStringLiteral("0412"), def) == QStringLiteral("412"));
  CHECK(NormalizeEventKey(QStringLiteral(" 0412 - 0430 "), def) == QStringLiteral("412-430"));

  uint32_t first = 0;
  uint32_t last = 0;
  CHECK(ParseEventRange(QStringLiteral("0412"), first, last));
  CHECK((first == 412 && last == 412));
  CHECK(ParseEventRange(QStringLiteral("412-430"), first, last));
  CHECK((first == 412 && last == 430));
  CHECK_FALSE(ParseEventRange(QStringLiteral("shadow"), first, last));
  CHECK_FALSE(ParseEventRange(QStringLiteral("1-2-3"), first, last));

  // The capture's own identity survives the file being copied elsewhere.
  const QString k1 = UnifyCaptureKey(QStringLiteral("Vulkan"), QStringLiteral("D:/a/frame.rdc"));
  const QString k2 = UnifyCaptureKey(QStringLiteral("Vulkan"), QStringLiteral("C:/b/frame.rdc"));
  CHECK(k1 == k2);
  CHECK(k1.contains(QStringLiteral("frame")));
}

static MCP::EventNode MakeMatchTestNode(uint32_t eventId, uint32_t parent, uint32_t children,
                                        ActionFlags flags, const char *name)
{
  MCP::EventNode n;
  n.eventId = eventId;
  n.actionId = eventId;
  n.parentEventId = parent;
  n.childCount = children;
  n.flags = flags;
  n.name = QString::fromUtf8(name);
  return n;
}

// P2-1: canonical event identity. Event ids are unstable across captures, so the
// identity is the normalized marker path plus a per-label occurrence ordinal -
// which is how two identically named passes ("Shadow pass") and two duplicate
// draws stay distinct instead of collapsing into one key.
TEST_CASE("MCP cross-capture canonical event identity", "[mcp][analysis]")
{
  using namespace MCP;

  rdcarray<EventNode> nodes;
  nodes.push_back(MakeMatchTestNode(10, 0, 2, ActionFlags::NoFlags, "Frame"));
  nodes.push_back(MakeMatchTestNode(20, 10, 1, ActionFlags::NoFlags, "Shadow pass"));
  nodes.push_back(MakeMatchTestNode(30, 20, 0, ActionFlags::Drawcall, "DrawIndexed(12)"));
  nodes.push_back(MakeMatchTestNode(40, 10, 1, ActionFlags::NoFlags, "Shadow pass"));
  nodes.push_back(MakeMatchTestNode(50, 40, 0, ActionFlags::Drawcall, "DrawIndexed(12)"));

  QHash<uint32_t, EventCanonicalKey> byEvent;
  QHash<QString, QVector<uint32_t>> byKey;
  BuildEventCanonicalKeys(nodes, byEvent, byKey);

  REQUIRE(byEvent.size() == 5);

  const EventCanonicalKey root = byEvent.value(10);
  CHECK(root.kind == EventCanonicalKind::Marker);
  CHECK(root.markerPath.isEmpty());
  CHECK(root.leafName == QStringLiteral("frame"));
  CHECK(root.occurrence == 0);

  const EventCanonicalKey draw0 = byEvent.value(30);
  CHECK(draw0.kind == EventCanonicalKind::RealEffect);
  CHECK(draw0.markerPath == QStringLiteral("frame > shadow pass"));
  CHECK(draw0.leafName == QStringLiteral("drawindexed"));
  CHECK(draw0.occurrence == 0);

  const EventCanonicalKey draw1 = byEvent.value(50);
  CHECK(draw1.occurrence == 1);
  CHECK(draw0.key != draw1.key);

  // Duplicate labels remain separate entries rather than fusing into one key.
  CHECK(byKey.value(draw0.key).size() == 1);
  REQUIRE(byKey.contains(draw1.key));
  CHECK(byKey.value(draw0.key)[0] == 30);
  CHECK(byKey.value(draw1.key)[0] == 50);

  // Repeated marker names are distinguished the same way.
  CHECK(byEvent.value(20).label == QStringLiteral("frame > shadow pass"));
  CHECK(byEvent.value(20).occurrence == 0);
  CHECK(byEvent.value(40).occurrence == 1);

  // The single-event overload agrees with the bulk build.
  CHECK(BuildEventCanonicalKey(nodes, 2).key == draw0.key);

  // An identical frame matches exactly, with every node accounted for.
  const CaptureMatchResult same = MatchEvents(nodes, byEvent, nodes, byEvent);
  CHECK(same.matchedCount() == 5);
  CHECK(same.exactCount == 5);
  CHECK(same.coverage == 1.0);
}

// P2-1: the matching policy and its confidence grading. A pairing is never
// silently upgraded: exact, normalized, fuzzy and positional are reported apart,
// and anything one-sided is reported as such.
TEST_CASE("MCP cross-capture matching policy and confidence", "[mcp][analysis]")
{
  using namespace MCP;

  const CaptureMatchPolicy policy;

  CaptureMatchEntry b0;
  b0.key = QStringLiteral("shadowpass#0");
  b0.displayName = QStringLiteral("ShadowPass");
  b0.index = 0;
  b0.position = 100;
  CaptureMatchEntry b1;
  b1.key = QStringLiteral("albedo#0");
  b1.displayName = QStringLiteral("Albedo (3)");
  b1.index = 1;
  b1.position = 200;
  CaptureMatchEntry c0;
  c0.key = QStringLiteral("shadowpass#0");
  c0.displayName = QStringLiteral("ShadowPass");
  c0.index = 0;
  c0.position = 110;
  CaptureMatchEntry c1;
  c1.key = QStringLiteral("albedo#0");
  c1.displayName = QStringLiteral("Albedo");
  c1.index = 1;
  c1.position = 210;

  QVector<CaptureMatchEntry> base;
  QVector<CaptureMatchEntry> cand;
  base << b0 << b1;
  cand << c0 << c1;

  const CaptureMatchResult r = MatchEntries(base, cand, policy);
  CHECK(r.matchedCount() == 2);
  CHECK(r.exactCount == 1);
  CHECK(r.normalizedCount == 1);
  CHECK(r.fuzzyCount == 0);
  CHECK(r.unmatchedA == 0);
  CHECK(r.unmatchedB == 0);
  CHECK(r.coverage == 1.0);

  bool sawNormalizedNote = false;
  for(const CaptureMatchPair &p : r.pairs)
  {
    if(p.confidence == CaptureMatchConfidence::Normalized)
      sawNormalizedNote = !p.note.isEmpty();
  }
  CHECK(sawNormalizedNote);

  // A near-miss name is reported as fuzzy, never as exact.
  QVector<CaptureMatchEntry> fb;
  QVector<CaptureMatchEntry> fc;
  CaptureMatchEntry f0;
  f0.displayName = QStringLiteral("BaseColorPass");
  CaptureMatchEntry f1;
  f1.displayName = QStringLiteral("BaseColourPass");
  fb << f0;
  fc << f1;

  const CaptureMatchResult fr = MatchEntries(fb, fc, policy);
  CHECK(fr.fuzzyCount == 1);
  CHECK(fr.exactCount == 0);
  REQUIRE(fr.pairs.size() == 1);
  CHECK(fr.pairs[0].confidence == CaptureMatchConfidence::Fuzzy);
  CHECK(fr.pairs[0].score >= policy.fuzzyThreshold);
  CHECK(fr.pairs[0].note.contains(QStringLiteral("threshold")));

  // With fuzzy and positional disabled the same pair is honestly unmatched.
  CaptureMatchPolicy strict = policy;
  strict.fuzzyThreshold = 1.0;
  strict.allowPositionalFallback = false;
  const CaptureMatchResult sr = MatchEntries(fb, fc, strict);
  CHECK(sr.fuzzyCount == 0);
  CHECK(sr.positionalCount == 0);
  CHECK(sr.unmatchedA == 1);
  CHECK(sr.unmatchedB == 1);

  // Dissimilar leftovers of equal size pair up by ordinal, flagged as a
  // hypothesis rather than a fact.
  QVector<CaptureMatchEntry> pb;
  QVector<CaptureMatchEntry> pc;
  CaptureMatchEntry p0;
  p0.displayName = QStringLiteral("Alpha");
  p0.index = 0;
  CaptureMatchEntry p1;
  p1.displayName = QStringLiteral("Beta");
  p1.index = 1;
  CaptureMatchEntry q0;
  q0.displayName = QStringLiteral("Gamma");
  q0.index = 0;
  CaptureMatchEntry q1;
  q1.displayName = QStringLiteral("Delta");
  q1.index = 1;
  pb << p0 << p1;
  pc << q0 << q1;

  const CaptureMatchResult pr = MatchEntries(pb, pc, policy);
  CHECK(pr.matchedCount() == 2);
  CHECK(pr.positionalCount == 2);
  CHECK(pr.fuzzyCount == 0);

  // One-sided entries drag the coverage down instead of being hidden.
  QVector<CaptureMatchEntry> ob;
  QVector<CaptureMatchEntry> oc;
  CaptureMatchEntry o0;
  o0.key = QStringLiteral("a#0");
  o0.displayName = QStringLiteral("A");
  CaptureMatchEntry o1;
  o1.key = QStringLiteral("b#0");
  o1.displayName = QStringLiteral("B");
  CaptureMatchEntry o2;
  o2.key = QStringLiteral("c#0");
  o2.displayName = QStringLiteral("C");
  CaptureMatchEntry onlyA;
  onlyA.key = QStringLiteral("a#0");
  onlyA.displayName = QStringLiteral("A");
  ob << o0 << o1 << o2;
  oc << onlyA;

  const CaptureMatchResult orr = MatchEntries(ob, oc, policy);
  CHECK(orr.exactCount == 1);
  CHECK(orr.unmatchedA == 2);
  CHECK(orr.unmatchedB == 0);
  CHECK(orr.coverage < 0.5);

  // The effective policy round-trips into JSON so a diff is always explainable.
  const QJsonObject pj = policy.ToJson();
  CHECK(pj.value(QStringLiteral("fuzzyThreshold")).toDouble() == policy.fuzzyThreshold);
  CHECK(pj.value(QStringLiteral("description")).toString().contains(QStringLiteral("Normalization")));
  CHECK_FALSE(policy.Describe().isEmpty());
  CHECK(CaptureMatchConfidenceName(CaptureMatchConfidence::Normalized) ==
        QStringLiteral("normalized"));
}

// P2-1: the resource and pass views, plus the canonical-capture header P2-2
// caches. A resolution change must show up as a change on ONE matched resource,
// not as one resource disappearing and an unrelated one appearing.
TEST_CASE("MCP cross-capture resource and pass canonicalization", "[mcp][analysis]")
{
  using namespace MCP;

  ResourceMemoryRow t0;
  t0.name = QStringLiteral("Albedo 4096x4096");
  t0.type = QStringLiteral("Texture");
  t0.byteSize = 1000;
  ResourceMemoryRow t1;
  t1.name = QStringLiteral("Albedo 2048x2048");
  t1.type = QStringLiteral("Texture");
  t1.byteSize = 250;
  ResourceMemoryRow anon;
  anon.type = QStringLiteral("Texture");
  anon.byteSize = 64;

  QVector<ResourceMemoryRow> rows;
  rows << t0 << t1 << anon;

  const QVector<ResourceCanonicalKey> keys = NormalizeResourceRows(rows);
  REQUIRE(keys.size() == 3);
  CHECK(keys[0].key == QStringLiteral("albedo res"));
  CHECK(keys[1].key == QStringLiteral("albedo res"));
  CHECK(keys[0].confidence == Confidence::Confirmed);
  // An anonymous resource is identified by kind alone - a heuristic, so Inferred.
  CHECK(keys[2].key == QStringLiteral("texture"));
  CHECK(keys[2].confidence == Confidence::Inferred);

  const CaptureMatchResult res = MatchResources(keys, keys);
  CHECK(res.matchedCount() == 3);
  CHECK(res.exactCount == 3);

  PassInfo pa;
  pa.id = QStringLiteral("GBuffer@410-430");
  pa.eventIdStart = 410;
  pa.eventIdEnd = 430;
  PassInfo pb;
  pb.name = QStringLiteral("Shadow");
  pb.id = QStringLiteral("Shadow@100-200");
  pb.eventIdStart = 100;
  pb.eventIdEnd = 200;

  QVector<PassInfo> passes;
  passes << pa << pb;

  const QVector<PassCanonicalKey> pkeys = NormalizePassRows(passes);
  REQUIRE(pkeys.size() == 2);
  // An id-only pass is keyed on the name part of "GBuffer@410-430".
  CHECK(pkeys[0].key == QStringLiteral("gbuffer"));
  CHECK(pkeys[1].key == QStringLiteral("shadow"));

  const CaptureMatchResult pm = MatchPasses(pkeys, pkeys);
  CHECK(pm.matchedCount() == 2);

  CanonicalCapture cap;
  cap.capturePath = QStringLiteral("D:/caps/frame.rdc");
  cap.api = QStringLiteral("Vulkan");
  cap.unifyKey = UnifyCaptureKey(cap.api, cap.capturePath);

  rdcarray<EventNode> nodes;
  nodes.push_back(MakeMatchTestNode(10, 0, 0, ActionFlags::Drawcall, "DrawIndexed(12)"));
  BuildCanonicalEvents(nodes, cap);
  BuildCanonicalResources(rows, cap);
  BuildCanonicalPasses(passes, cap);

  CHECK(cap.events.size() == 1);
  CHECK(cap.resources.size() == 3);
  CHECK(cap.passes.size() == 2);

  // The cached header is a compact summary, not the full tables.
  const QJsonObject cj = CanonicalCaptureToJson(cap);
  CHECK(cj.value(QStringLiteral("unifyKey")).toString() == cap.unifyKey);
  CHECK(cj.value(QStringLiteral("api")).toString() == QStringLiteral("Vulkan"));
  CHECK(cj.value(QStringLiteral("eventCount")).toInt() == 1);
  CHECK(cj.value(QStringLiteral("resourceCount")).toInt() == 3);
  CHECK(cj.value(QStringLiteral("passCount")).toInt() == 2);
  CHECK(cj.value(QStringLiteral("normalization")).isObject());
  CHECK_FALSE(cj.contains(QStringLiteral("limitation")));
}

// P2-2: the cache unit (a normalized projection of one capture) and the
// cross-capture regression diff built on top of the P2-1 matcher. The snapshot
// must survive a JSON round-trip, the cache must evict oldest-first, and the
// diff must report a direction for every matched row while staying honest when
// timings are missing.
TEST_CASE("MCP cross-capture regression snapshot and diff", "[mcp][analysis]")
{
  using namespace MCP;

  //
  // ---- Snapshot built from a canonical capture ----------------------------
  //
  CanonicalCapture cap;
  cap.capturePath = QStringLiteral("D:/caps/frame_a.rdc");
  cap.api = QStringLiteral("Vulkan");
  cap.frameNumber = 3;
  cap.unifyKey = UnifyCaptureKey(cap.api, cap.capturePath);

  rdcarray<EventNode> nodes;
  nodes.push_back(MakeMatchTestNode(10, 0, 0, ActionFlags::Drawcall, "DrawIndexed(12)"));
  BuildCanonicalEvents(nodes, cap);

  ResourceMemoryRow tex;
  tex.name = QStringLiteral("Albedo 4096x4096");
  tex.type = QStringLiteral("Texture");
  tex.byteSize = (uint64_t)4096 * 4096 * 4;
  QVector<ResourceMemoryRow> rows;
  rows << tex;
  BuildCanonicalResources(rows, cap);

  PassInfo pass;
  pass.name = QStringLiteral("GBuffer");
  pass.id = QStringLiteral("GBuffer@10-10");
  pass.eventIdStart = 10;
  pass.eventIdEnd = 10;
  QVector<PassInfo> passRows;
  passRows << pass;
  BuildCanonicalPasses(passRows, cap);

  RegressionSnapshot snap = BuildRegressionSnapshot(cap);
  CHECK(snap.unifyKey == cap.unifyKey);
  CHECK(snap.api == QStringLiteral("Vulkan"));
  CHECK(snap.frameNumber == 3);
  CHECK(snap.events.size() == 1);
  CHECK(snap.resources.size() == 1);
  CHECK(snap.passes.size() == 1);
  CHECK(snap.events[0].passKey == QStringLiteral("gbuffer"));
  CHECK(snap.resources[0].byteSize == (uint64_t)4096 * 4096 * 4);
  CHECK(snap.totalResourceBytes == (uint64_t)4096 * 4096 * 4);
  CHECK_FALSE(snap.timingAvailable);
  CHECK_FALSE(RegressionSnapshotToText(snap).isEmpty());

  // Timings and pass durations come from replay and are applied afterwards.
  QHash<uint32_t, double> timings;
  timings.insert(10, 250.0);
  ApplyEventTimings(snap, timings);
  CHECK(snap.events[0].durationUs == 250.0);
  CHECK(snap.totalDurationUs == 250.0);
  CHECK(snap.timedEventCount == 1);

  PassInfo timedPass;
  timedPass.id = snap.passes[0].id;
  timedPass.name = QStringLiteral("GBuffer");
  timedPass.eventIdStart = 10;
  timedPass.eventIdEnd = 10;
  timedPass.durationUs = 123.0;
  QVector<PassInfo> timedPasses;
  timedPasses << timedPass;
  ApplyPassDurations(snap, timedPasses);
  CHECK(snap.passes[0].durationUs == 123.0);

  //
  // ---- Cache round-trip and eviction --------------------------------------
  //
  RegressionSnapshotCache cache;
  cache.Upsert(snap);
  CHECK(cache.FindByUnifyKey(cap.unifyKey) == 0);

  RegressionSnapshotCache parsed;
  QString cacheError;
  REQUIRE(ParseRegressionCache(SerializeRegressionCache(cache), parsed, &cacheError));
  CHECK(parsed.snapshots.size() == 1);
  CHECK(parsed.snapshots[0].unifyKey == cap.unifyKey);
  CHECK(parsed.snapshots[0].events.size() == 1);
  CHECK(parsed.snapshots[0].events[0].durationUs == 250.0);
  CHECK(parsed.snapshots[0].resources[0].byteSize == (uint64_t)4096 * 4096 * 4);
  CHECK(parsed.snapshots[0].passes[0].durationUs == 123.0);

  RegressionSnapshot sB = snap;
  sB.unifyKey = QStringLiteral("B");
  RegressionSnapshot sC = snap;
  sC.unifyKey = QStringLiteral("C");
  cache.Upsert(sB, 2);
  cache.Upsert(sC, 2);
  CHECK(cache.snapshots.size() == 2);
  CHECK(cache.FindByUnifyKey(QStringLiteral("B")) == 0);
  CHECK(cache.FindByUnifyKey(QStringLiteral("C")) == 1);
  CHECK(cache.FindByUnifyKey(cap.unifyKey) == -1);

  // Upsert with an existing key replaces in place instead of appending.
  RegressionSnapshot sB2 = sB;
  sB2.frameNumber = 42;
  cache.Upsert(sB2, 0);
  CHECK(cache.FindByUnifyKey(QStringLiteral("B")) == 0);
  CHECK(cache.snapshots.size() == 2);
  CHECK(cache.snapshots[0].frameNumber == 42);

  // A newer format version is rejected; junk bytes fail to parse.
  QJsonObject v2 = RegressionSnapshotToJson(snap);
  v2[QStringLiteral("formatVersion")] = 2;
  RegressionSnapshot out;
  CHECK_FALSE(RegressionSnapshotFromJson(v2, out, &cacheError));
  CHECK_FALSE(cacheError.isEmpty());
  RegressionSnapshotCache bad;
  CHECK_FALSE(ParseRegressionCache(QByteArray("not json"), bad, &cacheError));

  //
  // ---- Shader bindings from a pipeline state object -----------------------
  //
  QJsonObject pipeline;
  pipeline[QStringLiteral("loaded")] = true;
  QJsonObject vsShader;
  vsShader[QStringLiteral("name")] = QStringLiteral("MainVS");
  QJsonObject vs;
  vs[QStringLiteral("shader")] = vsShader;
  vs[QStringLiteral("entryPoint")] = QStringLiteral("main");
  QJsonObject stageShaders;
  stageShaders[QStringLiteral("vertex")] = vs;
  pipeline[QStringLiteral("shaders")] = stageShaders;

  const QVector<RegressionShaderBindingEntry> binds = ShaderBindingsFromPipelineJson(pipeline);
  REQUIRE(binds.size() == 1);
  CHECK(binds[0].stage == QStringLiteral("vertex"));
  CHECK(binds[0].shader == QStringLiteral("MainVS"));
  CHECK(binds[0].entryPoint == QStringLiteral("main"));

  // An unbound (null) resource falls back to the entry point.
  QJsonObject psShader;
  psShader[QStringLiteral("name")] = QStringLiteral("ResourceId::0");
  QJsonObject ps;
  ps[QStringLiteral("shader")] = psShader;
  ps[QStringLiteral("entryPoint")] = QStringLiteral("PSMain");
  QJsonObject stageShaders2;
  stageShaders2[QStringLiteral("pixel")] = ps;
  pipeline[QStringLiteral("shaders")] = stageShaders2;
  const QVector<RegressionShaderBindingEntry> binds2 = ShaderBindingsFromPipelineJson(pipeline);
  REQUIRE(binds2.size() == 1);
  CHECK(binds2[0].shader == QStringLiteral("PSMain"));

  // An unloaded pipeline state yields no bindings rather than an exception.
  QJsonObject unloaded;
  unloaded[QStringLiteral("loaded")] = false;
  CHECK(ShaderBindingsFromPipelineJson(unloaded).isEmpty());

  //
  // ---- Cross-capture diff -------------------------------------------------
  //
  RegressionSnapshot base;
  base.api = QStringLiteral("Vulkan");
  base.capturePath = QStringLiteral("D:/caps/a.rdc");
  base.unifyKey = QStringLiteral("vulkan|a");
  base.timingAvailable = true;
  base.totalResourceBytes = 1000;

  RegressionEventEntry be0;
  be0.eventId = 10;
  be0.key = QStringLiteral("draw#0");
  be0.label = QStringLiteral("Frame > Draw#0");
  be0.kind = EventCanonicalKind::RealEffect;
  be0.durationUs = 100.0;
  RegressionEventEntry be1;
  be1.eventId = 20;
  be1.key = QStringLiteral("compute#0");
  be1.label = QStringLiteral("Frame > Compute#0");
  be1.kind = EventCanonicalKind::RealEffect;
  be1.durationUs = 50.0;
  base.events << be0 << be1;

  RegressionResourceEntry br0;
  br0.key = QStringLiteral("albedo");
  br0.displayName = QStringLiteral("Albedo");
  br0.type = QStringLiteral("Texture");
  br0.byteSize = 1000;
  br0.index = 0;
  base.resources << br0;

  RegressionPassEntry bp0;
  bp0.key = QStringLiteral("gbuffer");
  bp0.id = QStringLiteral("GBuffer@10-20");
  bp0.displayName = QStringLiteral("GBuffer");
  bp0.firstEventId = 10;
  bp0.lastEventId = 20;
  bp0.durationUs = 150.0;
  base.passes << bp0;

  RegressionShaderBindingEntry bs0;
  bs0.stage = QStringLiteral("vertex");
  bs0.shader = QStringLiteral("BaseVS");
  base.shaders << bs0;

  RegressionSnapshot cand;
  cand.api = QStringLiteral("Vulkan");
  cand.capturePath = QStringLiteral("D:/caps/b.rdc");
  cand.unifyKey = QStringLiteral("vulkan|b");
  cand.timingAvailable = true;
  cand.totalResourceBytes = 2500;

  RegressionEventEntry ce0;
  ce0.eventId = 12;
  ce0.key = QStringLiteral("draw#0");
  ce0.label = QStringLiteral("Frame > Draw#0");
  ce0.kind = EventCanonicalKind::RealEffect;
  ce0.durationUs = 200.0;
  RegressionEventEntry ce1;
  ce1.eventId = 22;
  ce1.key = QStringLiteral("compute#0");
  ce1.label = QStringLiteral("Frame > Compute#0");
  ce1.kind = EventCanonicalKind::RealEffect;
  ce1.durationUs = 40.0;
  RegressionEventEntry ce2;
  ce2.eventId = 30;
  ce2.key = QStringLiteral("extra#0");
  ce2.label = QStringLiteral("Frame > Extra#0");
  ce2.kind = EventCanonicalKind::RealEffect;
  ce2.durationUs = 30.0;
  cand.events << ce0 << ce1 << ce2;

  RegressionResourceEntry cr0;
  cr0.key = QStringLiteral("albedo");
  cr0.displayName = QStringLiteral("Albedo");
  cr0.type = QStringLiteral("Texture");
  cr0.byteSize = 2000;
  cr0.index = 0;
  RegressionResourceEntry cr1;
  cr1.key = QStringLiteral("extra tex");
  cr1.displayName = QStringLiteral("Extra");
  cr1.type = QStringLiteral("Texture");
  cr1.byteSize = 500;
  cr1.index = 1;
  cand.resources << cr0 << cr1;

  RegressionPassEntry cp0;
  cp0.key = QStringLiteral("gbuffer");
  cp0.id = QStringLiteral("GBuffer@12-22");
  cp0.displayName = QStringLiteral("GBuffer");
  cp0.firstEventId = 12;
  cp0.lastEventId = 22;
  cp0.durationUs = 300.0;
  cand.passes << cp0;

  RegressionShaderBindingEntry cs0;
  cs0.stage = QStringLiteral("vertex");
  cs0.shader = QStringLiteral("NewVS");
  RegressionShaderBindingEntry cs1;
  cs1.stage = QStringLiteral("pixel");
  cs1.shader = QStringLiteral("PS");
  cand.shaders << cs0 << cs1;

  RegressionOptions options;
  options.baselineLabel = QStringLiteral("A");
  options.candidateLabel = QStringLiteral("B");

  const RegressionDiffResult diff = DiffRegressionSnapshots(base, cand, options);
  CHECK(diff.available);
  CHECK(diff.timingAvailable);
  CHECK(diff.apiMatches);
  CHECK(diff.sameDriver);
  CHECK(diff.baselineLabel == QStringLiteral("A"));
  CHECK(diff.candidateLabel == QStringLiteral("B"));

  // Timings: the grown draw regressed, the shrunk compute improved, the
  // candidate-only event is added - and none of them is hidden as unchanged.
  REQUIRE(diff.timing.size() == 3);
  bool sawRegressedDraw = false;
  bool sawImprovedCompute = false;
  bool sawAddedExtra = false;
  for(const RegressionTimingRow &row : diff.timing)
  {
    if(row.key == QStringLiteral("draw#0"))
    {
      sawRegressedDraw = row.verdict == RegressionVerdict::Regressed;
      CHECK(row.deltaUs == 100.0);
      CHECK(row.deltaPercent == 100.0);
      CHECK(row.eventIdA == 10);
      CHECK(row.eventIdB == 12);
      CHECK(row.matchConfidence == CaptureMatchConfidence::Exact);
      CHECK_FALSE(row.evidence.isEmpty());
    }
    else if(row.key == QStringLiteral("compute#0"))
    {
      sawImprovedCompute = row.verdict == RegressionVerdict::Improved;
      CHECK(row.deltaUs == -10.0);
    }
    else if(row.key == QStringLiteral("extra#0"))
    {
      sawAddedExtra = row.verdict == RegressionVerdict::Added;
    }
  }
  CHECK(sawRegressedDraw);
  CHECK(sawImprovedCompute);
  CHECK(sawAddedExtra);

  // Resource memory: albedo doubled (regressed), the new texture is added.
  REQUIRE(diff.memory.size() == 2);
  bool sawRegressedAlbedo = false;
  bool sawAddedTexture = false;
  for(const RegressionMemoryRow &row : diff.memory)
  {
    if(row.key == QStringLiteral("albedo"))
    {
      sawRegressedAlbedo = row.verdict == RegressionVerdict::Regressed;
      CHECK(row.deltaBytes == 1000);
      CHECK(row.deltaPercent == 100.0);
    }
    else if(row.key == QStringLiteral("extra tex"))
    {
      sawAddedTexture = row.verdict == RegressionVerdict::Added;
    }
  }
  CHECK(sawRegressedAlbedo);
  CHECK(sawAddedTexture);

  REQUIRE(diff.passes.size() == 1);
  CHECK(diff.passes[0].verdict == RegressionVerdict::Regressed);
  CHECK(diff.passes[0].deltaUs == 150.0);

  REQUIRE(diff.shaders.size() == 2);
  bool sawChangedShader = false;
  bool sawAddedShader = false;
  for(const RegressionShaderRow &row : diff.shaders)
  {
    if(row.stage == QStringLiteral("vertex"))
    {
      sawChangedShader = row.verdict == RegressionVerdict::Changed;
      CHECK(row.baseline == QStringLiteral("BaseVS"));
      CHECK(row.candidate == QStringLiteral("NewVS"));
      CHECK_FALSE(row.note.isEmpty());
    }
    else if(row.stage == QStringLiteral("pixel"))
    {
      sawAddedShader = row.verdict == RegressionVerdict::Added;
    }
  }
  CHECK(sawChangedShader);
  CHECK(sawAddedShader);

  CHECK(diff.summary.regressedEvents == 1);
  CHECK(diff.summary.improvedEvents == 1);
  CHECK(diff.summary.addedEvents == 1);
  CHECK(diff.summary.regressedResources == 1);
  CHECK(diff.summary.addedResources == 1);
  CHECK(diff.summary.regressedPasses == 1);
  CHECK(diff.summary.changedShaders == 1);
  CHECK(diff.summary.addedShaders == 1);
  CHECK(diff.summary.totalBaselineUs == 150.0);
  CHECK(diff.summary.totalCandidateUs == 270.0);
  CHECK(diff.summary.totalBaselineBytes == 1000);
  CHECK(diff.summary.totalCandidateBytes == 2500);
  CHECK(diff.summary.totalDeltaBytes == 1500);

  CHECK_FALSE(diff.limitations.isEmpty());
  CHECK(diff.confidence == Confidence::Confirmed);
  CHECK(RegressionDiffHasChanges(diff));
  CHECK(RegressionVerdictName(RegressionVerdict::Regressed) == QStringLiteral("regressed"));

  const QJsonObject diffJson = RegressionDiffToJson(diff);
  CHECK(diffJson.value(QStringLiteral("timing")).toArray().size() == 3);
  CHECK(diffJson.value(QStringLiteral("summary"))
            .toObject()
            .value(QStringLiteral("events"))
            .toObject()
            .value(QStringLiteral("regressed"))
            .toInt() == 1);

  const QString markdown = RegressionDiffToMarkdown(diff);
  CHECK(markdown.contains(QStringLiteral("## Summary")));
  CHECK(markdown.contains(QStringLiteral("## Timing regressions")));
  CHECK(markdown.contains(QStringLiteral("## Resource memory")));
  CHECK(markdown.contains(QStringLiteral("## Pass timings")));
  CHECK(markdown.contains(QStringLiteral("## Shader bindings")));
  CHECK(markdown.contains(QStringLiteral("## Limitations")));

  // includeUnchanged surfaces the rows the default report intentionally drops.
  RegressionOptions wideOptions = options;
  wideOptions.includeUnchanged = true;
  RegressionSnapshot sameAsBase = base;
  const RegressionDiffResult unchangedDiff = DiffRegressionSnapshots(base, sameAsBase, wideOptions);
  CHECK(unchangedDiff.summary.unchangedEvents == 2);
  CHECK_FALSE(RegressionDiffHasChanges(unchangedDiff));

  // Missing timings never fabricate a measured delta: the verdicts are Unknown
  // and the whole result drops to "toVerify".
  RegressionSnapshot noTimingBase = base;
  noTimingBase.timingAvailable = false;
  RegressionSnapshot noTimingCand = cand;
  noTimingCand.timingAvailable = false;
  const RegressionDiffResult noTiming = DiffRegressionSnapshots(noTimingBase, noTimingCand, options);
  CHECK_FALSE(noTiming.timingAvailable);
  CHECK(noTiming.confidence == Confidence::ToVerify);
  bool sawUnknown = false;
  for(const RegressionTimingRow &row : noTiming.timing)
  {
    if(row.key == QStringLiteral("draw#0"))
      sawUnknown = row.verdict == RegressionVerdict::Unknown;
  }
  CHECK(sawUnknown);
}

////////////////////////////////////////////////////////////////////////////////
// P2-3: Mali Offline Compiler data model
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("Mali Offline Compiler helpers are deterministic", "[mcp][mali]")
{
  using namespace MCP;

  CHECK(MaliDeviceNames().size() == 19);
  CHECK(IsKnownMaliDevice(QStringLiteral("Mali-G720")));
  CHECK(NormalizeMaliDevice(QStringLiteral(" mali-g720 ")) == QStringLiteral("Mali-G720"));
  CHECK(NormalizeMaliDevice(QStringLiteral("Mali-Unknown")).isEmpty());

  MaliTargetOptions options;
  options.device = QStringLiteral("Mali-G720");
  options.extraArgs = QStringList{QStringLiteral("--verbose"), QStringLiteral("--foo=bar baz")};
  const QStringList command = BuildMaliCommandLine(QStringLiteral("malioc"),
                                                    QStringLiteral("C:/temp/shader.spv"), options);
  REQUIRE(command.size() == 6);
  CHECK(command[0] == QStringLiteral("malioc"));
  CHECK(command[1] == QStringLiteral("C:/temp/shader.spv"));
  CHECK(command[2] == QStringLiteral("-c"));
  CHECK(command[3] == QStringLiteral("Mali-G720"));
  CHECK(command[5] == QStringLiteral("--foo=bar baz"));

  const MaliShaderExport shader =
      BuildMaliShaderExport(QStringLiteral("Pixel"), QStringLiteral("main"), 42,
                             int(uint32_t(ShaderEncoding::SPIRV)), QByteArray("SPV", 3), 3);
  CHECK(shader.limitation.isEmpty());
  CHECK(shader.extension == QStringLiteral(".spv"));
  CHECK(shader.suggestedFileName == QStringLiteral("main_Pixel.spv"));
  CHECK(shader.revision.size() == 64);

  const MaliShaderExport truncated =
      BuildMaliShaderExport(QStringLiteral("Pixel"), QStringLiteral("main"), 42,
                            int(uint32_t(ShaderEncoding::SPIRV)), QByteArray("SPV", 3), 4);
  CHECK_FALSE(truncated.limitation.isEmpty());

  const MaliShaderExport unsupported =
      BuildMaliShaderExport(QStringLiteral("Vertex"), QStringLiteral("main"), 7, 0,
                            QByteArray("DXBC", 4), 4);
  CHECK_FALSE(unsupported.limitation.isEmpty());
  CHECK(unsupported.extension.isEmpty());

  MaliRunRequest missingTool;
  missingTool.configuredPath = QStringLiteral("C:/does-not-exist/malioc.exe");
  missingTool.shaderPath = QStringLiteral("C:/does-not-exist/shader.spv");
  const MaliRunResult missingToolResult = RunMaliAnalysis(missingTool);
  CHECK(missingToolResult.status == MaliRunStatus::ToolMissing);
  CHECK_FALSE(missingToolResult.report.ran);

  const MaliReport failed = ParseMaliOutput(QStringLiteral("error: invalid shader"), QString(), 1);
  CHECK(failed.ran);
  CHECK_FALSE(failed.hasTotals);
  CHECK(failed.confidence == Confidence::Low);
  CHECK_FALSE(failed.error.isEmpty());

  const MaliReport report = ParseMaliOutput(
      QStringLiteral("Mali Offline Compiler v7\nHardware: Mali-G720\n"
                     "Shader program for fragment\nTotal cycles: 128\n"
                     "Total instructions: 64\nRegisters: 12\nSpill bytes: 0\n"),
      QString(), 0);
  CHECK(report.ran);
  CHECK(report.hasTotals);
  CHECK(report.totalCycles == 128.0);
  CHECK(report.totalInstructions == 64.0);
  CHECK(report.registers == 12.0);
  CHECK(report.hardware == QStringLiteral("Mali-G720"));
  REQUIRE(FindMaliMetric(report, QStringLiteral("total cycles")) != NULL);
  CHECK(FindMaliMetric(report, QStringLiteral("cycles")) == NULL);
  CHECK(report.confidence == Confidence::Confirmed);

  const MaliReport detailed = ParseMaliOutput(
      QStringLiteral("Work registers: 64\nUniform registers: 128\nStack size: 32 bytes\n"
                     "Stack spilling: yes\nTotal instruction cycles: 15.38 40.20 0.47 13.38 LS\n"
                     "Shortest path cycles: 5.95 18.00 0.47 1.89 Texture\n"
                     "Longest path cycles: N/A N/A N/A N/A\n"),
      QString(), 0);
  CHECK(detailed.hasWorkRegisters);
  CHECK(detailed.workRegisters == 64.0);
  CHECK(detailed.hasUniformRegisters);
  CHECK(detailed.hasStackSize);
  CHECK(detailed.stackSpillingKnown);
  CHECK(detailed.stackSpilling);
  CHECK(detailed.totalPath.available);
  CHECK(detailed.shortestPath.available);
  CHECK_FALSE(detailed.longestPath.available);
  CHECK(detailed.riskSignals.contains(QStringLiteral("High register pressure")));
  CHECK(detailed.riskSignals.contains(QStringLiteral("Stack spilling")));
  CHECK(detailed.riskSignals.contains(QStringLiteral("Texture-bound path")));
  const QJsonObject detailedJson = MaliReportToJson(detailed);
  CHECK(detailedJson.value(QStringLiteral("workRegisters")).toDouble() == 64.0);
  CHECK(detailedJson.value(QStringLiteral("stackSpilling")).toBool());
  CHECK(detailedJson.value(QStringLiteral("shortestPath")).toObject()
            .value(QStringLiteral("available"))
            .toBool());
}

////////////////////////////////////////////////////////////////////////////////
// P2-4: Project source mapping
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("Project source mapping resolves explicit, fuzzy and ambiguous candidates", "[mcp][source]")
{
  using namespace MCP;

  SourceMappingOptions options;
  options.extensions = QStringList{QStringLiteral("hlsl")};
  options.fuzzyThreshold = 0.70;

  SourceMappingIndex index;
  index.options = options;
  index.files = {
      {QStringLiteral("C:/work/Game/Shaders/Lighting/Common.hlsl"),
       QStringLiteral("C:/work/Game"), QStringLiteral("Shaders/Lighting/Common.hlsl"), false},
      {QStringLiteral("C:/work/Game/Shaders/Lighting/DeferredLighting.hlsl"),
       QStringLiteral("C:/work/Game"), QStringLiteral("Shaders/Lighting/DeferredLighting.hlsl"), false},
      {QStringLiteral("C:/sdk/Include/Common.hlsl"), QStringLiteral("C:/sdk"),
       QStringLiteral("Include/Common.hlsl"), true},
  };

  CHECK(NormalizeSourcePath(QStringLiteral(" C:\\WORK\\Game\\Shaders\\Lighting\\Common.hlsl ")) ==
        QStringLiteral("c:/work/game/shaders/lighting/common.hlsl"));
  CHECK(SourceFilenameSimilarity(QStringLiteral("DeferredLightng.hlsl"),
                                 QStringLiteral("DeferredLighting.hlsl")) > 0.80);

  const SourceMappingResult relative =
      ResolveSourceMapping(index, QStringLiteral("Shaders/Lighting/DeferredLighting.hlsl"));
  REQUIRE(relative.selected.kind == SourceMatchKind::RelativePath);
  CHECK(relative.selected.confidence == Confidence::Inferred);
  CHECK(relative.selected.file.absolutePath.endsWith(QStringLiteral("DeferredLighting.hlsl")));
  CHECK_FALSE(relative.ambiguous);

  const SourceMappingResult fuzzy =
      ResolveSourceMapping(index, QStringLiteral("C:/capture/DeferredLightng.hlsl"));
  REQUIRE(fuzzy.selected.kind == SourceMatchKind::Fuzzy);
  CHECK(fuzzy.selected.confidence == Confidence::Low);
  CHECK_FALSE(fuzzy.ambiguous);

  const SourceMappingResult ambiguous = ResolveSourceMapping(index, QStringLiteral("Common.hlsl"));
  REQUIRE(ambiguous.selected.kind == SourceMatchKind::Filename);
  CHECK(ambiguous.ambiguous);
  CHECK(ambiguous.selected.confidence == Confidence::ToVerify);
  CHECK(ambiguous.candidates.size() == 2);

  const QVector<SourceMappingOverride> overrides = {
      {QStringLiteral("Common.hlsl"), QStringLiteral("C:/manual/Common.hlsl")},
  };
  const SourceMappingResult missingOverride = ResolveSourceMapping(index, QStringLiteral("Common.hlsl"), overrides);
  CHECK(missingOverride.ambiguous);
  CHECK_FALSE(missingOverride.limitations.isEmpty());

  const QJsonObject json = SourceMappingResultToJson(relative);
  CHECK(json.value(QStringLiteral("selected")).toObject().value(QStringLiteral("matchKind")).toString() ==
        QStringLiteral("relativePath"));

  // The indexer itself is filesystem-backed but deterministic: it honors both
  // project and INCLUDE roots, extension filtering, missing-root reporting and
  // the configured file cap without reading file contents.
  QTemporaryDir temporary;
  REQUIRE(temporary.isValid());
  const QString projectRoot = temporary.path() + QStringLiteral("/project");
  const QString includeRoot = temporary.path() + QStringLiteral("/include");
  REQUIRE(QDir().mkpath(projectRoot + QStringLiteral("/Shaders")));
  REQUIRE(QDir().mkpath(includeRoot));
  QFile projectFile(projectRoot + QStringLiteral("/Shaders/Bound.hlsl"));
  REQUIRE(projectFile.open(QIODevice::WriteOnly));
  projectFile.write("// shader");
  projectFile.close();
  QFile ignoredFile(projectRoot + QStringLiteral("/Shaders/Ignore.txt"));
  REQUIRE(ignoredFile.open(QIODevice::WriteOnly));
  ignoredFile.close();
  QFile includeFile(includeRoot + QStringLiteral("/Common.hlsl"));
  REQUIRE(includeFile.open(QIODevice::WriteOnly));
  includeFile.close();

  SourceMappingOptions scanOptions;
  scanOptions.projectRoots = QStringList{projectRoot, temporary.path() + QStringLiteral("/missing")};
  scanOptions.includePaths = QStringList{includeRoot};
  scanOptions.extensions = QStringList{QStringLiteral("hlsl")};
  const SourceMappingIndex scanned = IndexSourceMappingPaths(scanOptions);
  CHECK(scanned.files.size() == 2);
  CHECK(scanned.skippedRoots.size() == 1);
  CHECK_FALSE(scanned.truncated);
  CHECK((scanned.files[0].fromIncludePath || scanned.files[1].fromIncludePath));

  const QString manualTarget = temporary.path() + QStringLiteral("/Manual.hlsl");
  QFile manualFile(manualTarget);
  REQUIRE(manualFile.open(QIODevice::WriteOnly));
  manualFile.close();
  const SourceMappingResult manual = ResolveSourceMapping(
      scanned, QStringLiteral("Unrelated.hlsl"), {{QStringLiteral("Unrelated.hlsl"), manualTarget}});
  CHECK(manual.selected.kind == SourceMatchKind::Manual);
  CHECK(manual.selected.confidence == Confidence::Confirmed);
}

TEST_CASE("Environment diagnostics provide recovery advice and platform degradation", "[mcp][diagnostics]")
{
  using namespace MCP;

  EnvironmentDiagnosticInput input;
  input.platform = QStringLiteral("linux");
  input.serverEnabled = false;
  input.serverPort = 38967;
  input.sensitiveDataAllowed = false;
  input.maliConfiguredPath = QStringLiteral("/opt/mali/malioc");
  input.maliResolvedExecutable = input.maliConfiguredPath;
  input.maliExecutableExists = false;

  const EnvironmentDiagnosticReport unhealthy = BuildEnvironmentDiagnosticReport(input);
  CHECK_FALSE(unhealthy.healthy);
  REQUIRE(unhealthy.checks.size() == 4);
  CHECK(unhealthy.checks[0].id == QStringLiteral("mcp.server"));
  CHECK(unhealthy.checks[0].severity == DiagnosticSeverity::Warning);
  CHECK_FALSE(unhealthy.checks[0].recovery.isEmpty());
  CHECK(unhealthy.checks[3].severity == DiagnosticSeverity::Error);
  CHECK(unhealthy.limitations.contains(QStringLiteral("Configured Mali Offline Compiler executable is unavailable.")));

  const QJsonObject json = EnvironmentDiagnosticReportToJson(unhealthy);
  CHECK_FALSE(json.value(QStringLiteral("healthy")).toBool());
  CHECK(json.value(QStringLiteral("checks")).toArray().size() == 4);
  CHECK(json.value(QStringLiteral("checks")).toArray()[3].toObject()
            .value(QStringLiteral("severity"))
            .toString() == QStringLiteral("error"));

  input.platform = QStringLiteral("macos");
  input.supportsMali = false;
  input.supportsSourceMapping = false;
  input.maliExecutableExists = true;
  const EnvironmentDiagnosticReport degraded = BuildEnvironmentDiagnosticReport(input);
  CHECK(degraded.healthy);
  CHECK(degraded.checks[2].available == false);
  CHECK(degraded.checks[3].available == false);
  CHECK(degraded.limitations.size() == 2);
}

#endif    // ENABLE_UNIT_TESTS
