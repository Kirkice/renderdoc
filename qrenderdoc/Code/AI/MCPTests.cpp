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

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTcpSocket>
#include <QThread>

#include "3rdparty/catch/catch.hpp"

#include "Code/AI/MCPAnalysisModel.h"
#include "Code/AI/MCPHttpServer.h"
#include "Code/AI/MCPQueryAdapter.h"
#include "Code/AI/MCPToolRegistry.h"
#include "Code/AI/MCPTools.h"
#include "Code/AI/MCPTypes.h"

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
                         int timeoutMs)
{
  QByteArray req;
  req += "POST /mcp HTTP/1.1\r\n";
  req += "Host: 127.0.0.1\r\n";
  req += "Content-Type: application/json\r\n";
  req += "Accept: application/json\r\n";
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
  const int count = MCP::RegisterCoreTools(registry, adapter);

  REQUIRE(count > 0);
  CHECK(registry.Count() == count);
  CHECK(registry.Contains(QStringLiteral("renderdoc_getCaptureInfo")));
  CHECK(registry.Contains(QStringLiteral("renderdoc_getReplayStatus")));
  CHECK(registry.Contains(QStringLiteral("renderdoc_getDrawCalls")));

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

  MCP::ToolRegistry registry;
  MCP::QueryAdapter adapter(NULL);
  MCP::RegisterCoreTools(registry, adapter);

  MCP::HttpServer server;
  server.SetRegistry(&registry);

  // Port 0 lets the OS pick a free ephemeral port, avoiding collisions.
  QString err;
  REQUIRE(server.Start(0, err));
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

  const QByteArray initResp = HttpRoundTrip(sock, initBody, QByteArray(), 5000);
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
  const QByteArray listResp = HttpRoundTrip(sock, listBody, sessionHeader, 5000);
  REQUIRE_FALSE(listResp.isEmpty());
  CHECK(listResp.startsWith("HTTP/1.1 200"));

  const QJsonObject listJson = ParseJsonObject(QString::fromUtf8(HttpResponseBody(listResp)));
  const QJsonArray tools =
      listJson.value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
  CHECK(tools.size() == registry.Count());

  sock.disconnectFromHost();
  server.Stop();
  CHECK_FALSE(server.IsRunning());
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
    FrameStatistics stats;
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
    access.usage = ResourceUsage::RenderTarget;
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
    access.usage = ResourceUsage::Sampled;
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
    access.usage = ResourceUsage::RenderTarget;
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

#endif    // ENABLE_UNIT_TESTS
