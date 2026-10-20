#include "Code/QRDUtils.h"
#if ENABLE_UNIT_TESTS
#include "3rdparty/catch/catch.hpp"
#include "MCPCompatibilityTools.h"
#include "MCPQueryAdapter.h"
#include "MCPTools.h"
#include "MCPHttpServer.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <thread>

namespace Catch
{
template <> struct StringMaker<QString>
{
  static std::string convert(const QString &value)
  {
    const QByteArray utf8 = value.toUtf8();
    return std::string(utf8.constBegin(), utf8.constEnd());
  }
};
}

TEST_CASE("Compatibility bookmarks patch presence identity and migration", "[mcp][compatibility][bookmark]")
{
  using namespace MCP;
  BookmarkFile file;
  BookmarkTaggedItem item;
  item.id = QStringLiteral("stable"); item.eventId = QStringLiteral("7");
  item.title = QStringLiteral("before"); item.note = QStringLiteral("note");
  item.analysis = QStringLiteral("analysis"); item.conclusion = QStringLiteral("conclusion");
  item.screenshotPath = QStringLiteral("local.png"); item.confidence = Confidence::Inferred;
  file.items.append(item);
  BookmarkMutationPlan plan;
  QJsonObject patch{{QStringLiteral("id"), item.id}, {QStringLiteral("title"), QStringLiteral("after")},
      {QStringLiteral("note"), QString()}, {QStringLiteral("analysis"), QString()},
      {QStringLiteral("conclusion"), QString()}, {QStringLiteral("screenshotPath"), QString()},
      {QStringLiteral("eventId"), 9}};
  REQUIRE(PlanBookmarkMutation(file, patch, QStringLiteral("update"), 77, plan).ok);
  CHECK(plan.record.id == item.id);
  CHECK(plan.record.title == QStringLiteral("after"));
  CHECK(plan.record.note.isEmpty()); CHECK(plan.record.analysis.isEmpty());
  CHECK(plan.record.conclusion.isEmpty()); CHECK(plan.record.screenshotPath.isEmpty());
  CHECK(bool(plan.record.confidence == Confidence::Inferred));
  CHECK(plan.oldEventId == 7); CHECK(plan.newEventId == 9);
  CHECK(ParseBookmarkFile(SerializeBookmarkFile(plan.file)).items[0].note.isEmpty());
  REQUIRE(PlanBookmarkMutation(file, QJsonObject{{QStringLiteral("id"), item.id}}, QStringLiteral("update"), 77, plan).ok);
  CHECK(plan.record.note == item.note); CHECK(plan.newEventId == 7);
  REQUIRE(PlanBookmarkMutation(file, QJsonObject{{QStringLiteral("id"), item.id}, {QStringLiteral("eventId"), 0}}, QStringLiteral("update"), 77, plan).ok);
  CHECK(plan.oldEventId == 7); CHECK(plan.newEventId == 0);
  CHECK(plan.record.eventId == QStringLiteral("0"));
  REQUIRE(PlanBookmarkMutation(file, QJsonObject{{QStringLiteral("id"), item.id}}, QStringLiteral("remove"), 77, plan).ok);
  CHECK(plan.removed); CHECK(plan.oldEventId == 7); CHECK(plan.file.items.isEmpty());
  CHECK_FALSE(PlanBookmarkMutation(file, QJsonObject{{QStringLiteral("id"), QStringLiteral("unknown")}}, QStringLiteral("update"), 77, plan).ok);
  CHECK_FALSE(PlanBookmarkMutation(file, QJsonObject{{QStringLiteral("id"), item.id}, {QStringLiteral("title"), QStringLiteral("new")}}, QStringLiteral("add"), 77, plan).ok);
  CHECK_FALSE(PlanBookmarkMutation(file, QJsonObject{{QStringLiteral("title"), QStringLiteral("new")}}, QStringLiteral("add"), 7, plan).ok);
  REQUIRE(PlanBookmarkMutation(file, QJsonObject{{QStringLiteral("title"), QStringLiteral("new")}}, QStringLiteral("add"), 8, plan).ok);
  CHECK(plan.record.id != item.id); CHECK(plan.newEventId == 8); CHECK(plan.file.items.size() == 2);
  CHECK(plan.file.items[0].note == item.note);
  const QString generated = plan.record.id;
  REQUIRE(PlanBookmarkMutation(file, QJsonObject{{QStringLiteral("title"), QStringLiteral("new")}, {QStringLiteral("eventId"), 0}}, QStringLiteral("add"), 8, plan).ok);
  CHECK(plan.record.id != generated); CHECK(plan.newEventId == 0);
  REQUIRE(PlanBookmarkMutation(file, QJsonObject{{QStringLiteral("eventId"), 7}, {QStringLiteral("note"), QStringLiteral("legacy")}}, QStringLiteral("update"), 77, plan).ok);
  CHECK(plan.record.id == item.id); CHECK(plan.record.note == QStringLiteral("legacy"));
  REQUIRE(PlanBookmarkMutation(file, QJsonObject{{QStringLiteral("eventId"), 12}}, QStringLiteral("update"), 77, plan).ok);
  CHECK(plan.record.id == QStringLiteral("bm-12")); CHECK(plan.newEventId == 12);
  CHECK_FALSE(PlanBookmarkMutation(file, QJsonObject{{QStringLiteral("eventId"), -1}}, QStringLiteral("update"), 77, plan).ok);
  CHECK_FALSE(PlanBookmarkMutation(file, QJsonObject{{QStringLiteral("eventId"), 1.5}}, QStringLiteral("update"), 77, plan).ok);
  file.items.append(item);
  CHECK_FALSE(PlanBookmarkMutation(file, patch, QStringLiteral("update"), 77, plan).ok);
}

TEST_CASE("Compatibility native notes are authoritative even when empty", "[mcp][compatibility][bookmark]")
{
  using namespace MCP;
  rdcarray<EventBookmark> native;
  EventBookmark mark(7); mark.text = ""; native.push_back(mark);
  BookmarkFile file;
  BookmarkTaggedItem item; item.id = QStringLiteral("stable"); item.eventId = QStringLiteral("7");
  item.title = QStringLiteral("title"); item.note = QStringLiteral("stale sidecar note"); file.items.append(item);
  QVector<BookmarkTaggedItem> merged = MergeBookmarkSidecar(native, file);
  REQUIRE(merged.size() == 1); CHECK(merged[0].note.isEmpty()); CHECK(merged[0].id == item.id);
  merged = MergeBookmarkSidecar(native, BookmarkFile());
  REQUIRE(merged.size() == 1); CHECK(merged[0].id == QStringLiteral("bm-7"));
  mark.text = "same"; native[0] = mark; item.eventId = QStringLiteral("9"); item.note = QStringLiteral("same"); file.items[0] = item;
  merged = MergeBookmarkSidecar(native, file);
  REQUIRE(merged.size() == 2); CHECK(merged[0].id == QStringLiteral("bm-7"));
  item.eventId = QStringLiteral("0"); file.items[0] = item;
  merged = MergeBookmarkSidecar(native, ParseBookmarkFile(SerializeBookmarkFile(file)));
  REQUIRE(merged.size() == 2); CHECK(merged[1].id == item.id);
  CHECK(merged[1].eventId == QStringLiteral("0"));
}

TEST_CASE("Compatibility mutation authorization and advertised schemas", "[mcp][compatibility]")
{
  using namespace MCP;
  QueryAdapter adapter(NULL); ToolRegistry registry; CallContext call;
  RegisterCoreTools(registry, adapter);
  const int core = registry.Count();
  RegisterCompatibilityTools(registry, adapter); CHECK(registry.Count() == core + 3);
  RegisterCompatibilityTools(registry, adapter); CHECK(registry.Count() == core + 3);
  for(const char *name : {"renderdoc_addBookmark", "renderdoc_updateBookmark", "renderdoc_removeBookmark",
       "renderdoc_exportInvestigationReport", "renderdoc_diagnoseEnvironment"})
  {
    REQUIRE(registry.Find(QString::fromLatin1(name)) != NULL);
    CHECK(bool(registry.Find(QString::fromLatin1(name))->definition.effects == ToolEffects::SideEffect));
  }
  for(const char *op : {"add", "update", "remove"})
  {
    QJsonObject args{{QStringLiteral("id"), QStringLiteral("stable")}, {QStringLiteral("title"), QStringLiteral("title")}, {QStringLiteral("confirm"), false}};
    CHECK_FALSE(AuthorizeBookmarkMutation(QString::fromLatin1(op), args).ok);
    args.remove(QStringLiteral("confirm")); CHECK(AuthorizeBookmarkMutation(QString::fromLatin1(op), args).ok);
  }
  const QJsonObject legacy{{QStringLiteral("eventId"), 7}};
  CHECK_FALSE(AuthorizeBookmarkMutation(QStringLiteral("update"), legacy).ok);
  QJsonObject confirmed = legacy; confirmed[QStringLiteral("confirm")] = true;
  CHECK(AuthorizeBookmarkMutation(QStringLiteral("update"), confirmed).ok);
  CHECK(bool(registry.Call(QStringLiteral("renderdoc_updateBookmark"), legacy, call).error.code == ErrorCode::SideEffectNotConfirmed));
  CHECK(bool(registry.Call(QStringLiteral("renderdoc_addBookmark"), QJsonObject(), call).error.code == ErrorCode::InvalidToolInput));
  CHECK(bool(registry.Call(QStringLiteral("renderdoc_addBookmark"), QJsonObject{{QStringLiteral("title"), QStringLiteral("title")}}, call).error.code == ErrorCode::NoCaptureLoaded));
  CHECK(bool(registry.Call(QStringLiteral("renderdoc_removeBookmark"), QJsonObject{{QStringLiteral("id"), QStringLiteral("stable")}}, call).error.code == ErrorCode::NoCaptureLoaded));
  CHECK_FALSE(registry.Call(QStringLiteral("renderdoc_exportInvestigationReport"), QJsonObject(), call).ok);
}

TEST_CASE("Compatibility output publication is atomic no-replace and cleans temporaries", "[mcp][compatibility][output]")
{
  using namespace MCP;
  QTemporaryDir dir; REQUIRE(dir.isValid()); CallContext call;
  const QString path = dir.filePath(QStringLiteral("report.json"));
  const QByteArray bytes("{\"evidence\":\"UTF-8\"}\n");
  ToolResult result = WriteCompatibilityOutput(path, bytes, call); INFO(result.error.message); REQUIRE(result.ok);
  CHECK(result.structured.value(QStringLiteral("byteSize")).toInt() == bytes.size());
  QFile file(path); REQUIRE(file.open(QIODevice::ReadOnly)); CHECK(file.readAll() == bytes); file.close();
  CHECK_FALSE(WriteCompatibilityOutput(path, QByteArray("replacement"), call).ok);
  REQUIRE(file.open(QIODevice::ReadOnly)); CHECK(file.readAll() == bytes); file.close();
  CHECK_FALSE(WriteCompatibilityOutput(dir.filePath(QStringLiteral("missing/report")), bytes, call).ok);
  CHECK_FALSE(WriteCompatibilityOutput(dir.path(), bytes, call).ok);
  CHECK_FALSE(WriteCompatibilityOutput(QString(), bytes, call).ok);
  volatile bool cancelled = true; call.cancelled = &cancelled;
  CHECK_FALSE(WriteCompatibilityOutput(dir.filePath(QStringLiteral("cancelled")), bytes, call).ok);
  CHECK_FALSE(QFile::exists(dir.filePath(QStringLiteral("cancelled")))); call.cancelled = NULL;
  call.deadlineMs = QDateTime::currentMSecsSinceEpoch() - 1;
  CHECK_FALSE(WriteCompatibilityOutput(dir.filePath(QStringLiteral("expired")), bytes, call).ok);
  call.deadlineMs = 0;
  ToolResult a, b; const QString race = dir.filePath(QStringLiteral("race"));
  std::thread first([&]() { CallContext c; a = WriteCompatibilityOutput(race, QByteArray("first"), c); });
  std::thread second([&]() { CallContext c; b = WriteCompatibilityOutput(race, QByteArray("second"), c); });
  first.join(); second.join(); CHECK(a.ok != b.ok);
  QFile raced(race); REQUIRE(raced.open(QIODevice::ReadOnly));
  CHECK(raced.readAll() == (a.ok ? QByteArray("first") : QByteArray("second"))); raced.close();
  const QStringList entries = QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
  CHECK(entries.size() == 2);
}

TEST_CASE("Compatibility diagnosis works without capture and really exports", "[mcp][compatibility][diagnostics]")
{
  using namespace MCP;
  QueryAdapter adapter(NULL); ToolRegistry registry; RegisterCompatibilityTools(registry, adapter);
  CallContext call; ToolResult result = registry.Call(QStringLiteral("renderdoc_diagnoseEnvironment"), QJsonObject(), call);
  REQUIRE(result.ok); const QJsonObject report = result.structured.value(QStringLiteral("report")).toObject();
  CHECK(report.value(QStringLiteral("native")).toObject().value(QStringLiteral("inProcess")).toBool());
  CHECK(report.value(QStringLiteral("session")).toObject().value(QStringLiteral("phase")).toString() == QStringLiteral("idle"));
  CHECK(report.value(QStringLiteral("mcp")).toObject().value(QStringLiteral("supportedProtocolVersion")).toString() == QString::fromLatin1(kLatestProtocolVersion));
  CHECK(report.value(QStringLiteral("deviceProtocols")).isArray());
  CHECK(report.value(QStringLiteral("adb")).toObject().value(QStringLiteral("devices")).isNull());
  CHECK_FALSE(result.structured.value(QStringLiteral("written")).toBool());
  QTemporaryDir dir; REQUIRE(dir.isValid()); const QString path = dir.filePath(QStringLiteral("diagnosis.json"));
  QJsonObject args{{QStringLiteral("outputPath"), path}, {QStringLiteral("format"), QStringLiteral("json")}};
  result = registry.Call(QStringLiteral("renderdoc_diagnoseEnvironment"), args, call); INFO(result.error.message); REQUIRE(result.ok);
  QFile output(path); REQUIRE(output.open(QIODevice::ReadOnly));
  CHECK(bool(QJsonDocument::fromJson(output.readAll()).object() == result.structured.value(QStringLiteral("report")).toObject())); output.close();
  CHECK_FALSE(registry.Call(QStringLiteral("renderdoc_diagnoseEnvironment"), args, call).ok);
  args[QStringLiteral("outputPath")] = dir.filePath(QStringLiteral("refused")); args[QStringLiteral("confirm")] = false;
  CHECK(bool(registry.Call(QStringLiteral("renderdoc_diagnoseEnvironment"), args, call).error.code == ErrorCode::SideEffectNotConfirmed));
  CHECK_FALSE(QFile::exists(args.value(QStringLiteral("outputPath")).toString()));
  args.remove(QStringLiteral("outputPath")); args.remove(QStringLiteral("confirm")); args[QStringLiteral("format")] = QStringLiteral("markdown");
  result = registry.Call(QStringLiteral("renderdoc_diagnoseEnvironment"), args, call); REQUIRE(result.ok);
  CHECK(result.structured.value(QStringLiteral("content")).toString().startsWith(QStringLiteral("# Native RenderDoc")));
}
TEST_CASE("Compatibility investigation document keeps inline evidence and propagates export failure", "[mcp][compatibility][output]")
{
  using namespace MCP;
  QTemporaryDir dir; REQUIRE(dir.isValid()); CallContext call;
  const QString content = QString::fromUtf8("# Investigation\n\nEvidence: \xe4\xb9\xa6\xe7\xad\xbe\n");
  const QJsonObject report{{QStringLiteral("capturePath"), QStringLiteral("frame.rdc")}};
  const ToolResult document = ToolResult::Success(QJsonObject{{QStringLiteral("content"), content},
      {QStringLiteral("format"), QStringLiteral("markdown")}, {QStringLiteral("report"), report}});
  ToolResult result = CompatibilityDocument(document, QJsonObject(), call);
  REQUIRE(result.ok); CHECK(result.text == content); CHECK_FALSE(result.structured.value(QStringLiteral("written")).toBool());
  const QString path = dir.filePath(QStringLiteral("investigation.md"));
  QJsonObject args{{QStringLiteral("outputPath"), path}};
  result = CompatibilityDocument(document, args, call); INFO(result.error.message); REQUIRE(result.ok);
  CHECK(result.structured.value(QStringLiteral("written")).toBool());
  CHECK(bool(result.structured.value(QStringLiteral("report")).toObject() == report));
  QFile output(path); REQUIRE(output.open(QIODevice::ReadOnly)); CHECK(output.readAll() == content.toUtf8()); output.close();
  CHECK_FALSE(CompatibilityDocument(document, args, call).ok);
  args[QStringLiteral("outputPath")] = dir.filePath(QStringLiteral("failed.md"));
  const ToolResult failure = ToolResult::Failure(Error(ErrorCode::ReplayUnavailable, QStringLiteral("replay unavailable")));
  result = CompatibilityDocument(failure, args, call); CHECK_FALSE(result.ok);
  CHECK(bool(result.error.code == ErrorCode::ReplayUnavailable)); CHECK_FALSE(QFile::exists(args.value(QStringLiteral("outputPath")).toString()));
  volatile bool cancelled = true; call.cancelled = &cancelled;
  CHECK_FALSE(CompatibilityDocument(document, args, call).ok); CHECK_FALSE(QFile::exists(args.value(QStringLiteral("outputPath")).toString()));
}
#endif
