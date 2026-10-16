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

#include "MCPDiagnostics.h"

#include <QJsonArray>

namespace MCP
{
namespace
{
void AddCheck(EnvironmentDiagnosticReport &report, const QString &id, const QString &title,
              DiagnosticSeverity severity, bool available, const QString &summary,
              const QString &recovery = QString())
{
  DiagnosticCheck check;
  check.id = id;
  check.title = title;
  check.severity = severity;
  check.available = available;
  check.summary = summary;
  check.recovery = recovery;
  report.checks.push_back(check);

  if(severity == DiagnosticSeverity::Error)
    report.healthy = false;
}

void AddLimitation(EnvironmentDiagnosticReport &report, const QString &limitation)
{
  if(!limitation.isEmpty() && !report.limitations.contains(limitation))
    report.limitations.push_back(limitation);
}
}    // namespace

QString DiagnosticSeverityName(DiagnosticSeverity severity)
{
  switch(severity)
  {
    case DiagnosticSeverity::Info: return QStringLiteral("info");
    case DiagnosticSeverity::Warning: return QStringLiteral("warning");
    case DiagnosticSeverity::Error: return QStringLiteral("error");
  }
  return QStringLiteral("unknown");
}

EnvironmentDiagnosticReport BuildEnvironmentDiagnosticReport(const EnvironmentDiagnosticInput &input)
{
  EnvironmentDiagnosticReport report;
  report.platform = input.platform;

  if(input.serverEnabled)
  {
    AddCheck(report, QStringLiteral("mcp.server"), QStringLiteral("MCP server"),
             DiagnosticSeverity::Info, true,
             QStringLiteral("The loopback MCP server is enabled on port %1.").arg(input.serverPort));
  }
  else
  {
    AddCheck(report, QStringLiteral("mcp.server"), QStringLiteral("MCP server"),
             DiagnosticSeverity::Warning, false,
             QStringLiteral("The MCP server is disabled."),
             QStringLiteral("Enable MCP_Enabled in qrenderdoc settings, then restart the server."));
  }

  if(input.sensitiveDataAllowed)
  {
    AddCheck(report, QStringLiteral("mcp.sensitiveData"), QStringLiteral("Sensitive capture data"),
             DiagnosticSeverity::Warning, true,
             QStringLiteral("Sensitive MCP tools are enabled and may return shader or resource contents."),
             QStringLiteral("Disable MCP_AllowSensitiveData when a client does not require capture contents."));
  }
  else
  {
    AddCheck(report, QStringLiteral("mcp.sensitiveData"), QStringLiteral("Sensitive capture data"),
             DiagnosticSeverity::Info, true,
             QStringLiteral("Sensitive MCP tools are disabled; metadata-only tools remain available."));
  }

  if(!input.supportsSourceMapping)
  {
    AddCheck(report, QStringLiteral("sourceMapping"), QStringLiteral("Project source mapping"),
             DiagnosticSeverity::Warning, false,
             QStringLiteral("Project source mapping is unavailable on this platform configuration."));
    AddLimitation(report, QStringLiteral("Project source mapping is unavailable on this platform configuration."));
  }
  else
  {
    AddCheck(report, QStringLiteral("sourceMapping"), QStringLiteral("Project source mapping"),
             DiagnosticSeverity::Info, true,
             QStringLiteral("Project source mapping is available; each request indexes only explicitly supplied roots."));
  }

  if(!input.supportsMali)
  {
    AddCheck(report, QStringLiteral("mali"), QStringLiteral("Mali Offline Compiler"),
             DiagnosticSeverity::Warning, false,
             QStringLiteral("Mali Offline Compiler integration is unsupported on this platform."));
    AddLimitation(report, QStringLiteral("Mali Offline Compiler integration is unsupported on this platform."));
  }
  else if(input.maliConfiguredPath.isEmpty())
  {
    AddCheck(report, QStringLiteral("mali"), QStringLiteral("Mali Offline Compiler"),
             DiagnosticSeverity::Info, false,
             QStringLiteral("Mali Offline Compiler was not found."),
             QStringLiteral("Place malioc in a mali_offline_compiler folder next to qrenderdoc, "
                            "or add it to PATH."));
  }
  else if(!input.maliExecutableExists)
  {
    AddCheck(report, QStringLiteral("mali"), QStringLiteral("Mali Offline Compiler"),
             DiagnosticSeverity::Error, false,
             QStringLiteral("The Mali Offline Compiler executable is unavailable: %1")
                 .arg(input.maliResolvedExecutable.isEmpty() ? input.maliConfiguredPath
                                                             : input.maliResolvedExecutable),
             QStringLiteral("Install a compatible malioc executable next to qrenderdoc or on PATH."));
    AddLimitation(report, QStringLiteral("Mali Offline Compiler executable is unavailable."));
  }
  else
  {
    AddCheck(report, QStringLiteral("mali"), QStringLiteral("Mali Offline Compiler"),
             DiagnosticSeverity::Info, true,
             QStringLiteral("Mali Offline Compiler is available: %1").arg(input.maliResolvedExecutable));
  }

  return report;
}

QJsonObject DiagnosticCheckToJson(const DiagnosticCheck &check)
{
  QJsonObject json;
  json[QStringLiteral("id")] = check.id;
  json[QStringLiteral("title")] = check.title;
  json[QStringLiteral("severity")] = DiagnosticSeverityName(check.severity);
  json[QStringLiteral("available")] = check.available;
  json[QStringLiteral("summary")] = check.summary;
  if(!check.recovery.isEmpty())
    json[QStringLiteral("recovery")] = check.recovery;
  return json;
}

QJsonObject EnvironmentDiagnosticReportToJson(const EnvironmentDiagnosticReport &report)
{
  QJsonObject json;
  json[QStringLiteral("platform")] = report.platform;
  json[QStringLiteral("healthy")] = report.healthy;
  QJsonArray checks;
  for(const DiagnosticCheck &check : report.checks)
    checks.push_back(DiagnosticCheckToJson(check));
  json[QStringLiteral("checks")] = checks;
  json[QStringLiteral("limitations")] = QJsonArray::fromStringList(report.limitations);
  return json;
}
}    // namespace MCP
