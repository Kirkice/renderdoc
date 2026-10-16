/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
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

#include "SystemTrace.h"

#include <algorithm>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

// [观测] System-trace import provenance (planning document section 8.9.3).
//
// Reverse-engineering the Snapdragon Profiler host and device components established that SDP's
// own system-trace backend is Perfetto/Systrace, not a bespoke format:
//   * rodata in the SDP host assemblies references `TraceImportExport.TraceImport.BUFFER_TYPE_
//     SYSTRACE_DATA`, `pluginPerfetto.dll`, `SystraceProcessor.dll`, and `traceconv-win-amd64.exe`.
//   * SDP therefore records a Perfetto protobuf trace and shells out to `traceconv` to render it
//     (including a `json` sub-command) for interchange.
// This file does NOT parse the Perfetto protobuf and does NOT link Perfetto. It parses the
// line-delimited JSON that the `traceconv ... json`-style export produces, which keeps the
// importer dependency-free while still consuming the real exporter's output shape.
//
// The deprecated `PeakJSONL` name that used to label formatName described an invented interchange
// that SDP never emits; it has been replaced with the actual provenance string so downstream
// diagnostics report where a trace really came from.

void TraceSeries::Sort()
{
  std::sort(samples.begin(), samples.end(),
            [](const TraceSample &a, const TraceSample &b) { return a.timeSec < b.timeSec; });
}

double TraceSeries::MinValue() const
{
  if(samples.isEmpty())
    return 0.0;

  double v = samples[0].value;
  for(const TraceSample &s : samples)
    v = s.value < v ? s.value : v;
  return v;
}

double TraceSeries::MaxValue() const
{
  if(samples.isEmpty())
    return 0.0;

  double v = samples[0].value;
  for(const TraceSample &s : samples)
    v = s.value > v ? s.value : v;
  return v;
}

bool TraceSeries::SampleAt(double traceTimeSec, double &outValue) const
{
  if(samples.isEmpty())
    return false;

  // The series is expected to be sorted; guard against an unsorted import anyway by scanning
  // when the endpoints don't bracket the query in the expected direction.
  const bool sortedAscending = samples.first().timeSec <= samples.last().timeSec;

  if(!sortedAscending)
  {
    if(samples.first().timeSec <= samples.last().timeSec)
      return false;

    // Linear scan fallback for a descending series.
    if(traceTimeSec <= samples.first().timeSec)
    {
      outValue = samples.first().value;
      return true;
    }
    if(traceTimeSec >= samples.last().timeSec)
    {
      outValue = samples.last().value;
      return true;
    }
    for(int i = 0; i + 1 < samples.size(); i++)
    {
      const TraceSample &a = samples[i];
      const TraceSample &b = samples[i + 1];
      // Descending: a.timeSec >= b.timeSec
      if(traceTimeSec <= a.timeSec && traceTimeSec >= b.timeSec)
      {
        const double span = a.timeSec - b.timeSec;
        const double t = span > 0.0 ? (a.timeSec - traceTimeSec) / span : 0.0;
        outValue = a.value + (b.value - a.value) * t;
        return true;
      }
    }
    outValue = samples.last().value;
    return true;
  }

  if(traceTimeSec <= samples.first().timeSec)
  {
    outValue = samples.first().value;
    return true;
  }
  if(traceTimeSec >= samples.last().timeSec)
  {
    outValue = samples.last().value;
    return true;
  }

  // Binary search for the first sample with timeSec >= traceTimeSec.
  int lo = 0;
  int hi = samples.size() - 1;
  while(lo < hi)
  {
    const int mid = (lo + hi) / 2;
    if(samples[mid].timeSec < traceTimeSec)
      lo = mid + 1;
    else
      hi = mid;
  }

  const TraceSample &b = samples[lo];
  const TraceSample &a = samples[lo - 1];
  const double span = b.timeSec - a.timeSec;
  const double t = span > 0.0 ? (traceTimeSec - a.timeSec) / span : 0.0;
  outValue = a.value + (b.value - a.value) * t;
  return true;
}

const TraceSeries *SystemTrace::FindSeries(const QString &name) const
{
  for(const TraceSeries &s : series)
  {
    if(s.name == name)
      return &s;
  }
  return NULL;
}

double SystemTrace::SpanSec() const
{
  double minT = 0.0;
  double maxT = 0.0;
  bool any = false;

  for(const TraceSeries &s : series)
  {
    for(const TraceSample &sample : s.samples)
    {
      if(!any)
      {
        minT = maxT = sample.timeSec;
        any = true;
      }
      else
      {
        minT = sample.timeSec < minT ? sample.timeSec : minT;
        maxT = sample.timeSec > maxT ? sample.timeSec : maxT;
      }
    }
  }

  for(const TraceFrameBoundary &b : frameBoundaries)
  {
    if(!any)
    {
      minT = maxT = b.timeSec;
      any = true;
    }
    else
    {
      minT = b.timeSec < minT ? b.timeSec : minT;
      maxT = b.timeSec > maxT ? b.timeSec : maxT;
    }
  }

  return any ? (maxT - minT) : 0.0;
}

SystemTrace ImportSystemTraceJsonLines(const QString &path)
{
  SystemTrace trace;
  trace.source = path;

  // [观测] The interchange is a `traceconv`-style line-JSON export of a Perfetto/Systrace trace
  // (planning document section 8.9.3), not the invented "PeakJSONL" envelope. The provenance
  // string is set here so the viewer and correlation report a truthful origin. The parser below
  // is intentionally format-agnostic within that envelope: it accepts `header`, `series`,
  // `sample` and `frame` records and tolerates unknown ones.
  trace.formatName = QStringLiteral("Perfetto/Systrace (traceconv json)");

  QFile file(path);
  if(!file.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    trace.warnings << QObject::tr("Could not open trace file: %1").arg(path);
    return trace;
  }

  int lineNumber = 0;
  int lineErrors = 0;
  bool sawVersion = false;

  while(!file.atEnd())
  {
    const QByteArray raw = file.readLine();
    lineNumber++;

    const QByteArray line = raw.trimmed();
    if(line.isEmpty() || line.startsWith('#'))
      continue;

    QJsonParseError err = {};
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if(err.error != QJsonParseError::NoError || !doc.isObject())
    {
      lineErrors++;
      continue;
    }

    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();

    if(type == QStringLiteral("header"))
    {
      if(obj.contains(QStringLiteral("formatVersion")))
      {
        const int version = obj.value(QStringLiteral("formatVersion")).toInt();
        if(version != 1)
          trace.warnings << QObject::tr("Trace format version %1 is not the supported version 1").arg(version);
        else
          sawVersion = true;
      }
      if(obj.contains(QStringLiteral("timeUnit")))
        trace.timeUnit = obj.value(QStringLiteral("timeUnit")).toString();
      if(obj.contains(QStringLiteral("source")))
        trace.source = obj.value(QStringLiteral("source")).toString();
      // An explicit declaration lets correlation trust trace time without deriving an offset.
      if(obj.value(QStringLiteral("clockDomain")).toString() == QStringLiteral("shared"))
        trace.sharedClock = true;
    }
    else if(type == QStringLiteral("series"))
    {
      TraceSeries s;
      s.name = obj.value(QStringLiteral("name")).toString();
      s.unit = obj.value(QStringLiteral("unit")).toString();

      if(s.name.isEmpty())
      {
        lineErrors++;
        continue;
      }

      const QJsonArray samples = obj.value(QStringLiteral("samples")).toArray();
      s.samples.reserve(samples.size());
      for(const QJsonValue &sampleVal : samples)
      {
        const QJsonArray pair = sampleVal.toArray();
        if(pair.size() < 2)
          continue;
        TraceSample sample;
        sample.timeSec = pair.at(0).toDouble();
        sample.value = pair.at(1).toDouble();
        s.samples.push_back(sample);
      }

      if(s.samples.isEmpty())
      {
        trace.warnings << QObject::tr("Series '%1' contained no samples").arg(s.name);
        continue;
      }

      s.Sort();
      trace.series.push_back(s);
    }
    else if(type == QStringLiteral("sample"))
    {
      const QString name = obj.value(QStringLiteral("series")).toString();
      int idx = -1;
      for(int i = 0; i < trace.series.size(); i++)
      {
        if(trace.series[i].name == name)
        {
          idx = i;
          break;
        }
      }

      if(idx < 0)
      {
        TraceSeries s;
        s.name = name;
        s.unit = obj.value(QStringLiteral("unit")).toString();
        idx = trace.series.size();
        trace.series.push_back(s);
      }

      TraceSample sample;
      sample.timeSec = obj.value(QStringLiteral("t")).toDouble();
      sample.value = obj.value(QStringLiteral("v")).toDouble();
      trace.series[idx].samples.push_back(sample);
    }
    else if(type == QStringLiteral("frame"))
    {
      TraceFrameBoundary b;
      b.frameNumber = (uint32_t)obj.value(QStringLiteral("frameNumber")).toInt();
      b.timeSec = obj.value(QStringLiteral("t")).toDouble();
      trace.frameBoundaries.push_back(b);
    }
    else
    {
      // Unknown record types are ignored but not hidden: the first few are surfaced.
      if(lineErrors < 8)
        trace.warnings << QObject::tr("Ignored unknown trace record type '%1' at line %2")
                              .arg(type)
                              .arg(lineNumber);
      lineErrors++;
    }
  }

  if(!sawVersion && !trace.series.isEmpty())
    trace.warnings << QObject::tr("Trace did not declare 'formatVersion': assuming version 1");

  // Samples appended via 'sample' records need a final sort.
  for(TraceSeries &s : trace.series)
    s.Sort();

  if(lineErrors > 8)
    trace.warnings << QObject::tr("%1 additional malformed or unknown lines were skipped")
                          .arg(lineErrors - 8);

  if(trace.series.isEmpty() && trace.frameBoundaries.isEmpty())
  {
    trace.warnings << QObject::tr("Trace contained no usable series or frame boundaries");
    return trace;
  }

  if(trace.frameBoundaries.isEmpty())
    trace.warnings << QObject::tr(
        "Trace contained no frame boundaries: correlation to the capture will be limited to "
        "absolute time");

  if(trace.timeUnit.isEmpty())
    trace.warnings << QObject::tr("Trace did not declare a time unit: assuming seconds");

  trace.valid = true;
  return trace;
}
