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

#include "InvestigationReportDialog.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include "Code/AI/MCPQueryAdapter.h"
#include "Code/Interface/QRDInterface.h"
#include "Code/QRDUtils.h"

InvestigationReportDialog::InvestigationReportDialog(ICaptureContext &ctx, MCP::QueryAdapter *adapter,
                                                     QWidget *parent)
    : QDialog(parent), m_Ctx(ctx), m_Adapter(adapter)
{
  setWindowTitle(tr("Investigation Report"));

  m_Format = new QComboBox(this);
  // Keep the display strings localised but the values stable: index 0 is the
  // documented markdown default, index 1 the machine-readable JSON form.
  m_Format->addItem(tr("Markdown"), QStringLiteral("markdown"));
  m_Format->addItem(tr("JSON"), QStringLiteral("json"));

  m_Limit = new QSpinBox(this);
  // Matches the tool contract: default 20 rows, hard maximum 200. The adapter
  // clamps again, this is just so the UI can't express an out-of-range value.
  m_Limit->setRange(1, 200);
  m_Limit->setValue(20);

  m_IncludeResources = new QCheckBox(tr("Include resource footprint"), this);
  m_IncludeResources->setChecked(true);
  m_IncludeResources->setToolTip(
      tr("Attach the resource-memory audit (largest resources and format breakdown). Turn this off "
         "for a more compact report."));

  connect(m_Format, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          &InvestigationReportDialog::on_format_changed);
  // editingFinished() rather than valueChanged() so a partially typed number is
  // never fed to the (replay-backed) builder.
  connect(m_Limit, &QSpinBox::editingFinished, this,
          &InvestigationReportDialog::on_options_changed);
  connect(m_IncludeResources, &QCheckBox::toggled, this,
          &InvestigationReportDialog::on_options_changed);

  QGridLayout *options = new QGridLayout();
  options->addWidget(new QLabel(tr("Format:"), this), 0, 0);
  options->addWidget(m_Format, 0, 1);
  options->addWidget(new QLabel(tr("Row limit:"), this), 0, 2);
  options->addWidget(m_Limit, 0, 3);
  options->addWidget(m_IncludeResources, 1, 0, 1, 4);
  options->setColumnStretch(4, 1);

  m_Preview = new QPlainTextEdit(this);
  m_Preview->setReadOnly(true);
  m_Preview->setLineWrapMode(QPlainTextEdit::NoWrap);
  m_Preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

  m_Status = new QLabel(this);
  m_Status->setWordWrap(true);

  m_Export = new QPushButton(tr("Save As..."), this);
  m_Copy = new QPushButton(tr("Copy"), this);
  m_Refresh = new QPushButton(tr("Refresh"), this);

  connect(m_Export, &QPushButton::clicked, this, &InvestigationReportDialog::on_export_clicked);
  connect(m_Copy, &QPushButton::clicked, this, &InvestigationReportDialog::on_copy_clicked);
  connect(m_Refresh, &QPushButton::clicked, this, &InvestigationReportDialog::on_refresh_clicked);

  QHBoxLayout *buttons = new QHBoxLayout();
  buttons->addWidget(m_Copy);
  buttons->addWidget(m_Export);
  buttons->addStretch(1);
  buttons->addWidget(m_Refresh);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addLayout(options);
  layout->addWidget(m_Preview, 1);
  layout->addWidget(m_Status);
  layout->addLayout(buttons);

  resize(720, 540);

  refresh();
}

InvestigationReportDialog::~InvestigationReportDialog()
{
}

QString InvestigationReportDialog::selectedFormat() const
{
  const QString value = m_Format->currentData().toString();
  return value.isEmpty() ? QStringLiteral("markdown") : value;
}

bool InvestigationReportDialog::refresh()
{
  m_Content.clear();
  m_SuggestedName.clear();
  m_Preview->setPlainText(QString());

  if(m_Adapter == NULL)
  {
    m_Status->setText(tr("The investigation report is not available in this build."));
    m_Export->setEnabled(false);
    m_Copy->setEnabled(false);
    m_Refresh->setEnabled(false);
    return false;
  }

  // The dialog drives the same capability the MCP tool exposes, so it goes
  // through the adapter rather than re-implementing the composition. The
  // CallContext carries no cancellation token and no deadline: this is a
  // user-initiated, foreground build.
  MCP::CallContext call;
  call.ctx = &m_Ctx;

  const MCP::ToolResult result =
      m_Adapter->BuildInvestigationReport(selectedFormat(), m_Limit->value(),
                                          m_IncludeResources->isChecked(), call);

  if(!result.ok)
  {
    const QString message =
        result.error.message.isEmpty() ? tr("The report could not be built.")
                                       : result.error.message;
    m_Status->setText(message);
    m_Export->setEnabled(false);
    m_Copy->setEnabled(false);
    return false;
  }

  m_Content = result.structured.value(QStringLiteral("content")).toString();

  const QJsonObject report = result.structured.value(QStringLiteral("report")).toObject();
  const QString capturePath = report.value(QStringLiteral("capturePath")).toString();
  if(!capturePath.isEmpty())
  {
    const QFileInfo info(capturePath);
    m_SuggestedName =
        info.dir().absoluteFilePath(info.completeBaseName() + QStringLiteral("-investigation"));
  }

  m_Preview->setPlainText(m_Content);

  const int bookmarks = report.value(QStringLiteral("bookmarkCount")).toInt();
  const int hotspots = report.value(QStringLiteral("performance"))
                           .toObject()
                           .value(QStringLiteral("hottestEvents"))
                           .toArray()
                           .size();

  m_Status->setText(
      tr("Report built: %1 bookmarks, %2 hotspot events.").arg(bookmarks).arg(hotspots));
  m_Export->setEnabled(!m_Content.isEmpty());
  m_Copy->setEnabled(!m_Content.isEmpty());
  return true;
}

void InvestigationReportDialog::exportContent()
{
  // Export exactly the preview the user inspected. Option changes already rebuild
  // through their slots, so rebuilding here merely repeats expensive replay work.
  if(m_Content.isEmpty())
    return;

  const bool json = selectedFormat() == QStringLiteral("json");
  const QString filter =
      json ? tr("JSON Files (*.json);;All Files (*)") : tr("Markdown Files (*.md);;All Files (*)");

  QString path = RDDialog::getSaveFileName(this, tr("Save Investigation Report"), m_SuggestedName,
                                           filter);
  if(path.isEmpty())
    return;

  // Only add the extension the chosen format expects when the user typed a bare
  // name, so an explicit ".txt" is respected.
  if(QFileInfo(path).suffix().isEmpty())
    path += json ? QStringLiteral(".json") : QStringLiteral(".md");

  QFile file(path);
  if(!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    RDDialog::critical(this, tr("Save Investigation Report"),
                       tr("Could not open \"%1\" for writing:\n\n%2").arg(path).arg(file.errorString()));
    return;
  }

  const QByteArray bytes = m_Content.toUtf8();
  if(file.write(bytes) != bytes.size())
  {
    const QString error = file.errorString();
    file.close();
    RDDialog::critical(this, tr("Save Investigation Report"),
                       tr("Could not write \"%1\":\n\n%2").arg(path).arg(error));
    return;
  }

  file.close();

  RDDialog::information(this, tr("Save Investigation Report"),
                        tr("Wrote the investigation report to:\n\n%1").arg(path));
}

void InvestigationReportDialog::on_format_changed()
{
  refresh();
}

void InvestigationReportDialog::on_options_changed()
{
  refresh();
}

void InvestigationReportDialog::on_refresh_clicked()
{
  refresh();
}

void InvestigationReportDialog::on_export_clicked()
{
  exportContent();
}

void InvestigationReportDialog::on_copy_clicked()
{
  if(m_Content.isEmpty())
    return;

  QClipboard *clipboard = QApplication::clipboard();
  if(clipboard != NULL)
    clipboard->setText(m_Content);
}
