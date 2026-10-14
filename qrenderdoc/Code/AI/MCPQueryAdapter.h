/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2026 Baldur Karlsson
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

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <stdint.h>
#include "MCPToolRegistry.h"
#include "MCPTypes.h"
// Pulls in ResourceId, ResourceFormat, and the replay enums (ResourceType,
// TextureType, CompType, GraphicsAPI, TextureCategory, BufferCategory) that the
// serialisation helpers below reference. This is the same header the rest of
// qrenderdoc uses for the replay API surface.
#include "renderdoc_replay.h"

struct ICaptureContext;
struct IReplayManager;
struct IReplayController;

namespace MCP
{

// P0-5 CaptureContext snapshot adapter
// P0-6 Replay query dispatcher
// --------------------------------
// MCPQueryAdapter is the only part of the AI/MCP module that is allowed to touch
// the live qrenderdoc state. It has two clearly separated responsibilities:
//
//  * snapshot reads (P0-5) go through ICaptureContext directly. These read the
//    already-materialised data that the UI itself displays - the capture filename,
//    the selected event, the action tree, the resource/texture/buffer lists, the
//    frame statistics and the bookmarks. No replay thread is involved and the data
//    is safe to read from any thread *as long as the adapter is the only writer*,
//    which is guaranteed because the HTTP layer serialises access through
//    MCPQueryAdapter::EnsureSnapshotThread().
//
//  * replay queries (P0-6) must never call IReplayController directly. Every such
//    call is funnelled through IReplayManager::BlockInvoke() so it executes on the
//    replay thread with the correct locking and Python-GIL handling, exactly like
//    the native viewers do. If the replay thread is not running the invoke silently
//    does nothing; the adapter detects that and converts it into ReplayUnavailable
//    rather than returning stale or empty data.
//
// The adapter deliberately does NOT use dynamic_cast (qrenderdoc is built without
// reliable RTTI and the codebase avoids it), so it only ever talks to
// ICaptureContext / IReplayManager through their abstract interfaces.

// Contextual information about the currently loaded capture. This is the shape
// returned to clients by the session/status tools and embedded in most others so a
// client can always tell which capture and which event a result refers to.
struct CaptureSnapshot
{
  bool hasCapture = false;
  bool loading = false;
  bool local = true;
  bool temporary = false;
  bool replayAvailable = false;
  QString filename;
  QString api;           // human readable pipeline/graphics API name
  QString localRenderer; // human readable local replay API name
  uint32_t frameNumber = 0;
  uint32_t currentEvent = 0;
  uint32_t selectedEvent = 0;
  uint32_t numRootActions = 0;
  uint32_t numResources = 0;
  uint32_t numTextures = 0;
  uint32_t numBuffers = 0;
  uint32_t numTexturesInFrame = 0;
  uint32_t numBuffersInFrame = 0;
  uint32_t numSamplersInFrame = 0;
  uint32_t numConstantsInFrame = 0;
  bool isRemote = false;
  QString remoteHost;
  QStringList modifications;   // e.g. {"Bookmarks", "Notes", "EditedShaders"}
  QString fatalError;          // non-empty if the replay is in a fatal error state
};

class QueryAdapter
{
public:
  explicit QueryAdapter(ICaptureContext *ctx = NULL);

  // The context is stable for the lifetime of qrenderdoc; it is set once at
  // startup and only read afterwards.
  void SetContext(ICaptureContext *ctx) { m_Ctx = ctx; }

  // The capture context this adapter is bound to. Exposed so the transport layer
  // can populate CallContext.ctx without knowing anything about the adapter's
  // internals.
  ICaptureContext *Context() const { return m_Ctx; }

  // ---- P0-5 snapshot reads ------------------------------------------------

  // The capture/selection/status snapshot described above.
  CaptureSnapshot Snapshot();

  // Serialise a snapshot to JSON (used by session and status tools).
  static QJsonObject SnapshotToJson(const CaptureSnapshot &snap);

  // The API properties of the current capture, or an error if none is loaded.
  // `fields` optionally restricts which keys are emitted.
  ToolResult ApiProperties(const QStringList &fields = QStringList());

  // The current frame description + statistics.
  ToolResult FrameInfo();

  // Bitmask of modifications made in the UI.
  ToolResult Modifications();

  // The root actions of the capture, serialised. `eventId` filters to a single
  // top-level subtree when non-zero. `maxCount` bounds the number of siblings.
  ToolResult RootActions(uint32_t eventId = 0, int maxCount = 0,
                         const QStringList &fields = QStringList());

  // A single action at an explicit eventId, including its direct children names.
  ToolResult Action(uint32_t eventId, const QStringList &fields = QStringList());

  // The full list of resources (optionally filtered by resource type name).
  ToolResult Resources(const QString &typeFilter = QString(), int maxCount = 0,
                       const QStringList &fields = QStringList());

  // A single resource description.
  ToolResult Resource(ResourceId id);

  // The full list of textures, with the same optional filtering as Resources().
  ToolResult Textures(const QString &nameFilter = QString(), int maxCount = 0,
                      const QStringList &fields = QStringList());

  // A single texture description.
  ToolResult Texture(ResourceId id);

  // The full list of buffers, plus a common buffer-category breakdown.
  ToolResult Buffers(const QString &nameFilter = QString(), int maxCount = 0,
                     const QStringList &fields = QStringList());

  // A single buffer description.
  ToolResult Buffer(ResourceId id);

  // The current event and the event at a given id, using the structured file to
  // give call names, plus the currently bound resources (a lightweight stand-in
  // for the Event Browser selection).
  ToolResult EventInfo(uint32_t eventId = 0);

  // The list of bookmarks in the capture.
  ToolResult Bookmarks();

  // The full pipeline/API-state snapshot from the UI's own capture state: the
  // API properties, frame description/statistics and the current modifications.
  // These are the capture-load-time fields the native UI already has, so they do
  // not need the replay thread. Everything that does need replay is a separate
  // tool that goes through DispatchBlock().
  ToolResult PipelineSnapshot(const QStringList &fields = QStringList());

  // ---- P0-6 replay dispatch ----------------------------------------------

  // Runs `fn` on the replay thread via BlockInvoke, guarded by a loaded-capture
  // check and a validation predicate. `fn` receives a live IReplayController that
  // is guaranteed non-NULL. Returns the ToolResult produced by `fn`, or a
  // structured failure if the capture isn't loaded, is still loading, or the
  // replay thread refused the invoke (ReplayUnavailable).
  //
  // This is the ONLY sanctioned entry point for tools that need to issue
  // IReplayController calls; nothing else in the module should call
  // AsyncInvoke/BlockInvoke directly.
  ToolResult DispatchBlock(const char *toolName, CallContext &call,
                           const std::function<ToolResult(IReplayController &)> &fn);

  // Convenience wrapper for a plain function that returns whether the operation
  // succeeded and fills `out`. Useful for the many single-call replay queries.
  ToolResult DispatchBlockVoid(const char *toolName, CallContext &call,
                               const std::function<void(IReplayController &)> &fn,
                               const QJsonObject &structured = QJsonObject());

  // Whether a loaded capture currently has a usable replay controller.
  bool ReplayAvailable() const;

private:
  // Guards snapshot reads against concurrent UI mutation. The UI thread mutates
  // capture state, so snapshot reads are marshalled onto the UI thread when we
  // are not already there.
  template <typename T>
  T ReadOnUIThread(const std::function<T()> &fn) const;

  ICaptureContext *m_Ctx = NULL;
};

//
// ---- Serialisation helpers (shared by all tools) -------------------------
//
// These are free functions so tests and tools can reuse them without an adapter
// instance, and so the exact field names stay in one place.
//

// ResourceId <-> string, matching the native UI convention ("ResourceId::<n>").
// The pipeline-facing JSON keeps the numeric value in `id` and the display name
// in `name`, so clients don't have to parse the string form.
QString ResourceIdToString(ResourceId id);

namespace detail
{
// Compact, dependency-free name lookups. These intentionally do NOT include
// renderdoc_tostr.inl in this header; the .cpp includes it once so all enum
// ToStr<> specialisations are visible where they are needed.
QString ResourceTypeName(ResourceType t);
QString TextureTypeName(TextureType t);
QString CompTypeName(CompType t);
QString GraphicsAPIName(GraphicsAPI api);
QString TextureUsageString(TextureCategory flags);
QString BufferUsageString(BufferCategory flags);
QString ResourceFormatString(const ResourceFormat &f);
}    // namespace detail

}    // namespace MCP
