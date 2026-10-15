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

// P2-5 environment and external-tool diagnostics. This module is deliberately
// data-only: callers provide observed state/configuration, and it produces
// structured checks plus actionable recovery guidance without starting tools,
// probing the network, or requiring a loaded capture.

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace MCP
{
enum class DiagnosticSeverity
{
  Info,
  Warning,
  Error,
};

QString DiagnosticSeverityName(DiagnosticSeverity severity);

struct DiagnosticCheck
{
  QString id;
  QString title;
  DiagnosticSeverity severity = DiagnosticSeverity::Info;
  bool available = true;
  QString summary;
  QString recovery;
};

struct EnvironmentDiagnosticInput
{
  bool serverEnabled = false;
  int serverPort = 0;
  bool sensitiveDataAllowed = false;
  QString platform;
  QString maliConfiguredPath;
  QString maliResolvedExecutable;
  bool maliExecutableExists = false;
  bool supportsMali = true;
  bool supportsSourceMapping = true;
};

struct EnvironmentDiagnosticReport
{
  QString platform;
  QVector<DiagnosticCheck> checks;
  QStringList limitations;
  bool healthy = true;
};

EnvironmentDiagnosticReport BuildEnvironmentDiagnosticReport(const EnvironmentDiagnosticInput &input);
QJsonObject DiagnosticCheckToJson(const DiagnosticCheck &check);
QJsonObject EnvironmentDiagnosticReportToJson(const EnvironmentDiagnosticReport &report);
}    // namespace MCP
