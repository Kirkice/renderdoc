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
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include "Code/AI/MCPHttpServer.h"
#include "Code/Interface/QRDInterface.h"
#include "Code/QRDUtils.h"

MCPStatusDialog::MCPStatusDialog(ICaptureContext &ctx, MCP::HttpServer *server, QWidget *parent)
    : QWidget(parent), m_Ctx(ctx), m_Server(server)
{
  setWindowTitle(tr("MCP Server"));
  setMinimumSize(QSize(320, 280));
  resize(360, 360);

  // Preserve the purpose-built control-centre layout while deriving every colour from the
  // active RenderDoc palette. This keeps the page visually coherent in every application theme.
  setStyleSheet(lit(R"(
    QFrame#header, QFrame#connection {
      background: palette(alternate-base);
      border: 1px solid palette(mid);
      border-radius: 7px;
    }
    QLabel#title { font-size: 16px; font-weight: 700; }
    QLabel#subtitle, QLabel#fieldCaption, QLabel#statCaption { font-size: 11px; }
    QLabel#fieldCaption, QLabel#statCaption { font-weight: 600; }
    QLabel#statusBadge {
      background: palette(mid);
      color: palette(text);
      border-radius: 9px;
      padding: 3px 8px;
      font-size: 10px;
      font-weight: 700;
    }
    QLabel#statusBadge[running="true"] {
      background: palette(highlight);
      color: palette(highlighted-text);
    }
    QLabel#statusBadge[warning="true"] { background: palette(midlight); }
    QLabel#state { font-size: 12px; }
    QLabel#statValue { font-size: 15px; font-weight: 700; }
    QLabel#connectionValue {
      background: palette(base);
      border: 1px solid palette(mid);
      border-radius: 4px;
      font-family: Consolas, "Cascadia Mono", monospace;
      font-size: 11px;
      padding: 5px 6px;
    }
    QLabel#diagnosticValue { font-size: 11px; }
    QPushButton { font-weight: 600; padding: 6px 10px; }
    QPushButton#primaryButton {
      background: palette(highlight);
      color: palette(highlighted-text);
    }
  )"));

  m_State = new QLabel(this);
  m_Endpoint = new QLabel(this);
  m_AccessToken = new QLabel(this);
  m_Sessions = new QLabel(this);
  m_Tools = new QLabel(this);
  m_LastError = new QLabel(this);
  m_StatusBadge = new QLabel(this);
  m_Port = new QLabel(this);

  m_Endpoint->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  m_AccessToken->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  m_Endpoint->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  m_AccessToken->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  m_LastError->setWordWrap(true);

  auto makeCaption = [this](const QString &text) {
    QLabel *label = new QLabel(text, this);
    label->setObjectName(lit("fieldCaption"));
    return label;
  };

  QFrame *header = new QFrame(this);
  header->setObjectName(lit("header"));
  QHBoxLayout *headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(12, 10, 12, 10);
  QVBoxLayout *headerText = new QVBoxLayout();
  headerText->setSpacing(1);
  QLabel *title = new QLabel(tr("MCP Server"), header);
  title->setObjectName(lit("title"));
  m_State->setObjectName(lit("state"));
  headerText->addWidget(title);
  headerText->addWidget(m_State);
  headerLayout->addLayout(headerText, 1);
  headerLayout->addWidget(m_StatusBadge, 0, Qt::AlignTop | Qt::AlignRight);

  auto makeStat = [this, &makeCaption](const QString &caption, QLabel *value) {
    QWidget *card = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(1);
    QLabel *label = makeCaption(caption);
    label->setObjectName(lit("statCaption"));
    value->setObjectName(lit("statValue"));
    layout->addWidget(label);
    layout->addWidget(value);
    return card;
  };

  QHBoxLayout *stats = new QHBoxLayout();
  stats->setSpacing(16);
  stats->addWidget(makeStat(tr("PORT"), m_Port));
  stats->addWidget(makeStat(tr("CLIENTS"), m_Sessions));
  stats->addWidget(makeStat(tr("TOOLS"), m_Tools));
  stats->addStretch(1);

  QFrame *connectionPanel = new QFrame(this);
  connectionPanel->setObjectName(lit("connection"));
  QVBoxLayout *connection = new QVBoxLayout(connectionPanel);
  connection->setContentsMargins(12, 11, 12, 11);
  connection->setSpacing(5);
  connection->addWidget(makeCaption(tr("CONNECTION")));
  connection->addWidget(makeCaption(tr("MCP ENDPOINT")));
  m_Endpoint->setObjectName(lit("connectionValue"));
  connection->addWidget(m_Endpoint);
  connection->addWidget(makeCaption(tr("SESSION BEARER TOKEN")));
  m_AccessToken->setObjectName(lit("connectionValue"));
  connection->addWidget(m_AccessToken);
  m_LastError->setObjectName(lit("diagnosticValue"));

  m_Toggle = new QPushButton(tr("Start server"), this);
  m_Refresh = new QPushButton(tr("Refresh"), this);
  m_Copy = new QPushButton(tr("Copy connection"), this);
  m_Toggle->setObjectName(lit("primaryButton"));
  m_Copy->setObjectName(lit("copyButton"));

  connect(m_Toggle, &QPushButton::clicked, this, &MCPStatusDialog::on_toggle_clicked);
  connect(m_Refresh, &QPushButton::clicked, this, &MCPStatusDialog::on_refresh_clicked);
  connect(m_Copy, &QPushButton::clicked, this, &MCPStatusDialog::on_copy_clicked);

  QHBoxLayout *buttons = new QHBoxLayout();
  buttons->addWidget(m_Copy, 0, Qt::AlignLeft);
  buttons->addStretch(1);
  buttons->addWidget(m_Refresh);
  buttons->addWidget(m_Toggle);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 10, 10, 10);
  layout->setSpacing(9);
  layout->addWidget(header);
  layout->addLayout(stats);
  layout->addWidget(connectionPanel);
  layout->addWidget(m_LastError);
  layout->addLayout(buttons);
  layout->addStretch(1);

  m_AutoRefresh = new QTimer(this);
  m_AutoRefresh->setInterval(1500);
  connect(m_AutoRefresh, &QTimer::timeout, this, &MCPStatusDialog::refresh);
  m_AutoRefresh->start();

  refresh();
}

MCPStatusDialog::~MCPStatusDialog()
{
}

void MCPStatusDialog::refresh()
{
  if(m_Server == NULL)
  {
    m_State->setText(tr("Unavailable"));
    m_StatusBadge->setText(tr("UNAVAILABLE"));
    m_StatusBadge->setProperty("running", false);
    m_StatusBadge->setProperty("warning", true);
    m_Endpoint->setText(tr("(not listening)"));
    m_AccessToken->setText(tr("(not listening)"));
    m_Sessions->setText(QString());
    m_Tools->setText(QString());
    m_Port->setText(QString::number(m_Ctx.Config().MCP_Port));
    m_LastError->setText(tr("The MCP server is not available in this build."));
    m_Toggle->setEnabled(false);
    m_Refresh->setEnabled(false);
    m_Copy->setEnabled(false);
    return;
  }

  const MCP::ServerStatus status = m_Server->Status();
  const bool standby = !status.running && m_Ctx.Config().MCP_Enabled;

  m_State->setText(status.running ? tr("Local capture analysis is ready.")
                                  : (standby ? tr("Enabled in settings; not listening yet.")
                                             : tr("Disabled in settings.")));
  m_StatusBadge->setText(status.running ? tr("ONLINE")
                                        : (standby ? tr("STANDBY") : tr("DISABLED")));
  m_StatusBadge->setProperty("running", status.running);
  m_StatusBadge->setProperty("warning", !status.running);
  m_StatusBadge->style()->unpolish(m_StatusBadge);
  m_StatusBadge->style()->polish(m_StatusBadge);
  m_Port->setText(QString::number(m_Ctx.Config().MCP_Port));

  if(status.running && !status.endpoint.isEmpty())
  {
    m_Endpoint->setText(status.endpoint);
    m_AccessToken->setText(m_Server->RequiresToken() ? m_Server->AccessToken() : tr("Not required (unsafe)"));
  }
  else
  {
    m_Endpoint->setText(tr("(not listening)"));
    m_AccessToken->setText(tr("(not listening)"));
  }

  m_Sessions->setText(QString::number(status.activeSessions));
  m_Tools->setText(QString::number(status.registeredTools));

  const QString last = status.lastError.isEmpty() ? m_Server->LastError() : status.lastError;
  m_LastError->setText(last.isEmpty()
                            ? (m_Server->RequiresToken()
                                   ? tr("Loopback-only. The access token changes whenever the server restarts.")
                                   : tr("Warning: token authentication is disabled. Any local process can call this MCP server."))
                            : tr("Error: %1").arg(last));
  m_Toggle->setText(status.running ? tr("Stop server") : tr("Start server"));
  m_Copy->setEnabled(status.running && !status.endpoint.isEmpty());

}

bool MCPStatusDialog::startServer()
{
  if(m_Server == NULL || m_Server->IsRunning())
    return m_Server != NULL;

  const int port = m_Ctx.Config().MCP_Port;
  QString error;
  if(!m_Server->Start(quint16(port & 0xffff), m_Ctx.Config().MCP_RequireToken, error))
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
  if(m_Server == NULL || !m_Server->IsRunning())
    return;

  const QString endpoint = m_Endpoint->text();
  if(endpoint.isEmpty())
    return;

  QJsonObject config;
  config[QStringLiteral("url")] = endpoint;
  if(m_Server->RequiresToken())
  {
    const QString token = m_Server->AccessToken();
    if(token.isEmpty())
      return;

    QJsonObject headers;
    headers[QStringLiteral("Authorization")] = QStringLiteral("Bearer %1").arg(token);
    config[QStringLiteral("headers")] = headers;
  }

  QClipboard *clipboard = QApplication::clipboard();
  if(clipboard != NULL)
    clipboard->setText(QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Indented)));
}
