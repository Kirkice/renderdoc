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
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QPointer>
#include <QSharedPointer>
#include <QString>
#include <QThreadPool>
#include "MCPToolRegistry.h"

// QTcpServer is only needed here so that IsRunning() can query the listener
// directly; the definition is included by the .cpp.
#include <QTcpServer>

class QTcpSocket;

namespace MCP
{

// MCP Streamable HTTP transport.
// ---------------------------------
// A single-in-process HTTP server that binds ONLY to the loopback interface. It
// implements the Streamable HTTP transport for MCP: every HTTP POST carrying a
// JSON-RPC message is answered either with a single application/json body or, if
// the client advertised it, an SSE (text/event-stream) body containing the same
// message as one `message` event.
//
// Sessions are created lazily: the first request from a new client gets an
// `Mcp-Session-Id` response header, which the client must echo back. Requests
// without a valid session id are rejected, matching the specification.
//
// The server intentionally exposes the tools through ToolRegistry only. It has no
// knowledge of captures, replay or the UI, which keeps the transport testable in
// isolation and makes the "127.0.0.1 only" boundary easy to reason about.
class HttpServer : public QObject
{
  Q_OBJECT

public:
  HttpServer(QObject *parent = NULL);
  ~HttpServer();

  // Start listening on 127.0.0.1:port. Returns false and leaves `errorOut` set if
  // the socket can't be created or the address is already in use. Binding to
  // anything other than the loopback address is impossible by construction.
  // When requireToken is false, any local process can call the server.
  bool Start(quint16 port, bool requireToken, QString &errorOut);

  // Stop listening and drop every active session.
  // In-flight tool calls are cancelled and joined before this returns, so
  // callers can safely destroy the registry, adapter and capture context next.
  void Stop();

  // "Running" means the listener is actually accepting connections, not merely
  // that the QTcpServer object exists. Stop() closes the listener without
  // destroying it, so testing the pointer alone would report stale state.
  bool IsRunning() const { return m_Server != NULL && m_Server->isListening(); }

  // The port actually bound (valid once IsRunning()).
  quint16 Port() const { return m_Port; }

  // The tool registry whose tools are exposed. Not owned.
  void SetRegistry(const ToolRegistry *registry) { m_Registry = registry; }

  // The capture context handed to tools via CallContext. Owned elsewhere.
  void SetCaptureContext(ICaptureContext *ctx) { m_Ctx = ctx; }

  // Number of live sessions, for the status UI. Takes the session lock and
  // drops sessions that have been idle for too long.
  int ActiveSessions() const;

  // A per-listener bearer token. It is regenerated every time Start() succeeds
  // and must be sent as `Authorization: Bearer <token>` by HTTP clients only when
  // token authentication is enabled. It is not persisted.
  QString AccessToken() const;

  bool RequiresToken() const;

  // A ready-to-display snapshot of the server state, used by the MCP status UI
  // and by diagnostics. `registeredTools` reflects the attached registry.
  ServerStatus Status() const;

  // The most recent transport/JSON-RPC level failure, for diagnostics. Empty when
  // the last request completed cleanly.
  QString LastError() const { return m_LastError; }

  // Feed a raw HTTP request body through the full JSON-RPC/MCP dispatch without a
  // socket. Exposed so unit tests can exercise the protocol layer directly. When
  // non-NULL, `sessionOut` receives the session id that was used or created.
  //
  // This overload sends no session id, so it can only reach the methods that are
  // valid before/without a session (initialize and notifications). Everything
  // else correctly reports the missing session, which is itself worth testing.
  QString HandleJsonRpc(const QString &body, QString *sessionOut);

  // As above, but sends `sessionId` as the Mcp-Session-Id header so that
  // session-gated methods (tools/list, tools/call, ping, ...) can be driven from a
  // test without a real socket. When non-NULL, `statusOut` receives the HTTP status
  // the transport would have replied with (200/202/404/...).
  QString HandleJsonRpc(const QString &body, const QString &sessionId, QString *sessionOut,
                        int *statusOut = NULL);

private:
  struct Session
  {
    QString id;
    QString protocolVersion;
    QString clientName;
    QString clientVersion;
    qint64 createdAtMs = 0;
    qint64 lastSeenMs = 0;
  };

  struct HttpRequest
  {
    QString method;
    QString path;
    QString sessionId;
    bool wantsSse = false;
    bool authorized = false;
    QByteArray body;
    bool complete = false;

    QMap<QString, QString> headers;
  };

  struct HttpResponse
  {
    int status = 200;
    QString contentType;
    QString sessionId;    // when set, emitted as Mcp-Session-Id
    QByteArray body;
  };

  // Parse accumulated bytes into a request. Returns true once a complete request
  // (headers + body) has been buffered.
  bool ParseRequest(QByteArray &buffer, HttpRequest &req, bool &badRequest);

  // Full JSON-RPC dispatch. Implements initialize / notifications/initialized /
  // ping / tools/list / tools/call and the JSON-RPC structural checks.
  QString DispatchJsonRpc(const QString &body, HttpRequest &httpReq, HttpResponse &out);

  // The single entry point that produces a response for a parsed request.
  void Route(const HttpRequest &req, HttpResponse &out);

  // Wrap a JSON-RPC payload in an SSE frame when the client asked for one.
  QByteArray FrameBody(const QString &json, bool sse) const;

  void OnNewConnection();
  void OnReadyRead(QTcpSocket *sock);
  void OnSocketDisconnected(QTcpSocket *sock);
  void SendResponse(QTcpSocket *sock, const HttpResponse &resp);

  // Requests are serialised per socket to preserve HTTP response ordering. Tool
  // calls run off the socket event loop, while lightweight protocol operations
  // are still completed immediately.
  void ProcessNextRequest(QTcpSocket *sock);
  bool IsAsyncToolRequest(const HttpRequest &req) const;
  void DropPendingRequests(QTcpSocket *sock);
  void RejectAndClose(QTcpSocket *sock, int status, const QString &message);
  void FinishWorker(QTcpSocket *socket,
                    const QSharedPointer<QAtomicInt> &cancelled, const QByteArray &body,
                    const HttpRequest &req, quint64 generation);
  void WaitForWorkers();

  // Session helpers. These expect m_Lock to already be held by the caller.
  Session *FindSessionLocked(const QString &id);
  Session &EnsureSessionLocked(const HttpRequest &req, QString &createdId);
  void PurgeExpiredSessionsLocked(qint64 nowMs);

  void NoteError(const QString &message);

  QTcpServer *m_Server = NULL;
  quint16 m_Port = 0;
  const ToolRegistry *m_Registry = NULL;
  ICaptureContext *m_Ctx = NULL;

  QMap<QString, Session> m_Sessions;
  QMap<QTcpSocket *, QByteArray> m_Buffers;
  QMap<QTcpSocket *, QQueue<HttpRequest>> m_PendingRequests;
  QMap<QTcpSocket *, qint64> m_PendingRequestBytes;
  QMap<QTcpSocket *, QSharedPointer<QAtomicInt>> m_InFlightCancellation;
  QThreadPool m_ToolPool;
  int m_InFlightTools = 0;
  int m_TotalPendingRequests = 0;
  qint64 m_TotalPendingRequestBytes = 0;
  bool m_Stopping = false;
  bool m_RequireToken = false;
  quint64 m_Generation = 0;
  QString m_AccessToken;
  QString m_LastError;

  // Guards session bookkeeping; requests from multiple sockets can arrive at once.
  // A plain QMutex is used (QRecursiveMutex is Qt 5.14+ and qrenderdoc supports
  // Qt 5.6), so the *Locked helpers must never re-enter the lock.
  mutable QMutex m_Lock;
};

}    // namespace MCP
