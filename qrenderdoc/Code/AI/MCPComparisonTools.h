#pragma once

#include "MCPQueryAdapter.h"
#include "MCPRegressionDiff.h"

namespace MCP
{
enum class ComparisonKind
{
  Captures,
  EventTimings,
  ResourceMemory,
  ResourceLeaks,
};

// Detached facts only. The adapter owns the file/controller and collects these
// on its independent replay worker; comparison never borrows the UI controller.
struct ComparisonSnapshot
{
  RegressionSnapshot regression;
  QVector<ResourceMemoryRow> resources;
  QVector<bool> bytesKnown;
  QHash<uint32_t, double> timings;
  QString timingStatus;
};

QJsonObject BuildComparisonResult(const ComparisonSnapshot &baseline,
                                 const ComparisonSnapshot &candidate, ComparisonKind kind,
                                 int limit);
ToolResult CompareCapturePaths(QueryAdapter &adapter, const QJsonObject &args,
                               CallContext &call, ComparisonKind kind);
void RegisterComparisonTools(ToolRegistry &registry, QueryAdapter &adapter);
}    // namespace MCP
