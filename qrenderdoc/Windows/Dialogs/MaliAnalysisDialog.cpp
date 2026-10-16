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

#include "MaliAnalysisDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Code/Interface/QRDInterface.h"
#include "Code/QRDUtils.h"
#include "Windows/Dialogs/SettingsDialog.h"

namespace
{
QHash<QString, MCP::MaliRunResult> &MaliResultCache()
{
  static QHash<QString, MCP::MaliRunResult> cache;
  return cache;
}

QString DisplayCommand(const QStringList &argv)
{
  QStringList quoted;
  for(const QString &argument : argv)
  {
    QString value = argument;
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    if(value.contains(QRegularExpression(QStringLiteral("\\s"))))
      value = QLatin1Char('"') + value + QLatin1Char('"');
    quoted.push_back(value);
  }
  return quoted.join(QLatin1Char(' '));
}
}    // namespace

MaliAnalysisDialog::MaliAnalysisDialog(ICaptureContext &ctx, const MCP::MaliShaderExport &shader,
                                       QWidget *parent)
    : QDialog(parent), m_Ctx(ctx), m_Shader(shader),
      m_Cancelled(new std::atomic_bool(false))
{
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Mali Offline Compiler"));

  m_Device = new QComboBox(this);
  m_Device->addItem(tr("Default profile (chosen by malioc)"), QString());
  for(const QString &device : MCP::MaliDeviceNames())
    m_Device->addItem(device, device);

  const rdcstr &configuredDeviceName = m_Ctx.Config().MCP_MaliDevice;
  const QString configuredDevice =
      MCP::NormalizeMaliDevice(QString::fromUtf8(configuredDeviceName.c_str(),
                                                  int(configuredDeviceName.size())));
  const int configuredIndex = m_Device->findData(configuredDevice);
  if(configuredIndex >= 0)
    m_Device->setCurrentIndex(configuredIndex);

  m_ShaderInfo = new QLabel(this);
  m_ShaderInfo->setWordWrap(true);
  m_ShaderInfo->setText(tr("Captured shader: %1  |  Entry point: %2  |  Encoding: %3")
                            .arg(m_Shader.stage, m_Shader.entryPoint,
                                 MCP::MaliEncodingName(m_Shader.encoding)));

  m_Command = new QLabel(this);
  m_Command->setWordWrap(true);
  m_Command->setText(tr("The compiler command will be shown after analysis starts."));

  QFormLayout *options = new QFormLayout();
  options->addRow(tr("Target device:"), m_Device);
  options->addRow(tr("Shader:"), m_ShaderInfo);
  options->addRow(tr("Command:"), m_Command);

  m_Summary = new QLabel(this);
  m_Summary->setWordWrap(true);
  m_Summary->setText(tr("Choose a target profile and run static analysis of the captured shader."));

  m_Metrics = new QTreeWidget(this);
  m_Metrics->setColumnCount(3);
  m_Metrics->setHeaderLabels({tr("Metric"), tr("Value"), tr("Unit")});
  m_Metrics->setAlternatingRowColors(true);
  m_Metrics->setRootIsDecorated(true);

  m_Output = new QPlainTextEdit(this);
  m_Output->setReadOnly(true);
  m_Output->setLineWrapMode(QPlainTextEdit::NoWrap);
  m_Output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  m_Output->setPlainText(tr("Raw malioc output will appear here."));

  QTabWidget *tabs = new QTabWidget(this);
  tabs->addTab(m_Metrics, tr("Metrics"));
  tabs->addTab(m_Output, tr("Raw Output"));

  m_Status = new QLabel(this);
  m_Status->setWordWrap(true);

  m_Configure = new QPushButton(tr("Configure Path..."), this);
  m_Run = new QPushButton(tr("Run Analysis"), this);
  m_Copy = new QPushButton(tr("Copy Raw Output"), this);
  QPushButton *close = new QPushButton(tr("Close"), this);

  connect(m_Configure, &QPushButton::clicked, this, [this] { configureCompiler(); });
  connect(m_Run, &QPushButton::clicked, this,
          [this] { m_Running ? cancelAnalysis() : startAnalysis(); });
  connect(m_Device, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [this](int) { restoreCachedResult(); });
  connect(m_Copy, &QPushButton::clicked, this,
          [this] { QApplication::clipboard()->setText(m_Output->toPlainText()); });
  connect(close, &QPushButton::clicked, this, &QDialog::close);

  QHBoxLayout *buttons = new QHBoxLayout();
  buttons->addWidget(m_Configure);
  buttons->addWidget(m_Copy);
  buttons->addStretch(1);
  buttons->addWidget(m_Run);
  buttons->addWidget(close);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addLayout(options);
  layout->addWidget(m_Summary);
  layout->addWidget(tabs, 1);
  layout->addWidget(m_Status);
  layout->addLayout(buttons);

  resize(780, 580);
  restoreCachedResult();
}

MaliAnalysisDialog::~MaliAnalysisDialog()
{
  // RunMaliAnalysis polls this while its child process is alive, so closing the
  // window terminates an in-flight invocation rather than leaving it orphaned.
  m_Cancelled->store(true);
}

void MaliAnalysisDialog::configureCompiler()
{
  SettingsDialog settings(m_Ctx, this);
  RDDialog::show(&settings);

  const rdcstr &configuredDeviceName = m_Ctx.Config().MCP_MaliDevice;
  const QString configuredDevice =
      MCP::NormalizeMaliDevice(QString::fromUtf8(configuredDeviceName.c_str(),
                                                  int(configuredDeviceName.size())));
  const int configuredIndex = m_Device->findData(configuredDevice);
  if(configuredIndex >= 0)
    m_Device->setCurrentIndex(configuredIndex);
}

void MaliAnalysisDialog::setRunning(bool running)
{
  m_Running = running;
  m_Device->setEnabled(!running);
  m_Configure->setEnabled(!running);
  m_Run->setEnabled(true);
  m_Run->setText(running ? tr("Cancel Analysis") : tr("Run Analysis"));
}

QString MaliAnalysisDialog::cacheKey() const
{
  const rdcstr &configuredPath = m_Ctx.Config().MCP_MaliCompilerPath;
  const rdcstr &configuredArgs = m_Ctx.Config().MCP_MaliExtraArgs;
  return m_Shader.revision + QLatin1Char('|') + m_Device->currentData().toString() + QLatin1Char('|') +
         QString::fromUtf8(configuredPath.c_str(), int(configuredPath.size())) + QLatin1Char('|') +
         QString::fromUtf8(configuredArgs.c_str(), int(configuredArgs.size()));
}

void MaliAnalysisDialog::restoreCachedResult()
{
  const MCP::MaliRunResult result = MaliResultCache().value(cacheKey());
  if(result.status != MCP::MaliRunStatus::Ran)
  {
    clearDisplayedResult();
    return;
  }

  showResult(result);
  m_Status->setText(tr("Showing cached analysis for shader revision %1. Re-run to refresh.")
                        .arg(m_Shader.revision.left(12)));
}

void MaliAnalysisDialog::clearDisplayedResult()
{
  m_Metrics->clear();
  m_Command->setText(tr("The compiler command will be shown after analysis starts."));
  m_Summary->setText(tr("No cached result exists for this target/profile. Run static analysis of the captured shader."));
  m_Output->setPlainText(tr("Raw malioc output will appear here."));
  m_Copy->setEnabled(false);
  m_Status->setText(tr("Ready to analyze shader revision %1.").arg(m_Shader.revision.left(12)));
}

void MaliAnalysisDialog::cancelAnalysis()
{
  if(!m_Running)
    return;

  m_Cancelled->store(true);
  m_Run->setEnabled(false);
  m_Run->setText(tr("Cancelling..."));
  m_Status->setText(tr("Cancelling the running Mali Offline Compiler process..."));
}

void MaliAnalysisDialog::startAnalysis()
{
  if(m_Running)
    return;

  m_Cancelled->store(false);
  setRunning(true);
  m_Metrics->clear();
  m_Summary->setText(tr("malioc is running in the background..."));
  m_Status->setText(tr("Exporting the captured shader and starting Mali Offline Compiler."));
  m_Output->setPlainText(QString());

  MCP::MaliRunRequest request;
  const rdcstr &configuredPath = m_Ctx.Config().MCP_MaliCompilerPath;
  request.configuredPath =
      QString::fromUtf8(configuredPath.c_str(), int(configuredPath.size()));
  request.target.device = m_Device->currentData().toString();
  request.target.timeoutMs = 30000;

  const rdcstr &configuredArgsValue = m_Ctx.Config().MCP_MaliExtraArgs;
  const QString configuredArgs =
      QString::fromUtf8(configuredArgsValue.c_str(), int(configuredArgsValue.size()));
  if(!configuredArgs.isEmpty())
    request.target.extraArgs.push_back(configuredArgs);

  const MCP::MaliShaderExport shader = m_Shader;
  const QSharedPointer<std::atomic_bool> cancelled = m_Cancelled;
  QPointer<MaliAnalysisDialog> dialog(this);

  LambdaThread *thread = new LambdaThread([request, shader, cancelled, dialog]() mutable {
    MCP::MaliRunResult result;

    if(!shader.limitation.isEmpty())
    {
      result.status = MCP::MaliRunStatus::UnsupportedShader;
      result.hint = shader.limitation;
    }
    else if(cancelled->load())
    {
      result.status = MCP::MaliRunStatus::Cancelled;
      result.hint = QObject::tr("The analysis request was cancelled before export.");
    }
    else
    {
      QTemporaryFile temporary(QDir::tempPath() + QStringLiteral("/renderdoc_mali_XXXXXX") +
                               shader.extension);
      temporary.setAutoRemove(true);
      if(!temporary.open() || temporary.write(shader.contents) != shader.contents.size() ||
         !temporary.flush())
      {
        result.status = MCP::MaliRunStatus::FailedToStart;
        result.hint = QObject::tr("Could not create the temporary shader export file.");
        result.report.error = result.hint;
      }
      else
      {
        temporary.close();
        request.shaderPath = temporary.fileName();
        result = MCP::RunMaliAnalysis(request, [cancelled] { return cancelled->load(); });
      }
    }

    // The dialog can disappear as the capture/viewer closes. Queue through the
    // application object rather than the dialog so a destroyed dialog is never
    // used as GUIInvoke's target; QPointer guards the actual delivery.
    GUIInvoke::call(QCoreApplication::instance(), [dialog, result] {
      if(dialog)
        dialog->showResult(result);
    });
  });
  thread->setName(lit("Mali offline analysis"));
  thread->selfDelete(true);
  thread->start();
}

void MaliAnalysisDialog::showResult(const MCP::MaliRunResult &result)
{
  setRunning(false);

  if(result.status == MCP::MaliRunStatus::Ran)
    MaliResultCache().insert(cacheKey(), result);

  m_Command->setText(result.commandLine.isEmpty()
                          ? tr("No compiler command was launched.")
                          : DisplayCommand(result.commandLine));

  const MCP::MaliReport &report = result.report;
  QStringList details;
  if(!report.device.isEmpty())
    details << tr("Target: %1").arg(report.device);
  if(!report.hardware.isEmpty())
    details << tr("Hardware: %1").arg(report.hardware);
  if(!report.compilerVersion.isEmpty())
    details << report.compilerVersion;
  if(report.hasTotals)
  {
    details << tr("Cycles: %1").arg(QString::number(report.totalCycles, 'g', 12));
    details << tr("Instructions: %1").arg(QString::number(report.totalInstructions, 'g', 12));
    details << tr("Registers: %1").arg(QString::number(report.registers, 'g', 12));
    details << tr("Spills: %1 bytes").arg(QString::number(report.spillBytes, 'g', 12));
  }
  if(report.hasWorkRegisters)
    details << tr("Work registers: %1").arg(QString::number(report.workRegisters, 'g', 12));
  if(report.hasUniformRegisters)
    details << tr("Uniform registers: %1").arg(QString::number(report.uniformRegisters, 'g', 12));
  if(report.hasStackSize)
    details << tr("Stack size: %1 bytes").arg(QString::number(report.stackSize, 'g', 12));
  if(report.stackSpillingKnown)
    details << (report.stackSpilling ? tr("Stack spilling: yes") : tr("Stack spilling: no"));
  if(report.shortestPath.available)
    details << tr("Shortest path: %1 cycles").arg(QString::number(report.shortestPath.arithmetic, 'g', 12));
  if(report.longestPath.available)
    details << tr("Longest path: %1 cycles").arg(QString::number(report.longestPath.arithmetic, 'g', 12));
  if(!report.riskSignals.isEmpty())
    details << tr("Risks: %1").arg(report.riskSignals.join(QStringLiteral(", ")));
  m_Summary->setText(details.isEmpty() ? tr("No recognised summary metrics were reported.")
                                       : details.join(QStringLiteral("  |  ")));

  for(const MCP::MaliSection &section : report.sections)
  {
    QTreeWidgetItem *parent = new QTreeWidgetItem(m_Metrics, QStringList(section.title));
    parent->setFirstColumnSpanned(true);
    parent->setExpanded(true);
    for(const MCP::MaliMetric &metric : section.metrics)
    {
      const QString value = metric.numeric ? QString::number(metric.value, 'g', 12) : metric.text;
      new QTreeWidgetItem(parent, {metric.name, value, metric.unit});
    }
  }
  for(const QString &warning : report.warnings)
    new QTreeWidgetItem(m_Metrics, {tr("Warning"), warning, QString()});
  for(const QString &risk : report.riskSignals)
    new QTreeWidgetItem(m_Metrics, {tr("Risk"), risk, QString()});
  m_Metrics->resizeColumnToContents(0);
  m_Metrics->resizeColumnToContents(1);

  m_Output->setPlainText(report.rawOutput);
  m_Copy->setEnabled(!report.rawOutput.isEmpty());

  QString status;
  if(result.status == MCP::MaliRunStatus::Ran && report.error.isEmpty())
    status = tr("Analysis completed (malioc exit code %1).").arg(report.exitCode);
  else
    status = !result.hint.isEmpty() ? result.hint : report.error;
  if(!report.limitation.isEmpty())
    status += (status.isEmpty() ? QString() : QStringLiteral("\n")) + report.limitation;
  if(status.isEmpty())
    status = tr("Mali analysis did not return a status message.");
  m_Status->setText(status);
}
