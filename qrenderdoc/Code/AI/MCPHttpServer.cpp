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

#include "MCPHttpServer.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QEventLoop>
#include <QPointer>
#include <QRunnable>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>

#include <functional>

namespace MCP
{

namespace
{
QJsonObject MakeError(const QJsonValue &id, ErrorCode code, const QString &message,
                      const QJsonObject &data = QJsonObject())
{
  Error e(code, message, data);

  QJsonObject resp;
  resp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
  resp[QStringLiteral("id")] = id;
  resp[QStringLiteral("error")] = e.toJson();
  return resp;
}

static const int kMaxHeaderBytes = 64 * 1024;
static const int kMaxBodyBytes = 8 * 1024 * 1024;
static const int kMaxConnections = 32;
static const int kMaxPendingRequestsPerSocket = 8;
static const qint64 kMaxPendingBytesPerSocket = 16 * 1024 * 1024;
static const int kMaxPendingRequests = 64;
static const qint64 kMaxPendingBytes = 64 * 1024 * 1024;
static const int kMaxInFlightTools = 2;
static const int kMaxSessions = 128;
static const qint64 kSessionIdleTimeoutMs = 30 * 60 * 1000;
static const qint64 kToolDeadlineMs = 60 * 1000;

QString StatusText(int status)
{
  switch(status)
  {
    case 200: return QStringLiteral("OK");
    case 202: return QStringLiteral("Accepted");
    case 204: return QStringLiteral("No Content");
    case 400: return QStringLiteral("Bad Request");
    case 401: return QStringLiteral("Unauthorized");
    case 404: return QStringLiteral("Not Found");
    case 405: return QStringLiteral("Method Not Allowed");
    case 413: return QStringLiteral("Payload Too Large");
    case 415: return QStringLiteral("Unsupported Media Type");
    case 429: return QStringLiteral("Too Many Requests");
    default: return QStringLiteral("Unknown");
  }
}

QString JsonToCompact(const QJsonObject &obj)
{
  return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

class LambdaRunnable : public QRunnable
{
public:
  explicit LambdaRunnable(const std::function<void()> &func) : m_Func(func) {}

  void run() override { m_Func(); }

private:
  std::function<void()> m_Func;
};
}    // namespace

HttpServer::HttpServer(QObject *parent) : QObject(parent)
{
  m_Server = new QTcpServer(this);
  m_ToolPool.setMaxThreadCount(kMaxInFlightTools);
  connect(m_Server, &QTcpServer::newConnection, this, &HttpServer::OnNewConnection);
}

HttpServer::~HttpServer()
{
  Stop();

  if(m_Server != NULL)
  {
    m_Server->deleteLater();
    m_Server = NULL;
  }
}

bool HttpServer::Start(quint16 port, bool requireToken, QString &errorOut)
{
  errorOut.clear();

  // A restart must also cancel in-flight work and close already accepted
  // keep-alive sockets. Closing only the listener would leave those clients
  // connected with an invalidated session/token generation.
  if(IsRunning() || !m_Buffers.isEmpty())
    Stop();

  m_Stopping = false;
  m_Generation++;

  // QHostAddress::LocalHost resolves to the loopback interface only, so there is
  // no way for this server to be reached from off-box even if a firewall is off.
  if(!m_Server->listen(QHostAddress::LocalHost, port))
  {
    errorOut = m_Server->errorString();
    NoteError(errorOut);
    return false;
  }

  m_Port = m_Server->serverPort();
  {
    QMutexLocker lock(&m_Lock);
    m_RequireToken = requireToken;
    m_AccessToken = QUuid::createUuid().toString();
    m_AccessToken.remove(QLatin1Char('{'));
    m_AccessToken.remove(QLatin1Char('}'));
    m_LastError.clear();
  }
  return true;
}

void HttpServer::Stop()
{
  // Close the admission gate first. All workers are joined below before this
  // returns, so callers may safely destroy their registry/capture dependencies.
  m_Stopping = true;
  if(m_Server != NULL && m_Server->isListening())
    m_Server->close();

  m_Port = 0;

  const QList<QTcpSocket *> sockets = m_Buffers.keys();
  for(QTcpSocket *sock : sockets)
  {
    const QSharedPointer<QAtomicInt> cancelled = m_InFlightCancellation.value(sock);
    if(!cancelled.isNull())
      cancelled->storeRelease(1);
    sock->disconnect(this);
    sock->abort();
    sock->deleteLater();
  }
  for(auto it = m_InFlightCancellation.begin(); it != m_InFlightCancellation.end(); ++it)
    it.value()->storeRelease(1);

  WaitForWorkers();

  m_Buffers.clear();
  m_PendingRequests.clear();
  m_PendingRequestBytes.clear();
  m_InFlightCancellation.clear();
  m_TotalPendingRequests = 0;
  m_TotalPendingRequestBytes = 0;

  {
    QMutexLocker lock(&m_Lock);
    m_Sessions.clear();
    m_RequireToken = false;
    m_AccessToken.clear();
  }
}

int HttpServer::ActiveSessions() const
{
  QMutexLocker lock(&m_Lock);
  const_cast<HttpServer *>(this)->PurgeExpiredSessionsLocked(QDateTime::currentMSecsSinceEpoch());
  return m_Sessions.size();
}

QString HttpServer::AccessToken() const
{
  QMutexLocker lock(&m_Lock);
  return m_AccessToken;
}

bool HttpServer::RequiresToken() const
{
  QMutexLocker lock(&m_Lock);
  return m_RequireToken;
}

ServerStatus HttpServer::Status() const
{
  ServerStatus s;
  s.running = IsRunning();
  s.enabled = IsRunning();
  s.host = QString::fromUtf8(kDefaultHost);
  s.port = int(IsRunning() ? m_Port : 0);
  s.activeSessions = ActiveSessions();
  s.registeredTools = (m_Registry != NULL) ? m_Registry->Count() : 0;

  {
    QMutexLocker lock(&m_Lock);
    s.lastError = m_LastError;
  }

  // The endpoint the client should be pointed at. Only meaningful while running,
  // but keeping it populated makes the status dialog simpler to render.
  s.endpoint = QStringLiteral("http://%1:%2/mcp")
                   .arg(s.host)
                   .arg(IsRunning() ? int(m_Port) : int(kDefaultPort));

  return s;
}

void HttpServer::OnNewConnection()
{
  while(m_Server != NULL && m_Server->hasPendingConnections())
  {
    QTcpSocket *sock = m_Server->nextPendingConnection();
    if(sock == NULL)
      break;

    if(m_Stopping || m_Buffers.size() >= kMaxConnections)
    {
      RejectAndClose(sock, 429, QStringLiteral("MCP connection capacity is exhausted."));
      sock->deleteLater();
      continue;
    }

    m_Buffers[sock] = QByteArray();
    m_PendingRequests[sock] = QQueue<HttpRequest>();
    m_PendingRequestBytes[sock] = 0;

    connect(sock, &QTcpSocket::readyRead, this, [this, sock]() { OnReadyRead(sock); });
    connect(sock, &QTcpSocket::disconnected, this, [this, sock]() { OnSocketDisconnected(sock); });
  }
}

void HttpServer::OnSocketDisconnected(QTcpSocket *sock)
{
  const QSharedPointer<QAtomicInt> cancelled = m_InFlightCancellation.take(sock);
  if(!cancelled.isNull())
    cancelled->storeRelease(1);

  m_Buffers.remove(sock);
  DropPendingRequests(sock);
  if(!m_Stopping)
    sock->deleteLater();
}

void HttpServer::DropPendingRequests(QTcpSocket *sock)
{
  const qint64 bytes = m_PendingRequestBytes.take(sock);
  const int count = m_PendingRequests.value(sock).size();
  m_PendingRequests.remove(sock);
  m_TotalPendingRequestBytes = qMax<qint64>(0, m_TotalPendingRequestBytes - bytes);
  m_TotalPendingRequests = qMax(0, m_TotalPendingRequests - count);
}

void HttpServer::RejectAndClose(QTcpSocket *sock, int status, const QString &message)
{
  if(sock == NULL)
    return;

  HttpResponse response;
  response.status = status;
  response.body =
      JsonToCompact(MakeError(QJsonValue(QJsonValue::Null), ErrorCode::CaptureBusy, message)).toUtf8();
  SendResponse(sock, response);
  sock->disconnectFromHost();
}

void HttpServer::WaitForWorkers()
{
  // Pump the UI queue while waiting: tool paths may block on GUI/replay hops.
  // The bounded pool has no queued MCP work because admission limits in-flight
  // calls before they are submitted.
  while(!m_ToolPool.waitForDone(10))
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

  m_InFlightTools = 0;
}

void HttpServer::NoteError(const QString &message)
{
  QMutexLocker lock(&m_Lock);
  m_LastError = message;
}

bool HttpServer::ParseRequest(QByteArray &buffer, HttpRequest &req, bool &badRequest)
{
  badRequest = false;

  const int headerEnd = buffer.indexOf("\r\n\r\n");
  if(headerEnd < 0)
  {
    if(buffer.size() > kMaxHeaderBytes)
      badRequest = true;
    return false;
  }

  if(headerEnd > kMaxHeaderBytes)
  {
    badRequest = true;
    return false;
  }

  const QByteArray headerBlock = buffer.left(headerEnd);
  const QList<QByteArray> lines = headerBlock.split('\n');
  if(lines.isEmpty())
  {
    badRequest = true;
    return false;
  }

  const QByteArray requestLine = lines[0].trimmed();
  const QList<QByteArray> requestParts = requestLine.split(' ');
  if(requestParts.size() < 2)
  {
    badRequest = true;
    return false;
  }

  req.method = QString::fromUtf8(requestParts[0]).trimmed().toUpper();
  req.path = QString::fromUtf8(requestParts[1]).trimmed();

  for(int i = 1; i < lines.size(); i++)
  {
    const QByteArray line = lines[i].trimmed();
    if(line.isEmpty())
      continue;

    const int colon = line.indexOf(':');
    if(colon < 0)
      continue;

    const QString key = QString::fromUtf8(line.left(colon)).trimmed().toLower();
    const QString value = QString::fromUtf8(line.mid(colon + 1)).trimmed();
    req.headers[key] = value;
  }

  req.sessionId = req.headers.value(QStringLiteral("mcp-session-id"));

  const QString authorization = req.headers.value(QStringLiteral("authorization"));
  {
    QMutexLocker lock(&m_Lock);
    req.authorized = !m_RequireToken ||
                     (!m_AccessToken.isEmpty() &&
                      authorization == QStringLiteral("Bearer %1").arg(m_AccessToken));
  }

  const QString accept = req.headers.value(QStringLiteral("accept")).toLower();
  req.wantsSse = accept.contains(QStringLiteral("text/event-stream"));

  bool validLength = true;
  int contentLength = 0;
  if(req.headers.contains(QStringLiteral("content-length")))
  {
    contentLength = req.headers.value(QStringLiteral("content-length")).toInt(&validLength);
    if(!validLength || contentLength < 0 || contentLength > kMaxBodyBytes)
    {
      badRequest = true;
      return false;
    }
  }

  const int bodyStart = headerEnd + 4;
  if(buffer.size() - bodyStart < contentLength)
    return false;

  req.body = buffer.mid(bodyStart, contentLength);
  buffer.remove(0, bodyStart + contentLength);
  req.complete = true;
  return true;
}

QByteArray HttpServer::FrameBody(const QString &json, bool sse) const
{
  if(!sse)
    return json.toUtf8();

  // SSE framing: each line of the JSON payload becomes its own `data:` line, and
  // the event is terminated by a blank line. This matches how the Streamable HTTP
  // transport delivers a single JSON-RPC response as one `message` event.
  QByteArray out("event: message\n");

  const QList<QByteArray> lines = json.toUtf8().split('\n');
  for(const QByteArray &line : lines)
  {
    out += "data: ";
    out += line;
    out += "\n";
  }

  out += "\n";
  return out;
}

void HttpServer::SendResponse(QTcpSocket *sock, const HttpResponse &resp)
{
  const QString contentType =
      resp.contentType.isEmpty() ? QStringLiteral("application/json") : resp.contentType;

  QByteArray head;
  head += "HTTP/1.1 ";
  head += QByteArray::number(resp.status);
  head += " ";
  head += StatusText(resp.status).toUtf8();
  head += "\r\n";
  head += "Content-Type: ";
  head += contentType.toUtf8();
  head += "\r\n";
  head += "Content-Length: ";
  head += QByteArray::number(resp.body.size());
  head += "\r\n";
  head += "Cache-Control: no-store\r\n";
  if(!resp.sessionId.isEmpty())
  {
    head += "Mcp-Session-Id: ";
    head += resp.sessionId.toUtf8();
    head += "\r\n";
  }
  head += "Connection: keep-alive\r\n";
  head += "\r\n";

  sock->write(head);
  if(!resp.body.isEmpty())
    sock->write(resp.body);
  sock->flush();
}

void HttpServer::OnReadyRead(QTcpSocket *sock)
{
  auto it = m_Buffers.find(sock);
  if(m_Stopping || it == m_Buffers.end())
    return;

  it.value().append(sock->readAll());
  // A request may include the maximum legal header plus the maximum legal body.
  // One extra byte already proves either framing has been exceeded or a peer is
  // attempting to retain unbounded data in the connection buffer.
  if(it.value().size() > kMaxHeaderBytes + kMaxBodyBytes)
  {
    HttpResponse resp;
    resp.status = 413;
    resp.body = JsonToCompact(MakeError(QJsonValue(QJsonValue::Null), ErrorCode::InvalidRequest,
                                        QStringLiteral("HTTP request exceeds the maximum allowed size.")))
                    .toUtf8();
    SendResponse(sock, resp);
    sock->disconnectFromHost();
    return;
  }

  // A connection can carry more than one HTTP request (keep-alive). Queue them
  // so an asynchronous tools/call never reorders HTTP responses behind a later
  // lightweight request.
  for(;;)
  {
    HttpRequest req;
    bool badRequest = false;

    if(!ParseRequest(it.value(), req, badRequest))
    {
      if(badRequest)
      {
        HttpResponse resp;
        resp.status = 400;
        resp.body = JsonToCompact(MakeError(QJsonValue(QJsonValue::Null), ErrorCode::InvalidRequest,
                                            QStringLiteral("Malformed HTTP request.")))
                        .toUtf8();
        SendResponse(sock, resp);
        sock->disconnectFromHost();
      }
      break;
    }

    const qint64 bodyBytes = req.body.size();
    const QQueue<HttpRequest> &pending = m_PendingRequests[sock];
    if(pending.size() >= kMaxPendingRequestsPerSocket ||
       m_PendingRequestBytes.value(sock) + bodyBytes > kMaxPendingBytesPerSocket ||
       m_TotalPendingRequests >= kMaxPendingRequests ||
       m_TotalPendingRequestBytes + bodyBytes > kMaxPendingBytes)
    {
      DropPendingRequests(sock);
      RejectAndClose(sock, 429, QStringLiteral("MCP request queue capacity is exhausted."));
      sock->deleteLater();
      return;
    }

    m_PendingRequests[sock].enqueue(req);
    m_PendingRequestBytes[sock] += bodyBytes;
    m_TotalPendingRequestBytes += bodyBytes;
    m_TotalPendingRequests++;
  }

  if(!m_InFlightCancellation.contains(sock))
    ProcessNextRequest(sock);
}

bool HttpServer::IsAsyncToolRequest(const HttpRequest &req) const
{
  if(req.method != QStringLiteral("POST") || !req.authorized || m_Registry == NULL)
    return false;

  const QString contentType = req.headers.value(QStringLiteral("content-type")).toLower();
  if(req.path != QStringLiteral("/mcp") || !contentType.startsWith(QStringLiteral("application/json")))
    return false;

  QJsonParseError error;
  const QJsonDocument doc = QJsonDocument::fromJson(req.body, &error);
  if(error.error != QJsonParseError::NoError || !doc.isObject())
    return false;

  const QJsonObject msg = doc.object();
  return msg.value(QStringLiteral("jsonrpc")).toString() == QStringLiteral("2.0") &&
         msg.value(QStringLiteral("method")).toString() == QStringLiteral("tools/call") &&
         !msg.value(QStringLiteral("id")).isUndefined() && !msg.value(QStringLiteral("id")).isNull() &&
         !msg.value(QStringLiteral("params")).toObject().value(QStringLiteral("name")).toString().isEmpty();
}

void HttpServer::ProcessNextRequest(QTcpSocket *sock)
{
  if(m_Stopping || sock == NULL || !m_Buffers.contains(sock) || m_InFlightCancellation.contains(sock))
    return;

  QQueue<HttpRequest> &queue = m_PendingRequests[sock];
  if(queue.isEmpty())
    return;

  const HttpRequest req = queue.dequeue();
  const qint64 bodyBytes = req.body.size();
  m_PendingRequestBytes[sock] = qMax<qint64>(0, m_PendingRequestBytes.value(sock) - bodyBytes);
  m_TotalPendingRequestBytes = qMax<qint64>(0, m_TotalPendingRequestBytes - bodyBytes);
  m_TotalPendingRequests = qMax(0, m_TotalPendingRequests - 1);
  if(!IsAsyncToolRequest(req))
  {
    HttpResponse response;
    Route(req, response);
    SendResponse(sock, response);
    ProcessNextRequest(sock);
    return;
  }

  // The worker must not touch socket-owned/session state. Validate and refresh
  // the session on this object's thread, then only hand immutable request data,
  // context and a shared cancellation flag to the tool implementation.
  bool validSession = false;
  {
    QMutexLocker lock(&m_Lock);
    validSession = FindSessionLocked(req.sessionId) != NULL;
  }
  if(!validSession)
  {
    HttpResponse response;
    response.status = 404;
    response.body = JsonToCompact(MakeError(
                        QJsonValue(QJsonValue::Null), ErrorCode::InvalidRequest,
                        QStringLiteral("Missing or expired MCP session; re-run initialize.")))
                        .toUtf8();
    SendResponse(sock, response);
    ProcessNextRequest(sock);
    return;
  }

  if(m_InFlightTools >= kMaxInFlightTools)
  {
    HttpResponse response;
    response.status = 429;
    response.body = JsonToCompact(MakeError(
                        QJsonValue(QJsonValue::Null), ErrorCode::CaptureBusy,
                        QStringLiteral("MCP tool execution capacity is exhausted.")))
                        .toUtf8();
    SendResponse(sock, response);
    ProcessNextRequest(sock);
    return;
  }

  const QJsonObject msg = QJsonDocument::fromJson(req.body).object();
  const QJsonObject params = msg.value(QStringLiteral("params")).toObject();
  const QString toolName = params.value(QStringLiteral("name")).toString();
  const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();
  const QJsonValue requestId = msg.value(QStringLiteral("id"));
  const ToolRegistry *registry = m_Registry;
  ICaptureContext *context = m_Ctx;
  const QSharedPointer<QAtomicInt> cancelled(new QAtomicInt(0));
  m_InFlightCancellation[sock] = cancelled;
  m_InFlightTools++;
  const quint64 generation = m_Generation;
  QPointer<HttpServer> server(this);
  QPointer<QTcpSocket> socket(sock);

  QRunnable *task = new LambdaRunnable([server, socket, req, registry, context, toolName, arguments,
                                        requestId, cancelled, generation]() {
    CallContext call;
    call.ctx = context;
    call.cancellationState = cancelled;
    call.deadlineMs = QDateTime::currentMSecsSinceEpoch() + kToolDeadlineMs;
    const ToolResult result = registry->Call(toolName, arguments, call);

    QJsonObject item;
    item[QStringLiteral("type")] = QStringLiteral("text");
    item[QStringLiteral("text")] = result.ok ? result.text : result.error.message;
    QJsonObject payload;
    payload[QStringLiteral("content")] = QJsonArray{item};
    payload[QStringLiteral("isError")] = !result.ok;
    payload[QStringLiteral("structuredContent")] = result.ok ? result.structured : result.error.toJson();
    QJsonObject rpc;
    rpc[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    rpc[QStringLiteral("id")] = requestId;
    rpc[QStringLiteral("result")] = payload;
    const QString json = JsonToCompact(rpc);
    QByteArray body = json.toUtf8();
    if(req.wantsSse)
    {
      body = "event: message\n";
      for(const QByteArray &line : json.toUtf8().split('\n'))
      {
        body += "data: ";
        body += line;
        body += "\n";
      }
      body += "\n";
    }

    // A QPointer prevents a late worker completion from dereferencing a server
    // that was stopped/destroyed while capture shutdown was in progress. All
    // socket interaction and queue advancement return to the owning UI thread.
    if(server)
    {
      GUIInvoke::call(server, [server, socket, cancelled, body, req, generation]() {
        if(server)
          server->FinishWorker(socket, cancelled, body, req, generation);
      });
    }
  });
  task->setAutoDelete(true);
  m_ToolPool.start(task);
}

void HttpServer::FinishWorker(QTcpSocket *socket,
                              const QSharedPointer<QAtomicInt> &cancelled, const QByteArray &body,
                              const HttpRequest &req, quint64 generation)
{
  // A completion queued before Stop()/Start() belongs to the preceding server
  // generation. It must not consume a capacity slot owned by new work.
  if(generation != m_Generation)
    return;

  m_InFlightTools = qMax(0, m_InFlightTools - 1);

  if(m_Stopping || socket == NULL || !m_Buffers.contains(socket))
    return;

  m_InFlightCancellation.remove(socket);
  if(cancelled->loadAcquire() == 0)
  {
    HttpResponse response;
    response.contentType = req.wantsSse ? QStringLiteral("text/event-stream")
                                         : QStringLiteral("application/json");
    response.body = body;
    SendResponse(socket, response);
  }
  ProcessNextRequest(socket);
}

void HttpServer::Route(const HttpRequest &req, HttpResponse &out)
{
  if(req.path != QStringLiteral("/mcp"))
  {
    out.status = 404;
    out.body = JsonToCompact(MakeError(QJsonValue(QJsonValue::Null), ErrorCode::InvalidRequest,
                                       QStringLiteral("The MCP endpoint is /mcp.")))
                   .toUtf8();
    return;
  }

  if(!req.authorized)
  {
    out.status = 401;
    out.body = JsonToCompact(MakeError(QJsonValue(QJsonValue::Null), ErrorCode::InvalidRequest,
                                       QStringLiteral("Missing or invalid MCP bearer token.")))
                   .toUtf8();
    return;
  }

  // GET is used by the optional server-initiated SSE stream, which P0 does not
  // provide. Answering 405 tells the client to stick to POST requests.
  if(req.method == QStringLiteral("GET"))
  {
    out.status = 405;
    out.body = JsonToCompact(MakeError(QJsonValue(QJsonValue::Null), ErrorCode::MethodNotFound,
                                       QStringLiteral("The GET SSE stream is not supported.")))
                   .toUtf8();
    return;
  }

  if(req.method == QStringLiteral("DELETE"))
  {
    QMutexLocker lock(&m_Lock);
    if(!req.sessionId.isEmpty())
      m_Sessions.remove(req.sessionId);
    out.status = 204;
    return;
  }

  if(req.method == QStringLiteral("OPTIONS"))
  {
    out.status = 204;
    return;
  }

  if(req.method != QStringLiteral("POST"))
  {
    out.status = 405;
    out.body = JsonToCompact(MakeError(QJsonValue(QJsonValue::Null), ErrorCode::InvalidRequest,
                                       QStringLiteral("Only POST is supported on the MCP endpoint.")))
                   .toUtf8();
    return;
  }

  const QString contentType = req.headers.value(QStringLiteral("content-type")).toLower();
  if(!contentType.startsWith(QStringLiteral("application/json")))
  {
    out.status = 415;
    out.body = JsonToCompact(MakeError(QJsonValue(QJsonValue::Null), ErrorCode::InvalidRequest,
                                       QStringLiteral("POST requests must use application/json.")))
                   .toUtf8();
    return;
  }

  // A mutable copy is required: DispatchJsonRpc records the freshly created
  // session id back onto the request so the caller can echo it. Passing the
  // const `req` (or a temporary) would either fail to compile or drop the
  // session id, making every later tools/call fail the session check.
  HttpRequest httpReq = req;
  const QString json = DispatchJsonRpc(QString::fromUtf8(req.body), httpReq, out);

  Q_UNUSED(httpReq);

  // An empty payload means the message was a notification: accepted, no body.
  if(json.isEmpty())
  {
    if(out.status == 200)
      out.status = 202;
    out.body = QByteArray();
    return;
  }

  if(req.wantsSse)
  {
    out.contentType = QStringLiteral("text/event-stream");
    out.body = FrameBody(json, true);
  }
  else
  {
    out.contentType = QStringLiteral("application/json");
    out.body = json.toUtf8();
  }
}

QString HttpServer::DispatchJsonRpc(const QString &body, HttpRequest &httpReq, HttpResponse &out)
{
  out.contentType = QStringLiteral("application/json");

  QJsonParseError perr;
  const QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8(), &perr);
  if(perr.error != QJsonParseError::NoError || !doc.isObject())
  {
    const QString json = JsonToCompact(MakeError(QJsonValue(QJsonValue::Null), ErrorCode::ParseError,
                                                 QStringLiteral("Invalid JSON: %1")
                                                     .arg(perr.errorString())));
    out.body = json.toUtf8();
    return json;
  }

  const QJsonObject msg = doc.object();
  const QJsonValue id = msg.value(QStringLiteral("id"));
  const bool isNotification = id.isUndefined() || id.isNull();

  const QString jsonrpc = msg.value(QStringLiteral("jsonrpc")).toString();
  const QString method = msg.value(QStringLiteral("method")).toString();
  const QJsonObject params = msg.value(QStringLiteral("params")).toObject();

  auto errorReply = [&](ErrorCode code, const QString &message,
                        const QJsonObject &data = QJsonObject()) -> QString {
    const QString json = JsonToCompact(MakeError(id, code, message, data));
    out.body = json.toUtf8();
    return json;
  };

  auto resultReply = [&](const QJsonObject &result) -> QString {
    QJsonObject resp;
    resp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    resp[QStringLiteral("id")] = id;
    resp[QStringLiteral("result")] = result;
    const QString json = JsonToCompact(resp);
    out.body = json.toUtf8();
    return json;
  };

  if(jsonrpc != QStringLiteral("2.0"))
    return errorReply(ErrorCode::InvalidRequest, QStringLiteral("Unsupported JSON-RPC version."));

  // ---- initialize ---------------------------------------------------------
  if(method == QStringLiteral("initialize"))
  {
    const QString clientProto = params.value(QStringLiteral("protocolVersion")).toString();
    const QJsonObject clientInfo = params.value(QStringLiteral("clientInfo")).toObject();

    if(!clientProto.isEmpty() && clientProto != QString::fromUtf8(kLatestProtocolVersion))
    {
      // Only one revision is implemented; reject up front so the client can
      // downgrade instead of silently mis-negotiating.
      QJsonObject data;
      data[QStringLiteral("supported")] = QString::fromUtf8(kLatestProtocolVersion);
      data[QStringLiteral("requested")] = clientProto;
      return errorReply(ErrorCode::ProtocolVersionUnsupported,
                        QStringLiteral("Unsupported MCP protocol version."), data);
    }

    {
      QMutexLocker lock(&m_Lock);
      QString createdId;
      Session &s = EnsureSessionLocked(httpReq, createdId);
      s.protocolVersion = QString::fromUtf8(kLatestProtocolVersion);
      s.clientName = clientInfo.value(QStringLiteral("name")).toString();
      s.clientVersion = clientInfo.value(QStringLiteral("version")).toString();

      out.sessionId = createdId.isEmpty() ? httpReq.sessionId : createdId;
      if(!createdId.isEmpty())
        httpReq.sessionId = createdId;
    }

    if(isNotification)
      return QString();

    QJsonObject toolsCap;
    toolsCap[QStringLiteral("listChanged")] = false;
    QJsonObject caps;
    caps[QStringLiteral("tools")] = toolsCap;

    QJsonObject serverInfo;
    serverInfo[QStringLiteral("name")] = QString::fromUtf8(kServerName);
    serverInfo[QStringLiteral("version")] = QStringLiteral("0.1.0");

    QJsonObject result;
    result[QStringLiteral("protocolVersion")] = QString::fromUtf8(kLatestProtocolVersion);
    result[QStringLiteral("capabilities")] = caps;
    result[QStringLiteral("serverInfo")] = serverInfo;
    result[QStringLiteral("instructions")] =
        QStringLiteral("Native qrenderdoc tools for inspecting the currently loaded capture. Most "
                       "tools advertise their side effects. Bookmark add/id calls authorize local "
                       "mutation; legacy eventId calls require confirm=true and explicit confirm=false refuses. "
                       "Report outputPath creates a new local file without overwriting. Mali analysis may launch "
                       "the locally configured compiler.");

    return resultReply(result);
  }

  // ---- notifications ------------------------------------------------------
  if(method.startsWith(QStringLiteral("notifications/")))
  {
    if(method == QStringLiteral("notifications/initialized"))
    {
      QMutexLocker lock(&m_Lock);
      FindSessionLocked(httpReq.sessionId);
    }
    return QString();
  }

  // Every other method requires a live session, matching the Streamable HTTP
  // transport rules. A 404 tells the client to initialize again.
  {
    QMutexLocker lock(&m_Lock);
    if(FindSessionLocked(httpReq.sessionId) == NULL)
    {
      out.status = 404;
      QJsonObject data;
      data[QStringLiteral("header")] = QStringLiteral("Mcp-Session-Id");
      return errorReply(ErrorCode::InvalidRequest,
                        QStringLiteral("Missing or expired MCP session; re-run initialize."), data);
    }
  }

  // ---- ping ---------------------------------------------------------------
  if(method == QStringLiteral("ping"))
  {
    if(isNotification)
      return QString();
    return resultReply(QJsonObject());
  }

  // ---- tools/list ---------------------------------------------------------
  if(method == QStringLiteral("tools/list"))
  {
    if(isNotification)
      return QString();

    QJsonArray tools;
    if(m_Registry != NULL)
    {
      const QList<ToolDefinition> defs = m_Registry->Definitions();
      for(const ToolDefinition &d : defs)
      {
        QJsonObject t;
        t[QStringLiteral("name")] = d.name;
        if(!d.title.isEmpty())
          t[QStringLiteral("title")] = d.title;
        t[QStringLiteral("description")] = d.description;
        t[QStringLiteral("inputSchema")] = d.inputSchema;
        QJsonObject annotations;
        annotations[QStringLiteral("readOnlyHint")] = d.effects == ToolEffects::ReadOnly;
        annotations[QStringLiteral("destructiveHint")] = false;
        annotations[QStringLiteral("idempotentHint")] = d.effects == ToolEffects::ReadOnly;
        t[QStringLiteral("annotations")] = annotations;
        tools.append(t);
      }
    }

    QJsonObject result;
    result[QStringLiteral("tools")] = tools;
    return resultReply(result);
  }

  // ---- tools/call ---------------------------------------------------------
  if(method == QStringLiteral("tools/call"))
  {
    if(isNotification)
      return QString();

    if(m_Registry == NULL)
      return errorReply(ErrorCode::InternalError,
                        QStringLiteral("No tool registry is attached to the MCP server."));

    const QString name = params.value(QStringLiteral("name")).toString();
    if(name.isEmpty())
      return errorReply(ErrorCode::InvalidParams,
                        QStringLiteral("tools/call requires a tool name."));

    const QJsonObject args = params.value(QStringLiteral("arguments")).toObject();

    CallContext call;
    call.ctx = m_Ctx;
    call.deadlineMs = QDateTime::currentMSecsSinceEpoch() + kToolDeadlineMs;

    const ToolResult r = m_Registry->Call(name, args, call);

    QJsonObject textItem;
    textItem[QStringLiteral("type")] = QStringLiteral("text");
    textItem[QStringLiteral("text")] = r.ok ? r.text : r.error.message;

    QJsonArray content;
    content.append(textItem);

    QJsonObject result;
    result[QStringLiteral("content")] = content;
    result[QStringLiteral("isError")] = !r.ok;

    // Tool-level failures ride inside a successful JSON-RPC result (per the MCP
    // convention), with the structured error alongside the readable text so
    // clients can branch on it without parsing prose.
    result[QStringLiteral("structuredContent")] = r.ok ? r.structured : r.error.toJson();

    return resultReply(result);
  }

  if(isNotification)
    return QString();

  return errorReply(ErrorCode::MethodNotFound,
                    QStringLiteral("Unknown method: %1").arg(method));
}

QString HttpServer::HandleJsonRpc(const QString &body, QString *sessionOut)
{
  return HandleJsonRpc(body, QString(), sessionOut, NULL);
}

QString HttpServer::HandleJsonRpc(const QString &body, const QString &sessionId, QString *sessionOut,
                                  int *statusOut)
{
  HttpRequest req;
  req.method = QStringLiteral("POST");
  req.complete = true;
  req.sessionId = sessionId;
  req.body = body.toUtf8();

  HttpResponse out;
  const QString json = DispatchJsonRpc(body, req, out);

  if(sessionOut != NULL)
    *sessionOut = out.sessionId.isEmpty() ? req.sessionId : out.sessionId;

  if(statusOut != NULL)
    *statusOut = out.status;

  return json;
}

void HttpServer::PurgeExpiredSessionsLocked(qint64 nowMs)
{
  for(auto it = m_Sessions.begin(); it != m_Sessions.end();)
  {
    if(nowMs - it.value().lastSeenMs > kSessionIdleTimeoutMs)
      it = m_Sessions.erase(it);
    else
      ++it;
  }
}

HttpServer::Session *HttpServer::FindSessionLocked(const QString &id)
{
  PurgeExpiredSessionsLocked(QDateTime::currentMSecsSinceEpoch());

  if(id.isEmpty())
    return NULL;

  auto it = m_Sessions.find(id);
  if(it == m_Sessions.end())
    return NULL;

  it.value().lastSeenMs = QDateTime::currentMSecsSinceEpoch();
  return &it.value();
}

HttpServer::Session &HttpServer::EnsureSessionLocked(const HttpRequest &req, QString &createdId)
{
  PurgeExpiredSessionsLocked(QDateTime::currentMSecsSinceEpoch());

  if(!req.sessionId.isEmpty())
  {
    Session *existing = FindSessionLocked(req.sessionId);
    if(existing != NULL)
      return *existing;
  }

  if(m_Sessions.size() >= kMaxSessions)
  {
    auto oldest = m_Sessions.begin();
    for(auto it = m_Sessions.begin(); it != m_Sessions.end(); ++it)
    {
      if(it.value().lastSeenMs < oldest.value().lastSeenMs)
        oldest = it;
    }
    m_Sessions.erase(oldest);
  }

  Session s;
  s.id = QUuid::createUuid().toString();
  s.id.remove(QLatin1Char('{'));
  s.id.remove(QLatin1Char('}'));
  s.createdAtMs = QDateTime::currentMSecsSinceEpoch();
  s.lastSeenMs = s.createdAtMs;

  createdId = s.id;
  m_Sessions.insert(s.id, s);
  return m_Sessions[s.id];
}

}    // namespace MCP
