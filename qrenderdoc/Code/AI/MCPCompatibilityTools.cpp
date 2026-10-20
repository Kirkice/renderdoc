#include "MCPCompatibilityTools.h"
#include "MCPQueryAdapter.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryFile>
#include <QUuid>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <errno.h>
#include <string.h>
#endif

namespace MCP
{
namespace
{
ToolResult Fail(ErrorCode code, const QString &message)
{
  return ToolResult::Failure(Error(code, message));
}
ToolResult Interrupted(CallContext &call)
{
  if(call.IsCancelled()) return Fail(ErrorCode::Cancelled, QStringLiteral("Request cancelled"));
  if(call.IsExpired()) return Fail(ErrorCode::Timeout, QStringLiteral("Request deadline passed"));
  return ToolResult::Success(QJsonObject());
}
QJsonObject Property(const char *type)
{
  return QJsonObject{{QStringLiteral("type"), QString::fromLatin1(type)}};
}
QJsonObject Schema(const QJsonObject &properties, const QJsonArray &required = QJsonArray())
{
  return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                    {QStringLiteral("properties"), properties}, {QStringLiteral("required"), required},
                    {QStringLiteral("additionalProperties"), false}};
}
}

ToolResult AuthorizeBookmarkMutation(const QString &operation, const QJsonObject &args)
{
  // HTTP currently has no generic effects gate. Explicit opt-out always wins;
  // extension add/id calls are themselves the side-effect-authorized shape.
  if(args.contains(QStringLiteral("confirm")))
  {
    if(!args.value(QStringLiteral("confirm")).isBool() || !args.value(QStringLiteral("confirm")).toBool())
      return Fail(ErrorCode::SideEffectNotConfirmed, QStringLiteral("Explicit confirm=false refuses bookmark mutation"));
  }
  else if(operation != QStringLiteral("add") && args.value(QStringLiteral("id")).toString().isEmpty())
    return Fail(ErrorCode::SideEffectNotConfirmed, QStringLiteral("Legacy eventId bookmark mutation requires confirm=true"));
  return ToolResult::Success(QJsonObject());
}

ToolResult PlanBookmarkMutation(const BookmarkFile &existing, const QJsonObject &args,
                                const QString &operation, uint32_t currentEvent,
                                BookmarkMutationPlan &plan)
{
  plan = BookmarkMutationPlan();
  plan.file = existing;
  if(operation != QStringLiteral("add") && operation != QStringLiteral("update") && operation != QStringLiteral("remove"))
    return Fail(ErrorCode::InvalidParams, QStringLiteral("Unknown bookmark operation"));
  QString id = args.value(QStringLiteral("id")).toString();
  const bool explicitId = !id.isEmpty();
  const bool hasEvent = args.contains(QStringLiteral("eventId"));
  uint32_t eid = currentEvent;
  if(hasEvent)
  {
    const QJsonValue value = args.value(QStringLiteral("eventId"));
    const double n = value.toDouble(-1);
    if(!value.isDouble() || n < 0 || n > double(UINT32_MAX) || double(uint32_t(n)) != n)
      return Fail(ErrorCode::InvalidParams, QStringLiteral("eventId must be a uint32 event index"));
    eid = uint32_t(n);
  }
  QSet<QString> ids;
  for(const BookmarkTaggedItem &item : existing.items)
  {
    if(!item.id.isEmpty() && ids.contains(item.id))
      return Fail(ErrorCode::InvalidParams, QStringLiteral("Bookmark sidecar contains duplicate identities; refusing mutation"));
    if(!item.id.isEmpty()) ids.insert(item.id);
  }
  int found = -1;
  for(int i = 0; i < existing.items.size(); ++i)
  {
    const BookmarkTaggedItem &item = existing.items[i];
    if(explicitId ? item.id == id : (hasEvent && item.eventId.toUInt() == eid))
    {
      if(found >= 0)
        return Fail(ErrorCode::InvalidParams, QStringLiteral("Ambiguous event bookmark; select its stable id"));
      found = i;
    }
  }
  if(operation == QStringLiteral("add"))
  {
    if(explicitId && ids.contains(id))
      return Fail(ErrorCode::InvalidParams, QStringLiteral("Bookmark id already exists; add never overwrites"));
    if(args.value(QStringLiteral("title")).toString().isEmpty())
      return Fail(ErrorCode::InvalidParams, QStringLiteral("A non-empty bookmark title is required"));
    if(!explicitId)
      do { id = QStringLiteral("bm-") + QUuid::createUuid().toString().mid(1, 36); } while(ids.contains(id));
    found = -1;
  }
  else
  {
    if(!explicitId && !hasEvent)
      return Fail(ErrorCode::InvalidParams, QStringLiteral("A bookmark id or legacy eventId is required"));
    if(explicitId && found < 0)
      return Fail(ErrorCode::InvalidParams, QStringLiteral("Bookmark id was not found"));
    if(operation == QStringLiteral("remove") && found < 0)
      return Fail(ErrorCode::InvalidParams, QStringLiteral("Bookmark was not found"));
    // Preserve the legacy event-based upsert, but not unknown explicit id upsert.
    if(found < 0)
    {
      id = QStringLiteral("bm-%1").arg(eid);
      if(ids.contains(id))
        return Fail(ErrorCode::InvalidParams, QStringLiteral("Legacy generated bookmark id already belongs to another record"));
    }
    else id = existing.items[found].id;
  }
  if(found >= 0)
  {
    plan.record = existing.items[found];
    plan.oldEventId = plan.record.eventId.toUInt();
  }
  if(operation == QStringLiteral("remove"))
  {
    plan.file.items.remove(found);
    plan.removed = true;
  }
  else
  {
    plan.record.id = id;
    // Explicit zero is a detached record, not an old record lacking an event.
    // Preserve it so legacy note matching cannot attach it to an unrelated event.
    if(found < 0 || hasEvent) plan.record.eventId = QString::number(eid);
    struct Field { const char *name; QString BookmarkTaggedItem::*member; };
    const Field fields[] = {{"title", &BookmarkTaggedItem::title}, {"note", &BookmarkTaggedItem::note},
      {"conclusion", &BookmarkTaggedItem::conclusion}, {"analysis", &BookmarkTaggedItem::analysis},
      {"screenshotPath", &BookmarkTaggedItem::screenshotPath}};
    for(const Field &field : fields)
    {
      const QString key = QString::fromLatin1(field.name);
      if(!args.contains(key)) continue;
      if(!args.value(key).isString()) return Fail(ErrorCode::InvalidParams, key + QStringLiteral(" must be a string"));
      plan.record.*(field.member) = args.value(key).toString();
    }
    if(args.contains(QStringLiteral("confidence")))
    {
      const QString confidence = args.value(QStringLiteral("confidence")).toString();
      if(confidence == QStringLiteral("confirmed")) plan.record.confidence = Confidence::Confirmed;
      else if(confidence == QStringLiteral("inferred")) plan.record.confidence = Confidence::Inferred;
      else if(confidence == QStringLiteral("toVerify")) plan.record.confidence = Confidence::ToVerify;
      else if(confidence == QStringLiteral("low")) plan.record.confidence = Confidence::Low;
      else return Fail(ErrorCode::InvalidParams, QStringLiteral("Unknown bookmark confidence"));
    }
    plan.newEventId = plan.record.eventId.toUInt();
    // Native bookmarks are keyed by event. Refuse a collision rather than silently
    // overwriting another id's native note or inventing a multi-bookmark UI model.
    for(int i = 0; i < existing.items.size(); ++i)
      if(i != found && plan.newEventId && existing.items[i].eventId.toUInt() == plan.newEventId)
        return Fail(ErrorCode::InvalidParams, QStringLiteral("Event already has another bookmark; use its id or select another event"));
    if(found < 0) plan.file.items.append(plan.record);
    else plan.file.items[found] = plan.record;
  }
  plan.file.version = 1;
  return ToolResult::Success(QJsonObject{{QStringLiteral("bookmark"), BookmarkTaggedItemToJson(plan.record)},
      {QStringLiteral("id"), plan.record.id}, {QStringLiteral("removed"), plan.removed}});
}

ToolResult WriteCompatibilityOutput(const QString &path, const QByteArray &content, CallContext &call)
{
  ToolResult status = Interrupted(call);
  if(!status.ok) return status;
  if(path.isEmpty() || path.contains(QChar(0)))
    return Fail(ErrorCode::InvalidParams, QStringLiteral("outputPath must be a non-empty filesystem path"));
  const QFileInfo destination(path);
  const QString absolute = destination.absoluteFilePath();
  if(destination.exists() || destination.isSymLink())
    return Fail(ErrorCode::InvalidParams, QStringLiteral("outputPath already exists; refusing to overwrite: ") + absolute);
  if(!QFileInfo(destination.absolutePath()).isDir())
    return Fail(ErrorCode::InvalidParams, QStringLiteral("outputPath parent directory does not exist: ") + destination.absolutePath());
  QString temporaryPath;
  struct TemporaryCleanup
  {
    QString &path;
    ~TemporaryCleanup() { if(!path.isEmpty()) QFile::remove(path); }
  } cleanup{temporaryPath};
  {
    QTemporaryFile temporary(destination.absolutePath() + QStringLiteral("/.renderdoc-report-XXXXXX"));
    if(!temporary.open()) return Fail(ErrorCode::InternalError, temporary.errorString());
    if(temporary.write(content) != content.size() || !temporary.flush())
      return Fail(ErrorCode::InternalError, temporary.errorString());
    temporaryPath = temporary.fileName();
    temporary.setAutoRemove(false);
    // QTemporaryFile::close retains its native handle for reopening. Destroy it
    // before publication, otherwise Windows refuses to move the open file.
  }
  status = Interrupted(call);
  if(!status.ok) return status;
  // QFile::rename may fall back to copying. Use an OS no-replace publication so
  // racing creators cannot be overwritten and readers never see a partial file.
#if defined(_WIN32)
  if(!MoveFileExW(reinterpret_cast<LPCWSTR>(temporaryPath.utf16()),
                  reinterpret_cast<LPCWSTR>(absolute.utf16()), MOVEFILE_WRITE_THROUGH))
    return Fail(ErrorCode::InternalError, QStringLiteral("Atomic report publication failed (Windows error %1): %2").arg(GetLastError()).arg(absolute));
#else
  if(::link(QFile::encodeName(temporaryPath).constData(), QFile::encodeName(absolute).constData()) != 0)
    return Fail(ErrorCode::InternalError, QStringLiteral("Atomic report publication failed: ") + QString::fromLocal8Bit(strerror(errno)));
#endif
  return ToolResult::Success(QJsonObject{{QStringLiteral("outputPath"), absolute},
      {QStringLiteral("byteSize"), double(content.size())}, {QStringLiteral("written"), true}});
}

ToolResult CompatibilityDocument(ToolResult document, const QJsonObject &args, CallContext &call)
{
  if(!document.ok) return document;
  ToolResult status = Interrupted(call);
  if(!status.ok) return status;
  if(args.contains(QStringLiteral("outputPath")))
  {
    if(args.contains(QStringLiteral("confirm")) && !args.value(QStringLiteral("confirm")).toBool())
      return Fail(ErrorCode::SideEffectNotConfirmed, QStringLiteral("Explicit confirm=false refuses file export"));
    const ToolResult written = WriteCompatibilityOutput(args.value(QStringLiteral("outputPath")).toString(),
        document.structured.value(QStringLiteral("content")).toString().toUtf8(), call);
    if(!written.ok) return written;
    for(auto it = written.structured.begin(); it != written.structured.end(); ++it)
      document.structured[it.key()] = it.value();
  }
  else
  {
    document.structured[QStringLiteral("written")] = false;
    document.structured[QStringLiteral("outputPath")] = QJsonValue(QJsonValue::Null);
  }
  document.text = document.structured.value(QStringLiteral("content")).toString();
  return document;
}

void RegisterCompatibilityTools(ToolRegistry &registry, QueryAdapter &adapter, bool legacyCoreOnly)
{
  QJsonObject bookmarkProperties;
  for(const char *key : {"id", "title", "note", "conclusion", "analysis", "screenshotPath", "confidence"})
    bookmarkProperties[QString::fromLatin1(key)] = Property("string");
  bookmarkProperties[QStringLiteral("id")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}};
  bookmarkProperties[QStringLiteral("eventId")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
      {QStringLiteral("minimum"), 0}, {QStringLiteral("maximum"), double(UINT32_MAX)}};
  bookmarkProperties[QStringLiteral("confirm")] = Property("boolean");
  for(const char *operation : {"add", "update", "remove"})
  {
    const QString op = QString::fromLatin1(operation);
    if(legacyCoreOnly && op == QStringLiteral("add")) continue;
    const QString name = QStringLiteral("renderdoc_") + op + QStringLiteral("Bookmark");
    if(registry.Contains(name)) continue;
    QJsonObject properties = bookmarkProperties;
    if(op == QStringLiteral("remove"))
      for(const char *key : {"title", "note", "conclusion", "analysis", "screenshotPath", "confidence"}) properties.remove(QString::fromLatin1(key));
    QJsonArray required;
    if(op == QStringLiteral("add"))
    {
      required.append(QStringLiteral("title"));
      properties[QStringLiteral("title")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}};
    }
    registry.Add(ToolDefinition{name, name,
        QStringLiteral("Mutate native investigation bookmarks and the sidecar. Extension add/id shape authorizes mutation; legacy eventId shape requires confirm=true. Explicit confirm=false always refuses. Supplied empty strings clear fields; omitted fields are retained. Add never overwrites an existing id or native event note."),
        ToolEffects::SideEffect, ToolRequirements::LoadedCapture, Schema(properties, required)},
        [&adapter, op](const QJsonObject &args, CallContext &call) {
          ToolResult authorized = AuthorizeBookmarkMutation(op, args);
          return authorized.ok ? adapter.BookmarkTool(op, args, call) : authorized;
        });
  }
  const QString list = QStringLiteral("renderdoc_listBookmarks");
  if(!registry.Contains(list))
  {
    QJsonObject properties{{QStringLiteral("maxCount"), Property("integer")}, {QStringLiteral("fields"), Property("array")}};
    registry.Add(ToolDefinition{list, list, QStringLiteral("List native bookmarks merged with stable sidecar identities and investigation fields."),
        ToolEffects::ReadOnly, ToolRequirements::LoadedCapture, Schema(properties)},
        [&adapter](const QJsonObject &args, CallContext &) {
          QStringList fields;
          for(const QJsonValue &v : args.value(QStringLiteral("fields")).toArray()) fields.append(v.toString());
          return adapter.BookmarksExtended(args.value(QStringLiteral("maxCount")).toInt(), fields);
        });
  }
  const QString extended = QStringLiteral("renderdoc_bookmarksExtended");
  if(!registry.Contains(extended))
    registry.Add(ToolDefinition{extended, extended,
        QStringLiteral("Legacy extended bookmark query, including stable identities and investigation fields."),
        ToolEffects::ReadOnly, ToolRequirements::LoadedCapture,
        Schema(QJsonObject{{QStringLiteral("maxCount"), Property("integer")}, {QStringLiteral("fields"), Property("array")}})},
        [&adapter](const QJsonObject &args, CallContext &) {
          QStringList fields;
          for(const QJsonValue &v : args.value(QStringLiteral("fields")).toArray()) fields.append(v.toString());
          return adapter.BookmarksExtended(args.value(QStringLiteral("maxCount")).toInt(), fields);
        });
  if(legacyCoreOnly) return;
  QJsonObject documentProperties{{QStringLiteral("format"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
      {QStringLiteral("enum"), QJsonArray{QStringLiteral("markdown"), QStringLiteral("json")}}}},
      {QStringLiteral("outputPath"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}}},
      {QStringLiteral("confirm"), Property("boolean")}};
  const QString exportName = QStringLiteral("renderdoc_exportInvestigationReport");
  if(!registry.Contains(exportName))
  {
    QJsonObject properties = documentProperties;
    properties[QStringLiteral("timingLimit")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
        {QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 200}};
    registry.Add(ToolDefinition{exportName, exportName,
        QStringLiteral("Export the native investigation report inline, optionally atomically creating outputPath. Never overwrites existing files. Parent directory must exist."),
        ToolEffects::SideEffect, ToolRequirements::LoadedCapture, Schema(properties)},
        [&adapter](const QJsonObject &args, CallContext &call) {
          if(args.contains(QStringLiteral("confirm")) && !args.value(QStringLiteral("confirm")).toBool())
            return Fail(ErrorCode::SideEffectNotConfirmed, QStringLiteral("Explicit confirm=false refuses export"));
          return CompatibilityDocument(adapter.BuildInvestigationReport(args.value(QStringLiteral("format")).toString(QStringLiteral("markdown")),
              args.value(QStringLiteral("timingLimit")).toInt(20), true, call), args, call);
        });
  }
  const QString diagnose = QStringLiteral("renderdoc_diagnoseEnvironment");
  if(!registry.Contains(diagnose))
    registry.Add(ToolDefinition{diagnose, diagnose,
        QStringLiteral("Report observed native runtime, capture, application session, MCP configuration and device protocol/ADB configuration without launching adb, changing the environment or connecting targets. Optional outputPath creates a new report file atomically; never overwrites."),
        ToolEffects::SideEffect, ToolRequirements::None, Schema(documentProperties)},
        [&adapter](const QJsonObject &args, CallContext &call) {
          return CompatibilityDocument(adapter.DiagnoseEnvironment(args.value(QStringLiteral("format")).toString(QStringLiteral("json")), call), args, call);
        });
}
}    // namespace MCP
