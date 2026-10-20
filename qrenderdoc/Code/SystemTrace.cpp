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
#include <cstring>

#include <QFile>
#include <QHash>
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

  const auto extend = [&](double t) {
    if(!any)
    {
      minT = maxT = t;
      any = true;
    }
    else
    {
      minT = t < minT ? t : minT;
      maxT = t > maxT ? t : maxT;
    }
  };

  for(const TraceSeries &s : series)
  {
    for(const TraceSample &sample : s.samples)
      extend(sample.timeSec);
  }

  // Spans define the visible extent of a lane timeline just as samples do for a counter lane, so
  // they must be included: a trace holding only slices would otherwise report a zero span and be
  // drawn as an empty timeline.
  for(const TraceTrack &track : tracks)
  {
    for(const TraceSpan &span : track.spans)
    {
      extend(span.beginSec);
      extend(span.endSec);
    }
  }

  for(const TraceFrameBoundary &b : frameBoundaries)
    extend(b.timeSec);

  return any ? (maxT - minT) : 0.0;
}

int SystemTrace::SpanCount() const
{
  int count = 0;
  for(const TraceTrack &track : tracks)
    count += track.spans.size();

  return count;
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

namespace
{
// Chrome Trace timestamps are microseconds in the device clock.
const double MicrosecondsToSeconds = 1.0e-6;

struct ChromeEventStats
{
  int events = 0;
  int counters = 0;
  int spans = 0;
  int unmappedPhase = 0;
  int missingTimestamp = 0;
  int malformed = 0;
  int missingDuration = 0;
  int unterminatedSpans = 0;
  int reversedSpans = 0;
  int metadataApplied = 0;
  int metadataUnusable = 0;
  int framesFromArgs = 0;
  int framesFromName = 0;
};

// One (pid,tid) lane as read from the file, before names are resolved and before nesting depth is
// computed. Lanes are created on demand in first-seen order and then reordered for display.
struct RawLane
{
  int pid = -1;
  int tid = -1;
  QVector<TraceSpan> spans;
};

// Accumulates the whole file. Metadata is kept here rather than applied immediately because
// `thread_name` / `process_name` records routinely follow the spans that they caption.
struct ChromeTraceBuilder
{
  QVector<TraceSeries> series;
  QVector<TraceFrameBoundary> frames;
  QVector<RawLane> lanes;
  QHash<quint64, int> laneIndex;
  QHash<int, QString> threadNames;
  QHash<int, QString> processNames;
  // Begin/end ('B' then 'E') spans still open, per lane, outermost last.
  QHash<quint64, QVector<TraceSpan>> openSpans;
  ChromeEventStats stats;
};

// A lane is identified by the pair the trace itself declares. A missing id is not invented: it is
// folded into one explicitly "unknown" lane so those events stay visible instead of being dropped.
quint64 LaneKey(int pid, int tid)
{
  const quint32 p = pid < 0 ? 0xFFFFFFFFu : (quint32)pid;
  const quint32 t = tid < 0 ? 0xFFFFFFFFu : (quint32)tid;
  return ((quint64)p << 32) | (quint64)t;
}

RawLane &LaneFor(ChromeTraceBuilder &builder, int pid, int tid)
{
  const quint64 key = LaneKey(pid, tid);

  const auto it = builder.laneIndex.constFind(key);
  if(it != builder.laneIndex.constEnd())
    return builder.lanes[*it];

  RawLane lane;
  lane.pid = pid;
  lane.tid = tid;

  const int index = builder.lanes.size();
  builder.lanes.push_back(lane);
  builder.laneIndex.insert(key, index);

  return builder.lanes[index];
}

// `args` values are retained verbatim for the detail pane. Scalars are rendered the way the source
// wrote them; nested objects and arrays are rendered as compact JSON rather than being dropped,
// because the value is evidence even when it is not a scalar.
QString FormatArgValue(const QJsonValue &val)
{
  if(val.isString())
    return val.toString();
  if(val.isDouble())
    return QString::number(val.toDouble(), 'g', 10);
  if(val.isBool())
    return val.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  if(val.isNull() || val.isUndefined())
    return QStringLiteral("null");

  const QJsonDocument doc =
      val.isArray() ? QJsonDocument(val.toArray()) : QJsonDocument(val.toObject());
  return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

QStringList EventFields(const QJsonObject &obj)
{
  QStringList fields;

  const QJsonObject args = obj.value(QStringLiteral("args")).toObject();
  for(auto it = args.begin(); it != args.end(); ++it)
    fields << QStringLiteral("%1 = %2").arg(it.key(), FormatArgValue(it.value()));

  return fields;
}

// Depth from interval containment: spans already open when this one starts are its ancestors. The
// stack only ever holds intervals that are still open at the current start time, so siblings and
// later spans never contribute. Sorting outer-before-inner (longer span first on equal start) keeps
// the containment relationship consistent with what a reader sees in the trace.
void AssignNestingDepth(QVector<TraceSpan> &spans)
{
  std::sort(spans.begin(), spans.end(), [](const TraceSpan &a, const TraceSpan &b) {
    if(a.beginSec != b.beginSec)
      return a.beginSec < b.beginSec;
    return a.endSec > b.endSec;
  });

  QVector<double> openEnds;
  for(TraceSpan &span : spans)
  {
    while(!openEnds.isEmpty() && openEnds.last() <= span.beginSec)
      openEnds.removeLast();

    span.depth = openEnds.size();
    openEnds.push_back(span.endSec);
  }
}

// Extracts an explicit frame index from an event. Returns -1 when the event does not describe a
// frame boundary. Two forms are accepted, in order of authority:
//   1. an integer in args under frameNumber / frame_number / frame
//   2. a slice name of the exact form "Frame <n>"
// `fromName` reports which form matched so the caller can record the weaker provenance.
int64_t FrameIndexFromEvent(const QJsonObject &obj, bool &fromName)
{
  fromName = false;

  const QJsonObject args = obj.value(QStringLiteral("args")).toObject();
  const char *argKeys[] = {"frameNumber", "frame_number", "frame"};
  for(const char *key : argKeys)
  {
    const QJsonValue val = args.value(QLatin1String(key));
    if(val.isDouble())
      return (int64_t)val.toDouble();
  }

  const QString name = obj.value(QStringLiteral("name")).toString();
  if(name.startsWith(QStringLiteral("Frame "), Qt::CaseInsensitive))
  {
    bool ok = false;
    const int64_t index = name.mid(6).trimmed().toLongLong(&ok);
    if(ok)
    {
      fromName = true;
      return index;
    }
  }

  return -1;
}

// Collects a counter event's values. Chrome counters carry a name per value in `args`, which keeps
// distinct counters in one event separate rather than merged into one series.
void AddCounterEvent(const QJsonObject &obj, QVector<TraceSeries> &series, ChromeEventStats &stats)
{
  const double timeSec = obj.value(QStringLiteral("ts")).toDouble() * MicrosecondsToSeconds;
  const QString category = obj.value(QStringLiteral("cat")).toString();
  const QJsonObject args = obj.value(QStringLiteral("args")).toObject();

  if(args.isEmpty())
  {
    // A counter with no named values would need an invented name to be displayed, so it is
    // counted rather than guessed at.
    stats.malformed++;
    return;
  }

  for(auto it = args.begin(); it != args.end(); ++it)
  {
    if(!it.value().isDouble())
      continue;

    const QString name = category.isEmpty() ? it.key() : category + QStringLiteral("/") + it.key();

    int idx = -1;
    for(int i = 0; i < series.size(); i++)
    {
      if(series[i].name == name)
      {
        idx = i;
        break;
      }
    }

    if(idx < 0)
    {
      TraceSeries s;
      s.name = name;
      // Chrome counters do not declare a unit. Leaving it empty is the truthful choice.
      idx = series.size();
      series.push_back(s);
    }

    TraceSample sample;
    sample.timeSec = timeSec;
    sample.value = it.value().toDouble();
    series[idx].samples.push_back(sample);
  }

  stats.counters++;
}

// A lane id as the trace declares it. Chrome traces put pid/tid at the event's top level; the
// Perfetto exporter always writes both, but a trace without them still has to produce a lane
// rather than losing the event.
void EventLane(const QJsonObject &obj, int &pid, int &tid)
{
  const QJsonValue pidVal = obj.value(QStringLiteral("pid"));
  const QJsonValue tidVal = obj.value(QStringLiteral("tid"));

  pid = pidVal.isDouble() ? (int)pidVal.toDouble() : -1;
  tid = tidVal.isDouble() ? (int)tidVal.toDouble() : -1;
}

void ConsumeMetadata(const QJsonObject &obj, ChromeTraceBuilder &builder)
{
  const QString name = obj.value(QStringLiteral("name")).toString();
  const QJsonObject args = obj.value(QStringLiteral("args")).toObject();

  int pid = -1;
  int tid = -1;
  EventLane(obj, pid, tid);

  // Perfetto writes the captioned name in args.name; some producers use args directly as a
  // single-field object instead. Both are read, and neither is invented if absent.
  QString value;
  if(args.contains(QStringLiteral("name")))
    value = args.value(QStringLiteral("name")).toString();
  else if(args.size() == 1)
    value = args.begin().value().toString();

  if(value.isEmpty())
  {
    builder.stats.metadataUnusable++;
    return;
  }

  if(name == QStringLiteral("thread_name"))
  {
    if(tid >= 0)
    {
      builder.threadNames.insert(tid, value);
      builder.stats.metadataApplied++;
    }
    else
    {
      builder.stats.metadataUnusable++;
    }
    return;
  }

  if(name == QStringLiteral("process_name"))
  {
    if(pid >= 0)
    {
      builder.processNames.insert(pid, value);
      builder.stats.metadataApplied++;
    }
    else
    {
      builder.stats.metadataUnusable++;
    }
    return;
  }

  // Any other metadata key (labels, sort indices, ...) is not modelled. It is not counted as a
  // problem because nothing downstream can display it.
}

void ConsumeChromeEvent(const QJsonObject &obj, ChromeTraceBuilder &builder)
{
  builder.stats.events++;

  const QString phase = obj.value(QStringLiteral("ph")).toString();

  if(phase == QStringLiteral("C"))
  {
    AddCounterEvent(obj, builder.series, builder.stats);
    return;
  }

  if(phase == QStringLiteral("M"))
  {
    // Metadata carries no timestamp; it captions lanes that are resolved after the file is read.
    ConsumeMetadata(obj, builder);
    return;
  }

  if(!obj.contains(QStringLiteral("ts")))
  {
    builder.stats.malformed++;
    return;
  }

  const double timeSec = obj.value(QStringLiteral("ts")).toDouble() * MicrosecondsToSeconds;

  // Frame detection runs for every timestamped event, exactly as before, so a span that also
  // declares a frame index still contributes its boundary.
  bool fromName = false;
  const int64_t frameIndex = FrameIndexFromEvent(obj, fromName);

  if(frameIndex >= 0)
  {
    TraceFrameBoundary b;
    b.frameNumber = (uint32_t)frameIndex;
    b.timeSec = timeSec;
    builder.frames.push_back(b);

    if(fromName)
      builder.stats.framesFromName++;
    else
      builder.stats.framesFromArgs++;
  }

  int pid = -1;
  int tid = -1;
  EventLane(obj, pid, tid);

  if(phase == QStringLiteral("X"))
  {
    // A complete span. `dur` is required: without it there is no interval to draw, and inventing
    // one (e.g. zero-length) would misrepresent the trace.
    const QJsonValue durVal = obj.value(QStringLiteral("dur"));
    if(!durVal.isDouble())
    {
      builder.stats.missingDuration++;
      return;
    }

    TraceSpan span;
    span.beginSec = timeSec;
    span.endSec = timeSec + durVal.toDouble() * MicrosecondsToSeconds;
    span.name = obj.value(QStringLiteral("name")).toString();
    span.category = obj.value(QStringLiteral("cat")).toString();
    span.fields = EventFields(obj);

    if(span.endSec < span.beginSec)
    {
      // Cannot happen for a non-negative dur, but a negative dur is a malformed source, not a
      // reason to draw an inverted box.
      builder.stats.reversedSpans++;
      return;
    }

    LaneFor(builder, pid, tid).spans.push_back(span);
    builder.stats.spans++;
    return;
  }

  if(phase == QStringLiteral("B"))
  {
    // Begin: pushed onto this lane's stack. Its end time is unknown until the matching 'E'.
    TraceSpan span;
    span.beginSec = timeSec;
    span.endSec = timeSec;
    span.name = obj.value(QStringLiteral("name")).toString();
    span.category = obj.value(QStringLiteral("cat")).toString();
    span.fields = EventFields(obj);

    // Ensure the lane exists even if the 'E' never arrives, so the begin is still evident.
    LaneFor(builder, pid, tid);
    builder.openSpans[LaneKey(pid, tid)].push_back(span);
    return;
  }

  if(phase == QStringLiteral("E"))
  {
    QVector<TraceSpan> &open = builder.openSpans[LaneKey(pid, tid)];

    if(open.isEmpty())
    {
      // An 'E' without a 'B': there is no interval to close.
      builder.stats.unterminatedSpans++;
      return;
    }

    TraceSpan span = open.takeLast();
    span.endSec = timeSec;

    if(span.endSec < span.beginSec)
    {
      builder.stats.reversedSpans++;
      return;
    }

    LaneFor(builder, pid, tid).spans.push_back(span);
    builder.stats.spans++;
    return;
  }

  // Instant ('i'), async ('b'/'n'/'e'), flow ('s'/'t'/'f'), object ('N'/'O'/'D') and legacy phases
  // have no duration to draw as a lane interval, so they are counted rather than coerced.
  builder.stats.unmappedPhase++;
}
}

SystemTrace ImportChromeTraceEventFormat(const QString &path)
{
  SystemTrace trace;
  trace.source = path;
  trace.formatName = QStringLiteral("Chrome Trace Event Format (Perfetto traceconv json)");
  // Timestamps are assumed to be microseconds of device time; there is no shared clock with the
  // capture, so correlation has to derive an offset from frame boundaries.
  trace.timeUnit = QStringLiteral("s");

  QFile file(path);
  if(!file.open(QIODevice::ReadOnly))
  {
    trace.warnings << QObject::tr("Could not open trace file: %1").arg(path);
    return trace;
  }

  // A Chrome trace is one JSON document, but the events array can be far larger than is sensible
  // to materialise at once. The file is therefore scanned for balanced top-level event objects and
  // each one is parsed individually. This also means a single malformed event does not discard
  // the whole trace.
  const QByteArray contents = file.readAll();
  if(contents.isEmpty())
  {
    trace.warnings << QObject::tr("Trace file is empty: %1").arg(path);
    return trace;
  }

  // The builder accumulates everything, including metadata that captions lanes created by events
  // encountered earlier in the file.
  ChromeTraceBuilder builder;

  // Locate the event stream: either the value of "traceEvents", or the file's top-level array.
  int pos = 0;
  bool sawKey = false;

  {
    const int keyIdx = contents.indexOf("\"traceEvents\"");
    if(keyIdx >= 0)
    {
      pos = keyIdx + int(strlen("\"traceEvents\""));
      sawKey = true;
    }
  }

  // Advance to the opening bracket of the array.
  while(pos < contents.size() && contents.at(pos) != '[')
    pos++;

  if(pos >= contents.size())
  {
    trace.warnings << QObject::tr(
        "Trace does not contain a Chrome trace event array (expected a top-level array or a "
        "'traceEvents' key): %1")
                          .arg(path);
    return trace;
  }

  pos++; // past '['

  // `depth` counts brace nesting of the events themselves. Strings are tracked unconditionally
  // because event payloads routinely contain '[' and ']' inside names and arguments, and treating
  // those as structure would corrupt the depth count.
  int depth = 0;
  int objectStart = -1;
  bool inString = false;
  bool escaped = false;

  for(; pos < contents.size(); pos++)
  {
    const char c = contents.at(pos);

    if(inString)
    {
      if(escaped)
        escaped = false;
      else if(c == '\\')
        escaped = true;
      else if(c == '"')
        inString = false;
      continue;
    }

    if(c == '"')
    {
      inString = true;
      continue;
    }

    if(c == '{')
    {
      if(depth == 0)
        objectStart = pos;
      depth++;
      continue;
    }

    if(c == '}')
    {
      if(depth <= 0)
        continue;

      depth--;
      if(depth == 0 && objectStart >= 0)
      {
        QJsonParseError err = {};
        const QJsonDocument doc =
            QJsonDocument::fromJson(contents.mid(objectStart, pos - objectStart + 1), &err);

        if(err.error != QJsonParseError::NoError || !doc.isObject())
          builder.stats.malformed++;
        else
          ConsumeChromeEvent(doc.object(), builder);

        objectStart = -1;
      }
      continue;
    }

    // The event array's closing bracket at brace depth 0 ends the stream.
    if(c == ']' && depth == 0)
      break;
  }

  const ChromeEventStats &stats = builder.stats;

  if(!sawKey && stats.events == 0)
    trace.warnings << QObject::tr(
        "Trace has no recognisable top-level event array; the file may not be a Chrome trace "
        "export");

  trace.series = builder.series;
  trace.frameBoundaries = builder.frames;

  for(TraceSeries &s : trace.series)
    s.Sort();

  std::sort(trace.frameBoundaries.begin(), trace.frameBoundaries.end(),
            [](const TraceFrameBoundary &a, const TraceFrameBoundary &b) {
              return a.timeSec < b.timeSec;
            });

  // --- Lane construction ------------------------------------------------------------------------
  //
  // Lanes with spans carry the real timeline. A span open at end-of-file ('B' with no 'E') is
  // retained and closed at the last observed timestamp: dropping it would hide work that really
  // happened, while inventing an end time beyond the trace would draw a box off the timeline.
  double lastTs = 0.0;
  bool anyTs = false;
  for(const RawLane &lane : builder.lanes)
  {
    for(const TraceSpan &span : lane.spans)
    {
      if(!anyTs || span.endSec > lastTs)
        lastTs = span.endSec;
      anyTs = true;
    }
  }

  int closedUnterminated = 0;
  for(auto it = builder.openSpans.begin(); it != builder.openSpans.end(); ++it)
  {
    if(it.value().isEmpty())
      continue;

    // The lane is located through the same key it was opened under.
    const quint32 pidBits = (quint32)(it.key() >> 32);
    const quint32 tidBits = (quint32)(it.key() & 0xFFFFFFFFull);
    const int pid = pidBits == 0xFFFFFFFFu ? -1 : (int)pidBits;
    const int tid = tidBits == 0xFFFFFFFFu ? -1 : (int)tidBits;

    RawLane &lane = LaneFor(builder, pid, tid);
    for(TraceSpan &span : it.value())
    {
      span.endSec = anyTs ? lastTs : span.beginSec;
      lane.spans.push_back(span);
      closedUnterminated++;
    }
  }

  // Counter lanes come first and are ordered by series, so the metrics that explain GPU-bound
  // frames stay at the top of the view the way SDP presents them. Thread lanes follow, grouped by
  // process and then ordered by tid, which is stable across imports.
  for(int i = 0; i < trace.series.size(); i++)
  {
    TraceTrack track;
    track.kind = TraceTrackKind::Counter;
    track.name = trace.series[i].name;
    track.unit = trace.series[i].unit;
    track.counterSeries = i;
    trace.tracks.push_back(track);
  }

  // Order lanes by pid then tid so the group headers stay contiguous; lanes without ids sort last
  // and are named as unknown rather than being assigned a fabricated id.
  QVector<int> order;
  order.reserve(builder.lanes.size());
  for(int i = 0; i < builder.lanes.size(); i++)
    order.push_back(i);

  std::sort(order.begin(), order.end(), [&](int a, int b) {
    const RawLane &la = builder.lanes[a];
    const RawLane &lb = builder.lanes[b];

    if(la.pid != lb.pid)
    {
      if(la.pid < 0)
        return false;
      if(lb.pid < 0)
        return true;
      return la.pid < lb.pid;
    }

    if(la.tid != lb.tid)
    {
      if(la.tid < 0)
        return false;
      if(lb.tid < 0)
        return true;
      return la.tid < lb.tid;
    }

    return a < b;
  });

  for(int laneIdx : order)
  {
    RawLane &lane = builder.lanes[laneIdx];

    if(lane.spans.isEmpty())
      continue;

    AssignNestingDepth(lane.spans);

    TraceTrack track;
    track.pid = lane.pid;
    track.tid = lane.tid;
    track.kind = TraceTrackKind::Thread;

    // Names come only from metadata the trace itself declared. When it declared none, the caption
    // names the ids it did declare, which is truthful and still lets the user correlate a lane with
    // the trace's own pid/tid.
    const QString threadName = lane.tid >= 0 ? builder.threadNames.value(lane.tid) : QString();
    const QString processName = lane.pid >= 0 ? builder.processNames.value(lane.pid) : QString();

    if(!threadName.isEmpty())
      track.name = threadName;
    else if(lane.tid >= 0)
      track.name = QObject::tr("tid %1").arg(lane.tid);
    else
      track.name = QObject::tr("Unknown thread");

    if(!processName.isEmpty())
      track.group = processName;
    else if(lane.pid >= 0)
      track.group = QObject::tr("pid %1").arg(lane.pid);

    if(threadName.isEmpty() && processName.isEmpty() && lane.tid < 0 && lane.pid < 0)
      track.kind = TraceTrackKind::Process;

    track.spans = lane.spans;
    trace.tracks.push_back(track);
  }

  if(stats.counters == 0 && !trace.tracks.isEmpty())
    trace.warnings << QObject::tr(
        "Trace contained no counter ('ph':'C') events: only spans and frame boundaries can be "
        "shown");

  if(stats.framesFromName > 0)
    trace.warnings << QObject::tr(
        "%1 frame boundaries were identified from a 'Frame <n>' slice name rather than an "
        "explicit frame index in 'args'")
                          .arg(stats.framesFromName);

  if(stats.malformed > 0)
    trace.warnings << QObject::tr("%1 events could not be parsed and were skipped")
                          .arg(stats.malformed);

  if(stats.missingDuration > 0)
    trace.warnings << QObject::tr(
        "%1 complete ('ph':'X') events had no 'dur', so they could not be drawn as spans")
                          .arg(stats.missingDuration);

  if(stats.unterminatedSpans > 0)
    trace.warnings << QObject::tr(
        "%1 end events had no matching begin event and were skipped")
                          .arg(stats.unterminatedSpans);

  if(closedUnterminated > 0)
    trace.warnings << QObject::tr(
        "%1 spans were still open at the end of the trace and were closed at the last recorded "
        "timestamp: their true end is unknown")
                          .arg(closedUnterminated);

  if(stats.reversedSpans > 0)
    trace.warnings << QObject::tr("%1 spans ended before they began and were skipped")
                          .arg(stats.reversedSpans);

  if(stats.metadataUnusable > 0)
    trace.warnings << QObject::tr(
        "%1 metadata records carried no usable name or no id, so lanes keep their id captions")
                          .arg(stats.metadataUnusable);

  if(trace.series.isEmpty() && trace.frameBoundaries.isEmpty() && trace.SpanCount() == 0)
  {
    trace.warnings << QObject::tr(
        "Trace contained no counter events, spans and no frame boundaries, so nothing can be "
        "correlated (%1 events examined, %2 phases not drawn)")
                          .arg(stats.events)
                          .arg(stats.unmappedPhase);
    return trace;
  }

  if(trace.frameBoundaries.isEmpty())
    trace.warnings << QObject::tr(
        "Trace contained no frame boundaries: correlation to the capture will be limited to "
        "absolute time");

  trace.valid = true;
  return trace;
}
