/* Native resource inspection contracts. Capture ownership stays in QueryAdapter. */
#pragma once
#include "MCPQueryAdapter.h"

namespace MCP
{
struct DrawPreviewRequest
{
  uint32_t eventId = 0;
  int channelExtract = -1;
  DebugOverlay overlay = DebugOverlay::NoOverlay;
  QString overlayMode = QStringLiteral("none");
  ResourceId resourceId;
  ResourceId overlayResourceId;
  bool baseGammaEnabled = true;
  int width = 0;
  int height = 0;
  QString label = QStringLiteral("Current Draw Preview");
};

bool ParseResourceToolId(const QJsonValue &value, ResourceId &id);
ToolResult ParseResourceSelector(const QJsonObject &args, ResourceId &id, bool required);
bool ReadResourceInteger(const QJsonObject &args, const char *name, int64_t min, int64_t max,
                         int64_t &out);
// Pagination is applied after the owning adapter has normalized and filtered the inventory.
QJsonObject ResourcePage(const QJsonArray &resources, int limit, int offset);
QJsonObject LimitResourceUsage(const QJsonObject &usage, int maxEvents);
ToolResult SelectResourceEvent(const rdcarray<ActionDescription> &actions, uint32_t requested,
                               bool zeroMeansEnd, const std::function<void(uint32_t)> &select,
                               uint32_t &effective);
struct TextureReadRequest
{
  ResourceId id;
  Subresource sub = {0, 0, 0};
  uint32_t eventId = 0;
  bool selectEvent = false;
  int channelExtract = -1;
  bool png = false;
  uint64_t maxBytes = 65536;
};
ToolResult ParseTextureReadRequest(const QJsonObject &args, TextureReadRequest &request);
ToolResult ReadTexturePNG(IReplayController &replay, const TextureReadRequest &request,
                          CallContext &call);
ToolResult ParseDrawPreviewRequest(const QJsonObject &args, DrawPreviewRequest &request);
// Input is the documented ReplayOutput tightly packed RGB readback, not texture bytes.
ToolResult EncodePreviewPNG(const bytebuf &rgb, int width, int height, CallContext &call);
ToolResult CompositePreviewRGB(bytebuf &base, const bytebuf &overlay, const bytebuf &alpha);
ToolResult ReadDrawPreview(IReplayController &replay, const DrawPreviewRequest &request,
                           CallContext &call);
void RegisterResourceTools(ToolRegistry &registry, QueryAdapter &adapter);
}
