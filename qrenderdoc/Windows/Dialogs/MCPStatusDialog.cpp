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
#include <QPlainTextEdit>
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
  setWindowTitle(tr("MCP Control Center"));
  setMinimumSize(QSize(760, 560));
  resize(820, 610);

  setStyleSheet(lit(R"(
    MCPStatusDialog { background: #0d1118; color: #e7edf7; }
    QFrame#hero, QFrame#panel, QFrame#statCard { border: 1px solid #283346; border-radius: 12px; }
    QFrame#hero {
      background: qlineargradient(x1: 0, y1: 0, x2: 1, y2: 1,
                                  stop: 0 #17243a, stop: 0.55 #141d2b, stop: 1 #111823);
      border-color: #365070;
    }
    QFrame#panel { background: #121923; }
    QFrame#statCard { background: #101722; }
    QLabel#eyebrow, QLabel#fieldCaption, QLabel#statCaption {
      color: #8fa1ba; font-size: 10px; font-weight: 700; letter-spacing: 1px;
    }
    QLabel#heroTitle { color: #f5f8ff; font-size: 25px; font-weight: 700; }
    QLabel#heroSubtitle { color: #aec0d9; font-size: 12px; }
    QLabel#statusBadge {
      border-radius: 12px; padding: 5px 10px; font-size: 10px; font-weight: 700;
      letter-spacing: 1px; background: #293241; color: #b9c5d6;
    }
    QLabel#statusBadge[running="true"] { background: #123f39; color: #79ead5; }
    QLabel#statusBadge[warning="true"] { background: #49351b; color: #ffcf7a; }
    QLabel#statValue { color: #f4f7fc; font-size: 22px; font-weight: 700; }
    QLabel#connectionValue, QPlainTextEdit#configPreview {
      background: #090d13; border: 1px solid #29364a; border-radius: 7px; color: #b9f6e8;
      font-family: Consolas, "Cascadia Mono", monospace; font-size: 11px; padding: 7px;
    }
    QLabel#diagnosticValue { color: #c4d0e1; font-size: 12px; }
    QPushButton {
      background: #243146; border: 1px solid #3a4d69; border-radius: 7px; color: #eaf0fa;
      font-weight: 600; padding: 7px 13px;
    }
    QPushButton:hover { background: #30415b; border-color: #5d7ca7; }
    QPushButton#primaryButton { background: #3b67b1; border-color: #5e91e6; }
    QPushButton#primaryButton:hover { background: #4a79ca; }
    QPushButton#copyButton { background: #174e57; border-color: #2a8992; }
    QPushButton#copyButton:hover { background: #1d6570; }
    QPushButton:disabled { color: #718096; background: #18212e; border-color: #273444; }
  )"));

  m_State = new QLabel(this);
  m_Endpoint = new QLabel(this);
  m_AccessToken = new QLabel(this);
  m_Sessions = new QLabel(this);
  m_Tools = new QLabel(this);
  m_LastError = new QLabel(this);
  m_StatusBadge = new QLabel(this);
  m_Port = new QLabel(this);
  m_AccessHint = new QLabel(this);
  m_ConfigPreview = new QPlainTextEdit(this);

  m_Endpoint->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  m_AccessToken->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  m_Endpoint->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  m_AccessToken->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  m_LastError->setWordWrap(true);
  m_AccessHint->setWordWrap(true);
  m_AccessHint->setText(tr("Loopback-only endpoint. The bearer token is generated for this session and "
                           "changes whenever the server restarts."));
  m_AccessHint->setObjectName(lit("heroSubtitle"));
  m_ConfigPreview->setObjectName(lit("configPreview"));
  m_ConfigPreview->setReadOnly(true);
  m_ConfigPreview->setFixedHeight(102);
  m_ConfigPreview->setLineWrapMode(QPlainTextEdit::NoWrap);

  auto makeCaption = [this](const QString &text) {
    QLabel *label = new QLabel(text, this);
    label->setObjectName(lit("fieldCaption"));
    return label;
  };

  QFrame *hero = new QFrame(this);
  hero->setObjectName(lit("hero"));
  QHBoxLayout *heroLayout = new QHBoxLayout(hero);
  heroLayout->setContentsMargins(22, 18, 22, 18);
  QVBoxLayout *heroText = new QVBoxLayout();
  QLabel *eyebrow = new QLabel(tr("RENDERDOC  /  LOCAL AI TOOLING"), hero);
  eyebrow->setObjectName(lit("eyebrow"));
  QLabel *title = new QLabel(tr("MCP Control Center"), hero);
  title->setObjectName(lit("heroTitle"));
  QLabel *subtitle = new QLabel(tr("Secure, local access to your capture analysis tools."), hero);
  subtitle->setObjectName(lit("heroSubtitle"));
  heroText->addWidget(eyebrow);
  heroText->addSpacing(2);
  heroText->addWidget(title);
  heroText->addWidget(subtitle);
  heroLayout->addLayout(heroText, 1);
  heroLayout->addWidget(m_StatusBadge, 0, Qt::AlignTop | Qt::AlignRight);

  auto makeStat = [this, &makeCaption](const QString &caption, QLabel *value) {
    QFrame *card = new QFrame(this);
    card->setObjectName(lit("statCard"));
    QVBoxLayout *layout = new QVBoxLayout(card);
    layout->setContentsMargins(15, 11, 15, 11);
    QLabel *label = makeCaption(caption);
    label->setObjectName(lit("statCaption"));
    value->setObjectName(lit("statValue"));
    layout->addWidget(label);
    layout->addWidget(value);
    return card;
  };

  QHBoxLayout *stats = new QHBoxLayout();
  stats->setSpacing(10);
  stats->addWidget(makeStat(tr("SERVICE STATE"), m_State), 2);
  stats->addWidget(makeStat(tr("ACTIVE SESSIONS"), m_Sessions));
  stats->addWidget(makeStat(tr("REGISTERED TOOLS"), m_Tools));
  stats->addWidget(makeStat(tr("LISTENING PORT"), m_Port));

  QFrame *connectionPanel = new QFrame(this);
  connectionPanel->setObjectName(lit("panel"));
  QVBoxLayout *connection = new QVBoxLayout(connectionPanel);
  connection->setContentsMargins(18, 16, 18, 16);
  connection->setSpacing(7);
  connection->addWidget(makeCaption(tr("CONNECTION")));
  connection->addWidget(makeCaption(tr("MCP ENDPOINT")));
  m_Endpoint->setObjectName(lit("connectionValue"));
  connection->addWidget(m_Endpoint);
  connection->addWidget(makeCaption(tr("SESSION BEARER TOKEN")));
  m_AccessToken->setObjectName(lit("connectionValue"));
  connection->addWidget(m_AccessToken);
  connection->addWidget(m_AccessHint);

  QFrame *configPanel = new QFrame(this);
  configPanel->setObjectName(lit("panel"));
  QVBoxLayout *config = new QVBoxLayout(configPanel);
  config->setContentsMargins(18, 16, 18, 16);
  config->setSpacing(7);
  config->addWidget(makeCaption(tr("CLIENT CONFIGURATION")));
  QLabel *configHint = new QLabel(
      tr("Copy this JSON into a compatible MCP client. The token is session-scoped."), configPanel);
  configHint->setObjectName(lit("heroSubtitle"));
  configHint->setWordWrap(true);
  config->addWidget(configHint);
  config->addWidget(m_ConfigPreview);

  QHBoxLayout *mainPanels = new QHBoxLayout();
  mainPanels->setSpacing(10);
  mainPanels->addWidget(connectionPanel, 1);
  mainPanels->addWidget(configPanel, 1);

  QFrame *diagnosticPanel = new QFrame(this);
  diagnosticPanel->setObjectName(lit("panel"));
  QVBoxLayout *diagnostic = new QVBoxLayout(diagnosticPanel);
  diagnostic->setContentsMargins(18, 14, 18, 14);
  diagnostic->setSpacing(5);
  diagnostic->addWidget(makeCaption(tr("TRANSPORT DIAGNOSTICS")));
  m_LastError->setObjectName(lit("diagnosticValue"));
  diagnostic->addWidget(m_LastError);

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
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(10);
  layout->addWidget(hero);
  layout->addLayout(stats);
  layout->addLayout(mainPanels);
  layout->addWidget(diagnosticPanel);
  layout->addLayout(buttons);

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
    m_ConfigPreview->setPlainText(QString());
    m_Toggle->setEnabled(false);
    m_Refresh->setEnabled(false);
    m_Copy->setEnabled(false);
    return;
  }

  const MCP::ServerStatus status = m_Server->Status();
  const bool standby = !status.running && m_Ctx.Config().MCP_Enabled;

  m_State->setText(status.running ? tr("Running") : (standby ? tr("Standby") : tr("Disabled")));
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
    m_AccessToken->setText(m_Server->AccessToken());
  }
  else
  {
    m_Endpoint->setText(tr("(not listening)"));
    m_AccessToken->setText(tr("(not listening)"));
  }

  m_Sessions->setText(QString::number(status.activeSessions));
  m_Tools->setText(QString::number(status.registeredTools));

  const QString last = status.lastError.isEmpty() ? m_Server->LastError() : status.lastError;
  m_LastError->setText(last.isEmpty() ? tr("No transport errors recorded for this session.") : last);
  m_Toggle->setText(status.running ? tr("Stop server") : tr("Start server"));
  m_Copy->setEnabled(status.running && !status.endpoint.isEmpty());

  if(status.running && !status.endpoint.isEmpty())
  {
    QJsonObject headers;
    headers[QStringLiteral("Authorization")] =
        QStringLiteral("Bearer %1").arg(m_Server->AccessToken());
    QJsonObject config;
    config[QStringLiteral("url")] = status.endpoint;
    config[QStringLiteral("headers")] = headers;
    m_ConfigPreview->setPlainText(
        QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Indented)));
  }
  else
  {
    m_ConfigPreview->setPlainText(tr("Start the local MCP server to generate a connection profile."));
  }
}

bool MCPStatusDialog::startServer()
{
  if(m_Server == NULL || m_Server->IsRunning())
    return m_Server != NULL;

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
  if(m_Server == NULL || !m_Server->IsRunning())
    return;

  const QString endpoint = m_Endpoint->text();
  const QString token = m_Server->AccessToken();
  if(endpoint.isEmpty() || token.isEmpty())
    return;

  QJsonObject headers;
  headers[QStringLiteral("Authorization")] = QStringLiteral("Bearer %1").arg(token);
  QJsonObject config;
  config[QStringLiteral("url")] = endpoint;
  config[QStringLiteral("headers")] = headers;

  QClipboard *clipboard = QApplication::clipboard();
  if(clipboard != NULL)
    clipboard->setText(QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Indented)));
}
