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

#pragma once

#include <stdint.h>

#include <QString>
#include <QStringList>
#include <QVector>

// ---------------------------------------------------------------------------------------------
// P1 — frame-interior counters x system-level trace correlation view.
//
// This is deliberately a self-contained, cross-platform, offline module: it parses a
// documented export format into an in-memory timeline and correlates it against RenderDoc's
// frame timing. It never touches the capture core, never links against engine internals and
// never bundles any vendor binary.
//
// Evidence discipline (see the integration planning document):
//   * Everything this file can determine is an explicit field.
//   * Anything that could not be determined is recorded as unknown rather than guessed, and
//     surfaced through SystemTrace::warnings / ClockDomainAlignment::confidence.
//   * [观测] Snapdragon Profiler's trace backend is Perfetto / Systrace: its trace interop is
//     `TraceImportExport.TraceImport.BUFFER_TYPE_SYSTRACE_DATA`, served by `pluginPerfetto.dll`
//     and `SystraceProcessor.dll`, and the distribution bundles Perfetto's own
//     `traceconv-win-amd64.exe` (section 8.9.3). SDP therefore exports a Perfetto protobuf
//     trace, NOT a bespoke text format, and `traceconv` converts that to `json` / `systrace` /
//     `text`. This module consumes RenderDoc's own documented JSON-lines interchange, which is
//     the `traceconv ... json`-style output; the logic here is format-agnostic on top of it.
// ---------------------------------------------------------------------------------------------

// A single (timestamp, value) sample in a trace series. Timestamps are in the trace's own
// clock domain (see SystemTrace::timeUnit), normally seconds.
struct TraceSample
{
  double timeSec = 0.0;
  double value = 0.0;
};

// A named series of samples, e.g. "DDR Bandwidth" or "GPU Busy". A trace normally carries
// several of these over a common clock domain.
struct TraceSeries
{
  QString name;
  QString unit;    // display-only, may be empty if the source did not declare one
  QVector<TraceSample> samples;

  // Sort samples ascending by time. Safe to call more than once.
  void Sort();

  bool IsEmpty() const { return samples.isEmpty(); }
  double MinValue() const;
  double MaxValue() const;

  // Linear interpolation of the series at an arbitrary trace time. Returns false when the
  // series is empty. Times outside the sampled range are clamped to the nearest endpoint.
  bool SampleAt(double traceTimeSec, double &outValue) const;
};

// A frame boundary detected in the trace. `timeSec` is in the trace's own clock domain.
struct TraceFrameBoundary
{
  uint32_t frameNumber = 0;
  double timeSec = 0.0;
};

// A span (slice): something that occupied an interval on exactly one track lane, e.g. a scheduled
// thread slice or a GPU workload. Counters are sampled measurements and cannot express "this ran
// from t0 to t1", so they are kept as TraceSeries rather than as spans.
//
// Without this the import would lose every non-counter event, and a track view is impossible: SDP's
// trace panel is a lane-per-thread/context timeline of exactly these spans.
struct TraceSpan
{
  double beginSec = 0.0;
  double endSec = 0.0;
  QString name;
  // The source's own category (`cat`), kept verbatim. May be empty.
  QString category;
  // Nesting depth inside its lane, 0-based, derived from interval containment. The renderer uses it
  // to stack nested spans instead of inventing a layout.
  int depth = 0;
  // The event's `args` fields, already formatted "key = value" with the source's own values
  // (numbers formatted as JSON rendered them). Display-only: nothing parses these back.
  QStringList fields;
};

// What a lane represents. Only used for ordering and for the header caption; it never changes how
// spans are drawn.
enum class TraceTrackKind
{
  // A metric series, drawn as a curve rather than as spans.
  Counter,
  // A host thread, identified by the trace's own pid/tid pair.
  Thread,
  // A lane whose events carried no thread identity.
  Process,
};

// One lane of the timeline. `group` is the owning process (or the trace's own grouping) and is
// rendered as a group header, mirroring how SDP groups tracks per process/device.
struct TraceTrack
{
  // Resolved display name: the trace's declared thread_name / process_name when it declared one,
  // otherwise a truthful placeholder naming the id. Never invented.
  QString name;
  // Owning process display name, or empty when the trace declared no process identity.
  QString group;
  TraceTrackKind kind = TraceTrackKind::Thread;
  // -1 when the source declared no value. Kept as the source's own integer, never remapped.
  int pid = -1;
  int tid = -1;
  QVector<TraceSpan> spans;
  // Index into SystemTrace::series for a Counter lane, or -1 for a span lane.
  int counterSeries = -1;
  // Display unit for a counter lane, copied from the series so the header can show it.
  QString unit;
};

// How confidently trace time could be related to RenderDoc time.
enum class ClockDomainConfidence
{
  // No correlation was possible: the trace carried no frame boundaries, or none of them
  // matched a frame RenderDoc knows about.
  Unknown,
  // An offset was derived from matching a trace frame boundary to RenderDoc's frame timing.
  // The residual error across all matched frames is reported as the tolerance.
  EstimatedFromFrameTiming,
  // The source declared that the trace already shares RenderDoc's clock domain, so the
  // offset is zero by construction (tolerance 0).
  AssumedShared,
};

// The offset relating the two clocks. RenderDoc action times are expressed relative to the
// correlated frame's origin (0 at the start of the frame), so:
//   traceTimeSec = renderdocFrameRelativeTimeSec + offsetSec
struct ClockDomainAlignment
{
  ClockDomainConfidence confidence = ClockDomainConfidence::Unknown;
  double offsetSec = 0.0;
  // Worst-case residual (seconds) across all frames used to fit the offset. Zero for
  // AssumedShared, meaningless when confidence is Unknown.
  double toleranceSec = 0.0;
  // Human-readable provenance, shown verbatim in the UI so the user can see how the two
  // clock domains were related (or that they could not be).
  QString description;
};

// The outcome of importing a trace. `valid` is true only when at least one series, one span or one
// frame boundary was parsed. `warnings` records everything that was missing, ambiguous or dropped,
// rather than silently discarding it.
struct SystemTrace
{
  QString source;      // human-readable origin (file path or declared source)
  QString formatName;  // provenance of the interchange, e.g. "Perfetto/Systrace (traceconv json)"
  QString timeUnit;    // declared unit of the sample timestamps, e.g. "s"
  // True when the source declared (via a "clockDomain": "shared" header record) that its
  // timestamps already share the capture's clock domain, so no offset needs to be derived.
  bool sharedClock = false;
  QVector<TraceSeries> series;
  QVector<TraceFrameBoundary> frameBoundaries;
  // The lane structure of the trace, in display order: counter lanes first, then process/thread
  // lanes. Empty when the source carried only counters.
  QVector<TraceTrack> tracks;
  QStringList warnings;
  bool valid = false;

  const TraceSeries *FindSeries(const QString &name) const;
  // Total trace span in seconds; 0 when empty. Spans and series both contribute.
  double SpanSec() const;
  // Number of spans across all tracks, for status reporting.
  int SpanCount() const;
};

// Parse a trace in the documented JSON-lines interchange. This function never throws and never
// asserts: on any failure it returns an invalid SystemTrace whose `warnings` explain why.
//
// [观测] The interchange is the converter output for an SDP trace: SDP emits a Perfetto
// protobuf trace and Perfetto's bundled `traceconv` renders it to JSON/Systrace text
// (section 8.9.3). This importer is a deliberately format-agnostic front end for that text -
// it does not parse protobuf and does not link Perfetto.
SystemTrace ImportSystemTraceJsonLines(const QString &path);

// Parse a trace in the Chrome Trace Event Format, which is what `traceconv json` and Perfetto's
// own JSON export actually emit: either a bare JSON array of events or an object with a
// `traceEvents` array. This is the shape a real device trace arrives in, so it is the importer the
// capture path uses.
//
// What is mapped, and what is deliberately not:
//   * `ph:"C"` counter events become TraceSeries, one per (category, name) pair. Chrome counters
//     carry no unit, so `TraceSeries::unit` stays empty rather than being invented.
//   * `ph:"X"` (complete span, using `dur`) and `ph:"B"`/`ph:"E"` (begin/end pair) events become
//     TraceSpans on a per-(pid,tid) lane, so the panel can render a lane timeline. Their `args`
//     are retained verbatim as display fields. Nesting depth comes from interval containment.
//   * `ph:"M"` metadata is used only for `thread_name` / `process_name`, which supply lane captions.
//     The names are applied after the whole file is read, because metadata may follow its spans.
//   * Frame boundaries come only from events carrying an explicit frame index - an integer in
//     `args` under `frameNumber` / `frame_number` / `frame`, or a slice name of the form
//     `Frame <n>`. When the second form is used a warning records that the index came from the
//     slice name, so the provenance stays visible.
//   * Instant / async / flow events are counted as unmapped rather than being coerced into a span,
//     because they have no duration to draw.
//
// Timestamps are microseconds in the device's own clock domain, so samples are converted to
// seconds and `sharedClock` stays false; correlation must derive the offset from frame boundaries.
// The function never throws and never asserts: on failure it returns an invalid SystemTrace whose
// `warnings` explain why.
SystemTrace ImportChromeTraceEventFormat(const QString &path);
