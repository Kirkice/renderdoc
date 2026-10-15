/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2017-2026 Baldur Karlsson
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

#include <QDialog>

class QLabel;
class QPushButton;

struct ICaptureContext;

namespace MCP
{
class HttpServer;
}

// P0-8 MCP status UI
// ------------------
// A small, code-only dialog (no .ui file, so nothing extra has to be added to the
// build manifests) that surfaces the live MCP server state to the user: whether it
// is running, the loopback endpoint and ephemeral bearer token a client needs to
// connect, how many sessions are attached, how many tools are registered and the
// last transport/protocol error. It also lets the user start/stop the server at
// runtime without editing the config file, and copy a connection configuration for
// pasting into an MCP client.
//
// The dialog only ever reads through MCP::HttpServer's public interface
// (IsRunning/Status) - it has no knowledge of the transport internals or the tools.
class MCPStatusDialog : public QDialog
{
  Q_OBJECT

public:
  explicit MCPStatusDialog(ICaptureContext &ctx, MCP::HttpServer *server, QWidget *parent = 0);
  ~MCPStatusDialog();

private slots:
  void on_toggle_clicked();
  void on_refresh_clicked();
  void on_copy_clicked();

private:
  void refresh();

  // Pull the port from config, starting the server when it is not running.
  bool startServer();
  void stopServer();

  ICaptureContext &m_Ctx;
  MCP::HttpServer *m_Server;

  QLabel *m_State;
  QLabel *m_Endpoint;
  QLabel *m_AccessToken;
  QLabel *m_Sessions;
  QLabel *m_Tools;
  QLabel *m_LastError;

  QPushButton *m_Toggle;
  QPushButton *m_Refresh;
  QPushButton *m_Copy;
};
