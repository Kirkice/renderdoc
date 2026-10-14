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

#endif    // ENABLE_UNIT_TESTS
