#include "MCPComparisonTools.h"
#include <QJsonArray>
#include <cmath>
#include <limits>

namespace MCP
{
namespace
{
const QString Limitation = QStringLiteral(
    "Names/marker paths are low-confidence heuristics, not cross-capture identity. "
    "EIDs and ResourceIDs are capture-local only. Duplicate/anonymous names remain unmatched. "
    "Footprints are captured texture/buffer metadata, not runtime residency or leak proof.");

// The generic matcher supports positional/fuzzy fallback and duplicate pairing.
// This boundary deliberately disables them, and makes ambiguous keys one-sided.
void MakeUnique(RegressionSnapshot &snapshot, const QString &side)
{
  QHash<QString, int> eventCounts, resourceCounts;
  for(const auto &e : snapshot.events) ++eventCounts[e.key];
  for(const auto &r : snapshot.resources) ++resourceCounts[r.key];
  for(int i = 0; i < snapshot.events.size(); ++i)
    if(snapshot.events[i].key.isEmpty() || eventCounts[snapshot.events[i].key] != 1)
      snapshot.events[i].key = side + QStringLiteral("|ambiguous-event|") + QString::number(i);
  for(int i = 0; i < snapshot.resources.size(); ++i)
    if(snapshot.resources[i].key.isEmpty() || resourceCounts[snapshot.resources[i].key] != 1)
      snapshot.resources[i].key = side + QStringLiteral("|ambiguous-resource|") + QString::number(i);
}
QJsonValue Bytes(uint64_t value) { return QString::number(qulonglong(value)); }
QJsonObject Resource(const ComparisonSnapshot &snapshot, int index)
{
  if(index < 0) return QJsonObject();
  const auto &r = snapshot.resources[index];
  return QJsonObject{{QStringLiteral("path"), snapshot.regression.capturePath},
                     {QStringLiteral("resourceId"), ResourceIdToString(r.resourceId)},
                     {QStringLiteral("name"), r.name}, {QStringLiteral("type"), r.type},
                     {QStringLiteral("byteSize"), snapshot.bytesKnown.value(index) ? Bytes(r.byteSize) : QJsonValue(QJsonValue::Null)},
                     {QStringLiteral("width"), double(r.width)},
                     {QStringLiteral("height"), double(r.height)},
                     {QStringLiteral("depth"), double(r.depth)},
                     {QStringLiteral("mips"), double(r.mips)},
                     {QStringLiteral("arraySize"), double(r.arraySize)},
                     {QStringLiteral("format"), r.format}};
}
QJsonObject Inventory(const ComparisonSnapshot &snapshot)
{
  uint64_t bytes = 0;
  int unknown = 0;
  bool overflow = false;
  QJsonObject byType;
  for(int i = 0; i < snapshot.resources.size(); ++i)
  {
    const auto &r = snapshot.resources[i];
    QJsonObject type = byType[r.type].toObject();
    type[QStringLiteral("count")] = type[QStringLiteral("count")].toInt() + 1;
    const uint64_t previous = type[QStringLiteral("knownBytes")].toString().toULongLong();
    const bool typeOverflow = type[QStringLiteral("byteSumOverflow")].toBool() ||
        (snapshot.bytesKnown.value(i) && r.byteSize > std::numeric_limits<uint64_t>::max() - previous);
    const int missing = type[QStringLiteral("unknownByteSizeCount")].toInt() +
        (snapshot.bytesKnown.value(i) ? 0 : 1);
    const QJsonValue subtotal = typeOverflow ? QJsonValue(QJsonValue::Null) :
        Bytes(previous + (snapshot.bytesKnown.value(i) ? r.byteSize : 0));
    type[QStringLiteral("knownBytes")] = subtotal;
    type[QStringLiteral("bytes")] = missing || typeOverflow ? QJsonValue(QJsonValue::Null) : subtotal;
    type[QStringLiteral("unknownByteSizeCount")] = missing;
    type[QStringLiteral("byteSumOverflow")] = typeOverflow;
    byType[r.type] = type;
    if(!snapshot.bytesKnown.value(i)) { ++unknown; continue; }
    if(r.byteSize > std::numeric_limits<uint64_t>::max() - bytes) overflow = true;
    else bytes += r.byteSize;
  }
  return QJsonObject{{QStringLiteral("path"), snapshot.regression.capturePath},
                     {QStringLiteral("api"), snapshot.regression.api},
                     {QStringLiteral("resources"), snapshot.resources.size()},
                     {QStringLiteral("drawCalls"), snapshot.regression.events.size()},
                     {QStringLiteral("byType"), byType},
                     {QStringLiteral("bytes"), unknown || overflow ? QJsonValue(QJsonValue::Null) : Bytes(bytes)},
                     {QStringLiteral("knownBytes"), overflow ? QJsonValue(QJsonValue::Null) : Bytes(bytes)},
                     {QStringLiteral("unknownByteSizeCount"), unknown},
                     {QStringLiteral("byteSumOverflow"), overflow},
                     {QStringLiteral("timedEvents"), snapshot.timings.size()},
                     {QStringLiteral("timingStatus"), snapshot.timingStatus}};
}
QJsonArray Capped(const QJsonArray &rows, int limit)
{
  QJsonArray out;
  for(int i = 0; i < rows.size() && i < limit; ++i) out.append(rows[i]);
  return out;
}
}

QJsonObject BuildComparisonResult(const ComparisonSnapshot &baseline,
                                 const ComparisonSnapshot &candidate, ComparisonKind kind, int limit)
{
  RegressionSnapshot a = baseline.regression, b = candidate.regression;
  // The legacy diff's signed delta arithmetic cannot represent uint64 values
  // above INT64_MAX. Keep the original facts in the detached inventory and
  // prevent overflow in the reused arithmetic model; such deltas become unknown.
  for(auto &r : a.resources) if(r.byteSize > uint64_t(INT64_MAX)) r.byteSize = 0;
  for(auto &r : b.resources) if(r.byteSize > uint64_t(INT64_MAX)) r.byteSize = 0;
  MakeUnique(a, QStringLiteral("baseline"));
  MakeUnique(b, QStringLiteral("candidate"));
  RegressionOptions options;
  options.match.fuzzyThreshold = 1.0;
  options.match.allowPositionalFallback = false;
  options.match.preferNearestEvent = false;
  options.includeUnchanged = true;
  options.limit = 0; // examine everything before applying the output cap
  const RegressionDiffResult diff = DiffRegressionSnapshots(a, b, options);
  QHash<QString, int> indicesA, indicesB;
  for(int i = 0; i < a.resources.size(); ++i) indicesA.insert(a.resources[i].key, i);
  for(int i = 0; i < b.resources.size(); ++i) indicesB.insert(b.resources[i].key, i);
  QJsonArray memory, retained, timing;
  int matchedResources = 0, candidateOnly = 0, baselineOnly = 0, matchedEvents = 0;
  for(const auto &row : diff.memory)
  {
    const int ia = indicesA.value(row.key, -1), ib = indicesB.value(row.key, -1);
    const bool matched = ia >= 0 && ib >= 0 && a.api == b.api;
    if(matched) ++matchedResources;
    else { if(ia >= 0) ++baselineOnly; if(ib >= 0) ++candidateOnly; }
    QJsonObject j = RegressionMemoryRowToJson(row);
    QJsonArray evidence;
    if(ia >= 0) evidence.append(Resource(baseline, ia));
    if(ib >= 0) evidence.append(Resource(candidate, ib));
    j[QStringLiteral("evidence")] = evidence;
    j[QStringLiteral("baseline")] = ia >= 0 ? QJsonValue(Resource(baseline, ia)) : QJsonValue(QJsonValue::Null);
    j[QStringLiteral("candidate")] = ib >= 0 ? QJsonValue(Resource(candidate, ib)) : QJsonValue(QJsonValue::Null);
    j[QStringLiteral("matchConfidence")] = matched ? QStringLiteral("low") : QStringLiteral("unknown");
    j[QStringLiteral("confidence")] = matched ? QStringLiteral("low") : QStringLiteral("unknown");
    j[QStringLiteral("matchingMethod")] = matched ? QStringLiteral("unique normalized type/name heuristic") : QStringLiteral("unmatched or ambiguous");
    j[QStringLiteral("beforeBytes")] = ia >= 0 && baseline.bytesKnown.value(ia) ? Bytes(baseline.resources[ia].byteSize) : QJsonValue(QJsonValue::Null);
    j[QStringLiteral("afterBytes")] = ib >= 0 && candidate.bytesKnown.value(ib) ? Bytes(candidate.resources[ib].byteSize) : QJsonValue(QJsonValue::Null);
    j[QStringLiteral("baselineBytes")] = j[QStringLiteral("beforeBytes")];
    j[QStringLiteral("candidateBytes")] = j[QStringLiteral("afterBytes")];
    const bool measurable = matched && baseline.bytesKnown.value(ia) && candidate.bytesKnown.value(ib) &&
        baseline.resources[ia].byteSize <= uint64_t(INT64_MAX) && candidate.resources[ib].byteSize <= uint64_t(INT64_MAX);
    if(!measurable)
    {
      j[QStringLiteral("verdict")] = QStringLiteral("unknown");
      j[QStringLiteral("deltaBytes")] = QJsonValue(QJsonValue::Null);
      j[QStringLiteral("deltaPercent")] = QJsonValue(QJsonValue::Null);
    }
    else if(row.baselineBytes == 0) j[QStringLiteral("deltaPercent")] = QJsonValue(QJsonValue::Null);
    memory.append(j);
    if(matched) retained.append(j);
  }
  for(const auto &row : diff.timing)
  {
    const bool matched = row.eventIdA != 0 && row.eventIdB != 0 && a.api == b.api;
    if(matched) ++matchedEvents;
    const bool beforeKnown = baseline.timings.contains(row.eventIdA);
    const bool afterKnown = candidate.timings.contains(row.eventIdB);
    QJsonObject j = RegressionTimingRowToJson(row);
    // RegressionDiff's legacy evidence emits zero timing references for missing
    // samples. Replace those with only actually collected, capture-scoped facts.
    QJsonArray evidence;
    if(beforeKnown) evidence.append(QJsonObject{
        {QStringLiteral("path"), a.capturePath}, {QStringLiteral("eventId"), double(row.eventIdA)},
        {QStringLiteral("counter"), QStringLiteral("EventGPUDuration")},
        {QStringLiteral("durationUs"), baseline.timings.value(row.eventIdA)}});
    if(afterKnown) evidence.append(QJsonObject{
        {QStringLiteral("path"), b.capturePath}, {QStringLiteral("eventId"), double(row.eventIdB)},
        {QStringLiteral("counter"), QStringLiteral("EventGPUDuration")},
        {QStringLiteral("durationUs"), candidate.timings.value(row.eventIdB)}});
    j[QStringLiteral("evidence")] = evidence;
    j[QStringLiteral("baselinePath")] = a.capturePath;
    j[QStringLiteral("candidatePath")] = b.capturePath;
    j[QStringLiteral("matchConfidence")] = matched ? QStringLiteral("low") : QStringLiteral("unknown");
    j[QStringLiteral("confidence")] = matched ? QStringLiteral("low") : QStringLiteral("unknown");
    j[QStringLiteral("matchingMethod")] = matched ? QStringLiteral("unique normalized marker/name heuristic") : QStringLiteral("unmatched or ambiguous");
    j[QStringLiteral("baselineUs")] = beforeKnown ? QJsonValue(row.baselineUs) : QJsonValue(QJsonValue::Null);
    j[QStringLiteral("candidateUs")] = afterKnown ? QJsonValue(row.candidateUs) : QJsonValue(QJsonValue::Null);
    j[QStringLiteral("beforeUs")] = j[QStringLiteral("baselineUs")];
    j[QStringLiteral("afterUs")] = j[QStringLiteral("candidateUs")];
    if(!matched || !beforeKnown || !afterKnown)
    {
      j[QStringLiteral("verdict")] = QStringLiteral("unknown");
      j[QStringLiteral("deltaUs")] = QJsonValue(QJsonValue::Null);
      j[QStringLiteral("deltaPercent")] = QJsonValue(QJsonValue::Null);
    }
    else if(row.baselineUs == 0)
    {
      j[QStringLiteral("deltaPercent")] = QJsonValue(QJsonValue::Null);
      j[QStringLiteral("verdict")] = row.candidateUs > 0 ? QStringLiteral("regressed") : QStringLiteral("unchanged");
    }
    timing.append(j);
  }
  const QJsonObject before = Inventory(baseline), after = Inventory(candidate);
  QJsonObject out{{QStringLiteral("available"), true},
                  {QStringLiteral("baseline"), before}, {QStringLiteral("candidate"), after},
                  {QStringLiteral("confidence"), QStringLiteral("low")},
                  {QStringLiteral("limitation"), Limitation},
                  {QStringLiteral("policy"), options.match.ToJson()},
                  {QStringLiteral("apiMatches"), a.api == b.api},
                  {QStringLiteral("matchedResources"), matchedResources},
                  {QStringLiteral("candidateOnlyCount"), candidateOnly},
                  {QStringLiteral("baselineOnlyCount"), baselineOnly},
                  {QStringLiteral("unmatchedMeaning"), QStringLiteral("No unique heuristic pairing; not proof of allocation addition/removal.")},
                  {QStringLiteral("limit"), limit},
                  {QStringLiteral("rowSemantics"), QStringLiteral("All examined rows, including unchanged and unknown; output cap does not limit matching coverage.")}};
  if(kind == ComparisonKind::EventTimings)
  {
    out[QStringLiteral("matchedEvents")] = matchedEvents;
    out[QStringLiteral("timingAvailable")] = !baseline.timings.isEmpty() && !candidate.timings.isEmpty();
    out[QStringLiteral("changes")] = Capped(timing, limit);
    out[QStringLiteral("totalRows")] = timing.size();
    out[QStringLiteral("truncated")] = timing.size() > limit;
  }
  else if(kind == ComparisonKind::ResourceLeaks)
  {
    out[QStringLiteral("retainedResources")] = Capped(retained, limit);
    out[QStringLiteral("totalRows")] = retained.size();
    out[QStringLiteral("truncated")] = retained.size() > limit;
    out[QStringLiteral("runtimeLeakStatus")] = QStringLiteral("unknown");
  }
  else
  {
    out[QStringLiteral("baselineBytes")] = before[QStringLiteral("bytes")];
    out[QStringLiteral("candidateBytes")] = after[QStringLiteral("bytes")];
    out[QStringLiteral("changedResources")] = Capped(memory, limit);
    out[QStringLiteral("totalRows")] = memory.size();
    out[QStringLiteral("truncated")] = memory.size() > limit;
    QJsonValue delta(QJsonValue::Null), percent(QJsonValue::Null);
    if(!before[QStringLiteral("bytes")].isNull() && !after[QStringLiteral("bytes")].isNull())
    {
      const uint64_t ba = before[QStringLiteral("bytes")].toString().toULongLong();
      const uint64_t bb = after[QStringLiteral("bytes")].toString().toULongLong();
      delta = bb >= ba ? QString::number(qulonglong(bb - ba)) :
          QStringLiteral("-") + QString::number(qulonglong(ba - bb));
      if(ba != 0) percent = (double(bb) - double(ba)) / double(ba) * 100.0;
    }
    out[QStringLiteral("bytesDelta")] = delta;
    out[QStringLiteral("bytesPercentChange")] = percent;
    if(kind == ComparisonKind::Captures)
      out[QStringLiteral("deltas")] = QJsonObject{
          {QStringLiteral("resourceCount"), b.resources.size() - a.resources.size()},
          {QStringLiteral("drawCalls"), b.events.size() - a.events.size()},
          {QStringLiteral("unmatchedCandidateResources"), candidateOnly},
          {QStringLiteral("unmatchedBaselineResources"), baselineOnly},
          {QStringLiteral("bytesDelta"), delta}, {QStringLiteral("bytesPercentChange"), percent},
          {QStringLiteral("changedResources"), Capped(memory, limit)}};
  }
  return out;
}

ToolResult CompareCapturePaths(QueryAdapter &adapter, const QJsonObject &args,
                               CallContext &call, ComparisonKind kind)
{
  for(const char *key : {"baselinePath", "candidatePath"})
    if(!args[QString::fromUtf8(key)].isString() || args[QString::fromUtf8(key)].toString().trimmed().isEmpty())
      return ToolResult::Failure(Error(ErrorCode::InvalidParams, "both non-empty capture paths are required"));
  if(args.contains(QStringLiteral("limit")) && !args.value(QStringLiteral("limit")).isDouble())
    return ToolResult::Failure(Error(ErrorCode::InvalidParams, "limit must be an integer from 1 to 500"));
  const double requested = args.value(QStringLiteral("limit")).toDouble(100);
  if(!std::isfinite(requested) || requested < 1 || requested > 500 || std::floor(requested) != requested)
    return ToolResult::Failure(Error(ErrorCode::InvalidParams, "limit must be an integer from 1 to 500"));
  ComparisonSnapshot baseline, candidate;
  const bool timing = kind == ComparisonKind::EventTimings;
  ToolResult result = adapter.ReadComparisonSnapshot(args[QStringLiteral("baselinePath")].toString(), timing, call, baseline);
  if(!result.ok) return result;
  result = adapter.ReadComparisonSnapshot(args[QStringLiteral("candidatePath")].toString(), timing, call, candidate);
  if(!result.ok) return result;
  return ToolResult::Success(BuildComparisonResult(baseline, candidate, kind, int(requested)));
}

void RegisterComparisonTools(ToolRegistry &registry, QueryAdapter &adapter)
{
  struct Tool { const char *name; ComparisonKind kind; };
  for(const Tool &tool : {Tool{"renderdoc_compareCaptures", ComparisonKind::Captures},
                          Tool{"renderdoc_compareEventTimings", ComparisonKind::EventTimings},
                          Tool{"renderdoc_compareResourceMemory", ComparisonKind::ResourceMemory}})
  {
    const QJsonObject path{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}};
    const QJsonObject schema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("baselinePath"), path}, {QStringLiteral("candidatePath"), path},
            {QStringLiteral("limit"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 500}}}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("baselinePath"), QStringLiteral("candidatePath")}}};
    registry.Add(ToolDefinition{QString::fromUtf8(tool.name), QString::fromUtf8(tool.name),
        QStringLiteral("Compare two independent read-only local captures. ") + Limitation,
        ToolEffects::ReadOnly, ToolRequirements::None, schema},
        [&adapter, tool](const QJsonObject &args, CallContext &call) {
          return CompareCapturePaths(adapter, args, call, tool.kind);
        });
  }
}
}    // namespace MCP
