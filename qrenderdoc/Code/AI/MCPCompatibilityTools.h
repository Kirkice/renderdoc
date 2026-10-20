#pragma once

#include "MCPAnalysisModel.h"
#include "MCPToolRegistry.h"

namespace MCP
{
class QueryAdapter;

// Presence-aware patch planning. Native notes are folded in by the adapter before
// planning; no tool or model helper accesses CaptureContext independently.
struct BookmarkMutationPlan
{
  BookmarkFile file;
  BookmarkTaggedItem record;
  uint32_t oldEventId = 0;
  uint32_t newEventId = 0;
  bool removed = false;
};
ToolResult PlanBookmarkMutation(const BookmarkFile &existing, const QJsonObject &args,
                                const QString &operation, uint32_t currentEvent,
                                BookmarkMutationPlan &plan);
ToolResult AuthorizeBookmarkMutation(const QString &operation, const QJsonObject &args);
// Publishes a complete new file in the existing parent directory. Never replaces
// an existing destination (including symlinks); temporary files are RAII-owned.
ToolResult WriteCompatibilityOutput(const QString &path, const QByteArray &content,
                                    CallContext &call);
ToolResult CompatibilityDocument(ToolResult document, const QJsonObject &args, CallContext &call);
void RegisterCompatibilityTools(ToolRegistry &registry, QueryAdapter &adapter, bool legacyCoreOnly = false);
}    // namespace MCP
