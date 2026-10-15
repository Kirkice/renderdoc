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

#pragma once

// P2-2 cross-capture snapshot cache and regression diff
// ----------------------------------------------------
// The plugin's CompareCapturesTool compares two captures side by side. Doing
// that in an AI-facing server has a hard constraint the plugin never had: the
// second capture is frequently *not open*, and opening one is a slow, stateful,
// disruptive operation that cannot run in the middle of a tool call. So P2-2
// splits the problem in two:
//
//   * a *snapshot* is the normalized, flattened projection of one capture -
//     events with their canonical key and GPU duration, resources with their
//     creation size, passes with their duration and shader-stage bindings. It is
//     plain data that serialises to JSON, so it can be cached in a sidecar file
//     next to the capture and re-read without reopening anything;
//
//   * the *diff* is a pure function of two snapshots. It pairs entries with the
//     P2-1 matcher (so event ids, naming noise, duplicate resources and pass
//     reordering are already absorbed), computes per-entry deltas and stamps
//     every row with a verdict and the match confidence that produced it.
//
// Everything here is UI-free, capture-free and replay-free: a snapshot can be
// produced from the live capture and then diffed in a unit test with two literal
// structs. That is deliberate - the regression report is only trustworthy if its
// arithmetic can be exercised without a driver.
//
// Verdicts are *directional*, not merely "different": a longer event or a larger
// resource is a regression, a shorter/smaller one is an improvement. Thresholds
// are policy (RegressionOptions::significantPercent), so a 0.2% timing wobble is
// reported as Unchanged while a 30% jump is called out. When GPU timings are
// unavailable the timing rows carry Verdict::Unknown rather than a fabricated
// zero, and the limitation string says why.

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <stdint.h>

// Pulls in the P2-1 vocabulary (NormalizationOptions, EventCanonicalKind,
// CanonicalCapture, CaptureMatchPolicy) and, through it, MCPAnalysisModel.h.
#include "MCPCaptureMatch.h"

namespace MCP
{
//
// ---- Snapshot (the cache unit) --------------------------------------------
//

// One event as it appears in a snapshot. `key` is the P2-1 canonical key (the
// cross-capture identity), `eventId` the id *within this capture* - kept only so
// a follow-up query can be scoped to the right event, never used for matching.
struct RegressionEventEntry
{
  uint32_t eventId = 0;
  QString key;
  QString label;      // the canonical label, "Marker > Draw#"
  EventCanonicalKind kind = EventCanonicalKind::Other;
  QString passKey;    // canonical key of the enclosing pass, empty for a root
  double durationUs = 0.0;
};

// One resource. `index` is the position in the capture's resource list and is
// the back-pointer the matcher returns.
struct RegressionResourceEntry
{
  QString key;
  QString displayName;
  QString type;
  uint64_t byteSize = 0;
  int index = -1;
  Confidence confidence = Confidence::Confirmed;
};

// Named ...Entry (not ...Row) because RegressionPassRow below is the *diff* row;
// the snapshot group is deliberately "entries" so the two never collide.
struct RegressionPassEntry
{
  QString key;
  QString id;
  QString displayName;
  uint32_t firstEventId = 0;
  uint32_t lastEventId = 0;
  double durationUs = 0.0;
};

// A shader stage binding. `shader` is the display name (resource name, else the
// entry point) and empty when the stage is unbound at the sampled event.
struct RegressionShaderBindingEntry
{
  QString stage;
  QString shader;
  QString entryPoint;
};

// The normalized projection of one capture. This is what the cache stores and
// what the diff consumes; it holds no ResourceIds, no ActionDescriptions and no
// replay handles, so it stays valid after the capture is closed.
struct RegressionSnapshot
{
  int formatVersion = 1;

  QString unifyKey;        // UnifyCaptureKey(): api + capture stem [+ frame]
  QString capturePath;
  QString api;
  QString localRenderer;
  uint32_t frameNumber = 0;
  QString capturedAt;      // ISO 8601 UTC, supplied by the caller

  bool timingAvailable = false;
  uint32_t timedEventCount = 0;
  double totalDurationUs = 0.0;

  uint64_t totalResourceBytes = 0;
  QString driverName;
  QString driverVersion;

  NormalizationOptions options;

  QVector<RegressionEventEntry> events;      // pre-order, so the matcher is deterministic
  QVector<RegressionResourceEntry> resources;
  QVector<RegressionPassEntry> passes;
  QVector<RegressionShaderBindingEntry> shaders;

  Confidence confidence = Confidence::Confirmed;
  QString limitation;
};

// Build the event/resource/pass tables from the P2-1 canonical capture. Timings,
// pass durations, shader bindings and driver labels are not part of a
// CanonicalCapture (they come from replay), so they are applied afterwards.
RegressionSnapshot BuildRegressionSnapshot(const CanonicalCapture &canonical);

// Copy GPU durations onto the event rows by event id. Events with no sample keep
// 0 and the caller is expected to reflect that in `timingAvailable`.
void ApplyEventTimings(RegressionSnapshot &snapshot, const QHash<uint32_t, double> &timings);

// Copy pass durations (the canonical pass rows only carry the event range).
void ApplyPassDurations(RegressionSnapshot &snapshot, const QVector<PassInfo> &passes);

void ApplyShaderBindings(RegressionSnapshot &snapshot,
                         const QVector<RegressionShaderBindingEntry> &bindings);

// Recompute totalDurationUs / timedEventCount / totalResourceBytes from the rows.
void RecomputeSnapshotTotals(RegressionSnapshot &snapshot);

// Extract the shader-stage bindings from a PipelineStateToJson() object, using
// the same name resolution as the P1-5 diff (resource display name, else entry
// point). Stages absent from the state are not emitted.
QVector<RegressionShaderBindingEntry> ShaderBindingsFromPipelineJson(const QJsonObject &pipeline);

QJsonObject RegressionSnapshotToJson(const RegressionSnapshot &snapshot);
bool RegressionSnapshotFromJson(const QJsonObject &json, RegressionSnapshot &out,
                                QString *error = NULL);

// Compact prose summary for an AI client ("capture X, api Y, N events, ...").
QString RegressionSnapshotToText(const RegressionSnapshot &snapshot);

//
// ---- Snapshot cache --------------------------------------------------------
//

// A small, versioned bag of snapshots keyed by unifyKey. Intentionally not a
// file: the caller supplies the bytes, so the same structure backs an in-memory
// cache, a sidecar file and a unit test.
struct RegressionSnapshotCache
{
  int formatVersion = 1;
  QVector<RegressionSnapshot> snapshots;

  // -1 when absent.
  int FindByUnifyKey(const QString &unifyKey) const;

  // Insert or replace by unifyKey. When `maxEntries` is > 0 the oldest entries
  // beyond the cap are dropped (newest first is not tracked, so `snapshots` is
  // treated as an LRU-by-insertion list maintained by the caller).
  void Upsert(const RegressionSnapshot &snapshot, int maxEntries = 0);
};

QJsonObject RegressionSnapshotCacheToJson(const RegressionSnapshotCache &cache);
bool RegressionSnapshotCacheFromJson(const QJsonObject &json, RegressionSnapshotCache &out,
                                     QString *error = NULL);

// Serialise/parse the whole cache as a document. Parsing is defensive: a bad
// version or a malformed entry is skipped (with `error` set) rather than
// failing the whole read, so a partially written cache never blocks a diff.
QByteArray SerializeRegressionCache(const RegressionSnapshotCache &cache);
bool ParseRegressionCache(const QByteArray &json, RegressionSnapshotCache &out,
                          QString *error = NULL);

//
// ---- Diff -----------------------------------------------------------------
//

// Direction of a change. `Added`/`Removed` are one-sided (present in the
// candidate only / baseline only); `Changed` is a categorical swap that has no
// direction (a shader binding).
enum class RegressionVerdict
{
  Improved,
  Regressed,
  Unchanged,
  Added,
  Removed,
  Changed,
  Unknown,   // could not be decided (timings unavailable, unmatchable)
};

QString RegressionVerdictName(RegressionVerdict verdict);

struct RegressionOptions
{
  // Reuse the P2-1 policy verbatim: the diff must not silently use a different
  // notion of identity than the matcher the report describes.
  CaptureMatchPolicy match;

  // |delta%| at or above this is Improved/Regressed, below it is Unchanged.
  // Ignored for one-sided entries, which are always Added/Removed.
  double significantPercent = 5.0;

  // Emit Unchanged rows too (default false: a regression report is about change).
  bool includeUnchanged = false;

  // Per-section cap applied after sorting (0 = no cap).
  int limit = 0;

  QString baselineLabel;    // display label for side A, e.g. the capture name
  QString candidateLabel;   // display label for side B

  QString Describe() const;
  QJsonObject ToJson() const;
};

struct RegressionTimingRow
{
  QString key;
  QString displayA;
  QString displayB;
  EventCanonicalKind kind = EventCanonicalKind::Other;
  uint32_t eventIdA = 0;
  uint32_t eventIdB = 0;
  double baselineUs = 0.0;
  double candidateUs = 0.0;
  double deltaUs = 0.0;
  double deltaPercent = 0.0;   // 0 when the baseline sample was 0
  RegressionVerdict verdict = RegressionVerdict::Unknown;
  CaptureMatchConfidence matchConfidence = CaptureMatchConfidence::None;
  QVector<EvidenceRef> evidence;
};

struct RegressionMemoryRow
{
  QString key;
  QString displayA;
  QString displayB;
  QString type;
  uint64_t baselineBytes = 0;
  uint64_t candidateBytes = 0;
  int64_t deltaBytes = 0;
  double deltaPercent = 0.0;
  RegressionVerdict verdict = RegressionVerdict::Unknown;
  CaptureMatchConfidence matchConfidence = CaptureMatchConfidence::None;
  Confidence confidence = Confidence::Confirmed;
  QVector<EvidenceRef> evidence;
};

struct RegressionPassRow
{
  QString key;
  QString displayA;
  QString displayB;
  uint32_t firstEventIdA = 0;
  uint32_t lastEventIdA = 0;
  uint32_t firstEventIdB = 0;
  uint32_t lastEventIdB = 0;
  double baselineUs = 0.0;
  double candidateUs = 0.0;
  double deltaUs = 0.0;
  double deltaPercent = 0.0;
  RegressionVerdict verdict = RegressionVerdict::Unknown;
  CaptureMatchConfidence matchConfidence = CaptureMatchConfidence::None;
  QVector<EvidenceRef> evidence;
};

struct RegressionShaderRow
{
  QString stage;
  QString baseline;    // display name, empty when unbound
  QString candidate;
  RegressionVerdict verdict = RegressionVerdict::Unknown;   // Unchanged/Added/Removed/Changed
  QString note;
  QVector<EvidenceRef> evidence;
};

// Counters for the report header, so a client can triage without walking rows.
struct RegressionSummary
{
  int regressedEvents = 0;
  int improvedEvents = 0;
  int unchangedEvents = 0;
  int addedEvents = 0;
  int removedEvents = 0;

  int regressedPasses = 0;
  int improvedPasses = 0;

  int addedResources = 0;
  int removedResources = 0;
  int regressedResources = 0;
  int improvedResources = 0;

  int addedShaders = 0;
  int removedShaders = 0;
  int changedShaders = 0;

  double totalBaselineUs = 0.0;
  double totalCandidateUs = 0.0;
  double totalDeltaUs = 0.0;
  double totalDeltaPercent = 0.0;

  uint64_t totalBaselineBytes = 0;
  uint64_t totalCandidateBytes = 0;
  int64_t totalDeltaBytes = 0;
};

struct RegressionDiffResult
{
  bool available = true;
  int formatVersion = 1;

  QString baselineUnifyKey;
  QString candidateUnifyKey;
  QString baselinePath;
  QString candidatePath;
  QString baselineLabel;
  QString candidateLabel;
  QString api;

  bool apiMatches = true;
  bool timingAvailable = false;   // both sides carried GPU timings
  bool sameDriver = true;
  QString driverChange;           // "nvidia 5xx -> nvidia 6xx", empty when none

  QVector<RegressionTimingRow> timing;
  QVector<RegressionMemoryRow> memory;
  QVector<RegressionPassRow> passes;
  QVector<RegressionShaderRow> shaders;

  RegressionSummary summary;
  QVector<EvidenceRef> evidence;   // report-level anchors (both captures)
  QStringList limitations;
  Confidence confidence = Confidence::Confirmed;

  RegressionOptions options;
};

// Pair two snapshots and compute every delta. Pure: no capture, no replay, no
// filesystem. Rows are sorted hottest/worst first, then truncated to
// options.limit, and Unchanged rows are dropped unless asked for.
RegressionDiffResult DiffRegressionSnapshots(const RegressionSnapshot &baseline,
                                             const RegressionSnapshot &candidate,
                                             const RegressionOptions &options = RegressionOptions());

QJsonObject RegressionTimingRowToJson(const RegressionTimingRow &row);
QJsonObject RegressionMemoryRowToJson(const RegressionMemoryRow &row);
QJsonObject RegressionPassRowToJson(const RegressionPassRow &row);
QJsonObject RegressionShaderRowToJson(const RegressionShaderRow &row);
QJsonObject RegressionSummaryToJson(const RegressionSummary &summary);
QJsonObject RegressionDiffToJson(const RegressionDiffResult &result);

// Render as Markdown with the plugin's section vocabulary ("## Summary",
// "## Timing regressions", "## Resource memory", "## Pass timings",
// "## Shader bindings", "## Limitations").
QString RegressionDiffToMarkdown(const RegressionDiffResult &result);

// True when the diff found anything a reviewer should look at (a regression, an
// add/remove or a shader swap). Unchanged-only diffs return false so a caller can
// skip writing a report.
bool RegressionDiffHasChanges(const RegressionDiffResult &result);

}    // namespace MCP
