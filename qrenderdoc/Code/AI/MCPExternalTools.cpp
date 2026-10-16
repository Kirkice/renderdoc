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

#include "MCPExternalTools.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <string.h>
#include "renderdoc_replay.h"
#include "Code/AI/MCPTools.h"
#include "Code/Interface/QRDInterface.h"

namespace MCP
{

namespace
{
QString FromRdcStr(const rdcstr &str)
{
  return QString::fromUtf8(str.c_str(), int(str.size()));
}

uint64_t ResIdValue(ResourceId id)
{
  uint64_t value = 0;
  memcpy(&value, &id, sizeof(value));
  return value;
}

QString ArgStr(const QJsonObject &args, const char *key, const QString &dflt = QString())
{
  const QJsonValue value = args.value(QString::fromUtf8(key));
  return value.isString() ? value.toString() : dflt;
}

qint64 ArgI64(const QJsonObject &args, const char *key, qint64 dflt = 0)
{
  const QJsonValue value = args.value(QString::fromUtf8(key));
  return value.isDouble() ? qint64(value.toDouble()) : dflt;
}

QStringList ArgStringList(const QJsonObject &args, const char *key)
{
  QStringList out;
  const QJsonValue value = args.value(QString::fromUtf8(key));
  if(!value.isArray())
    return out;

  for(const QJsonValue &entry : value.toArray())
    if(entry.isString())
      out.push_back(entry.toString());
  return out;
}

bool SensitiveAllowed(CallContext &call)
{
  return call.ctx != NULL && call.ctx->Config().MCP_AllowSensitiveData;
}

ToolResult SensitiveDenied(const char *toolName)
{
  QJsonObject data;
  data[QStringLiteral("setting")] = QStringLiteral("MCP_AllowSensitiveData");
  data[QStringLiteral("tool")] = QString::fromUtf8(toolName);
  return ToolResult::Failure(
      Error(ErrorCode::Unsupported,
            QObject::tr("This tool exports compiled shader contents, which are disabled by default. "
                        "Enable 'MCP_AllowSensitiveData' in qrenderdoc's settings to allow it."),
            data));
}

ToolResult WithCapture(QueryAdapter &adapter, const ToolResult &result)
{
  if(!result.ok)
    return result;

  QJsonObject out = result.structured;
  out[QStringLiteral("capture")] = QueryAdapter::SnapshotToJson(adapter.Snapshot());
  return ToolResult::Success(out, result.text);
}

void AddSchemaProperty(QJsonObject &properties, const char *name, const char *type,
                       const char *description)
{
  QJsonObject property;
  property[QStringLiteral("type")] = QString::fromUtf8(type);
  if(description != NULL)
    property[QStringLiteral("description")] = QString::fromUtf8(description);
  properties[QString::fromUtf8(name)] = property;
}

QJsonObject MakeSchema(const QJsonObject &properties)
{
  QJsonObject schema;
  schema[QStringLiteral("type")] = QStringLiteral("object");
  schema[QStringLiteral("properties")] = properties;
  schema[QStringLiteral("additionalProperties")] = true;
  return schema;
}

QString SafeFilePart(const QString &value)
{
  QString result = value;
  result.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]+")), QStringLiteral("_"));
  result = result.trimmed();
  return result.isEmpty() ? QStringLiteral("shader") : result;
}

QString StageName(ShaderStage stage)
{
  switch(stage)
  {
    case ShaderStage::Vertex: return QStringLiteral("Vertex");
    case ShaderStage::Hull: return QStringLiteral("Hull");
    case ShaderStage::Domain: return QStringLiteral("Domain");
    case ShaderStage::Geometry: return QStringLiteral("Geometry");
    case ShaderStage::Pixel: return QStringLiteral("Pixel");
    case ShaderStage::Compute: return QStringLiteral("Compute");
    case ShaderStage::Task: return QStringLiteral("Task");
    case ShaderStage::Mesh: return QStringLiteral("Mesh");
    case ShaderStage::RayGen: return QStringLiteral("RayGen");
    case ShaderStage::Intersection: return QStringLiteral("Intersection");
    case ShaderStage::AnyHit: return QStringLiteral("AnyHit");
    case ShaderStage::ClosestHit: return QStringLiteral("ClosestHit");
    case ShaderStage::Miss: return QStringLiteral("Miss");
    case ShaderStage::Callable: return QStringLiteral("Callable");
    default: break;
  }
  return QStringLiteral("Unknown");
}

ShaderStage StageFromName(const QString &name)
{
  const QString stage = name.trimmed().toLower();
  if(stage == QStringLiteral("vertex") || stage == QStringLiteral("vs")) return ShaderStage::Vertex;
  if(stage == QStringLiteral("hull") || stage == QStringLiteral("tcs") || stage == QStringLiteral("hs")) return ShaderStage::Hull;
  if(stage == QStringLiteral("domain") || stage == QStringLiteral("tes") || stage == QStringLiteral("ds")) return ShaderStage::Domain;
  if(stage == QStringLiteral("geometry") || stage == QStringLiteral("gs")) return ShaderStage::Geometry;
  if(stage == QStringLiteral("pixel") || stage == QStringLiteral("fragment") || stage == QStringLiteral("ps") || stage == QStringLiteral("fs")) return ShaderStage::Pixel;
  if(stage == QStringLiteral("compute") || stage == QStringLiteral("cs")) return ShaderStage::Compute;
  if(stage == QStringLiteral("task") || stage == QStringLiteral("amplification") || stage == QStringLiteral("as")) return ShaderStage::Task;
  if(stage == QStringLiteral("mesh") || stage == QStringLiteral("ms")) return ShaderStage::Mesh;
  return ShaderStage::Invalid;
}

void AppendLimitation(QStringList &limitations, const QString &limitation)
{
  if(!limitation.isEmpty() && !limitations.contains(limitation))
    limitations.push_back(limitation);
}

QString MetricNumber(double value)
{
  return QString::number(value, 'g', 12);
}

bool MetricNameIs(const QString &name, const char *expected)
{
  return name.compare(QString::fromUtf8(expected), Qt::CaseInsensitive) == 0;
}

const MaliMetric *FindMaliMetricAny(const MaliReport &report, const QStringList &names)
{
  for(const QString &name : names)
  {
    const MaliMetric *metric = FindMaliMetric(report, name);
    if(metric != NULL)
      return metric;
  }
  return NULL;
}

void CopyMetricValue(const MaliMetric *metric, bool &hasValue, double &value)
{
  if(metric != NULL && metric->numeric)
  {
    hasValue = true;
    value = metric->value;
  }
}

bool ParseMaliBoolean(const QString &text, bool &value)
{
  const QString normalized = text.trimmed().toLower();
  if(normalized.contains(QStringLiteral("no")) || normalized.contains(QStringLiteral("false")) ||
     normalized.contains(QStringLiteral("none")) || normalized.contains(QStringLiteral("off")))
  {
    value = false;
    return true;
  }
  if(normalized.contains(QStringLiteral("yes")) || normalized.contains(QStringLiteral("true")) ||
     normalized.contains(QStringLiteral("spill")) || normalized.contains(QStringLiteral("on")))
  {
    value = true;
    return true;
  }
  return false;
}

MaliCycleBreakdown ParseCycleBreakdown(const QString &text)
{
  MaliCycleBreakdown result;
  if(text.contains(QStringLiteral("n/a"), Qt::CaseInsensitive))
    return result;

  const QRegularExpression numberPattern(
      QStringLiteral("[-+]?\\d+(?:\\.\\d+)?(?:[eE][-+]?\\d+)?"));
  QRegularExpressionMatchIterator it = numberPattern.globalMatch(text);
  double values[4] = {};
  int count = 0;
  int lastEnd = 0;
  while(it.hasNext() && count < 4)
  {
    const QRegularExpressionMatch match = it.next();
    values[count++] = match.captured(0).toDouble();
    lastEnd = match.capturedEnd(0);
  }
  if(count < 4)
    return result;

  result.available = true;
  result.arithmetic = values[0];
  result.loadStore = values[1];
  result.varying = values[2];
  result.texture = values[3];
  result.bound = text.mid(lastEnd).trimmed();
  return result;
}

void AddRiskSignal(QStringList &risks, const QString &signal)
{
  if(!risks.contains(signal))
    risks.push_back(signal);
}

QJsonObject MaliCycleBreakdownToJson(const MaliCycleBreakdown &breakdown)
{
  QJsonObject json;
  json[QStringLiteral("available")] = breakdown.available;
  json[QStringLiteral("arithmetic")] = breakdown.arithmetic;
  json[QStringLiteral("loadStore")] = breakdown.loadStore;
  json[QStringLiteral("varying")] = breakdown.varying;
  json[QStringLiteral("texture")] = breakdown.texture;
  json[QStringLiteral("bound")] = breakdown.bound;
  return json;
}
}    // namespace

QStringList MaliDeviceNames()
{
  return QStringList{QStringLiteral("Mali-G725"), QStringLiteral("Mali-G720"),
                     QStringLiteral("Mali-G715"), QStringLiteral("Mali-G710"),
                     QStringLiteral("Mali-G625"), QStringLiteral("Mali-G620"),
                     QStringLiteral("Mali-G615"), QStringLiteral("Mali-G610"),
                     QStringLiteral("Mali-G510"), QStringLiteral("Mali-G310"),
                     QStringLiteral("Mali-G78"), QStringLiteral("Mali-G77"),
                     QStringLiteral("Mali-G76"), QStringLiteral("Mali-G72"),
                     QStringLiteral("Mali-G71"), QStringLiteral("Mali-G68"),
                     QStringLiteral("Mali-G57"), QStringLiteral("Mali-G52"),
                     QStringLiteral("Mali-G31")};
}

bool IsKnownMaliDevice(const QString &device)
{
  for(const QString &known : MaliDeviceNames())
    if(known.compare(device.trimmed(), Qt::CaseInsensitive) == 0)
      return true;
  return false;
}

QString NormalizeMaliDevice(const QString &device)
{
  const QString trimmed = device.trimmed();
  for(const QString &known : MaliDeviceNames())
    if(known.compare(trimmed, Qt::CaseInsensitive) == 0)
      return known;
  return QString();
}

QString MaliExecutableName()
{
#ifdef Q_OS_WIN
  return QStringLiteral("malioc.exe");
#else
  return QStringLiteral("malioc");
#endif
}

QString MaliExecutableInDirectory(const QString &directory)
{
  return QDir::cleanPath(QDir(directory).filePath(MaliExecutableName()));
}

QString ResolveMaliExecutablePath(const QString &configured)
{
  const QString trimmed = configured.trimmed();
  if(!trimmed.isEmpty())
  {
    const QFileInfo info(trimmed);
    if(info.isDir() || trimmed.endsWith(QLatin1Char('/')) || trimmed.endsWith(QLatin1Char('\\')))
      return MaliExecutableInDirectory(trimmed);

    return QDir::cleanPath(trimmed);
  }

  // Local packages carry the complete Mali Offline Compiler runtime alongside
  // qrenderdoc. Keep an explicit user setting authoritative, but otherwise make
  // the bundled copy work without requiring any configuration.
  const QString bundled =
      MaliExecutableInDirectory(QDir(QCoreApplication::applicationDirPath()).filePath(
          QStringLiteral("mali_offline_compiler")));
  if(QFileInfo(bundled).isFile())
    return bundled;

  // This also preserves the normal Arm installer workflow, which registers
  // malioc on PATH, and makes local developer installations convenient.
  return QStandardPaths::findExecutable(MaliExecutableName());
}

QJsonObject MaliTargetOptionsToJson(const MaliTargetOptions &options)
{
  QJsonObject result;
  result[QStringLiteral("device")] = options.device;
  result[QStringLiteral("normalizedDevice")] = NormalizeMaliDevice(options.device);
  result[QStringLiteral("extraArgs")] = QJsonArray::fromStringList(options.extraArgs);
  result[QStringLiteral("timeoutMs")] = options.timeoutMs;
  return result;
}

bool MaliSupportsEncoding(int encoding)
{
  // malioc consumes SPIR-V binary modules (Vulkan captures, and the SPIR-V an
  // OpenGL ES capture can carry) as well as GLSL source text, which is what a
  // typical OpenGL ES capture actually stores. Treat all of those as analysable
  // and reject DXBC/DXIL/HLSL/assembly/Slang with an explicit limitation rather
  // than handing malioc something it cannot read.
  return encoding == int(uint32_t(ShaderEncoding::SPIRV)) ||
         encoding == int(uint32_t(ShaderEncoding::OpenGLSPIRV)) ||
         encoding == int(uint32_t(ShaderEncoding::GLSL));
}

QString MaliStageExtension(const QString &stage)
{
  const QString s = stage.trimmed().toLower();
  if(s == QStringLiteral("vertex")) return QStringLiteral(".vert");
  if(s == QStringLiteral("hull")) return QStringLiteral(".tesc");
  if(s == QStringLiteral("domain")) return QStringLiteral(".tese");
  if(s == QStringLiteral("geometry")) return QStringLiteral(".geom");
  if(s == QStringLiteral("pixel") || s == QStringLiteral("fragment")) return QStringLiteral(".frag");
  if(s == QStringLiteral("compute")) return QStringLiteral(".comp");
  if(s == QStringLiteral("raygen")) return QStringLiteral(".rgen");
  if(s == QStringLiteral("intersection")) return QStringLiteral(".rint");
  if(s == QStringLiteral("anyhit")) return QStringLiteral(".rahit");
  if(s == QStringLiteral("closesthit")) return QStringLiteral(".rchit");
  if(s == QStringLiteral("miss")) return QStringLiteral(".rmiss");
  if(s == QStringLiteral("callable")) return QStringLiteral(".rcall");
  // malioc infers the stage from a recognised source extension, so an unknown
  // stage falls back to a generic GLSL suffix where the caller must pass the
  // explicit stage flag through extra args.
  return QStringLiteral(".glsl");
}

QString MaliEncodingExtension(int encoding)
{
  if(encoding == int(uint32_t(ShaderEncoding::SPIRV)) ||
     encoding == int(uint32_t(ShaderEncoding::OpenGLSPIRV)))
    return QStringLiteral(".spv");
  // GLSL source has no single canonical extension - the captured stage decides
  // it, and BuildMaliShaderExport resolves it from the stage.
  return QString();
}

QString MaliEncodingName(int encoding)
{
  if(encoding == int(uint32_t(ShaderEncoding::SPIRV))) return QStringLiteral("SPIR-V (Vulkan)");
  if(encoding == int(uint32_t(ShaderEncoding::OpenGLSPIRV))) return QStringLiteral("SPIR-V (OpenGL)");
  if(encoding == int(uint32_t(ShaderEncoding::GLSL))) return QStringLiteral("GLSL");
  if(encoding == int(uint32_t(ShaderEncoding::HLSL))) return QStringLiteral("HLSL");
  if(encoding == int(uint32_t(ShaderEncoding::DXBC))) return QStringLiteral("DXBC");
  if(encoding == int(uint32_t(ShaderEncoding::DXIL))) return QStringLiteral("DXIL");
  if(encoding == int(uint32_t(ShaderEncoding::SPIRVAsm))) return QStringLiteral("SPIR-V assembly");
  if(encoding == int(uint32_t(ShaderEncoding::OpenGLSPIRVAsm)))
    return QStringLiteral("SPIR-V assembly (OpenGL)");
  if(encoding == int(uint32_t(ShaderEncoding::Slang))) return QStringLiteral("Slang");
  return QStringLiteral("Unknown");
}

MaliShaderExport BuildMaliShaderExport(const QString &stage, const QString &entryPoint,
                                       uint64_t resourceValue, int encoding,
                                       const QByteArray &rawBytes, int64_t byteLength)
{
  MaliShaderExport result;
  result.stage = stage;
  result.entryPoint = entryPoint;
  result.resourceValue = resourceValue;
  result.encoding = encoding;
  result.contents = rawBytes;
  result.revision = QString::fromLatin1(
      QCryptographicHash::hash(rawBytes, QCryptographicHash::Sha256).toHex());
  const bool supported = MaliSupportsEncoding(encoding);

  // SPIR-V is a binary module and the bare ".spv" suffix is enough for malioc to
  // detect the stage from the module itself. GLSL capture data is source text, so
  // the extension carries the stage (".frag", ".comp", ...) that malioc uses to
  // select the pipeline stage. An unsupported encoding keeps the extension empty
  // rather than inventing one malioc would mistake for a real shader.
  if(encoding == int(uint32_t(ShaderEncoding::SPIRV)) ||
     encoding == int(uint32_t(ShaderEncoding::OpenGLSPIRV)))
  {
    result.binary = true;
    result.extension = QStringLiteral(".spv");
  }
  else if(supported)
  {
    result.binary = false;
    result.extension = MaliStageExtension(stage);
  }

  result.suggestedFileName = SafeFilePart(entryPoint) + QLatin1Char('_') + SafeFilePart(stage) +
                             result.extension;

  if(!supported)
    result.limitation = QObject::tr("Mali Offline Compiler can only analyse captured SPIR-V or "
                                    "GLSL shader source; this shader is %1.")
                            .arg(MaliEncodingName(encoding));
  else if(rawBytes.isEmpty())
    result.limitation = QObject::tr("The capture did not retain any shader data to export.");
  else if(byteLength >= 0 && byteLength != rawBytes.size())
    result.limitation = QObject::tr("The capture retained %1 of %2 shader bytes; analysis may be incomplete.")
                            .arg(rawBytes.size())
                            .arg(byteLength);

  return result;
}

QStringList BuildMaliCommandLine(const QString &executable, const QString &shaderPath,
                                 const MaliTargetOptions &options)
{
  QStringList command;
  command << executable << shaderPath;
  const QString device = NormalizeMaliDevice(options.device);
  if(!device.isEmpty())
    command << QStringLiteral("-c") << device;
  command.append(options.extraArgs);
  return command;
}

MaliReport ParseMaliOutput(const QString &stdoutText, const QString &stderrText, int exitCode)
{
  MaliReport report;
  report.ran = true;
  report.exitCode = exitCode;
  report.rawOutput = stdoutText;
  if(!stderrText.isEmpty())
  {
    if(!report.rawOutput.isEmpty() && !report.rawOutput.endsWith(QLatin1Char('\n')))
      report.rawOutput += QLatin1Char('\n');
    report.rawOutput += stderrText;
  }

  MaliSection *current = NULL;
  const QRegularExpression sectionPattern(
      QStringLiteral("^\\s*(?:Shader\\s+program\\s+for|Shader\\s+program|Analysis\\s+for)\\s*:?[ \\t]*(.+?)\\s*$"),
      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpression metricPattern(
      QStringLiteral("^\\s*([^:=]+?)\\s*[:=]\\s*([-+]?\\d+(?:\\.\\d+)?(?:[eE][-+]?\\d+)?)\\s*([^\\s,;]*)?.*$"));
  const QRegularExpression textMetricPattern(QStringLiteral("^\\s*([^:=]+?)\\s*[:=]\\s*(.+?)\\s*$"));

  const QStringList lines = report.rawOutput.split(QLatin1Char('\n'));
  for(const QString &rawLine : lines)
  {
    const QString line = rawLine.trimmed();
    if(line.isEmpty())
      continue;

    const QRegularExpressionMatch section = sectionPattern.match(line);
    if(section.hasMatch())
    {
      report.sections.push_back(MaliSection());
      current = &report.sections.last();
      current->title = section.captured(1).trimmed();
      continue;
    }

    if(line.startsWith(QStringLiteral("Mali Offline Compiler"), Qt::CaseInsensitive))
    {
      report.compilerVersion = line;
      continue;
    }

    const QRegularExpressionMatch metric = metricPattern.match(line);
    if(metric.hasMatch())
    {
      if(current == NULL)
      {
        report.sections.push_back(MaliSection());
        current = &report.sections.last();
        current->title = QStringLiteral("Summary");
      }

      MaliMetric parsed;
      parsed.name = metric.captured(1).trimmed();
      parsed.numeric = true;
      parsed.value = metric.captured(2).toDouble();
      parsed.unit = metric.captured(3).trimmed();
      parsed.raw = rawLine;
      current->metrics.push_back(parsed);

      if(MetricNameIs(parsed.name, "Hardware"))
        report.hardware = parsed.unit;
      continue;
    }

    const QRegularExpressionMatch textMetric = textMetricPattern.match(line);
    if(textMetric.hasMatch())
    {
      if(current == NULL)
      {
        report.sections.push_back(MaliSection());
        current = &report.sections.last();
        current->title = QStringLiteral("Summary");
      }

      MaliMetric parsed;
      parsed.name = textMetric.captured(1).trimmed();
      parsed.text = textMetric.captured(2).trimmed();
      parsed.raw = rawLine;
      current->metrics.push_back(parsed);
      if(MetricNameIs(parsed.name, "Hardware"))
        report.hardware = parsed.text;
      continue;
    }

    if(line.contains(QStringLiteral("warning"), Qt::CaseInsensitive))
      report.warnings.push_back(line);
  }

  const MaliMetric *cycles = FindMaliMetric(report, QStringLiteral("Total cycles"));
  const MaliMetric *instructions = FindMaliMetric(report, QStringLiteral("Total instructions"));
  const MaliMetric *registers = FindMaliMetric(report, QStringLiteral("Registers"));
  const MaliMetric *spills = FindMaliMetric(report, QStringLiteral("Spill bytes"));
  if(cycles != NULL || instructions != NULL || registers != NULL || spills != NULL)
  {
    report.hasTotals = true;
    if(cycles != NULL) report.totalCycles = cycles->value;
    if(instructions != NULL) report.totalInstructions = instructions->value;
    if(registers != NULL) report.registers = registers->value;
    if(spills != NULL) report.spillBytes = spills->value;
  }

  CopyMetricValue(FindMaliMetricAny(report, {QStringLiteral("Work registers"),
                                               QStringLiteral("Registers used")}),
                  report.hasWorkRegisters, report.workRegisters);
  CopyMetricValue(FindMaliMetricAny(report, {QStringLiteral("Uniform registers")}),
                  report.hasUniformRegisters, report.uniformRegisters);
  CopyMetricValue(FindMaliMetricAny(report, {QStringLiteral("Stack size")}), report.hasStackSize,
                  report.stackSize);

  const MaliMetric *spilling = FindMaliMetricAny(
      report, {QStringLiteral("Stack spilling"), QStringLiteral("Spilling")});
  if(spilling != NULL)
  {
    const QString value = spilling->numeric ? QString::number(spilling->value) : spilling->text;
    report.stackSpillingKnown = ParseMaliBoolean(value, report.stackSpilling);
  }
  if(!report.stackSpillingKnown && spills != NULL)
  {
    report.stackSpillingKnown = true;
    report.stackSpilling = spills->value > 0.0;
  }

  const MaliMetric *totalPath = FindMaliMetricAny(
      report, {QStringLiteral("Total instruction cycles"), QStringLiteral("Total path cycles")});
  const MaliMetric *shortestPath = FindMaliMetricAny(
      report, {QStringLiteral("Shortest path cycles"), QStringLiteral("Shortest cycles")});
  const MaliMetric *longestPath = FindMaliMetricAny(
      report, {QStringLiteral("Longest path cycles"), QStringLiteral("Longest cycles")});
  if(totalPath != NULL) report.totalPath = ParseCycleBreakdown(totalPath->raw);
  if(shortestPath != NULL) report.shortestPath = ParseCycleBreakdown(shortestPath->raw);
  if(longestPath != NULL) report.longestPath = ParseCycleBreakdown(longestPath->raw);

  if(report.hasWorkRegisters)
  {
    if(report.workRegisters >= 64.0)
      AddRiskSignal(report.riskSignals, QObject::tr("High register pressure"));
    else if(report.workRegisters >= 32.0)
      AddRiskSignal(report.riskSignals, QObject::tr("Register-heavy shader"));
  }
  if(report.stackSpillingKnown && report.stackSpilling)
    AddRiskSignal(report.riskSignals, QObject::tr("Stack spilling"));
  if(report.longestPath.available)
  {
    const double longest = qMax(qMax(report.longestPath.arithmetic, report.longestPath.loadStore),
                                qMax(report.longestPath.varying, report.longestPath.texture));
    if(longest >= 64.0)
      AddRiskSignal(report.riskSignals, QObject::tr("Long critical path"));
    else if(longest >= 32.0)
      AddRiskSignal(report.riskSignals, QObject::tr("Long path"));
  }
  if(report.totalPath.bound.contains(QStringLiteral("texture"), Qt::CaseInsensitive) ||
     report.shortestPath.bound.contains(QStringLiteral("texture"), Qt::CaseInsensitive) ||
     report.longestPath.bound.contains(QStringLiteral("texture"), Qt::CaseInsensitive))
    AddRiskSignal(report.riskSignals, QObject::tr("Texture-bound path"));

  if(exitCode != 0)
  {
    report.error = QObject::tr("malioc exited with code %1.").arg(exitCode);
    report.confidence = Confidence::Low;
  }
  else if(report.hasTotals)
  {
    report.confidence = Confidence::Confirmed;
  }
  else
  {
    report.confidence = Confidence::ToVerify;
    report.limitation = QObject::tr("malioc completed, but no recognised total metrics were found.");
  }

  return report;
}

const MaliMetric *FindMaliMetric(const MaliReport &report, const QString &name)
{
  for(const MaliSection &section : report.sections)
    for(const MaliMetric &metric : section.metrics)
      if(metric.name.compare(name, Qt::CaseInsensitive) == 0)
        return &metric;
  return NULL;
}

QJsonObject MaliReportToJson(const MaliReport &report)
{
  QJsonObject result;
  result[QStringLiteral("ran")] = report.ran;
  result[QStringLiteral("exitCode")] = report.exitCode;
  result[QStringLiteral("device")] = report.device;
  result[QStringLiteral("compilerVersion")] = report.compilerVersion;
  result[QStringLiteral("hardware")] = report.hardware;
  result[QStringLiteral("warnings")] = QJsonArray::fromStringList(report.warnings);
  result[QStringLiteral("hasTotals")] = report.hasTotals;
  result[QStringLiteral("totalCycles")] = report.totalCycles;
  result[QStringLiteral("totalInstructions")] = report.totalInstructions;
  result[QStringLiteral("registers")] = report.registers;
  result[QStringLiteral("spillBytes")] = report.spillBytes;
  result[QStringLiteral("hasWorkRegisters")] = report.hasWorkRegisters;
  result[QStringLiteral("workRegisters")] = report.workRegisters;
  result[QStringLiteral("hasUniformRegisters")] = report.hasUniformRegisters;
  result[QStringLiteral("uniformRegisters")] = report.uniformRegisters;
  result[QStringLiteral("hasStackSize")] = report.hasStackSize;
  result[QStringLiteral("stackSize")] = report.stackSize;
  result[QStringLiteral("stackSpillingKnown")] = report.stackSpillingKnown;
  result[QStringLiteral("stackSpilling")] = report.stackSpilling;
  result[QStringLiteral("totalPath")] = MaliCycleBreakdownToJson(report.totalPath);
  result[QStringLiteral("shortestPath")] = MaliCycleBreakdownToJson(report.shortestPath);
  result[QStringLiteral("longestPath")] = MaliCycleBreakdownToJson(report.longestPath);
  result[QStringLiteral("riskSignals")] = QJsonArray::fromStringList(report.riskSignals);
  result[QStringLiteral("error")] = report.error;
  result[QStringLiteral("rawOutput")] = report.rawOutput;
  result[QStringLiteral("confidence")] = ConfidenceName(report.confidence);
  result[QStringLiteral("limitation")] = report.limitation;

  QJsonArray sections;
  for(const MaliSection &section : report.sections)
  {
    QJsonObject jsonSection;
    jsonSection[QStringLiteral("title")] = section.title;
    QJsonArray metrics;
    for(const MaliMetric &metric : section.metrics)
    {
      QJsonObject jsonMetric;
      jsonMetric[QStringLiteral("name")] = metric.name;
      jsonMetric[QStringLiteral("numeric")] = metric.numeric;
      jsonMetric[QStringLiteral("value")] = metric.value;
      jsonMetric[QStringLiteral("unit")] = metric.unit;
      jsonMetric[QStringLiteral("text")] = metric.text;
      jsonMetric[QStringLiteral("raw")] = metric.raw;
      metrics.push_back(jsonMetric);
    }
    jsonSection[QStringLiteral("metrics")] = metrics;
    sections.push_back(jsonSection);
  }
  result[QStringLiteral("sections")] = sections;
  return result;
}

QString MaliRunStatusName(MaliRunStatus status)
{
  switch(status)
  {
    case MaliRunStatus::NotConfigured: return QStringLiteral("notConfigured");
    case MaliRunStatus::ToolMissing: return QStringLiteral("toolMissing");
    case MaliRunStatus::UnsupportedShader: return QStringLiteral("unsupportedShader");
    case MaliRunStatus::Timeout: return QStringLiteral("timeout");
    case MaliRunStatus::Cancelled: return QStringLiteral("cancelled");
    case MaliRunStatus::FailedToStart: return QStringLiteral("failedToStart");
    case MaliRunStatus::Ran: return QStringLiteral("ran");
  }
  return QStringLiteral("unknown");
}

MaliRunResult RunMaliAnalysis(const MaliRunRequest &request, const std::function<bool()> &cancelled)
{
  MaliRunResult result;
  result.executable = ResolveMaliExecutablePath(request.configuredPath);
  result.commandLine = BuildMaliCommandLine(result.executable, request.shaderPath, request.target);
  result.report.device = NormalizeMaliDevice(request.target.device);

  if(result.executable.isEmpty())
  {
    result.status = MaliRunStatus::NotConfigured;
    result.hint = QObject::tr("No malioc executable was found. Place it in a mali_offline_compiler "
                              "folder next to qrenderdoc, or add it to PATH.");
    return result;
  }
  if(!QFileInfo::exists(result.executable))
  {
    result.status = MaliRunStatus::ToolMissing;
    result.hint = QObject::tr("The selected malioc executable does not exist: %1").arg(result.executable);
    return result;
  }
  if(request.shaderPath.isEmpty() || !QFileInfo::exists(request.shaderPath))
  {
    result.status = MaliRunStatus::UnsupportedShader;
    result.hint = QObject::tr("The exported shader file is unavailable.");
    return result;
  }
  if(cancelled && cancelled())
  {
    result.status = MaliRunStatus::Cancelled;
    result.hint = QObject::tr("The analysis request was cancelled before malioc was started.");
    return result;
  }

  QProcess process;
  if(!request.workDir.isEmpty())
    process.setWorkingDirectory(request.workDir);
  process.start(result.executable, result.commandLine.mid(1));
  if(!process.waitForStarted(3000))
  {
    result.status = MaliRunStatus::FailedToStart;
    result.hint = QObject::tr("malioc could not be started: %1").arg(process.errorString());
    result.report.error = result.hint;
    result.report.confidence = Confidence::Low;
    return result;
  }

  result.report.ran = true;
  QElapsedTimer elapsed;
  elapsed.start();
  const int timeout = request.target.timeoutMs > 0 ? request.target.timeoutMs : 30000;
  QByteArray stdoutBytes;
  QByteArray stderrBytes;
  while(process.state() != QProcess::NotRunning)
  {
    // Drain both pipes while the child runs. Waiting for a process which writes
    // more than an OS pipe buffer without reading it can otherwise deadlock.
    stdoutBytes += process.readAllStandardOutput();
    stderrBytes += process.readAllStandardError();

    if(cancelled && cancelled())
    {
      process.kill();
      process.waitForFinished(1000);
      stdoutBytes += process.readAllStandardOutput();
      stderrBytes += process.readAllStandardError();
      result.status = MaliRunStatus::Cancelled;
      result.hint = QObject::tr("The analysis request was cancelled.");
      result.report = ParseMaliOutput(QString::fromUtf8(stdoutBytes), QString::fromUtf8(stderrBytes), -1);
      result.report.device = NormalizeMaliDevice(request.target.device);
      result.report.error = result.hint;
      result.report.confidence = Confidence::Low;
      return result;
    }
    if(elapsed.elapsed() >= timeout)
    {
      process.kill();
      process.waitForFinished(1000);
      stdoutBytes += process.readAllStandardOutput();
      stderrBytes += process.readAllStandardError();
      result.status = MaliRunStatus::Timeout;
      result.hint = QObject::tr("malioc exceeded the configured %1 ms timeout.").arg(timeout);
      result.report = ParseMaliOutput(QString::fromUtf8(stdoutBytes), QString::fromUtf8(stderrBytes), -1);
      result.report.device = NormalizeMaliDevice(request.target.device);
      result.report.error = result.hint;
      result.report.confidence = Confidence::Low;
      return result;
    }
    process.waitForFinished(50);
  }

  stdoutBytes += process.readAllStandardOutput();
  stderrBytes += process.readAllStandardError();
  result.status = MaliRunStatus::Ran;
  result.report = ParseMaliOutput(QString::fromUtf8(stdoutBytes), QString::fromUtf8(stderrBytes),
                                  process.exitCode());
  result.report.device = NormalizeMaliDevice(request.target.device);
  if(process.exitStatus() != QProcess::NormalExit)
  {
    result.report.error = QObject::tr("malioc terminated unexpectedly.");
    result.report.confidence = Confidence::Low;
  }
  return result;
}

QJsonObject MaliRunResultToJson(const MaliRunResult &result)
{
  QJsonObject json;
  json[QStringLiteral("status")] = MaliRunStatusName(result.status);
  json[QStringLiteral("executable")] = result.executable;
  json[QStringLiteral("commandLine")] = QJsonArray::fromStringList(result.commandLine);
  json[QStringLiteral("hint")] = result.hint;
  json[QStringLiteral("report")] = MaliReportToJson(result.report);
  return json;
}

void FinalizeMaliEventAnalysis(MaliEventAnalysis &analysis)
{
  analysis.limitations.clear();
  AppendLimitation(analysis.limitations, analysis.shader.limitation);
  AppendLimitation(analysis.limitations, analysis.run.report.limitation);
  AppendLimitation(analysis.limitations, analysis.run.hint);

  if(!analysis.available || analysis.run.status != MaliRunStatus::Ran || analysis.run.report.exitCode != 0)
    analysis.confidence = Confidence::Low;
  else if(!analysis.limitations.isEmpty() || !analysis.run.report.hasTotals)
    analysis.confidence = Confidence::ToVerify;
  else
    analysis.confidence = Confidence::Confirmed;
}

QJsonObject MaliEventAnalysisToJson(const MaliEventAnalysis &analysis)
{
  QJsonObject json;
  json[QStringLiteral("available")] = analysis.available;
  json[QStringLiteral("eventId")] = double(analysis.eventId);
  json[QStringLiteral("eventName")] = analysis.eventName;
  json[QStringLiteral("capturePath")] = analysis.capturePath;
  json[QStringLiteral("confidence")] = ConfidenceName(analysis.confidence);
  json[QStringLiteral("limitations")] = QJsonArray::fromStringList(analysis.limitations);

  QJsonObject shader;
  shader[QStringLiteral("stage")] = analysis.shader.stage;
  shader[QStringLiteral("entryPoint")] = analysis.shader.entryPoint;
  shader[QStringLiteral("resourceValue")] = double(analysis.shader.resourceValue);
  shader[QStringLiteral("encoding")] = analysis.shader.encoding;
  shader[QStringLiteral("binary")] = analysis.shader.binary;
  shader[QStringLiteral("extension")] = analysis.shader.extension;
  shader[QStringLiteral("suggestedFileName")] = analysis.shader.suggestedFileName;
  shader[QStringLiteral("byteLength")] = analysis.shader.contents.size();
  shader[QStringLiteral("revision")] = analysis.shader.revision;
  shader[QStringLiteral("limitation")] = analysis.shader.limitation;
  json[QStringLiteral("shader")] = shader;
  json[QStringLiteral("run")] = MaliRunResultToJson(analysis.run);

  QJsonArray evidence;
  for(const EvidenceRef &item : analysis.evidence)
    evidence.push_back(EvidenceToJson(item));
  json[QStringLiteral("evidence")] = evidence;
  return json;
}

QString MaliEventAnalysisToText(const MaliEventAnalysis &analysis)
{
  QStringList lines;
  lines << QObject::tr("Mali Offline Compiler Analysis")
        << QObject::tr("Capture: %1").arg(analysis.capturePath)
        << QObject::tr("Event: %1 %2").arg(analysis.eventId).arg(analysis.eventName)
        << QObject::tr("Shader: %1 (%2, %3 bytes)")
               .arg(analysis.shader.entryPoint)
               .arg(analysis.shader.stage)
               .arg(analysis.shader.contents.size())
        << QObject::tr("Status: %1").arg(MaliRunStatusName(analysis.run.status));

  if(!analysis.run.report.hardware.isEmpty())
    lines << QObject::tr("Hardware: %1").arg(analysis.run.report.hardware);
  if(analysis.run.report.hasTotals)
  {
    lines << QObject::tr("Totals")
          << QObject::tr("Total cycles: %1").arg(MetricNumber(analysis.run.report.totalCycles))
          << QObject::tr("Total instructions: %1").arg(MetricNumber(analysis.run.report.totalInstructions))
          << QObject::tr("Registers: %1").arg(MetricNumber(analysis.run.report.registers))
          << QObject::tr("Spill bytes: %1").arg(MetricNumber(analysis.run.report.spillBytes));
  }
  for(const MaliSection &section : analysis.run.report.sections)
  {
    lines << section.title;
    for(const MaliMetric &metric : section.metrics)
      lines << QStringLiteral("  %1: %2%3")
                   .arg(metric.name)
                   .arg(metric.numeric ? MetricNumber(metric.value) : metric.text)
                   .arg(metric.unit.isEmpty() ? QString() : QStringLiteral(" ") + metric.unit);
  }
  if(!analysis.limitations.isEmpty())
    lines << QObject::tr("Limitations: %1").arg(analysis.limitations.join(QStringLiteral("; ")));
  return lines.join(QLatin1Char('\n'));
}

int RegisterExternalToolTools(ToolRegistry &registry, QueryAdapter &adapter)
{
  int count = 0;

  {
    QJsonObject properties;
    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_getMaliToolStatus"),
                       QStringLiteral("Get Mali Offline Compiler Status"),
                       QStringLiteral("Return the configured Mali Offline Compiler path, target profile, "
                                      "arguments and whether the executable is currently available."),
                       ToolEffects::ReadOnly, ToolRequirements::None, MakeSchema(properties)},
        [&adapter](const QJsonObject &, CallContext &call) -> ToolResult {
          if(call.ctx == NULL)
            return ToolResult::Failure(Error(ErrorCode::InternalError,
                                             QObject::tr("Capture context is unavailable.")));

          MaliTargetOptions options;
          options.device = FromRdcStr(call.ctx->Config().MCP_MaliDevice);
          const QString configuredArgs = FromRdcStr(call.ctx->Config().MCP_MaliExtraArgs);
          if(!configuredArgs.isEmpty())
            options.extraArgs.push_back(configuredArgs);

          const QString configured = FromRdcStr(call.ctx->Config().MCP_MaliCompilerPath);
          const QString executable = ResolveMaliExecutablePath(configured);
          QJsonObject out;
          out[QStringLiteral("configuredPath")] = configured;
          out[QStringLiteral("resolvedExecutable")] = executable;
          out[QStringLiteral("configured")] = !configured.isEmpty();
          out[QStringLiteral("exists")] = !executable.isEmpty() && QFileInfo::exists(executable);
          out[QStringLiteral("target")] = MaliTargetOptionsToJson(options);
          out[QStringLiteral("knownDevices")] = QJsonArray::fromStringList(MaliDeviceNames());
          return WithCapture(adapter, ToolResult::Success(out, QString()));
        });
    count++;
  }

  {
    QJsonObject properties;
    AddSchemaProperty(properties, "eventId", "integer",
                       "Event whose currently bound shader should be exported and analysed. Defaults to the selected event.");
    AddSchemaProperty(properties, "stage", "string",
                       "Shader stage (vertex/vs, fragment/fs, compute/cs, geometry/gs, hull/hs, domain/ds, task/as, mesh/ms). Required.");
    AddSchemaProperty(properties, "device", "string", "Optional Mali target profile overriding MCP_MaliDevice.");
    AddSchemaProperty(properties, "extraArgs", "array", "Optional explicit malioc arguments appended after the target.");
    QJsonObject schema = MakeSchema(properties);
    schema[QStringLiteral("required")] = QJsonArray{QStringLiteral("stage")};

    registry.Add(
        ToolDefinition{QStringLiteral("renderdoc_analyzeMaliShader"),
                       QStringLiteral("Analyze Bound Shader With Mali Offline Compiler"),
                       QStringLiteral("Export the shader bound at an event (SPIR-V bytecode, or GLSL source for "
                                      "OpenGL captures), run the configured Mali Offline Compiler without a shell, "
                                      "and return parsed metrics with event and shader evidence."),
                       ToolEffects::ReadOnly,
                       ToolRequirements::LoadedCapture | ToolRequirements::ReplayController, schema},
        [&adapter](const QJsonObject &args, CallContext &call) -> ToolResult {
          if(!SensitiveAllowed(call))
            return SensitiveDenied("renderdoc_analyzeMaliShader");

          const ShaderStage stage = StageFromName(ArgStr(args, "stage"));
          if(stage == ShaderStage::Invalid)
            return ToolResult::Failure(Error(ErrorCode::InvalidParams,
                                             QObject::tr("stage must name a supported shader stage.")));

          const CaptureSnapshot snapshot = adapter.Snapshot();
          const uint32_t eventId =
              uint32_t(ArgI64(args, "eventId", snapshot.selectedEvent));
          if(eventId == 0)
            return ToolResult::Failure(Error(ErrorCode::InvalidParams,
                                             QObject::tr("eventId is required when no event is selected.")));

          MaliShaderExport shader;
          ToolResult exportResult = adapter.DispatchBlock(
              "renderdoc_analyzeMaliShader", call,
              [eventId, stage, &shader](IReplayController &replay) -> ToolResult {
                try
                {
                  replay.SetFrameEvent(eventId, false);
                }
                catch(...)
                {
                  return ToolResult::Failure(
                      Error(ErrorCode::InvalidParams, QObject::tr("Could not select the requested event.")));
                }

                const PipeState &pipe = replay.GetPipelineState();
                const ResourceId shaderId = pipe.GetShader(stage);
                const ShaderReflection *reflection = pipe.GetShaderReflection(stage);
                if(shaderId == ResourceId() || reflection == NULL)
                  return ToolResult::Failure(Error(ErrorCode::InvalidParams,
                                                   QObject::tr("No shader is bound for the requested stage.")));

                QByteArray rawBytes;
                if(!reflection->rawBytes.empty())
                  rawBytes = QByteArray(reinterpret_cast<const char *>(reflection->rawBytes.data()),
                                        int(reflection->rawBytes.size()));
                shader = BuildMaliShaderExport(StageName(stage), FromRdcStr(reflection->entryPoint),
                                               ResIdValue(shaderId), int(uint32_t(reflection->encoding)), rawBytes,
                                               int64_t(reflection->rawBytes.size()));
                return ToolResult::Success(QJsonObject(), QString());
              });
          if(!exportResult.ok)
            return exportResult;

          MaliEventAnalysis analysis;
          analysis.available = true;
          analysis.eventId = eventId;
          analysis.capturePath = snapshot.filename;
          analysis.shader = shader;
          analysis.evidence.push_back(CaptureEvidence(snapshot.filename));
          EvidenceRef shaderEvidence;
          shaderEvidence.kind = QStringLiteral("shader");
          shaderEvidence.resourceValue = analysis.shader.resourceValue;
          shaderEvidence.resourceName = analysis.shader.entryPoint;
          shaderEvidence.shaderStage = analysis.shader.stage;
          shaderEvidence.confidence = Confidence::Confirmed;
          analysis.evidence.push_back(shaderEvidence);
          analysis.evidence.push_back(EventEvidence(eventId));

          if(!analysis.shader.limitation.isEmpty())
          {
            analysis.run.status = MaliRunStatus::UnsupportedShader;
            analysis.run.hint = analysis.shader.limitation;
            FinalizeMaliEventAnalysis(analysis);
            return WithCapture(adapter, ToolResult::Success(MaliEventAnalysisToJson(analysis),
                                                            MaliEventAnalysisToText(analysis)));
          }

          QTemporaryFile temporary(QDir::tempPath() + QStringLiteral("/renderdoc_mali_XXXXXX") +
                                   analysis.shader.extension);
          temporary.setAutoRemove(true);
          if(!temporary.open() || temporary.write(analysis.shader.contents) != analysis.shader.contents.size() ||
             !temporary.flush())
          {
            analysis.available = false;
            analysis.run.status = MaliRunStatus::FailedToStart;
            analysis.run.hint = QObject::tr("Could not create the temporary shader export file.");
            FinalizeMaliEventAnalysis(analysis);
            return WithCapture(adapter, ToolResult::Success(MaliEventAnalysisToJson(analysis),
                                                            MaliEventAnalysisToText(analysis)));
          }
          // Close before starting malioc: this is required on platforms where an
          // open temporary file cannot be read by a separate process.
          temporary.close();

          MaliRunRequest request;
          request.configuredPath = FromRdcStr(call.ctx->Config().MCP_MaliCompilerPath);
          request.shaderPath = temporary.fileName();
          request.target.device = ArgStr(args, "device", FromRdcStr(call.ctx->Config().MCP_MaliDevice));
          request.target.extraArgs = ArgStringList(args, "extraArgs");
          if(request.target.extraArgs.isEmpty())
          {
            const QString configuredArgs = FromRdcStr(call.ctx->Config().MCP_MaliExtraArgs);
            if(!configuredArgs.isEmpty())
              request.target.extraArgs.push_back(configuredArgs);
          }
          request.target.timeoutMs = 30000;
          analysis.run = RunMaliAnalysis(request, [&call]() { return call.IsCancelled() || call.IsExpired(); });
          FinalizeMaliEventAnalysis(analysis);
          return WithCapture(adapter, ToolResult::Success(MaliEventAnalysisToJson(analysis),
                                                          MaliEventAnalysisToText(analysis)));
        });
    count++;
  }

  return count;
}

}    // namespace MCP
