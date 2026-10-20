#include "Code/QRDUtils.h"
#if ENABLE_UNIT_TESTS
#include "3rdparty/catch/catch.hpp"
#include "MCPComparisonTools.h"
#include "MCPExternalTools.h"
#include "MCPSessionTools.h"
#include "MCPShaderTools.h"
#include "MCPTools.h"
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonArray>

// Identical to the existing MCP test formatter: RenderDoc's generic ToStr
// does not provide a QString conversion.
namespace Catch
{
template <>
struct StringMaker<QString>
{
  static std::string convert(const QString &value)
  {
    const QByteArray utf8 = value.toUtf8();
    return std::string(utf8.constBegin(), utf8.constEnd());
  }
};
}

namespace
{
MCP::ComparisonSnapshot Snapshot(const QString &path, uint32_t eid, uint64_t bytes)
{
  MCP::ComparisonSnapshot s;
  s.regression.capturePath = path;
  s.regression.api = QStringLiteral("D3D11");
  MCP::RegressionEventEntry e;
  e.eventId = eid;
  e.key = QStringLiteral("effect|main > draw");
  e.label = QStringLiteral("main > draw");
  e.kind = MCP::EventCanonicalKind::RealEffect;
  s.regression.events.push_back(e);
  MCP::ResourceMemoryRow r;
  r.name = QStringLiteral("Albedo");
  r.type = QStringLiteral("Texture");
  r.byteSize = bytes;
  s.resources.push_back(r);
  s.bytesKnown.push_back(true);
  MCP::RegressionResourceEntry re;
  re.key = QStringLiteral("Texture|albedo");
  re.displayName = r.name;
  re.type = r.type;
  re.byteSize = bytes;
  re.index = 0;
  s.regression.resources.push_back(re);
  return s;
}
}
TEST_CASE("Native comparison catalog preserves session shader and legacy leak contracts", "[mcp][comparison]")
{
  MCP::QueryAdapter adapter(NULL);
  MCP::ToolRegistry registry;
  MCP::RegisterCoreTools(registry, adapter);
  MCP::RegisterExternalToolTools(registry, adapter);
  MCP::RegisterSessionTools(registry, adapter);
  MCP::RegisterShaderTools(registry, adapter);
  const int count = registry.Count();
  MCP::RegisterComparisonTools(registry, adapter);
  REQUIRE(registry.Count() == count + 3);
  MCP::RegisterComparisonTools(registry, adapter);
  REQUIRE(registry.Count() == count + 3);
  MCP::CallContext call;
  const QJsonObject paths{{QStringLiteral("baselinePath"), QStringLiteral("missing-before.rdc")},
                          {QStringLiteral("candidatePath"), QStringLiteral("missing-after.rdc")}};
  for(const char *name : {"renderdoc_compareCaptures", "renderdoc_compareEventTimings", "renderdoc_compareResourceMemory"})
  {
    const auto *tool = registry.Find(QString::fromUtf8(name));
    REQUIRE(tool != NULL);
    REQUIRE(int(tool->definition.requirements) == int(MCP::ToolRequirements::None));
    REQUIRE(int(tool->definition.effects) == int(MCP::ToolEffects::ReadOnly));
    QString error;
    REQUIRE(MCP::JsonSchema::Validate(tool->definition.inputSchema, paths, error));
    REQUIRE_FALSE(registry.Call(QString::fromUtf8(name), QJsonObject(), call).ok);
    QJsonObject bad = paths;
    bad[QStringLiteral("limit")] = 1.5;
    REQUIRE_FALSE(registry.Call(QString::fromUtf8(name), bad, call).ok);
  }
  QJsonObject badLimit = paths;
  badLimit[QStringLiteral("limit")] = QStringLiteral("10");
  REQUIRE(int(MCP::CompareCapturePaths(adapter, badLimit, call, MCP::ComparisonKind::ResourceLeaks).error.code) ==
          int(MCP::ErrorCode::InvalidParams));
  REQUIRE(registry.Find(QStringLiteral("renderdoc_getSessionState")) != NULL);
  REQUIRE(registry.Find(QStringLiteral("renderdoc_getShaderSource")) != NULL);
  REQUIRE_FALSE(registry.Call(QStringLiteral("renderdoc_findResourceLeaks"), QJsonObject(), call).ok);
  REQUIRE_FALSE(registry.Call(QStringLiteral("renderdoc_findResourceLeaks"),
      QJsonObject{{QStringLiteral("baselinePath"), QStringLiteral("before.rdc")}}, call).ok);
  REQUIRE_FALSE(registry.Call(QStringLiteral("renderdoc_findResourceLeaks"),
      QJsonObject{{QStringLiteral("candidatePath"), QStringLiteral("")}}, call).ok);
}
TEST_CASE("Native comparison uses local ids only as evidence and preserves missing samples", "[mcp][comparison]")
{
  auto a = Snapshot(QStringLiteral("before.rdc"), 10, 100);
  auto b = Snapshot(QStringLiteral("after.rdc"), 500, 200);
  a.timings.insert(10, 0.0); // measured zero is not a missing sample
  b.timings.insert(500, 12.0);
  MCP::ApplyEventTimings(a.regression, a.timings);
  MCP::ApplyEventTimings(b.regression, b.timings);
  a.regression.timingAvailable = b.regression.timingAvailable = true;
  auto out = MCP::BuildComparisonResult(a, b, MCP::ComparisonKind::EventTimings, 100);
  REQUIRE(out[QStringLiteral("matchedEvents")].toInt() == 1);
  auto row = out[QStringLiteral("changes")].toArray()[0].toObject();
  REQUIRE(row[QStringLiteral("eventIdA")].toInt() == 10);
  REQUIRE(row[QStringLiteral("eventIdB")].toInt() == 500);
  REQUIRE(row[QStringLiteral("matchConfidence")].toString() == QStringLiteral("low"));
  REQUIRE(row[QStringLiteral("deltaUs")].toDouble() == 12.0);
  REQUIRE(row[QStringLiteral("deltaPercent")].isNull());
  REQUIRE(row[QStringLiteral("verdict")].toString() == QStringLiteral("regressed"));
  b.timings.clear();
  out = MCP::BuildComparisonResult(a, b, MCP::ComparisonKind::EventTimings, 100);
  row = out[QStringLiteral("changes")].toArray()[0].toObject();
  REQUIRE(row[QStringLiteral("candidateUs")].isNull());
  REQUIRE(row[QStringLiteral("deltaUs")].isNull());
  REQUIRE(row[QStringLiteral("verdict")].toString() == QStringLiteral("unknown"));
  REQUIRE(row[QStringLiteral("evidence")].toArray().size() == 1);
  b.regression.events[0].key = QStringLiteral("effect|unrelated");
  b.regression.events[0].eventId = 10; // equal EID does not imply a match
  out = MCP::BuildComparisonResult(a, b, MCP::ComparisonKind::EventTimings, 100);
  REQUIRE(out[QStringLiteral("matchedEvents")].toInt() == 0);
}
TEST_CASE("Native comparison memory and leak candidates stay heuristic and ambiguous", "[mcp][comparison]")
{
  auto a = Snapshot(QStringLiteral("before.rdc"), 1, 100);
  auto b = Snapshot(QStringLiteral("after.rdc"), 2, 200);
  auto out = MCP::BuildComparisonResult(a, b, MCP::ComparisonKind::ResourceMemory, 1);
  auto row = out[QStringLiteral("changedResources")].toArray()[0].toObject();
  REQUIRE(row[QStringLiteral("deltaBytes")].toString() == QStringLiteral("100"));
  REQUIRE(row[QStringLiteral("confidence")].toString() == QStringLiteral("low"));
  b.bytesKnown[0] = false;
  out = MCP::BuildComparisonResult(a, b, MCP::ComparisonKind::ResourceMemory, 1);
  row = out[QStringLiteral("changedResources")].toArray()[0].toObject();
  REQUIRE(out[QStringLiteral("candidateBytes")].isNull());
  REQUIRE(row[QStringLiteral("candidateBytes")].isNull());
  REQUIRE(row[QStringLiteral("deltaBytes")].isNull());
  b.bytesKnown[0] = true;
  out = MCP::BuildComparisonResult(a, b, MCP::ComparisonKind::ResourceLeaks, 1);
  REQUIRE(out[QStringLiteral("retainedResources")].toArray().size() == 1);
  REQUIRE(out[QStringLiteral("runtimeLeakStatus")].toString() == QStringLiteral("unknown"));
  b.regression.resources.push_back(b.regression.resources[0]);
  b.resources.push_back(b.resources[0]);
  b.bytesKnown.push_back(true);
  out = MCP::BuildComparisonResult(a, b, MCP::ComparisonKind::ResourceLeaks, 1);
  REQUIRE(out[QStringLiteral("retainedResources")].toArray().isEmpty());
  REQUIRE(out[QStringLiteral("candidateOnlyCount")].toInt() == 2);
  out = MCP::BuildComparisonResult(a, b, MCP::ComparisonKind::ResourceMemory, 1);
  REQUIRE(out[QStringLiteral("truncated")].toBool());
  REQUIRE(out[QStringLiteral("totalRows")].toInt() == 3);
  REQUIRE(out[QStringLiteral("changedResources")].toArray().size() == 1);
  b.regression.api = QStringLiteral("Vulkan");
  out = MCP::BuildComparisonResult(a, b, MCP::ComparisonKind::ResourceLeaks, 1);
  REQUIRE_FALSE(out[QStringLiteral("apiMatches")].toBool());
  REQUIRE(out[QStringLiteral("retainedResources")].toArray().isEmpty());
}
TEST_CASE("Native comparison preserves unsigned bytes and refuses anonymous identity", "[mcp][comparison]")
{
  auto a = Snapshot(QStringLiteral("before.rdc"), 1, UINT64_MAX);
  auto b = Snapshot(QStringLiteral("after.rdc"), 2, UINT64_MAX - 1);
  auto out = MCP::BuildComparisonResult(a, b, MCP::ComparisonKind::ResourceMemory, 100);
  REQUIRE(out[QStringLiteral("baselineBytes")].toString() == QStringLiteral("18446744073709551615"));
  REQUIRE(out[QStringLiteral("bytesDelta")].toString() == QStringLiteral("-1"));
  REQUIRE(out[QStringLiteral("changedResources")].toArray()[0].toObject()[QStringLiteral("deltaBytes")].isNull());
  b.resources.push_back(b.resources[0]);
  b.bytesKnown.push_back(true);
  b.regression.resources.push_back(b.regression.resources[0]);
  out = MCP::BuildComparisonResult(a, b, MCP::ComparisonKind::ResourceMemory, 100);
  REQUIRE(out[QStringLiteral("candidateBytes")].isNull());
  REQUIRE(out[QStringLiteral("candidate")].toObject()[QStringLiteral("byteSumOverflow")].toBool());
  a.regression.resources[0].key.clear();
  b = a; // equal local ids and shape, but anonymous: still not identity
  b.regression.capturePath = QStringLiteral("after.rdc");
  out = MCP::BuildComparisonResult(a, b, MCP::ComparisonKind::ResourceLeaks, 100);
  REQUIRE(out[QStringLiteral("retainedResources")].toArray().isEmpty());
}
TEST_CASE("Native independent replay reports actual file failures and cancellation", "[mcp][comparison]")
{
  MCP::QueryAdapter adapter(NULL);
  MCP::CallContext call;
  MCP::ComparisonSnapshot snapshot;
  snapshot.regression.capturePath = QStringLiteral("sentinel");
  QTemporaryDir dir;
  REQUIRE(dir.isValid());
  QFile invalid(dir.filePath(QStringLiteral("invalid.rdc")));
  REQUIRE(invalid.open(QIODevice::WriteOnly));
  invalid.write("not a renderdoc capture");
  invalid.close();
  auto result = adapter.ReadComparisonSnapshot(invalid.fileName(), true, call, snapshot);
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.error.data[QStringLiteral("operation")].toString() == QStringLiteral("OpenFile"));
  REQUIRE(result.error.data[QStringLiteral("path")].toString() == invalid.fileName());
  REQUIRE(snapshot.regression.capturePath == QStringLiteral("sentinel"));
  REQUIRE_FALSE(adapter.ReadComparisonSnapshot(dir.filePath(QStringLiteral("missing.rdc")), false, call, snapshot).ok);
  volatile bool cancelled = true;
  call.cancelled = &cancelled;
  result = adapter.ReadComparisonSnapshot(invalid.fileName(), false, call, snapshot);
  REQUIRE(int(result.error.code) == int(MCP::ErrorCode::Cancelled));
  call.cancelled = NULL;
  call.deadlineMs = QDateTime::currentMSecsSinceEpoch() - 1;
  result = adapter.ReadComparisonSnapshot(invalid.fileName(), false, call, snapshot);
  REQUIRE(int(result.error.code) == int(MCP::ErrorCode::Timeout));
}
#endif
