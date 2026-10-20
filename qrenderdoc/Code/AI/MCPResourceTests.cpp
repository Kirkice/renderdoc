/* CPU contract tests do not establish real GPU replay coverage. */
#include "Code/QRDUtils.h"
#if ENABLE_UNIT_TESTS
#include "3rdparty/catch/catch.hpp"
#include "MCPResourceTools.h"
#include "MCPTools.h"
#include "MCPExternalTools.h"
#include "MCPSessionTools.h"
#include "MCPShaderTools.h"
#include "MCPComparisonTools.h"
#include "MCPCompatibilityTools.h"
#include <QImage>
#include <QJsonArray>
#include <QDateTime>
#include <QSet>

#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <limits>

namespace
{
void RegisterAll(MCP::ToolRegistry &registry, MCP::QueryAdapter &adapter)
{
  MCP::RegisterCoreTools(registry, adapter);
  MCP::RegisterExternalToolTools(registry, adapter);
  MCP::RegisterSessionTools(registry, adapter);
  MCP::RegisterShaderTools(registry, adapter);
  MCP::RegisterComparisonTools(registry, adapter);
  MCP::RegisterCompatibilityTools(registry, adapter);
  MCP::RegisterResourceTools(registry, adapter);
}
ActionDescription ExtensionAction(uint32_t eid, const char *name, ActionFlags flags)
{
  ActionDescription a;
  a.eventId = eid; a.customName = name; a.flags = flags;
  a.numIndices = 3; a.numInstances = 1;
  return a;
}
rdcarray<ActionDescription> ExtensionActionTree()
{
  ActionDescription pass = ExtensionAction(10, "Main Pass", ActionFlags::PushMarker);
  ActionDescription debug = ExtensionAction(13, "DEBUG group", ActionFlags::PushMarker);
  debug.children = {ExtensionAction(14, "Draw hidden", ActionFlags::Drawcall)};
  pass.children = {ExtensionAction(11, "Draw alpha", ActionFlags::Drawcall),
      ExtensionAction(12, "Clear target", ActionFlags::Clear), debug,
      ExtensionAction(15, "Draw zero", ActionFlags::Drawcall),
      ExtensionAction(16, "Copy", ActionFlags::Copy)};
  return {pass, ExtensionAction(20, "Draw outside", ActionFlags::Dispatch)};
}
}
TEST_CASE("Extension action filters share owner semantics and preserve operation order", "[mcp][resource][extension-actions]")
{
  using namespace MCP;
  const rdcarray<ActionDescription> roots = ExtensionActionTree(); SDFile sdfile;
  EventBrowserFilterOptions o;
  QJsonObject args{{QStringLiteral("filter"), QStringLiteral("DRAW")},
      {QStringLiteral("markerFilter"), QStringLiteral("MAIN")}, {QStringLiteral("excludeMarkers"), true},
      {QStringLiteral("onlyDrawCalls"), true}, {QStringLiteral("eventIdMin"), 11}, {QStringLiteral("eventIdMax"), 15}};
  REQUIRE(ParseEventBrowserFilters(args, o).ok);
  // getDrawCalls drops the ancestor before advanced marker filtering: no scope survives.
  CHECK(FilterActionTree(roots, sdfile, o, true).empty());
  const auto contextTree = FilterActionTree(roots, sdfile, o, false);
  REQUIRE(contextTree.size() == 3);
  CHECK(contextTree[0].eventId == 11); CHECK(contextTree[1].eventId == 14); CHECK(contextTree[2].eventId == 15);
  QVector<EventBrowserContextRow> rows; EventBrowserFilterStats stats;
  FilterEventBrowserActions(roots, sdfile, o, QHash<quint32, double>(), rows, stats);
  CHECK(stats.totalEvents == 8); CHECK(stats.matchedEvents == 3);
  REQUIRE(rows.size() == 3); CHECK(rows[1].depth == 2); CHECK_FALSE(rows[0].hasDuration);
  CHECK(bool(EventBrowserFilterOptionsToJson(o).value(QStringLiteral("filter")).toString() == QStringLiteral("DRAW")));
  args.remove(QStringLiteral("filter")); REQUIRE(ParseEventBrowserFilters(args, o).ok);
  const auto draws = FilterActionTree(roots, sdfile, o, true);
  REQUIRE(draws.size() == 4); CHECK(draws[1].eventId == 12); // clear is a real effect in tools.ts
  args[QStringLiteral("eventIdMax")] = 0; REQUIRE(ParseEventBrowserFilters(args, o).ok);
  CHECK(o.hasEventIdMax); CHECK(FilterActionTree(roots, sdfile, o).empty());
  args.remove(QStringLiteral("eventIdMax")); REQUIRE(ParseEventBrowserFilters(args, o).ok);
  CHECK_FALSE(FilterActionTree(roots, sdfile, o).empty());
  for(const char *key : {"filter", "markerFilter", "excludeMarkers", "onlyDrawCalls", "eventIdMin", "eventIdMax"})
  {
    QJsonObject bad = args; bad[QString::fromLatin1(key)] = QJsonValue(QJsonValue::Null);
    CHECK_FALSE(ParseEventBrowserFilters(bad, o).ok);
  }
  QJsonObject bad{{QStringLiteral("eventIdMin"), 1.5}}; CHECK_FALSE(ParseEventBrowserFilters(bad, o).ok);
  bad[QStringLiteral("eventIdMin")] = -1; CHECK_FALSE(ParseEventBrowserFilters(bad, o).ok);
}
TEST_CASE("Extension name filtering retains parent subtree and descendant promotions before cap", "[mcp][resource][extension-actions]")
{
  using namespace MCP;
  ActionDescription parent = ExtensionAction(1, "match parent", ActionFlags::PushMarker);
  parent.children = {ExtensionAction(2, "other", ActionFlags::Drawcall), ExtensionAction(3, "match leaf", ActionFlags::Drawcall)};
  SDFile sdfile; EventBrowserFilterOptions o; o.filter = QStringLiteral("MATCH");
  QVector<EventBrowserContextRow> rows; EventBrowserFilterStats stats;
  FilterEventBrowserActions({parent}, sdfile, o, QHash<quint32, double>(), rows, stats);
  REQUIRE(rows.size() == 4); CHECK(rows[0].eventId == 1); CHECK(rows[1].eventId == 2);
  CHECK(rows[2].eventId == 3); CHECK(rows[3].eventId == 3);
  CHECK(stats.totalEvents == 3); CHECK(stats.matchedEvents == 4); // real extension duplicates, not fabricated completeness
  o.sortByDuration = true; rows.clear(); stats = EventBrowserFilterStats();
  QHash<quint32, double> timings; timings.insert(2, 200); timings.insert(3, 10);
  FilterEventBrowserActions({parent}, sdfile, o, timings, rows, stats);
  REQUIRE(rows.size() == 4); CHECK(rows[0].eventId == 2);
  EventBrowserContext context; context.totalEvents = stats.totalEvents; context.matchedEvents = stats.matchedEvents;
  context.rows = rows.mid(0, 1); context.truncated = rows.size() > context.rows.size();
  const QJsonObject json = EventBrowserContextToJson(context);
  CHECK(json.value(QStringLiteral("matchedEvents")).toInt() == 4);
  CHECK(json.value(QStringLiteral("truncated")).toBool()); CHECK(json.value(QStringLiteral("events")).toArray().size() == 1);
}
TEST_CASE("Extension timings filter leaves then rank and cap with truthful missing IDs", "[mcp][resource][extension-actions]")
{
  using namespace MCP;
  const auto roots = ExtensionActionTree(); SDFile sdfile;
  QHash<quint32, double> timings; timings.insert(10, 9999); timings.insert(11, 10); timings.insert(12, 20);
  timings.insert(14, 500); timings.insert(15, 0); timings.insert(16, 30); timings.insert(20, 1000);
  ActionTimingOptions o;
  QJsonObject args{{QStringLiteral("eventIds"), QJsonArray{11, 12, 14, 15, 16, 999, 11}},
      {QStringLiteral("markerFilter"), QStringLiteral("MAIN")},
      {QStringLiteral("excludeMarkers"), QJsonArray{QStringLiteral("debug")}},
      {QStringLiteral("onlyDrawCalls"), true}, {QStringLiteral("limit"), 1}, {QStringLiteral("top"), 2}};
  REQUIRE(ParseActionTimingOptions(args, o).ok); CHECK(o.eventIds.size() == 6);
  const ToolResult result = SelectActionTimings(roots, sdfile, timings, o); REQUIRE(result.ok);
  const QJsonObject &j = result.structured;
  CHECK(j.value(QStringLiteral("totalMatched")).toInt() == 2); CHECK(j.value(QStringLiteral("returned")).toInt() == 1);
  CHECK(j.value(QStringLiteral("truncated")).toBool()); CHECK(j.value(QStringLiteral("fetchedEventCount")).toInt() == 7);
  CHECK(j.value(QStringLiteral("examinedEvents")).toInt() == 7); // excluded marker's leaf was not examined
  const QJsonArray returned = j.value(QStringLiteral("timings")).toArray(); REQUIRE(returned.size() == 1);
  CHECK(returned[0].toObject().value(QStringLiteral("eventId")).toInt() == 12);
  CHECK(returned[0].toObject().value(QStringLiteral("durationMicro")).toDouble() == 20);
  CHECK(returned[0].toObject().value(QStringLiteral("durationMs")).toDouble() == 0.02);
  CHECK(bool(returned[0].toObject().value(QStringLiteral("fullPath")).toString() == QStringLiteral("Main Pass -> Clear target")));
  const QJsonArray missing = j.value(QStringLiteral("missingEventIds")).toArray();
  CHECK(bool(missing == QJsonArray{14, 15, 16, 999})); // 11 is paged out, not missing
  CHECK(j.value(QStringLiteral("slowest")).toArray().size() == 2);
  args[QStringLiteral("onlyDrawCalls")] = false; args[QStringLiteral("eventIds")] = QJsonArray();
  args[QStringLiteral("limit")] = 0; REQUIRE(ParseActionTimingOptions(args, o).ok); CHECK(o.limit == 1);
  CHECK(SelectActionTimings(roots, sdfile, timings, o).structured.value(QStringLiteral("totalMatched")).toInt() == 3);
  args[QStringLiteral("limit")] = 5000; REQUIRE(ParseActionTimingOptions(args, o).ok); CHECK(o.limit == 2000);
  args[QStringLiteral("eventId")] = 11; REQUIRE(ParseActionTimingOptions(args, o).ok);
  CHECK(SelectActionTimings(roots, sdfile, timings, o).structured.value(QStringLiteral("totalMatched")).toInt() == 1);
  timings[11] = std::numeric_limits<double>::infinity();
  CHECK(SelectActionTimings(roots, sdfile, timings, o).structured.value(QStringLiteral("totalMatched")).toInt() == 0);
  for(const QJsonValue &v : {QJsonValue(true), QJsonValue(QJsonArray{1.5}), QJsonValue(QJsonArray{-1})})
  {
    args[QStringLiteral("eventIds")] = v; CHECK_FALSE(ParseActionTimingOptions(args, o).ok);
  }
  args.remove(QStringLiteral("eventIds")); args[QStringLiteral("excludeMarkers")] = true;
  CHECK_FALSE(ParseActionTimingOptions(args, o).ok);
}
TEST_CASE("Legacy timing calls preserve complete counters and independent slowest cap", "[mcp][resource][behavior]")
{
  using namespace MCP;
  ActionTimingOptions options;
  REQUIRE(ParseActionTimingOptions(QJsonObject{{QStringLiteral("top"), 4096}}, options).ok);
  CHECK(options.legacyCounters); CHECK(options.top == 4096);
  QHash<quint32, double> timings;
  for(quint32 i = 0; i < 220; i++) timings.insert(i + 1, double(i));
  SDFile sdfile; rdcarray<ActionDescription> roots;
  ToolResult result = SelectActionTimings(roots, sdfile, timings, options);
  REQUIRE(result.ok);
  CHECK(result.structured.value(QStringLiteral("timings")).toArray().size() == 220);
  CHECK(result.structured.value(QStringLiteral("slowest")).toArray().size() == 220);
  CHECK(result.structured.value(QStringLiteral("timings")).toArray()[0].toObject()
      .value(QStringLiteral("durationMicro")).toDouble() == 0);
  REQUIRE(ParseActionTimingOptions(QJsonObject{{QStringLiteral("eventId"), 7}, {QStringLiteral("top"), 1}}, options).ok);
  result = SelectActionTimings(roots, sdfile, timings, options);
  CHECK(result.structured.value(QStringLiteral("eventCount")).toInt() == 1);
  REQUIRE(ParseActionTimingOptions(QJsonObject{{QStringLiteral("eventIds"), QJsonArray()}}, options).ok);
  CHECK_FALSE(options.legacyCounters);
}
TEST_CASE("Extension performance report requires side-effect authority and atomically publishes rendered content", "[mcp][resource][extension-actions]")
{
  using namespace MCP;
  QueryAdapter adapter(NULL); ToolRegistry registry; RegisterCoreTools(registry, adapter); CallContext call;
  const RegisteredTool *tool = registry.Find(QStringLiteral("renderdoc_generatePerformanceReport")); REQUIRE(tool != NULL);
  CHECK(bool(tool->definition.effects == ToolEffects::SideEffect));
  const ToolResult denied = tool->handler(QJsonObject{{QStringLiteral("confirm"), false}}, call);
  CHECK(bool(denied.error.code == ErrorCode::SideEffectNotConfirmed));
  CHECK(bool(tool->handler(QJsonObject{{QStringLiteral("format"), QStringLiteral("html")}}, call).error.code == ErrorCode::InvalidParams));
  CHECK_FALSE(tool->handler(QJsonObject{{QStringLiteral("format"), QStringLiteral("json")}}, call).ok);
  PerformanceReport report;
  const QString markdown = PerformanceReportToMarkdown(report);
  CHECK(markdown.contains(QStringLiteral("Performance Report")));
  const QJsonObject reportJson = PerformanceReportToJson(report);
  const QString json = QString::fromUtf8(QJsonDocument(reportJson).toJson(QJsonDocument::Indented));
  CHECK(bool(QJsonDocument::fromJson(json.toUtf8()).object() == reportJson));
  QTemporaryDir directory; REQUIRE(directory.isValid());
  for(const QString &format : {QStringLiteral("json"), QStringLiteral("markdown")})
  {
    const QString content = format == QStringLiteral("json") ? json : markdown;
    const QString destination = directory.path() + QLatin1Char('/') + format;
    const ToolResult document = ToolResult::Success(QJsonObject{{QStringLiteral("format"), format},
        {QStringLiteral("report"), reportJson}, {QStringLiteral("content"), content}});
    ToolResult inlineResult = CompatibilityDocument(document, QJsonObject(), call); REQUIRE(inlineResult.ok);
    CHECK_FALSE(inlineResult.structured.value(QStringLiteral("written")).toBool());
    const QJsonObject args{{QStringLiteral("outputPath"), destination}, {QStringLiteral("confirm"), true}};
    ToolResult written = CompatibilityDocument(document, args, call); REQUIRE(written.ok);
    CHECK(written.structured.value(QStringLiteral("written")).toBool());
    QFile file(destination); REQUIRE(file.open(QIODevice::ReadOnly)); CHECK(bool(file.readAll() == content.toUtf8())); file.close();
    CHECK_FALSE(CompatibilityDocument(document, args, call).ok); // no overwrite
    QJsonObject refused = args; refused[QStringLiteral("confirm")] = false;
    CHECK(bool(CompatibilityDocument(document, refused, call).error.code == ErrorCode::SideEffectNotConfirmed));
  }
  volatile bool cancelled = true; call.cancelled = &cancelled;
  const QString cancelledPath = directory.path() + QStringLiteral("/cancelled");
  CHECK_FALSE(CompatibilityDocument(ToolResult::Success(QJsonObject{{QStringLiteral("content"), json}}),
      QJsonObject{{QStringLiteral("outputPath"), cancelledPath}}, call).ok);
  CHECK_FALSE(QFile::exists(cancelledPath));
}
TEST_CASE("Resource preview encodes real decodable PNG from RGB output", "[mcp][resource][png]")
{
  MCP::CallContext call;
  bytebuf rgb; rgb.resize(18);
  const byte data[] = {255,0,0, 0,255,0, 0,0,255, 7,19,31, 90,80,70, 255,255,255};
  memcpy(rgb.data(), data, sizeof(data));
  MCP::ToolResult result = MCP::EncodePreviewPNG(rgb, 3, 2, call);
  INFO(result.error.message.toStdString()); REQUIRE(result.ok);
  const QByteArray png = QByteArray::fromBase64(result.structured.value(QStringLiteral("base64")).toString().toLatin1());
  CHECK(png.startsWith(QByteArray::fromHex("89504e470d0a1a0a")));
  QImage decoded = QImage::fromData(png, "PNG"); REQUIRE_FALSE(decoded.isNull());
  CHECK(decoded.width() == 3); CHECK(decoded.height() == 2);
  CHECK(bool(decoded.pixelColor(0, 0) == QColor(255,0,0)));
  CHECK(bool(decoded.pixelColor(2, 0) == QColor(0,0,255)));
  CHECK(bool(decoded.pixelColor(0, 1) == QColor(7,19,31)));
  CHECK_FALSE(MCP::EncodePreviewPNG(rgb, 0, 2, call).ok);
  CHECK_FALSE(MCP::EncodePreviewPNG(rgb, INT32_MAX, INT32_MAX, call).ok);
  CHECK_FALSE(MCP::EncodePreviewPNG(rgb, 2, 2, call).ok);
  rgb.resize(0); CHECK_FALSE(MCP::EncodePreviewPNG(rgb, 3, 2, call).ok);
  volatile bool cancelled = true; call.cancelled = &cancelled;
  CHECK(bool(MCP::EncodePreviewPNG(rgb, 3, 2, call).error.code == MCP::ErrorCode::Cancelled));
  call.cancelled = NULL; call.deadlineMs = QDateTime::currentMSecsSinceEpoch() - 1;
  CHECK(bool(MCP::EncodePreviewPNG(rgb, 3, 2, call).error.code == MCP::ErrorCode::Timeout));
}
TEST_CASE("Resource preview contract preserves all native extension options", "[mcp][resource][contract]")
{
  using namespace MCP;
  DrawPreviewRequest request;
  QJsonObject args{{QStringLiteral("eventId"), 42}, {QStringLiteral("channelExtract"), 3},
      {QStringLiteral("resourceId"), QStringLiteral("18446744073709551615")},
      {QStringLiteral("overlayResourceId"), QStringLiteral("ResourceId::17")},
      {QStringLiteral("overlayMode"), QStringLiteral("Wireframe")}, {QStringLiteral("baseGammaEnabled"), false},
      {QStringLiteral("width"), 320}, {QStringLiteral("height"), 180}, {QStringLiteral("label"), QStringLiteral("caller label")}};
  REQUIRE(ParseDrawPreviewRequest(args, request).ok);
  CHECK(request.eventId == 42); CHECK(request.channelExtract == 3);
  CHECK(bool(request.overlay == DebugOverlay::Wireframe)); CHECK_FALSE(request.baseGammaEnabled);
  CHECK(request.width == 320); CHECK(request.height == 180);
  CHECK(bool(request.label == QStringLiteral("caller label")));
  CHECK(bool(ResourceIdToString(request.resourceId) == QStringLiteral("ResourceId::18446744073709551615")));
  CHECK(bool(ResourceIdToString(request.overlayResourceId) == QStringLiteral("ResourceId::17")));
  for(const char *mode : {"none", "drawcall", "wireframe", "depth", "stencil", "backfacecull", "viewportscissor", "nan"})
  {
    QJsonObject input{{QStringLiteral("eventId"), 0}, {QStringLiteral("overlayMode"), QString::fromLatin1(mode)}};
    CHECK(ParseDrawPreviewRequest(input, request).ok);
  }
  for(const char *key : {"eventId", "channelExtract", "width", "height"})
  {
    QJsonObject bad = args; bad[QString::fromLatin1(key)] = 1.5;
    CHECK_FALSE(ParseDrawPreviewRequest(bad, request).ok);
  }
  QJsonObject bad = args; bad[QStringLiteral("overlayMode")] = QStringLiteral("invented"); CHECK_FALSE(ParseDrawPreviewRequest(bad, request).ok);
  bad = args; bad[QStringLiteral("channelExtract")] = 4; CHECK_FALSE(ParseDrawPreviewRequest(bad, request).ok);
  bad = args; bad[QStringLiteral("gamma")] = true; CHECK_FALSE(ParseDrawPreviewRequest(bad, request).ok);
  bad = args; bad.remove(QStringLiteral("baseGammaEnabled")); bad[QStringLiteral("gamma")] = false;
  CHECK(ParseDrawPreviewRequest(bad, request).ok); CHECK_FALSE(request.baseGammaEnabled);
  ResourceId id;
  for(const QJsonValue &value : {QJsonValue(QStringLiteral("0")), QJsonValue(QStringLiteral("-1")),
      QJsonValue(QStringLiteral("18446744073709551616")), QJsonValue(9007199254740992.0), QJsonValue(1.5), QJsonValue(QJsonValue::Null)})
    CHECK_FALSE(ParseResourceToolId(value, id));
}
TEST_CASE("Resource schemas and unavailable captures fail honestly", "[mcp][resource][contract]")
{
  using namespace MCP;
  QueryAdapter adapter(NULL); ToolRegistry registry; RegisterAll(registry, adapter);
  QString error; CallContext call;
  const RegisteredTool *preview = registry.Find(QStringLiteral("renderdoc_getCurrentDrawPreview")); REQUIRE(preview != NULL);
  QJsonObject args{{QStringLiteral("eventId"), 10}, {QStringLiteral("channelExtract"), -1},
      {QStringLiteral("overlayMode"), QStringLiteral("none")}, {QStringLiteral("resourceId"), QStringLiteral("17")},
      {QStringLiteral("baseGammaEnabled"), true}, {QStringLiteral("width"), 256}, {QStringLiteral("height"), 128}};
  CHECK(JsonSchema::Validate(preview->definition.inputSchema, args, error));
  CHECK_FALSE(JsonSchema::Validate(preview->definition.inputSchema, QJsonObject(), error));
  const RegisteredTool *bound = registry.Find(QStringLiteral("renderdoc_getBoundResources")); REQUIRE(bound != NULL);
  CHECK(JsonSchema::Validate(bound->definition.inputSchema, QJsonObject{{QStringLiteral("eventId"), 7}, {QStringLiteral("includeUnused"), false}, {QStringLiteral("includeConstantBuffers"), false}}, error));
  const RegisteredTool *lifetime = registry.Find(QStringLiteral("renderdoc_getResourceLifetime")); REQUIRE(lifetime != NULL);
  CHECK(JsonSchema::Validate(lifetime->definition.inputSchema, QJsonObject{{QStringLiteral("resourceId"), QStringLiteral("17")}, {QStringLiteral("limit"), 500}}, error));
  CHECK(registry.Contains(QStringLiteral("renderdoc_resourceLifetime")));
  for(const char *name : {"renderdoc_getBoundResources", "renderdoc_getCurrentDrawPreview", "renderdoc_getResourceLifetime"})
  {
    ToolResult result = adapter.ResourceTool(QString::fromLatin1(name), args, call);
    CHECK_FALSE(result.ok); CHECK(result.error.isError()); CHECK(result.structured.isEmpty());
  }
}
TEST_CASE("Preview overlay uses measured alpha and preserves base outside overlay", "[mcp][resource][png]")
{
  bytebuf base, overlay, alpha;
  base.resize(9); overlay.resize(9); alpha.resize(9);
  const byte baseData[] = {10,20,30, 10,20,30, 10,20,30};
  const byte overlayData[] = {210,120,80, 210,120,80, 210,120,80};
  const byte alphaData[] = {0,0,0, 255,255,255, 128,128,128};
  memcpy(base.data(), baseData, 9); memcpy(overlay.data(), overlayData, 9); memcpy(alpha.data(), alphaData, 9);
  REQUIRE(MCP::CompositePreviewRGB(base, overlay, alpha).ok);
  CHECK(base[0] == 10); CHECK(base[1] == 20); CHECK(base[2] == 30);
  CHECK(base[3] == 210); CHECK(base[4] == 120); CHECK(base[5] == 80);
  CHECK(base[6] == 110); CHECK(base[7] == 70); CHECK(base[8] == 55);
  alpha.resize(6); CHECK_FALSE(MCP::CompositePreviewRGB(base, overlay, alpha).ok);
  base.resize(0); CHECK_FALSE(MCP::CompositePreviewRGB(base, overlay, alpha).ok);
}
TEST_CASE("Resource pagination slices normalized filtered facts and preserves continuation", "[mcp][resource][behavior]")
{
  using namespace MCP;
  QJsonArray inventory{QJsonObject{{QStringLiteral("resourceId"), QStringLiteral("ResourceId::1")}},
      QJsonObject{{QStringLiteral("resourceId"), QStringLiteral("ResourceId::3")}},
      QJsonObject{{QStringLiteral("resourceId"), QStringLiteral("ResourceId::7")}}};
  QJsonObject page = ResourcePage(inventory, 1, 1);
  REQUIRE(page.value(QStringLiteral("resources")).toArray().size() == 1);
  CHECK(bool(page.value(QStringLiteral("resources")).toArray()[0] == inventory[1]));
  CHECK(page.value(QStringLiteral("total")).toInt() == 3);
  CHECK(page.value(QStringLiteral("nextOffset")).toInt() == 2);
  CHECK(page.value(QStringLiteral("truncated")).toBool());
  page = ResourcePage(inventory, 500, 2);
  CHECK(page.value(QStringLiteral("returned")).toInt() == 1);
  CHECK_FALSE(page.value(QStringLiteral("truncated")).toBool());
  CHECK(page.value(QStringLiteral("nextOffset")).isNull());
  page = ResourcePage(inventory, 1, INT32_MAX);
  CHECK(page.value(QStringLiteral("returned")).toInt() == 0);
  CHECK_FALSE(page.value(QStringLiteral("truncated")).toBool());
}
TEST_CASE("Resource event selection executes callback for API events and resolves texture end of frame", "[mcp][resource][behavior]")
{
  using namespace MCP;
  rdcarray<ActionDescription> roots;
  ActionDescription root, child;
  root.eventId = 4; child.eventId = 12;
  APIEvent api; api.eventId = 9; child.events.push_back(api);
  root.children.push_back(child); roots.push_back(root);
  uint32_t selected = 77, effective = 77; int calls = 0;
  auto select = [&](uint32_t event) { selected = event; calls++; };
  REQUIRE(SelectResourceEvent(roots, 9, false, select, effective).ok);
  CHECK(selected == 9); CHECK(effective == 9); CHECK(calls == 1);
  REQUIRE(SelectResourceEvent(roots, 0, true, select, effective).ok);
  CHECK(selected == 12); CHECK(effective == 12); CHECK(calls == 2);
  REQUIRE(SelectResourceEvent(roots, 0, false, select, effective).ok);
  CHECK(effective == 0); CHECK(calls == 2); // native buffer/pipeline zero never selects
  CHECK_FALSE(SelectResourceEvent(roots, 11, false, select, effective).ok);
  CHECK(calls == 2);
  roots.clear(); CHECK_FALSE(SelectResourceEvent(roots, 0, true, select, effective).ok);
}
TEST_CASE("Texture read compatibility preserves raw subresources and rejects ignored conversion", "[mcp][resource][behavior]")
{
  using namespace MCP;
  TextureReadRequest request;
  QJsonObject args{{QStringLiteral("resourceId"), 17}, {QStringLiteral("mip"), 2},
      {QStringLiteral("slice"), 3}, {QStringLiteral("sample"), 1}, {QStringLiteral("maxBytes"), 123}};
  REQUIRE(ParseTextureReadRequest(args, request).ok);
  CHECK_FALSE(request.png); CHECK_FALSE(request.selectEvent);
  CHECK(request.sub.mip == 2); CHECK(request.sub.slice == 3); CHECK(request.sub.sample == 1);
  CHECK(request.maxBytes == 123);
  args[QStringLiteral("textureId")] = QStringLiteral("ResourceId::17");
  REQUIRE(ParseTextureReadRequest(args, request).ok);
  args[QStringLiteral("textureId")] = QStringLiteral("18446744073709551615");
  CHECK_FALSE(ParseTextureReadRequest(args, request).ok);
  args.remove(QStringLiteral("resourceId")); args[QStringLiteral("eventId")] = 0;
  REQUIRE(ParseTextureReadRequest(args, request).ok); CHECK(request.selectEvent);
  CHECK(bool(ResourceIdToString(request.id) == QStringLiteral("ResourceId::18446744073709551615")));
  args[QStringLiteral("channelExtract")] = 3;
  CHECK(bool(ParseTextureReadRequest(args, request).error.code == ErrorCode::Unsupported));
  args[QStringLiteral("outputFormat")] = QStringLiteral("png");
  CHECK(bool(ParseTextureReadRequest(args, request).error.code == ErrorCode::Unsupported));
  args.remove(QStringLiteral("maxBytes"));
  REQUIRE(ParseTextureReadRequest(args, request).ok); CHECK(request.png); CHECK(request.channelExtract == 3);
  for(const char *key : {"eventId", "mip", "slice", "sample", "channelExtract"})
  {
    QJsonObject bad = args; bad[QString::fromLatin1(key)] = 1.5;
    CHECK_FALSE(ParseTextureReadRequest(bad, request).ok);
  }
  args[QStringLiteral("channelExtract")] = 4; CHECK_FALSE(ParseTextureReadRequest(args, request).ok);
  ResourceId id; REQUIRE(ParseResourceSelector(QJsonObject(), id, false).ok);
  CHECK(id == ResourceId()); CHECK_FALSE(ParseResourceSelector(QJsonObject(), id, true).ok);
}
TEST_CASE("Resource usage limits each category without changing examined totals", "[mcp][resource][behavior]")
{
  using namespace MCP;
  QJsonArray events{QJsonObject{{QStringLiteral("eventId"), 4}}, QJsonObject{{QStringLiteral("eventId"), 8}},
      QJsonObject{{QStringLiteral("eventId"), 12}}};
  QJsonObject all{{QStringLiteral("producerEvents"), events}, {QStringLiteral("consumerEvents"), QJsonArray{events[0]}},
      {QStringLiteral("accesses"), events}, {QStringLiteral("producerCount"), 3}, {QStringLiteral("consumerCount"), 1}};
  QJsonObject limited = LimitResourceUsage(all, 2);
  CHECK(limited.value(QStringLiteral("producerCount")).toInt() == 3);
  CHECK(limited.value(QStringLiteral("consumerCount")).toInt() == 1);
  CHECK(limited.value(QStringLiteral("producerEvents")).toArray().size() == 2);
  CHECK(limited.value(QStringLiteral("consumerEvents")).toArray().size() == 1);
  CHECK(limited.value(QStringLiteral("accesses")).toArray().size() == 2);
  CHECK(limited.value(QStringLiteral("producerEventsTruncated")).toBool());
  CHECK_FALSE(limited.value(QStringLiteral("consumerEventsTruncated")).toBool());
  CHECK(bool(limited.value(QStringLiteral("producerEvents")).toArray()[1] == events[1]));
  CHECK_FALSE(LimitResourceUsage(all, 500).value(QStringLiteral("truncated")).toBool());
  const QJsonObject legacy = LimitResourceUsage(all, 0);
  CHECK(legacy.value(QStringLiteral("producerEvents")).toArray().size() == 3);
  CHECK(legacy.value(QStringLiteral("accesses")).toArray().size() == 3);
  CHECK_FALSE(legacy.value(QStringLiteral("truncated")).toBool());
}
TEST_CASE("Resource parameter contracts invoke registered handlers without fabricating capture data", "[mcp][resource][behavior]")
{
  using namespace MCP;
  QueryAdapter adapter(NULL); ToolRegistry registry; RegisterAll(registry, adapter);
  CallContext call; QString error;
  const QJsonObject valid[] = {
      QJsonObject{{QStringLiteral("limit"), 500}, {QStringLiteral("offset"), 1}},
      QJsonObject{{QStringLiteral("eventId"), 9}},
      QJsonObject{{QStringLiteral("textureId"), QStringLiteral("ResourceId::17")}},
      QJsonObject{{QStringLiteral("textureId"), QStringLiteral("ResourceId::17")},
          {QStringLiteral("eventId"), 12}, {QStringLiteral("channelExtract"), 3},
          {QStringLiteral("outputFormat"), QStringLiteral("png")}},
      QJsonObject{{QStringLiteral("resourceId"), QStringLiteral("ResourceId::17")}, {QStringLiteral("eventId"), 9}},
      QJsonObject{{QStringLiteral("resourceId"), 17}, {QStringLiteral("maxEvents"), 2}}};
  const char *names[] = {"renderdoc_getResources", "renderdoc_getPipelineState", "renderdoc_getTextureInfo",
      "renderdoc_getTextureData", "renderdoc_getBufferContents", "renderdoc_traceResourceUsage"};
  for(int i = 0; i < 6; i++)
  {
    const RegisteredTool *tool = registry.Find(QString::fromLatin1(names[i]));
    REQUIRE(tool != NULL);
    CHECK(JsonSchema::Validate(tool->definition.inputSchema, valid[i], error));
    ToolResult result = registry.Call(QString::fromLatin1(names[i]), valid[i], call);
    CHECK_FALSE(result.ok); CHECK(result.error.isError()); CHECK(result.structured.isEmpty());
  }
  CHECK_FALSE(registry.Call(QStringLiteral("renderdoc_getResources"),
      QJsonObject{{QStringLiteral("limit"), 501}}, call).ok);
  CHECK_FALSE(registry.Call(QStringLiteral("renderdoc_traceResourceUsage"),
      QJsonObject{{QStringLiteral("resourceId"), 17}, {QStringLiteral("maxEvents"), 0}}, call).ok);
  CHECK_FALSE(registry.Call(QStringLiteral("renderdoc_getTextureData"),
      QJsonObject{{QStringLiteral("textureId"), QStringLiteral("17")}, {QStringLiteral("channelExtract"), 4}}, call).ok);
}
#include "MCPResourceContractFixture.inl"
TEST_CASE("Every extension catalog entry and original native capability remains registered", "[mcp][resource][catalog]")
{
  using namespace MCP;
  QueryAdapter adapter(NULL); ToolRegistry registry; RegisterAll(registry, adapter);
  QSet<QString> names;
  CHECK(registry.Count() == 75);
  // No parameter exemptions: every extension parameter must have a native contract.
  QSet<QString> actualGaps;
  for(const ExtensionContract &contract : ExtensionContracts)
  {
    INFO(contract.name);
    const QString name = QString::fromLatin1(contract.name);
    CHECK_FALSE(names.contains(name)); names.insert(name);
    const RegisteredTool *tool = registry.Find(name);
    REQUIRE(tool != NULL);
    CHECK(bool(tool->handler));
    const QJsonObject properties = tool->definition.inputSchema.value(QStringLiteral("properties")).toObject();
    for(const QString &parameter : QString::fromLatin1(contract.parameters).split(QLatin1Char(','), QString::SkipEmptyParts))
    {
      INFO(parameter.toStdString());
      const QString key = name + QLatin1Char('.') + parameter;
      if(!properties.contains(parameter)) actualGaps.insert(key);
      CHECK(properties.contains(parameter));
    }
  }
  CHECK(actualGaps.isEmpty());
  CHECK(names.size() == 60);
  CHECK(sizeof(OriginalNativeTools) / sizeof(OriginalNativeTools[0]) == 48);
  for(const char *name : OriginalNativeTools)
  {
    INFO(name);
    const RegisteredTool *tool = registry.Find(QString::fromLatin1(name));
    REQUIRE(tool != NULL);
    CHECK(bool(tool->handler));
  }
}
#endif
