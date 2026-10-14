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

#include "MCPStatusDialog.h"
#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include "Code/AI/MCPHttpServer.h"
#include "Code/Interface/QRDInterface.h"
#include "Code/QRDUtils.h"

MCPStatusDialog::MCPStatusDialog(ICaptureContext &ctx, MCP::HttpServer *server, QWidget *parent)
    : QDialog(parent), m_Ctx(ctx), m_Server(server)
{
  setWindowTitle(tr("MCP Server"));

  m_State = new QLabel(this);
  m_Endpoint = new QLabel(this);
  m_Sessions = new QLabel(this);
  m_Tools = new QLabel(this);
  m_LastError = new QLabel(this);

  // The endpoint is copied verbatim into an MCP client, so show it in a fixed-width
  // font and make it selectable rather than relying purely on the copy button.
  m_Endpoint->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  m_Endpoint->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  m_LastError->setWordWrap(true);

  QGridLayout *grid = new QGridLayout();
  grid->addWidget(new QLabel(tr("State:"), this), 0, 0);
  grid->addWidget(m_State, 0, 1);
  grid->addWidget(new QLabel(tr("Endpoint:"), this), 1, 0);
  grid->addWidget(m_Endpoint, 1, 1);
  grid->addWidget(new QLabel(tr("Active sessions:"), this), 2, 0);
  grid->addWidget(m_Sessions, 2, 1);
  grid->addWidget(new QLabel(tr("Registered tools:"), this), 3, 0);
  grid->addWidget(m_Tools, 3, 1);
  grid->addWidget(new QLabel(tr("Last error:"), this), 4, 0);
  grid->addWidget(m_LastError, 4, 1);
  grid->setColumnStretch(1, 1);

  m_Toggle = new QPushButton(tr("Start"), this);
  m_Refresh = new QPushButton(tr("Refresh"), this);
  m_Copy = new QPushButton(tr("Copy endpoint"), this);

  connect(m_Toggle, &QPushButton::clicked, this, &MCPStatusDialog::on_toggle_clicked);
  connect(m_Refresh, &QPushButton::clicked, this, &MCPStatusDialog::on_refresh_clicked);
  connect(m_Copy, &QPushButton::clicked, this, &MCPStatusDialog::on_copy_clicked);

  QHBoxLayout *buttons = new QHBoxLayout();
  buttons->addWidget(m_Copy);
  buttons->addStretch(1);
  buttons->addWidget(m_Refresh);
  buttons->addWidget(m_Toggle);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addLayout(grid);
  layout->addLayout(buttons);

  refresh();
}

MCPStatusDialog::~MCPStatusDialog()
{
}

void MCPStatusDialog::refresh()
{
  if(m_Server == NULL)
  {
    m_State->setText(tr("unavailable"));
    m_Endpoint->setText(tr("(not listening)"));
    m_Sessions->setText(QString());
    m_Tools->setText(QString());
    m_LastError->setText(tr("the MCP server is not available in this build"));
    m_Toggle->setEnabled(false);
    m_Refresh->setEnabled(false);
    m_Copy->setEnabled(false);
    return;
  }

  const MCP::ServerStatus status = m_Server->Status();

  if(status.running)
    m_State->setText(tr("running"));
  else if(m_Ctx.Config().MCP_Enabled)
    m_State->setText(tr("enabled, not listening"));
  else
    m_State->setText(tr("disabled"));

  if(status.running && !status.endpoint.isEmpty())
    m_Endpoint->setText(status.endpoint);
  else
    m_Endpoint->setText(tr("(not listening)"));

  m_Sessions->setText(QString::number(status.activeSessions));
  m_Tools->setText(QString::number(status.registeredTools));

  // Status() already carries the last error, but fall back to the raw accessor in
  // case a diagnostic was recorded outside of a status snapshot.
  const QString last = status.lastError.isEmpty() ? m_Server->LastError() : status.lastError;
  m_LastError->setText(last.isEmpty() ? tr("(none)") : last);

  m_Toggle->setText(status.running ? tr("Stop") : tr("Start"));
  m_Copy->setEnabled(status.running && !status.endpoint.isEmpty());
}

bool MCPStatusDialog::startServer()
{
  if(m_Server == NULL)
    return false;

  if(m_Server->IsRunning())
    return true;

  const int port = m_Ctx.Config().MCP_Port;

  QString error;
  if(!m_Server->Start(quint16(port & 0xffff), error))
  {
    RDDialog::critical(this, tr("MCP Server"),
                       tr("Could not start the MCP server on 127.0.0.1:%1.\n\n%2")
                           .arg(port)
                           .arg(error));
    return false;
  }

  return true;
}

void MCPStatusDialog::stopServer()
{
  if(m_Server != NULL)
    m_Server->Stop();
}

void MCPStatusDialog::on_toggle_clicked()
{
  if(m_Server == NULL)
    return;

  if(m_Server->IsRunning())
    stopServer();
  else
    startServer();

  refresh();
}

void MCPStatusDialog::on_refresh_clicked()
{
  refresh();
}

void MCPStatusDialog::on_copy_clicked()
{
  const QString endpoint = m_Endpoint->text();
  if(endpoint.isEmpty())
    return;

  QClipboard *clipboard = QApplication::clipboard();
  if(clipboard != NULL)
    clipboard->setText(endpoint);
}
