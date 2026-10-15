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

#include "MCPRegressionDiff.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <algorithm>
#include <limits.h>

namespace MCP
{
//
// ---- Small helpers --------------------------------------------------------
//

// uint64 counts (bytes) do not survive a JSON double round-trip above 2^53, so
// they are written as decimal strings and read back through this helper, which
// also accepts a plain number for hand-written caches.
static uint64_t JsonU64(const QJsonValue &v)
{
  if(v.isString())
    return (uint64_t)v.toString().toULongLong();
  if(v.isDouble())
    return (uint64_t)v.toDouble();
  return 0;
}

static QString JsonU64String(uint64_t v)
{
  return QString::number((qulonglong)v, 10);
}

static QString FormatUs(double us)
{
  return QString::number(us, 'f', 2);
}

// Compact human-readable byte count, used only for prose (the JSON keeps the
// exact integer).
static QString FormatBytes(uint64_t bytes)
{
  const double kb = 1024.0;
  if(bytes < (uint64_t)kb)
    return QStringLiteral("%1 B").arg(QString::number((qulonglong)bytes));

  const double kib = (double)bytes / kb;
  if(kib < kb)
    return QStringLiteral("%1 KiB").arg(QString::number(kib, 'f', 1));

  const double mib = kib / kb;
  if(mib < kb)
    return QStringLiteral("%1 MiB").arg(QString::number(mib, 'f', 2));

  return QStringLiteral("%1 GiB").arg(QString::number(mib / kb, 'f', 2));
}

static QString PercentText(double pct)
{
  const QChar sign = pct > 0.0 ? QLatin1Char('+') : QLatin1Char('-');
  return QStringLiteral("%1%2%").arg(sign).arg(QString::number(qAbs(pct), 'f', 2));
}

// A deliberately ordinal ranking for "worst first": a regression outranks a
// new entry, which outranks a removal, a swap, an improvement, an unknown and
// finally an unchanged row.
static int VerdictRank(RegressionVerdict v)
{
  switch(v)
  {
    case RegressionVerdict::Regressed: return 0;
    case RegressionVerdict::Added: return 1;
    case RegressionVerdict::Removed: return 2;
    case RegressionVerdict::Changed: return 3;
    case RegressionVerdict::Improved: return 4;
    case RegressionVerdict::Unknown: return 5;
    case RegressionVerdict::Unchanged: break;
  }
  return 6;
}

// Directional verdict for a measurable quantity (a delta where a positive value
// means "worse"). `measurable` false means there was no sample to compare.
static RegressionVerdict VerdictForDelta(double delta, double deltaPercent, double significantPercent,
                                         bool measurable)
{
  if(!measurable)
    return RegressionVerdict::Unknown;
  if(qAbs(deltaPercent) < significantPercent)
    return RegressionVerdict::Unchanged;
  return delta > 0.0 ? RegressionVerdict::Regressed : RegressionVerdict::Improved;
}

// Memory is unsigned and can legitimately start at zero, so the percent rule
// cannot be applied blindly: growing from nothing is a regression and freeing
// everything is an improvement, even though the percent is undefined.
static RegressionVerdict VerdictForBytes(uint64_t baselineBytes, uint64_t candidateBytes,
                                         double deltaPercent, double significantPercent)
{
  if(baselineBytes == candidateBytes)
    return RegressionVerdict::Unchanged;
  if(baselineBytes == 0)
    return candidateBytes > 0 ? RegressionVerdict::Regressed : RegressionVerdict::Unchanged;
  if(candidateBytes == 0)
    return RegressionVerdict::Improved;
  if(qAbs(deltaPercent) < significantPercent)
    return RegressionVerdict::Unchanged;
  return candidateBytes > baselineBytes ? RegressionVerdict::Regressed : RegressionVerdict::Improved;
}

static double DeltaPercentInt64(int64_t delta, uint64_t baseline)
{
  if(baseline == 0)
    return 0.0;
  return (double)delta / (double)baseline * 100.0;
}

// A pass range containing an event id, preferring the tightest range when the
// pass list overlaps (a nested marker group and its parent both "contain" the
// event).
static QString PassKeyForEvent(const QVector<RegressionPassEntry> &passes, uint32_t eventId)
{
  QString best;
  uint32_t bestWidth = 0;

  for(int i = 0; i < passes.size(); i++)
  {
    const RegressionPassEntry &p = passes[i];
    if(eventId < p.firstEventId || eventId > p.lastEventId)
      continue;

    const uint32_t width = p.lastEventId - p.firstEventId;
    if(best.isEmpty() || width < bestWidth)
    {
      best = p.key;
      bestWidth = width;
    }
  }

  return best;
}

//
// ---- Snapshot (the cache unit) --------------------------------------------
//

RegressionSnapshot BuildRegressionSnapshot(const CanonicalCapture &canonical)
{
  RegressionSnapshot snap;
  snap.unifyKey = canonical.unifyKey;
  snap.capturePath = canonical.capturePath;
  snap.api = canonical.api;
  snap.frameNumber = canonical.frameNumber;
  snap.options = canonical.options;
  snap.confidence = canonical.confidence;
  snap.limitation = canonical.limitation;

  // Passes first: the event rows carry the key of their enclosing pass, which is
  // resolved by event range and therefore needs the pass table to exist.
  snap.passes.reserve(canonical.passes.size());
  for(const PassCanonicalKey &p : canonical.passes)
  {
    RegressionPassEntry e;
    e.key = p.key;
    e.id = p.id;
    e.displayName = p.displayName;
    e.firstEventId = p.firstEventId;
    e.lastEventId = p.lastEventId;
    e.durationUs = 0.0;
    snap.passes.push_back(e);
  }

  // The canonical event table is a hash; sort by event id to recover the
  // pre-order the matcher expects (event ids ascend with the tree walk, which is
  // what makes the nearest-position tie-break meaningful).
  QVector<uint32_t> ids;
  ids.reserve(canonical.events.size());
  for(auto it = canonical.events.constBegin(); it != canonical.events.constEnd(); ++it)
    ids.push_back(it.key());
  std::sort(ids.begin(), ids.end());

  snap.events.reserve(ids.size());
  for(int i = 0; i < ids.size(); i++)
  {
    const EventCanonicalKey ck = canonical.events.value(ids[i]);

    RegressionEventEntry e;
    e.eventId = ids[i];
    e.key = ck.key;
    e.label = ck.label.isEmpty() ? ck.rawName : ck.label;
    e.kind = ck.kind;
    e.passKey = PassKeyForEvent(snap.passes, ids[i]);
    e.durationUs = 0.0;
    snap.events.push_back(e);
  }

  snap.resources.reserve(canonical.resources.size());
  for(int i = 0; i < canonical.resources.size(); i++)
  {
    const ResourceCanonicalKey &r = canonical.resources[i];

    RegressionResourceEntry e;
    e.key = r.key;
    e.displayName = r.displayName;
    e.type = r.type;
    e.byteSize = r.byteSize;
    e.index = r.index >= 0 ? r.index : i;
    e.confidence = r.confidence;
    snap.resources.push_back(e);
  }

  RecomputeSnapshotTotals(snap);
  return snap;
}

void ApplyEventTimings(RegressionSnapshot &snapshot, const QHash<uint32_t, double> &timings)
{
  for(int i = 0; i < snapshot.events.size(); i++)
    snapshot.events[i].durationUs = timings.value(snapshot.events[i].eventId, 0.0);

  RecomputeSnapshotTotals(snapshot);
}

void ApplyPassDurations(RegressionSnapshot &snapshot, const QVector<PassInfo> &passes)
{
  // Match on the pass id (the "<name>@<start>-<end>" identity built by the
  // pass-graph builder), falling back to position for an id-less pass list.
  QHash<QString, const PassInfo *> byId;
  for(int i = 0; i < passes.size(); i++)
  {
    if(!passes[i].id.isEmpty())
      byId.insert(passes[i].id, &passes[i]);
  }

  for(int i = 0; i < snapshot.passes.size(); i++)
  {
    RegressionPassEntry &e = snapshot.passes[i];

    const PassInfo *p = NULL;
    if(!e.id.isEmpty())
      p = byId.value(e.id, NULL);
    if(p == NULL && i < passes.size() && passes[i].id == e.id)
      p = &passes[i];
    if(p == NULL)
      continue;

    e.durationUs = p->durationUs;
    if(p->eventIdStart != 0 || p->eventIdEnd != 0)
    {
      e.firstEventId = p->eventIdStart;
      e.lastEventId = p->eventIdEnd;
    }
    if(!p->name.isEmpty())
      e.displayName = p->name;
  }
}

void ApplyShaderBindings(RegressionSnapshot &snapshot,
                         const QVector<RegressionShaderBindingEntry> &bindings)
{
  snapshot.shaders = bindings;
}

void RecomputeSnapshotTotals(RegressionSnapshot &snapshot)
{
  double total = 0.0;
  uint32_t timed = 0;

  for(int i = 0; i < snapshot.events.size(); i++)
  {
    const double d = snapshot.events[i].durationUs;
    if(d > 0.0)
    {
      total += d;
      timed++;
    }
  }

  snapshot.totalDurationUs = total;
  snapshot.timedEventCount = timed;

  uint64_t bytes = 0;
  for(int i = 0; i < snapshot.resources.size(); i++)
    bytes += snapshot.resources[i].byteSize;
  snapshot.totalResourceBytes = bytes;
}

QVector<RegressionShaderBindingEntry> ShaderBindingsFromPipelineJson(const QJsonObject &pipeline)
{
  QVector<RegressionShaderBindingEntry> out;

  if(!pipeline.value(QStringLiteral("loaded")).toBool(true))
    return out;

  const QJsonObject shaders = pipeline.value(QStringLiteral("shaders")).toObject();

  // QJsonObject iterates its keys in sorted order, so this is deterministic
  // without inventing a stage-order table (and without guessing at the stage
  // token spelling each API uses).
  for(auto it = shaders.constBegin(); it != shaders.constEnd(); ++it)
  {
    const QJsonObject s = it.value().toObject();
    if(s.isEmpty())
      continue;

    RegressionShaderBindingEntry e;
    e.stage = it.key();

    // Same resolution as the P1-5 pipeline diff: the bound resource's display
    // name wins, the entry point is the fallback, a null ResourceId is ignored.
    const QJsonObject shader = s.value(QStringLiteral("shader")).toObject();
    const QString name = shader.value(QStringLiteral("name")).toString();
    if(!name.isEmpty() && name != QStringLiteral("ResourceId::0"))
      e.shader = name;
    else
      e.shader = s.value(QStringLiteral("entryPoint")).toString();

    e.entryPoint = s.value(QStringLiteral("entryPoint")).toString();

    out.push_back(e);
  }

  return out;
}

QJsonObject RegressionSnapshotToJson(const RegressionSnapshot &snapshot)
{
  QJsonObject j;
  j[QStringLiteral("formatVersion")] = snapshot.formatVersion;
  j[QStringLiteral("unifyKey")] = snapshot.unifyKey;
  j[QStringLiteral("capturePath")] = snapshot.capturePath;
  j[QStringLiteral("api")] = snapshot.api;
  if(!snapshot.localRenderer.isEmpty())
    j[QStringLiteral("localRenderer")] = snapshot.localRenderer;
  j[QStringLiteral("frameNumber")] = (int)snapshot.frameNumber;
  if(!snapshot.capturedAt.isEmpty())
    j[QStringLiteral("capturedAt")] = snapshot.capturedAt;

  j[QStringLiteral("timingAvailable")] = snapshot.timingAvailable;
  j[QStringLiteral("timedEventCount")] = (int)snapshot.timedEventCount;
  j[QStringLiteral("totalDurationUs")] = snapshot.totalDurationUs;
  j[QStringLiteral("totalResourceBytes")] = JsonU64String(snapshot.totalResourceBytes);

  if(!snapshot.driverName.isEmpty())
    j[QStringLiteral("driverName")] = snapshot.driverName;
  if(!snapshot.driverVersion.isEmpty())
    j[QStringLiteral("driverVersion")] = snapshot.driverVersion;

  j[QStringLiteral("normalization")] = NormalizationOptionsToJson(snapshot.options);

  QJsonArray events;
  for(const RegressionEventEntry &e : snapshot.events)
  {
    QJsonObject o;
    o[QStringLiteral("eventId")] = (int)e.eventId;
    o[QStringLiteral("key")] = e.key;
    o[QStringLiteral("label")] = e.label;
    o[QStringLiteral("kind")] = EventCanonicalKindName(e.kind);
    if(!e.passKey.isEmpty())
      o[QStringLiteral("passKey")] = e.passKey;
    o[QStringLiteral("durationUs")] = e.durationUs;
    events.append(o);
  }
  j[QStringLiteral("events")] = events;

  QJsonArray resources;
  for(const RegressionResourceEntry &e : snapshot.resources)
  {
    QJsonObject o;
    o[QStringLiteral("key")] = e.key;
    o[QStringLiteral("displayName")] = e.displayName;
    o[QStringLiteral("type")] = e.type;
    o[QStringLiteral("byteSize")] = JsonU64String(e.byteSize);
    o[QStringLiteral("index")] = e.index;
    o[QStringLiteral("confidence")] = ConfidenceName(e.confidence);
    resources.append(o);
  }
  j[QStringLiteral("resources")] = resources;

  QJsonArray passes;
  for(const RegressionPassEntry &e : snapshot.passes)
  {
    QJsonObject o;
    o[QStringLiteral("key")] = e.key;
    if(!e.id.isEmpty())
      o[QStringLiteral("id")] = e.id;
    o[QStringLiteral("displayName")] = e.displayName;
    o[QStringLiteral("firstEventId")] = (int)e.firstEventId;
    o[QStringLiteral("lastEventId")] = (int)e.lastEventId;
    o[QStringLiteral("durationUs")] = e.durationUs;
    passes.append(o);
  }
  j[QStringLiteral("passes")] = passes;

  QJsonArray shaders;
  for(const RegressionShaderBindingEntry &e : snapshot.shaders)
  {
    QJsonObject o;
    o[QStringLiteral("stage")] = e.stage;
    o[QStringLiteral("shader")] = e.shader;
    if(!e.entryPoint.isEmpty())
      o[QStringLiteral("entryPoint")] = e.entryPoint;
    shaders.append(o);
  }
  j[QStringLiteral("shaders")] = shaders;

  j[QStringLiteral("confidence")] = ConfidenceName(snapshot.confidence);
  if(!snapshot.limitation.isEmpty())
    j[QStringLiteral("limitation")] = snapshot.limitation;

  return j;
}

// Hand-parsed because MCPCaptureMatch.h only exposes the options in one
// direction (ToJson); a cache must be readable as well as writable.
static NormalizationOptions NormalizationOptionsFromJson(const QJsonObject &j)
{
  NormalizationOptions o;
  if(j.isEmpty())
    return o;

  if(j.contains(QStringLiteral("caseInsensitive")))
    o.caseInsensitive = j.value(QStringLiteral("caseInsensitive")).toBool(true);
  if(j.contains(QStringLiteral("collapseWhitespace")))
    o.collapseWhitespace = j.value(QStringLiteral("collapseWhitespace")).toBool(true);
  if(j.contains(QStringLiteral("stripExtension")))
    o.stripExtension = j.value(QStringLiteral("stripExtension")).toBool(true);
  if(j.contains(QStringLiteral("stripPath")))
    o.stripPath = j.value(QStringLiteral("stripPath")).toBool(true);
  if(j.contains(QStringLiteral("stripInstanceSuffix")))
    o.stripInstanceSuffix = j.value(QStringLiteral("stripInstanceSuffix")).toBool(true);
  if(j.contains(QStringLiteral("stripResolutionSuffix")))
    o.stripResolutionSuffix = j.value(QStringLiteral("stripResolutionSuffix")).toBool(true);
  if(j.contains(QStringLiteral("stripNumericSuffix")))
    o.stripNumericSuffix = j.value(QStringLiteral("stripNumericSuffix")).toBool(true);
  if(j.contains(QStringLiteral("replaceEmptyWithAnon")))
    o.replaceEmptyWithAnon = j.value(QStringLiteral("replaceEmptyWithAnon")).toBool(true);
  if(j.contains(QStringLiteral("contextPrefixSegments")))
    o.contextPrefixSegments = j.value(QStringLiteral("contextPrefixSegments")).toInt(2);

  return o;
}

static Confidence ConfidenceFromName(const QString &name, Confidence dflt = Confidence::Confirmed)
{
  if(name == QStringLiteral("confirmed"))
    return Confidence::Confirmed;
  if(name == QStringLiteral("inferred"))
    return Confidence::Inferred;
  if(name == QStringLiteral("toVerify"))
    return Confidence::ToVerify;
  if(name == QStringLiteral("low"))
    return Confidence::Low;
  return dflt;
}

static EventCanonicalKind EventKindFromName(const QString &name)
{
  if(name == QStringLiteral("realEffect"))
    return EventCanonicalKind::RealEffect;
  if(name == QStringLiteral("marker"))
    return EventCanonicalKind::Marker;
  return EventCanonicalKind::Other;
}

bool RegressionSnapshotFromJson(const QJsonObject &json, RegressionSnapshot &out, QString *error)
{
  if(json.isEmpty())
  {
    if(error != NULL)
      *error = QStringLiteral("snapshot is not an object");
    return false;
  }

  const int version = json.value(QStringLiteral("formatVersion")).toInt(1);
  if(version > 1)
  {
    if(error != NULL)
      *error = QStringLiteral("snapshot format version %1 is newer than this build understands")
                   .arg(version);
    return false;
  }

  RegressionSnapshot snap;
  snap.formatVersion = version;
  snap.unifyKey = json.value(QStringLiteral("unifyKey")).toString();
  snap.capturePath = json.value(QStringLiteral("capturePath")).toString();
  snap.api = json.value(QStringLiteral("api")).toString();
  snap.localRenderer = json.value(QStringLiteral("localRenderer")).toString();
  snap.frameNumber = (uint32_t)json.value(QStringLiteral("frameNumber")).toInt(0);
  snap.capturedAt = json.value(QStringLiteral("capturedAt")).toString();

  snap.timingAvailable = json.value(QStringLiteral("timingAvailable")).toBool(false);
  snap.timedEventCount = (uint32_t)json.value(QStringLiteral("timedEventCount")).toInt(0);
  snap.totalDurationUs = json.value(QStringLiteral("totalDurationUs")).toDouble(0.0);
  snap.totalResourceBytes = JsonU64(json.value(QStringLiteral("totalResourceBytes")));

  snap.driverName = json.value(QStringLiteral("driverName")).toString();
  snap.driverVersion = json.value(QStringLiteral("driverVersion")).toString();

  snap.options = NormalizationOptionsFromJson(json.value(QStringLiteral("normalization")).toObject());

  const QJsonArray events = json.value(QStringLiteral("events")).toArray();
  snap.events.reserve(events.size());
  for(const QJsonValue &v : events)
  {
    const QJsonObject o = v.toObject();
    RegressionEventEntry e;
    e.eventId = (uint32_t)o.value(QStringLiteral("eventId")).toInt(0);
    e.key = o.value(QStringLiteral("key")).toString();
    e.label = o.value(QStringLiteral("label")).toString();
    e.kind = EventKindFromName(o.value(QStringLiteral("kind")).toString());
    e.passKey = o.value(QStringLiteral("passKey")).toString();
    e.durationUs = o.value(QStringLiteral("durationUs")).toDouble(0.0);
    snap.events.push_back(e);
  }

  const QJsonArray resources = json.value(QStringLiteral("resources")).toArray();
  snap.resources.reserve(resources.size());
  for(const QJsonValue &v : resources)
  {
    const QJsonObject o = v.toObject();
    RegressionResourceEntry e;
    e.key = o.value(QStringLiteral("key")).toString();
    e.displayName = o.value(QStringLiteral("displayName")).toString();
    e.type = o.value(QStringLiteral("type")).toString();
    e.byteSize = JsonU64(o.value(QStringLiteral("byteSize")));
    e.index = o.value(QStringLiteral("index")).toInt(-1);
    e.confidence = ConfidenceFromName(o.value(QStringLiteral("confidence")).toString());
    snap.resources.push_back(e);
  }

  const QJsonArray passes = json.value(QStringLiteral("passes")).toArray();
  snap.passes.reserve(passes.size());
  for(const QJsonValue &v : passes)
  {
    const QJsonObject o = v.toObject();
    RegressionPassEntry e;
    e.key = o.value(QStringLiteral("key")).toString();
    e.id = o.value(QStringLiteral("id")).toString();
    e.displayName = o.value(QStringLiteral("displayName")).toString();
    e.firstEventId = (uint32_t)o.value(QStringLiteral("firstEventId")).toInt(0);
    e.lastEventId = (uint32_t)o.value(QStringLiteral("lastEventId")).toInt(0);
    e.durationUs = o.value(QStringLiteral("durationUs")).toDouble(0.0);
    snap.passes.push_back(e);
  }

  const QJsonArray shaders = json.value(QStringLiteral("shaders")).toArray();
  snap.shaders.reserve(shaders.size());
  for(const QJsonValue &v : shaders)
  {
    const QJsonObject o = v.toObject();
    RegressionShaderBindingEntry e;
    e.stage = o.value(QStringLiteral("stage")).toString();
    e.shader = o.value(QStringLiteral("shader")).toString();
    e.entryPoint = o.value(QStringLiteral("entryPoint")).toString();
    snap.shaders.push_back(e);
  }

  snap.confidence = ConfidenceFromName(json.value(QStringLiteral("confidence")).toString());
  snap.limitation = json.value(QStringLiteral("limitation")).toString();

  out = snap;
  return true;
}

QString RegressionSnapshotToText(const RegressionSnapshot &snapshot)
{
  QStringList lines;
  lines << QStringLiteral("Snapshot %1")
               .arg(snapshot.unifyKey.isEmpty() ? QStringLiteral("(no key)") : snapshot.unifyKey);
  lines << QStringLiteral("- Capture: %1")
               .arg(snapshot.capturePath.isEmpty() ? QStringLiteral("(unknown)")
                                                   : snapshot.capturePath);
  lines << QStringLiteral("- API: %1").arg(
      snapshot.api.isEmpty() ? QStringLiteral("(unknown)") : snapshot.api);
  if(snapshot.frameNumber != 0)
    lines << QStringLiteral("- Frame: %1").arg(snapshot.frameNumber);
  if(!snapshot.capturedAt.isEmpty())
    lines << QStringLiteral("- Captured: %1").arg(snapshot.capturedAt);
  lines << QStringLiteral("- Events: %1 (%2 timed, %3 us total)")
               .arg(snapshot.events.size())
               .arg(snapshot.timedEventCount)
               .arg(FormatUs(snapshot.totalDurationUs));
  lines << QStringLiteral("- Resources: %1 (%2)")
               .arg(snapshot.resources.size())
               .arg(FormatBytes(snapshot.totalResourceBytes));
  lines << QStringLiteral("- Passes: %1").arg(snapshot.passes.size());
  lines << QStringLiteral("- Shader bindings: %1").arg(snapshot.shaders.size());
  lines << QStringLiteral("- GPU timings: %1")
               .arg(snapshot.timingAvailable ? QStringLiteral("available")
                                             : QStringLiteral("unavailable"));
  if(!snapshot.driverName.isEmpty())
    lines << QStringLiteral("- Driver: %1 %2")
                 .arg(snapshot.driverName)
                 .arg(snapshot.driverVersion);
  lines << QStringLiteral("- Confidence: %1").arg(ConfidenceName(snapshot.confidence));
  if(!snapshot.limitation.isEmpty())
    lines << QStringLiteral("- Limitation: %1").arg(snapshot.limitation);

  return lines.join(QString::fromUtf8("\n"));
}

//
// ---- Snapshot cache --------------------------------------------------------
//

int RegressionSnapshotCache::FindByUnifyKey(const QString &unifyKey) const
{
  if(unifyKey.isEmpty())
    return -1;

  for(int i = 0; i < snapshots.size(); i++)
  {
    if(snapshots[i].unifyKey == unifyKey)
      return i;
  }

  return -1;
}

void RegressionSnapshotCache::Upsert(const RegressionSnapshot &snapshot, int maxEntries)
{
  const int existing = FindByUnifyKey(snapshot.unifyKey);
  if(existing >= 0)
  {
    snapshots[existing] = snapshot;
  }
  else
  {
    snapshots.push_back(snapshot);
  }

  // Newest-first eviction: the list is append-ordered, so dropping from the
  // front discards the least recently inserted entry.
  if(maxEntries > 0 && snapshots.size() > maxEntries)
  {
    const int excess = snapshots.size() - maxEntries;
    snapshots.remove(0, excess);
  }
}

QJsonObject RegressionSnapshotCacheToJson(const RegressionSnapshotCache &cache)
{
  QJsonObject j;
  j[QStringLiteral("formatVersion")] = cache.formatVersion;

  QJsonArray snapshots;
  for(const RegressionSnapshot &s : cache.snapshots)
    snapshots.append(RegressionSnapshotToJson(s));
  j[QStringLiteral("snapshots")] = snapshots;
  j[QStringLiteral("count")] = snapshots.size();

  return j;
}

bool RegressionSnapshotCacheFromJson(const QJsonObject &json, RegressionSnapshotCache &out,
                                     QString *error)
{
  if(json.isEmpty())
  {
    if(error != NULL)
      *error = QStringLiteral("cache is not an object");
    return false;
  }

  const int version = json.value(QStringLiteral("formatVersion")).toInt(1);
  if(version > 1)
  {
    if(error != NULL)
      *error = QStringLiteral("cache format version %1 is newer than this build understands")
                   .arg(version);
    return false;
  }

  RegressionSnapshotCache cache;
  cache.formatVersion = version;

  // Defensive: one malformed entry is skipped (and reported) rather than
  // discarding the whole cache, so a partially written file still yields a diff.
  QStringList skipped;
  const QJsonArray snapshots = json.value(QStringLiteral("snapshots")).toArray();
  for(int i = 0; i < snapshots.size(); i++)
  {
    RegressionSnapshot snap;
    QString entryError;
    if(RegressionSnapshotFromJson(snapshots[i].toObject(), snap, &entryError))
    {
      cache.snapshots.push_back(snap);
    }
    else
    {
      skipped << QStringLiteral("entry %1: %2").arg(i).arg(entryError);
    }
  }

  if(!skipped.isEmpty() && error != NULL)
    *error = skipped.join(QStringLiteral("; "));

  out = cache;
  return true;
}

QByteArray SerializeRegressionCache(const RegressionSnapshotCache &cache)
{
  return QJsonDocument(RegressionSnapshotCacheToJson(cache)).toJson(QJsonDocument::Indented);
}

bool ParseRegressionCache(const QByteArray &json, RegressionSnapshotCache &out, QString *error)
{
  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
  if(doc.isNull() || !doc.isObject())
  {
    if(error != NULL)
    {
      *error = parseError.error != QJsonParseError::NoError
                   ? parseError.errorString()
                   : QStringLiteral("cache document is not a JSON object");
    }
    return false;
  }

  return RegressionSnapshotCacheFromJson(doc.object(), out, error);
}

//
// ---- Diff -----------------------------------------------------------------
//

QString RegressionVerdictName(RegressionVerdict verdict)
{
  switch(verdict)
  {
    case RegressionVerdict::Improved: return QStringLiteral("improved");
    case RegressionVerdict::Regressed: return QStringLiteral("regressed");
    case RegressionVerdict::Unchanged: return QStringLiteral("unchanged");
    case RegressionVerdict::Added: return QStringLiteral("added");
    case RegressionVerdict::Removed: return QStringLiteral("removed");
    case RegressionVerdict::Changed: return QStringLiteral("changed");
    case RegressionVerdict::Unknown: break;
  }
  return QStringLiteral("unknown");
}

QString RegressionOptions::Describe() const
{
  QStringList out;
  out << QStringLiteral("Matching: %1").arg(match.Describe());
  out << QStringLiteral("Significance threshold: %1%")
             .arg(QString::number(significantPercent, 'f', 2));
  out << QStringLiteral("Include unchanged: %1")
             .arg(includeUnchanged ? QStringLiteral("yes") : QStringLiteral("no"));
  out << QStringLiteral("Per-section limit: %1")
             .arg(limit > 0 ? QString::number(limit) : QStringLiteral("none"));
  if(!baselineLabel.isEmpty() || !candidateLabel.isEmpty())
    out << QStringLiteral("Labels: %1 -> %2")
               .arg(baselineLabel.isEmpty() ? QStringLiteral("baseline") : baselineLabel)
               .arg(candidateLabel.isEmpty() ? QStringLiteral("candidate") : candidateLabel);
  return out.join(QStringLiteral("; "));
}

QJsonObject RegressionOptions::ToJson() const
{
  QJsonObject j;
  j[QStringLiteral("match")] = match.ToJson();
  j[QStringLiteral("significantPercent")] = significantPercent;
  j[QStringLiteral("includeUnchanged")] = includeUnchanged;
  j[QStringLiteral("limit")] = limit;
  if(!baselineLabel.isEmpty())
    j[QStringLiteral("baselineLabel")] = baselineLabel;
  if(!candidateLabel.isEmpty())
    j[QStringLiteral("candidateLabel")] = candidateLabel;
  j[QStringLiteral("description")] = Describe();
  return j;
}

// Truncate a sorted section to options.limit after sorting, so the rows the
// caller keeps are always the worst ones rather than the first-encountered.
template <typename T>
static void TrimSection(QVector<T> &rows, int limit)
{
  if(limit > 0 && rows.size() > limit)
    rows.resize(limit);
}

static QVector<CaptureMatchEntry> EventEntriesFromSnapshot(const RegressionSnapshot &snapshot)
{
  QVector<CaptureMatchEntry> out;
  out.reserve(snapshot.events.size());

  for(int i = 0; i < snapshot.events.size(); i++)
  {
    const RegressionEventEntry &e = snapshot.events[i];

    CaptureMatchEntry m;
    m.key = e.key;
    m.displayName = e.label;
    m.index = i;
    // Event ids ascend with the pre-order walk, so nearest-id is the tie-break
    // that keeps a re-ordered frame paired up.
    m.position = e.eventId;
    out.push_back(m);
  }

  return out;
}

static QVector<CaptureMatchEntry> ResourceEntriesFromSnapshot(const RegressionSnapshot &snapshot)
{
  QVector<CaptureMatchEntry> out;
  out.reserve(snapshot.resources.size());

  for(int i = 0; i < snapshot.resources.size(); i++)
  {
    CaptureMatchEntry m;
    m.key = snapshot.resources[i].key;
    m.displayName = snapshot.resources[i].displayName;
    m.index = i;
    m.position = (uint32_t)i;
    out.push_back(m);
  }

  return out;
}

static QVector<CaptureMatchEntry> PassEntriesFromSnapshot(const RegressionSnapshot &snapshot)
{
  QVector<CaptureMatchEntry> out;
  out.reserve(snapshot.passes.size());

  for(int i = 0; i < snapshot.passes.size(); i++)
  {
    CaptureMatchEntry m;
    m.key = snapshot.passes[i].key;
    m.displayName = snapshot.passes[i].displayName;
    m.index = i;
    m.position = snapshot.passes[i].firstEventId;
    out.push_back(m);
  }

  return out;
}

// A shared template that walks a match result and hands each pair to a lambda,
// so the three sections cannot drift in how they interpret indexA/indexB.
template <typename RowT, typename Fn>
static void BuildRowsFromMatch(const CaptureMatchResult &match, QVector<RowT> &rows, Fn fill)
{
  rows.reserve(match.pairs.size());
  for(const CaptureMatchPair &pair : match.pairs)
  {
    RowT row;
    fill(pair, row);
    rows.push_back(row);
  }
}

RegressionDiffResult DiffRegressionSnapshots(const RegressionSnapshot &baseline,
                                             const RegressionSnapshot &candidate,
                                             const RegressionOptions &options)
{
  RegressionDiffResult result;
  result.options = options;
  result.baselineUnifyKey = baseline.unifyKey;
  result.candidateUnifyKey = candidate.unifyKey;
  result.baselinePath = baseline.capturePath;
  result.candidatePath = candidate.capturePath;
  result.baselineLabel = options.baselineLabel.isEmpty() ? baseline.capturePath
                                                         : options.baselineLabel;
  result.candidateLabel = options.candidateLabel.isEmpty() ? candidate.capturePath
                                                           : options.candidateLabel;
  result.api = !candidate.api.isEmpty() ? candidate.api : baseline.api;

  result.available = !(baseline.events.isEmpty() && baseline.resources.isEmpty() &&
                       baseline.passes.isEmpty() && candidate.events.isEmpty() &&
                       candidate.resources.isEmpty() && candidate.passes.isEmpty());

  result.apiMatches = baseline.api.isEmpty() || candidate.api.isEmpty() ||
                      baseline.api.compare(candidate.api, Qt::CaseInsensitive) == 0;
  result.timingAvailable = baseline.timingAvailable && candidate.timingAvailable;

  const bool driverKnown = !baseline.driverName.isEmpty() || !candidate.driverName.isEmpty();
  result.sameDriver =
      !driverKnown ||
      (baseline.driverName == candidate.driverName &&
       baseline.driverVersion == candidate.driverVersion);
  if(!result.sameDriver)
  {
    result.driverChange = QStringLiteral("%1 %2 -> %3 %4")
                              .arg(baseline.driverName)
                              .arg(baseline.driverVersion)
                              .arg(candidate.driverName)
                              .arg(candidate.driverVersion);
  }

  const bool measurable = result.timingAvailable;

  //
  // ---- Timing events -------------------------------------------------------
  //
  const CaptureMatchResult eventMatch =
      MatchEntries(EventEntriesFromSnapshot(baseline), EventEntriesFromSnapshot(candidate),
                   options.match);

  BuildRowsFromMatch<RegressionTimingRow>(
      eventMatch, result.timing, [&](const CaptureMatchPair &pair, RegressionTimingRow &row) {
        row.key = pair.key;
        row.displayA = pair.displayA;
        row.displayB = pair.displayB;
        row.matchConfidence = pair.confidence;

        if(pair.indexA >= 0)
        {
          row.eventIdA = baseline.events[pair.indexA].eventId;
          row.baselineUs = baseline.events[pair.indexA].durationUs;
          row.kind = baseline.events[pair.indexA].kind;
        }
        if(pair.indexB >= 0)
        {
          row.eventIdB = candidate.events[pair.indexB].eventId;
          row.candidateUs = candidate.events[pair.indexB].durationUs;
          if(row.kind == EventCanonicalKind::Other && pair.indexA < 0)
            row.kind = candidate.events[pair.indexB].kind;
        }

        if(pair.matched)
        {
          row.deltaUs = row.candidateUs - row.baselineUs;
          row.deltaPercent =
              row.baselineUs > 0.0 ? row.deltaUs / row.baselineUs * 100.0 : 0.0;
          row.verdict =
              VerdictForDelta(row.deltaUs, row.deltaPercent, options.significantPercent, measurable);

          row.evidence.push_back(
              TimingEvidence(row.eventIdA, row.baselineUs,
                             QStringLiteral("%1: %2 [%3 us]")
                                 .arg(result.baselineLabel.isEmpty() ? QStringLiteral("baseline")
                                                                     : result.baselineLabel)
                                 .arg(row.key)
                                 .arg(FormatUs(row.baselineUs))));
          row.evidence.push_back(
              TimingEvidence(row.eventIdB, row.candidateUs,
                             QStringLiteral("%1: %2 [%3 us]")
                                 .arg(result.candidateLabel.isEmpty() ? QStringLiteral("candidate")
                                                                      : result.candidateLabel)
                                 .arg(row.key)
                                 .arg(FormatUs(row.candidateUs))));
        }
        else if(pair.indexB >= 0)
        {
          row.verdict = RegressionVerdict::Added;
          row.evidence.push_back(TimingEvidence(
              row.eventIdB, row.candidateUs,
              QStringLiteral("candidate-only event %1").arg(row.key)));
        }
        else
        {
          row.verdict = RegressionVerdict::Removed;
          row.evidence.push_back(
              TimingEvidence(row.eventIdA, row.baselineUs,
                             QStringLiteral("baseline-only event %1").arg(row.key)));
        }
      });

  //
  // ---- Resource memory -----------------------------------------------------
  //
  const CaptureMatchResult resourceMatch = MatchEntries(
      ResourceEntriesFromSnapshot(baseline), ResourceEntriesFromSnapshot(candidate), options.match);

  BuildRowsFromMatch<RegressionMemoryRow>(
      resourceMatch, result.memory, [&](const CaptureMatchPair &pair, RegressionMemoryRow &row) {
        row.key = pair.key;
        row.displayA = pair.displayA;
        row.displayB = pair.displayB;
        row.matchConfidence = pair.confidence;

        if(pair.indexA >= 0)
        {
          row.baselineBytes = baseline.resources[pair.indexA].byteSize;
          row.type = baseline.resources[pair.indexA].type;
          row.confidence = baseline.resources[pair.indexA].confidence;
          row.evidence.push_back(
              ResourceEvidence(ResourceId(), baseline.resources[pair.indexA].displayName));
        }
        if(pair.indexB >= 0)
        {
          row.candidateBytes = candidate.resources[pair.indexB].byteSize;
          if(row.type.isEmpty())
            row.type = candidate.resources[pair.indexB].type;
          if(pair.indexA < 0)
            row.confidence = candidate.resources[pair.indexB].confidence;
          row.evidence.push_back(
              ResourceEvidence(ResourceId(), candidate.resources[pair.indexB].displayName));
        }

        const int64_t delta = (int64_t)row.candidateBytes - (int64_t)row.baselineBytes;
        row.deltaBytes = delta;
        row.deltaPercent = DeltaPercentInt64(delta, row.baselineBytes);

        if(pair.matched)
        {
          row.verdict = VerdictForBytes(row.baselineBytes, row.candidateBytes, row.deltaPercent,
                                        options.significantPercent);
        }
        else if(pair.indexB >= 0)
        {
          row.verdict = RegressionVerdict::Added;
        }
        else
        {
          row.verdict = RegressionVerdict::Removed;
        }
      });

  //
  // ---- Pass timings --------------------------------------------------------
  //
  const CaptureMatchResult passMatch = MatchEntries(
      PassEntriesFromSnapshot(baseline), PassEntriesFromSnapshot(candidate), options.match);

  BuildRowsFromMatch<RegressionPassRow>(
      passMatch, result.passes, [&](const CaptureMatchPair &pair, RegressionPassRow &row) {
        row.key = pair.key;
        row.displayA = pair.displayA;
        row.displayB = pair.displayB;
        row.matchConfidence = pair.confidence;

        if(pair.indexA >= 0)
        {
          row.firstEventIdA = baseline.passes[pair.indexA].firstEventId;
          row.lastEventIdA = baseline.passes[pair.indexA].lastEventId;
          row.baselineUs = baseline.passes[pair.indexA].durationUs;
          row.evidence.push_back(EventEvidence(
              row.firstEventIdA,
              QStringLiteral("%1: pass %2 [EID %3-%4]")
                  .arg(result.baselineLabel.isEmpty() ? QStringLiteral("baseline")
                                                      : result.baselineLabel)
                  .arg(row.displayA)
                  .arg(row.firstEventIdA)
                  .arg(row.lastEventIdA)));
        }
        if(pair.indexB >= 0)
        {
          row.firstEventIdB = candidate.passes[pair.indexB].firstEventId;
          row.lastEventIdB = candidate.passes[pair.indexB].lastEventId;
          row.candidateUs = candidate.passes[pair.indexB].durationUs;
          row.evidence.push_back(EventEvidence(
              row.firstEventIdB,
              QStringLiteral("%1: pass %2 [EID %3-%4]")
                  .arg(result.candidateLabel.isEmpty() ? QStringLiteral("candidate")
                                                       : result.candidateLabel)
                  .arg(row.displayB)
                  .arg(row.firstEventIdB)
                  .arg(row.lastEventIdB)));
        }

        if(pair.matched)
        {
          row.deltaUs = row.candidateUs - row.baselineUs;
          row.deltaPercent = row.baselineUs > 0.0 ? row.deltaUs / row.baselineUs * 100.0 : 0.0;
          row.verdict =
              VerdictForDelta(row.deltaUs, row.deltaPercent, options.significantPercent, measurable);
        }
        else if(pair.indexB >= 0)
        {
          row.verdict = RegressionVerdict::Added;
        }
        else
        {
          row.verdict = RegressionVerdict::Removed;
        }
      });

  //
  // ---- Shader bindings -----------------------------------------------------
  //
  // Shader stages are keyed by their stage token, so a plain union is enough and
  // no canonical matching is needed. The verdict is categorical (a swap has no
  // direction).
  {
    QHash<QString, QString> baseStage;
    QHash<QString, QString> candStage;
    QStringList stageOrder;

    for(const RegressionShaderBindingEntry &e : baseline.shaders)
    {
      if(!baseStage.contains(e.stage))
        stageOrder.push_back(e.stage);
      baseStage.insert(e.stage, e.shader);
    }
    for(const RegressionShaderBindingEntry &e : candidate.shaders)
    {
      if(!candStage.contains(e.stage) && !baseStage.contains(e.stage))
        stageOrder.push_back(e.stage);
      candStage.insert(e.stage, e.shader);
    }

    std::sort(stageOrder.begin(), stageOrder.end());

    for(const QString &stage : stageOrder)
    {
      const bool inA = baseStage.contains(stage);
      const bool inB = candStage.contains(stage);
      const QString a = baseStage.value(stage);
      const QString b = candStage.value(stage);

      RegressionShaderRow row;
      row.stage = stage;
      row.baseline = a;
      row.candidate = b;

      if(inA && inB && a == b)
      {
        row.verdict = RegressionVerdict::Unchanged;
      }
      else if(inA && inB)
      {
        row.verdict = RegressionVerdict::Changed;
        row.note = QStringLiteral("stage '%1' bound a different shader").arg(stage);
      }
      else if(inB)
      {
        row.verdict = RegressionVerdict::Added;
        row.note = QStringLiteral("stage '%1' is newly bound").arg(stage);
      }
      else
      {
        row.verdict = RegressionVerdict::Removed;
        row.note = QStringLiteral("stage '%1' is no longer bound").arg(stage);
      }

      EvidenceRef ev;
      ev.kind = QStringLiteral("pipeline");
      ev.shaderStage = stage;
      ev.resourceName = row.candidate.isEmpty() ? row.baseline : row.candidate;
      ev.label = QStringLiteral("%1: %2 -> %3")
                     .arg(stage)
                     .arg(a.isEmpty() ? QStringLiteral("(unbound)") : a)
                     .arg(b.isEmpty() ? QStringLiteral("(unbound)") : b);
      ev.confidence = row.verdict == RegressionVerdict::Unchanged ? Confidence::Confirmed
                                                                  : Confidence::Inferred;
      row.evidence.push_back(ev);

      result.shaders.push_back(row);
    }
  }

  //
  // ---- Summary -------------------------------------------------------------
  //
  for(const RegressionTimingRow &row : result.timing)
  {
    switch(row.verdict)
    {
      case RegressionVerdict::Regressed: result.summary.regressedEvents++; break;
      case RegressionVerdict::Improved: result.summary.improvedEvents++; break;
      case RegressionVerdict::Added: result.summary.addedEvents++; break;
      case RegressionVerdict::Removed: result.summary.removedEvents++; break;
      case RegressionVerdict::Unchanged: result.summary.unchangedEvents++; break;
      default: break;
    }
  }

  for(const RegressionMemoryRow &row : result.memory)
  {
    switch(row.verdict)
    {
      case RegressionVerdict::Added: result.summary.addedResources++; break;
      case RegressionVerdict::Removed: result.summary.removedResources++; break;
      case RegressionVerdict::Regressed: result.summary.regressedResources++; break;
      case RegressionVerdict::Improved: result.summary.improvedResources++; break;
      default: break;
    }
  }

  for(const RegressionPassRow &row : result.passes)
  {
    switch(row.verdict)
    {
      case RegressionVerdict::Regressed: result.summary.regressedPasses++; break;
      case RegressionVerdict::Improved: result.summary.improvedPasses++; break;
      default: break;
    }
  }

  for(const RegressionShaderRow &row : result.shaders)
  {
    switch(row.verdict)
    {
      case RegressionVerdict::Added: result.summary.addedShaders++; break;
      case RegressionVerdict::Removed: result.summary.removedShaders++; break;
      case RegressionVerdict::Changed: result.summary.changedShaders++; break;
      default: break;
    }
  }

  // Totals are the whole-frame sums, not the (possibly truncated) row sums, so a
  // limited report still states the true frame cost.
  double totalA = 0.0;
  for(const RegressionEventEntry &e : baseline.events)
    totalA += e.durationUs;
  double totalB = 0.0;
  for(const RegressionEventEntry &e : candidate.events)
    totalB += e.durationUs;

  result.summary.totalBaselineUs = totalA;
  result.summary.totalCandidateUs = totalB;
  result.summary.totalDeltaUs = totalB - totalA;
  result.summary.totalDeltaPercent = totalA > 0.0 ? result.summary.totalDeltaUs / totalA * 100.0 : 0.0;

  result.summary.totalBaselineBytes = baseline.totalResourceBytes;
  result.summary.totalCandidateBytes = candidate.totalResourceBytes;
  result.summary.totalDeltaBytes =
      (int64_t)candidate.totalResourceBytes - (int64_t)baseline.totalResourceBytes;

  //
  // ---- Sorting, truncation and filtering -----------------------------------
  //
  std::sort(result.timing.begin(), result.timing.end(),
            [](const RegressionTimingRow &x, const RegressionTimingRow &y) {
              const int rx = VerdictRank(x.verdict);
              const int ry = VerdictRank(y.verdict);
              if(rx != ry)
                return rx < ry;
              return qAbs(x.deltaUs) > qAbs(y.deltaUs);
            });
  std::sort(result.memory.begin(), result.memory.end(),
            [](const RegressionMemoryRow &x, const RegressionMemoryRow &y) {
              const int rx = VerdictRank(x.verdict);
              const int ry = VerdictRank(y.verdict);
              if(rx != ry)
                return rx < ry;
              return qAbs(x.deltaBytes) > qAbs(y.deltaBytes);
            });
  std::sort(result.passes.begin(), result.passes.end(),
            [](const RegressionPassRow &x, const RegressionPassRow &y) {
              const int rx = VerdictRank(x.verdict);
              const int ry = VerdictRank(y.verdict);
              if(rx != ry)
                return rx < ry;
              return qAbs(x.deltaUs) > qAbs(y.deltaUs);
            });
  std::sort(result.shaders.begin(), result.shaders.end(),
            [](const RegressionShaderRow &x, const RegressionShaderRow &y) {
              const int rx = VerdictRank(x.verdict);
              const int ry = VerdictRank(y.verdict);
              if(rx != ry)
                return rx < ry;
              return x.stage < y.stage;
            });

  if(!options.includeUnchanged)
  {
    QVector<RegressionTimingRow> timing;
    for(const RegressionTimingRow &row : result.timing)
    {
      if(row.verdict != RegressionVerdict::Unchanged)
        timing.push_back(row);
    }
    result.timing = timing;

    QVector<RegressionMemoryRow> memory;
    for(const RegressionMemoryRow &row : result.memory)
    {
      if(row.verdict != RegressionVerdict::Unchanged)
        memory.push_back(row);
    }
    result.memory = memory;

    QVector<RegressionPassRow> passes;
    for(const RegressionPassRow &row : result.passes)
    {
      if(row.verdict != RegressionVerdict::Unchanged)
        passes.push_back(row);
    }
    result.passes = passes;

    QVector<RegressionShaderRow> shaders;
    for(const RegressionShaderRow &row : result.shaders)
    {
      if(row.verdict != RegressionVerdict::Unchanged)
        shaders.push_back(row);
    }
    result.shaders = shaders;
  }

  TrimSection(result.timing, options.limit);
  TrimSection(result.memory, options.limit);
  TrimSection(result.passes, options.limit);
  TrimSection(result.shaders, options.limit);

  //
  // ---- Evidence, limitations and confidence --------------------------------
  //
  if(!baseline.capturePath.isEmpty())
    result.evidence.push_back(CaptureEvidence(baseline.capturePath));
  if(!candidate.capturePath.isEmpty())
    result.evidence.push_back(CaptureEvidence(candidate.capturePath));

  QStringList &lim = result.limitations;
  auto appendOnce = [&lim](const QString &s) {
    if(!s.isEmpty() && !lim.contains(s))
      lim.append(s);
  };

  appendOnce(QStringLiteral("Matching policy: %1").arg(options.match.Describe()));

  if(!result.timingAvailable)
    appendOnce(QStringLiteral("GPU timings are unavailable for one or both captures, so every "
                              "timing verdict is Unknown rather than a measured delta."));

  if(!result.apiMatches)
    appendOnce(QStringLiteral("The two captures use different graphics APIs ('%1' vs '%2'), so "
                              "structural differences may reflect the API rather than the "
                              "application.")
                   .arg(baseline.api)
                   .arg(candidate.api));

  if(!result.sameDriver)
    appendOnce(QStringLiteral("The driver changed between captures (%1), so timing deltas may "
                              "include a driver effect.")
                   .arg(result.driverChange));

  if(!baseline.unifyKey.isEmpty() && baseline.unifyKey == candidate.unifyKey)
    appendOnce(QStringLiteral("Both sides share the unify key '%1'; they may be the same capture "
                              "or two runs of the same frame.")
                   .arg(baseline.unifyKey));

  const int hypothesisPairs =
      eventMatch.fuzzyCount + eventMatch.positionalCount + resourceMatch.fuzzyCount +
      resourceMatch.positionalCount + passMatch.fuzzyCount + passMatch.positionalCount;
  if(hypothesisPairs > 0)
    appendOnce(QStringLiteral("%1 pairing(s) were made by fuzzy or ordinal fallback, so those "
                              "rows rest on a hypothesis rather than an exact identity.")
                   .arg(hypothesisPairs));

  const double coverage = qMin(qMin(eventMatch.coverage, resourceMatch.coverage),
                               passMatch.coverage);
  if(coverage < 1.0 && (eventMatch.countA > 0 || eventMatch.countB > 0))
    appendOnce(QStringLiteral("The worst-case match coverage is %1%, so part of the frame is "
                              "unpaired and reported as added or removed.")
                   .arg(QString::number(coverage * 100.0, 'f', 1)));

  int zeroSizedResources = 0;
  for(const RegressionResourceEntry &r : baseline.resources)
  {
    if(r.byteSize == 0)
      zeroSizedResources++;
  }
  for(const RegressionResourceEntry &r : candidate.resources)
  {
    if(r.byteSize == 0)
      zeroSizedResources++;
  }
  if(zeroSizedResources > 0)
    appendOnce(QStringLiteral("%1 resource(s) have an unknown creation size, so the memory totals "
                              "are a lower bound.")
                   .arg(zeroSizedResources));

  if(!baseline.limitation.isEmpty())
    appendOnce(QStringLiteral("Baseline snapshot: %1").arg(baseline.limitation));
  if(!candidate.limitation.isEmpty())
    appendOnce(QStringLiteral("Candidate snapshot: %1").arg(candidate.limitation));

  if(!result.timingAvailable)
    result.confidence = Confidence::ToVerify;
  else if(hypothesisPairs > 0)
    result.confidence = Confidence::Inferred;
  else
    result.confidence = Confidence::Confirmed;

  return result;
}

//
// ---- JSON renderers --------------------------------------------------------
//

QJsonObject RegressionTimingRowToJson(const RegressionTimingRow &row)
{
  QJsonObject j;
  j[QStringLiteral("key")] = row.key;
  j[QStringLiteral("displayA")] = row.displayA;
  j[QStringLiteral("displayB")] = row.displayB;
  j[QStringLiteral("kind")] = EventCanonicalKindName(row.kind);
  j[QStringLiteral("eventIdA")] = (int)row.eventIdA;
  j[QStringLiteral("eventIdB")] = (int)row.eventIdB;
  j[QStringLiteral("baselineUs")] = row.baselineUs;
  j[QStringLiteral("candidateUs")] = row.candidateUs;
  j[QStringLiteral("deltaUs")] = row.deltaUs;
  j[QStringLiteral("deltaPercent")] = row.deltaPercent;
  j[QStringLiteral("verdict")] = RegressionVerdictName(row.verdict);
  j[QStringLiteral("matchConfidence")] = CaptureMatchConfidenceName(row.matchConfidence);

  QJsonArray evidence;
  for(const EvidenceRef &e : row.evidence)
    evidence.append(EvidenceToJson(e));
  j[QStringLiteral("evidence")] = evidence;

  return j;
}

QJsonObject RegressionMemoryRowToJson(const RegressionMemoryRow &row)
{
  QJsonObject j;
  j[QStringLiteral("key")] = row.key;
  j[QStringLiteral("displayA")] = row.displayA;
  j[QStringLiteral("displayB")] = row.displayB;
  if(!row.type.isEmpty())
    j[QStringLiteral("type")] = row.type;
  j[QStringLiteral("baselineBytes")] = JsonU64String(row.baselineBytes);
  j[QStringLiteral("candidateBytes")] = JsonU64String(row.candidateBytes);
  j[QStringLiteral("deltaBytes")] = QString::number((qlonglong)row.deltaBytes, 10);
  j[QStringLiteral("deltaPercent")] = row.deltaPercent;
  j[QStringLiteral("verdict")] = RegressionVerdictName(row.verdict);
  j[QStringLiteral("matchConfidence")] = CaptureMatchConfidenceName(row.matchConfidence);
  j[QStringLiteral("confidence")] = ConfidenceName(row.confidence);

  QJsonArray evidence;
  for(const EvidenceRef &e : row.evidence)
    evidence.append(EvidenceToJson(e));
  j[QStringLiteral("evidence")] = evidence;

  return j;
}

QJsonObject RegressionPassRowToJson(const RegressionPassRow &row)
{
  QJsonObject j;
  j[QStringLiteral("key")] = row.key;
  j[QStringLiteral("displayA")] = row.displayA;
  j[QStringLiteral("displayB")] = row.displayB;
  j[QStringLiteral("firstEventIdA")] = (int)row.firstEventIdA;
  j[QStringLiteral("lastEventIdA")] = (int)row.lastEventIdA;
  j[QStringLiteral("firstEventIdB")] = (int)row.firstEventIdB;
  j[QStringLiteral("lastEventIdB")] = (int)row.lastEventIdB;
  j[QStringLiteral("baselineUs")] = row.baselineUs;
  j[QStringLiteral("candidateUs")] = row.candidateUs;
  j[QStringLiteral("deltaUs")] = row.deltaUs;
  j[QStringLiteral("deltaPercent")] = row.deltaPercent;
  j[QStringLiteral("verdict")] = RegressionVerdictName(row.verdict);
  j[QStringLiteral("matchConfidence")] = CaptureMatchConfidenceName(row.matchConfidence);

  QJsonArray evidence;
  for(const EvidenceRef &e : row.evidence)
    evidence.append(EvidenceToJson(e));
  j[QStringLiteral("evidence")] = evidence;

  return j;
}

QJsonObject RegressionShaderRowToJson(const RegressionShaderRow &row)
{
  QJsonObject j;
  j[QStringLiteral("stage")] = row.stage;
  j[QStringLiteral("baseline")] = row.baseline;
  j[QStringLiteral("candidate")] = row.candidate;
  j[QStringLiteral("verdict")] = RegressionVerdictName(row.verdict);
  if(!row.note.isEmpty())
    j[QStringLiteral("note")] = row.note;

  QJsonArray evidence;
  for(const EvidenceRef &e : row.evidence)
    evidence.append(EvidenceToJson(e));
  j[QStringLiteral("evidence")] = evidence;

  return j;
}

QJsonObject RegressionSummaryToJson(const RegressionSummary &summary)
{
  QJsonObject j;

  QJsonObject events;
  events[QStringLiteral("regressed")] = summary.regressedEvents;
  events[QStringLiteral("improved")] = summary.improvedEvents;
  events[QStringLiteral("unchanged")] = summary.unchangedEvents;
  events[QStringLiteral("added")] = summary.addedEvents;
  events[QStringLiteral("removed")] = summary.removedEvents;
  j[QStringLiteral("events")] = events;

  QJsonObject passes;
  passes[QStringLiteral("regressed")] = summary.regressedPasses;
  passes[QStringLiteral("improved")] = summary.improvedPasses;
  j[QStringLiteral("passes")] = passes;

  QJsonObject resources;
  resources[QStringLiteral("added")] = summary.addedResources;
  resources[QStringLiteral("removed")] = summary.removedResources;
  resources[QStringLiteral("regressed")] = summary.regressedResources;
  resources[QStringLiteral("improved")] = summary.improvedResources;
  j[QStringLiteral("resources")] = resources;

  QJsonObject shaders;
  shaders[QStringLiteral("added")] = summary.addedShaders;
  shaders[QStringLiteral("removed")] = summary.removedShaders;
  shaders[QStringLiteral("changed")] = summary.changedShaders;
  j[QStringLiteral("shaders")] = shaders;

  QJsonObject timing;
  timing[QStringLiteral("baselineUs")] = summary.totalBaselineUs;
  timing[QStringLiteral("candidateUs")] = summary.totalCandidateUs;
  timing[QStringLiteral("deltaUs")] = summary.totalDeltaUs;
  timing[QStringLiteral("deltaPercent")] = summary.totalDeltaPercent;
  j[QStringLiteral("timing")] = timing;

  QJsonObject memory;
  memory[QStringLiteral("baselineBytes")] = JsonU64String(summary.totalBaselineBytes);
  memory[QStringLiteral("candidateBytes")] = JsonU64String(summary.totalCandidateBytes);
  memory[QStringLiteral("deltaBytes")] = QString::number((qlonglong)summary.totalDeltaBytes, 10);
  j[QStringLiteral("memory")] = memory;

  return j;
}

QJsonObject RegressionDiffToJson(const RegressionDiffResult &result)
{
  QJsonObject j;
  j[QStringLiteral("available")] = result.available;
  j[QStringLiteral("formatVersion")] = result.formatVersion;

  j[QStringLiteral("baselineUnifyKey")] = result.baselineUnifyKey;
  j[QStringLiteral("candidateUnifyKey")] = result.candidateUnifyKey;
  j[QStringLiteral("baselinePath")] = result.baselinePath;
  j[QStringLiteral("candidatePath")] = result.candidatePath;
  j[QStringLiteral("baselineLabel")] = result.baselineLabel;
  j[QStringLiteral("candidateLabel")] = result.candidateLabel;
  j[QStringLiteral("api")] = result.api;
  j[QStringLiteral("apiMatches")] = result.apiMatches;
  j[QStringLiteral("timingAvailable")] = result.timingAvailable;
  j[QStringLiteral("sameDriver")] = result.sameDriver;
  if(!result.driverChange.isEmpty())
    j[QStringLiteral("driverChange")] = result.driverChange;

  QJsonArray timing;
  for(const RegressionTimingRow &row : result.timing)
    timing.append(RegressionTimingRowToJson(row));
  j[QStringLiteral("timing")] = timing;

  QJsonArray memory;
  for(const RegressionMemoryRow &row : result.memory)
    memory.append(RegressionMemoryRowToJson(row));
  j[QStringLiteral("memory")] = memory;

  QJsonArray passes;
  for(const RegressionPassRow &row : result.passes)
    passes.append(RegressionPassRowToJson(row));
  j[QStringLiteral("passes")] = passes;

  QJsonArray shaders;
  for(const RegressionShaderRow &row : result.shaders)
    shaders.append(RegressionShaderRowToJson(row));
  j[QStringLiteral("shaders")] = shaders;

  j[QStringLiteral("summary")] = RegressionSummaryToJson(result.summary);

  QJsonArray evidence;
  for(const EvidenceRef &e : result.evidence)
    evidence.append(EvidenceToJson(e));
  j[QStringLiteral("evidence")] = evidence;

  QJsonArray limitations;
  for(const QString &s : result.limitations)
    limitations.append(s);
  j[QStringLiteral("limitations")] = limitations;

  j[QStringLiteral("confidence")] = ConfidenceName(result.confidence);
  j[QStringLiteral("options")] = result.options.ToJson();

  return j;
}

//
// ---- Markdown --------------------------------------------------------------
//

QString RegressionDiffToMarkdown(const RegressionDiffResult &result)
{
  QStringList lines;
  lines << QStringLiteral("# RenderDoc Cross-Capture Regression Report");
  lines << QString();
  lines << QStringLiteral("- Baseline: %1").arg(
      result.baselineLabel.isEmpty() ? QStringLiteral("(unknown)") : result.baselineLabel);
  if(!result.baselineUnifyKey.isEmpty())
    lines << QStringLiteral("  - Key: `%1`").arg(result.baselineUnifyKey);
  lines << QStringLiteral("- Candidate: %1").arg(
      result.candidateLabel.isEmpty() ? QStringLiteral("(unknown)") : result.candidateLabel);
  if(!result.candidateUnifyKey.isEmpty())
    lines << QStringLiteral("  - Key: `%1`").arg(result.candidateUnifyKey);
  lines << QStringLiteral("- API: %1%2")
               .arg(result.api.isEmpty() ? QStringLiteral("(unknown)") : result.api)
               .arg(result.apiMatches ? QString() : QStringLiteral(" (differs between sides)"));
  lines << QStringLiteral("- GPU timings: %1")
               .arg(result.timingAvailable ? QStringLiteral("available")
                                           : QStringLiteral("unavailable"));
  if(!result.sameDriver && !result.driverChange.isEmpty())
    lines << QStringLiteral("- Driver change: %1").arg(result.driverChange);
  lines << QStringLiteral("- Confidence: %1").arg(ConfidenceName(result.confidence));
  lines << QString();

  lines << QStringLiteral("## Summary");
  lines << QStringLiteral("- Events: %1 regressed, %2 improved, %3 unchanged, %4 added, %5 removed")
               .arg(result.summary.regressedEvents)
               .arg(result.summary.improvedEvents)
               .arg(result.summary.unchangedEvents)
               .arg(result.summary.addedEvents)
               .arg(result.summary.removedEvents);
  lines << QStringLiteral("- Passes: %1 regressed, %2 improved")
               .arg(result.summary.regressedPasses)
               .arg(result.summary.improvedPasses);
  lines << QStringLiteral("- Resources: %1 regressed, %2 improved, %3 added, %4 removed")
               .arg(result.summary.regressedResources)
               .arg(result.summary.improvedResources)
               .arg(result.summary.addedResources)
               .arg(result.summary.removedResources);
  lines << QStringLiteral("- Shader bindings: %1 changed, %2 added, %3 removed")
               .arg(result.summary.changedShaders)
               .arg(result.summary.addedShaders)
               .arg(result.summary.removedShaders);
  lines << QStringLiteral("- Total GPU time: %1 us -> %2 us (%3)")
               .arg(FormatUs(result.summary.totalBaselineUs))
               .arg(FormatUs(result.summary.totalCandidateUs))
               .arg(PercentText(result.summary.totalDeltaPercent));
  lines << QStringLiteral("- Total resource bytes: %1 -> %2 (%3)")
               .arg(FormatBytes(result.summary.totalBaselineBytes))
               .arg(FormatBytes(result.summary.totalCandidateBytes))
               .arg(result.summary.totalDeltaBytes >= 0
                        ? QStringLiteral("+%1").arg(FormatBytes((uint64_t)result.summary.totalDeltaBytes))
                        : QStringLiteral("-%1")
                              .arg(FormatBytes((uint64_t)(-result.summary.totalDeltaBytes))));
  lines << QString();

  lines << QStringLiteral("## Timing regressions");
  if(result.timing.isEmpty())
  {
    lines << QStringLiteral("- None");
  }
  else
  {
    for(const RegressionTimingRow &row : result.timing)
    {
      lines << QStringLiteral("- [%1] %2: %3 us -> %4 us (%5), match %6 (EID %7 -> %8)")
                   .arg(RegressionVerdictName(row.verdict))
                   .arg(row.displayB.isEmpty() ? row.displayA : row.displayB)
                   .arg(FormatUs(row.baselineUs))
                   .arg(FormatUs(row.candidateUs))
                   .arg(PercentText(row.deltaPercent))
                   .arg(CaptureMatchConfidenceName(row.matchConfidence))
                   .arg(row.eventIdA)
                   .arg(row.eventIdB);
    }
  }
  lines << QString();

  lines << QStringLiteral("## Resource memory");
  if(result.memory.isEmpty())
  {
    lines << QStringLiteral("- None");
  }
  else
  {
    for(const RegressionMemoryRow &row : result.memory)
    {
      lines << QStringLiteral("- [%1] %2: %3 -> %4 (%5), match %6")
                   .arg(RegressionVerdictName(row.verdict))
                   .arg(row.displayB.isEmpty() ? row.displayA : row.displayB)
                   .arg(FormatBytes(row.baselineBytes))
                   .arg(FormatBytes(row.candidateBytes))
                   .arg(PercentText(row.deltaPercent))
                   .arg(CaptureMatchConfidenceName(row.matchConfidence));
    }
  }
  lines << QString();

  lines << QStringLiteral("## Pass timings");
  if(result.passes.isEmpty())
  {
    lines << QStringLiteral("- None");
  }
  else
  {
    for(const RegressionPassRow &row : result.passes)
    {
      lines << QStringLiteral("- [%1] %2: %3 us -> %4 us (%5), match %6 (EID %7-%8 -> %9-%10)")
                   .arg(RegressionVerdictName(row.verdict))
                   .arg(row.displayB.isEmpty() ? row.displayA : row.displayB)
                   .arg(FormatUs(row.baselineUs))
                   .arg(FormatUs(row.candidateUs))
                   .arg(PercentText(row.deltaPercent))
                   .arg(CaptureMatchConfidenceName(row.matchConfidence))
                   .arg(row.firstEventIdA)
                   .arg(row.lastEventIdA)
                   .arg(row.firstEventIdB)
                   .arg(row.lastEventIdB);
    }
  }
  lines << QString();

  lines << QStringLiteral("## Shader bindings");
  if(result.shaders.isEmpty())
  {
    lines << QStringLiteral("- None");
  }
  else
  {
    for(const RegressionShaderRow &row : result.shaders)
    {
      lines << QStringLiteral("- [%1] %2: %3 -> %4%5")
                   .arg(RegressionVerdictName(row.verdict))
                   .arg(row.stage)
                   .arg(row.baseline.isEmpty() ? QStringLiteral("(unbound)") : row.baseline)
                   .arg(row.candidate.isEmpty() ? QStringLiteral("(unbound)") : row.candidate)
                   .arg(row.note.isEmpty() ? QString() : QStringLiteral(" - %1").arg(row.note));
    }
  }
  lines << QString();

  lines << QStringLiteral("## Limitations");
  if(result.limitations.isEmpty())
  {
    lines << QStringLiteral("- None");
  }
  else
  {
    for(const QString &s : result.limitations)
      lines << QStringLiteral("- %1").arg(s);
  }

  return lines.join(QString::fromUtf8("\n"));
}

bool RegressionDiffHasChanges(const RegressionDiffResult &result)
{
  const RegressionSummary &s = result.summary;

  return s.regressedEvents > 0 || s.addedEvents > 0 || s.removedEvents > 0 ||
         s.regressedPasses > 0 || s.addedResources > 0 || s.removedResources > 0 ||
         s.regressedResources > 0 || s.addedShaders > 0 || s.removedShaders > 0 ||
         s.changedShaders > 0;
}

}    // namespace MCP
