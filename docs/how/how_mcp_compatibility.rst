VS Code extension to qrenderdoc MCP compatibility
==================================================

This page records the migration contract for the ``renderdoc-for-vscode``
extension capabilities. It is an implementation inventory, not a promise that
all VS Code workflows become remotely callable from qrenderdoc.

The native MCP server is deliberately capture-analysis focused. It listens only
on the local machine, mediates replay access through qrenderdoc, and protects
capture-content operations with the sensitive-data setting. Capture launching,
remote-target lifecycle, and interactive views remain qrenderdoc UI workflows
unless a dedicated MCP tool is listed below.

Status meanings
---------------

``Retained``
  A native MCP tool provides the capability. Multiple tools in a cell are an
  intentional composition, not a missing mapping.

``Retained (UI)``
  qrenderdoc already provides the workflow interactively, but it is not a
  native MCP operation.

``Degraded``
  A related native tool exists, with an explicitly narrower contract. Clients
  must use the listed limitation instead of assuming extension behaviour.

``Deprecated``
  The extension capability is an editor-specific presentation, bridge bootstrap,
  or capture-lifecycle orchestration feature. It is intentionally not exposed by
  the current native MCP server.

``Future``
  A valid native automation gap. No currently registered MCP tool implements
  it, so clients must not probe for a similarly named tool.

Read-only capture inspection
----------------------------

.. list-table::
   :header-rows: 1
   :widths: 24 36 14 26

   * - Extension capability
     - Native replacement
     - Status
     - Migration note
   * - ``GetCaptureInfoTool``
     - ``renderdoc_getCaptureInfo``
     - Retained
     - Capture metadata.
   * - ``GetReplayStatusTool``
     - ``renderdoc_getReplayStatus``
     - Retained
     - Native replay availability and mode.
   * - ``GetSessionStateTool``
     - ``renderdoc_getSessionState``
     - Degraded
     - Reports qrenderdoc capture session, not extension launch-state phases.
   * - ``OpenCaptureTool``
     - qrenderdoc Open Capture UI
     - Retained (UI)
     - Loading a file is intentionally not a remote MCP side effect.
   * - ``GetSelectionContextTool``
     - ``renderdoc_getSelectionContext``
     - Retained
     - Current event/resource context.
   * - ``GetDrawCallsTool``
     - ``renderdoc_getDrawCalls``
     - Retained
     - Event hierarchy and action metadata.
   * - ``GetEventDetailsTool``
     - ``renderdoc_getEventDetails``
     - Retained
     - Detail for an EID.
   * - ``GetEventChunksTool``
     - ``renderdoc_getEventChunks``
     - Retained
     - API chunk data for an EID.
   * - ``GetResourcesTool``
     - ``renderdoc_getResources``
     - Retained
     - Resource inventory.
   * - ``GetResourceDetailTool``
     - ``renderdoc_getResourceDetail``
     - Retained
     - Detailed resource metadata.
   * - ``GetTextureInfoTool``
     - ``renderdoc_getTextures`` and ``renderdoc_getTextureInfo``
     - Retained
     - Use the detail tool when a resource ID is known.
   * - ``GetTextureDataTool``
     - ``renderdoc_getTextureData``
     - Degraded
     - Requires sensitive-data permission; it is unavailable when that policy is disabled.
   * - ``GetBufferContentsTool``
     - ``renderdoc_getBuffers``, ``renderdoc_getBufferDetail``, and ``renderdoc_getBufferContents``
     - Degraded
     - Contents require sensitive-data permission.
   * - ``GetMeshDataTool``
     - ``renderdoc_getMeshData``
     - Degraded
     - Mesh payload requires sensitive-data permission.
   * - ``GetActionTimingsTool``
     - ``renderdoc_getActionTimings`` and ``renderdoc_enumerateCounters``
     - Retained
     - Counter discovery is available separately.
   * - ``GetPipelineStateTool``
     - ``renderdoc_getPipelineState`` and ``renderdoc_getPipelineStateAtEvent``
     - Retained
     - The at-event form avoids changing selection.
   * - ``GetBoundResourcesTool``
     - ``renderdoc_getPipelineStateAtEvent``
     - Degraded
     - Bindings are returned as structured pipeline state, not the extension-normalised summary.
   * - ``GetShaderSourceTool``
     - ``renderdoc_getShaderInfo`` and ``renderdoc_getShaderEntryPoints``
     - Degraded
     - Captured source/disassembly is only returned when available and permitted.
   * - ``GetShaderInfoTool``
     - ``renderdoc_getShaderInfo``
     - Degraded
     - Captured shader information is protected by sensitive-data policy.


Analysis, reporting, and investigation
---------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 24 36 14 26

   * - Extension capability
     - Native replacement
     - Status
     - Migration note
   * - ``AnalyzeFrameTool``
     - ``renderdoc_getFrameSummary`` and ``renderdoc_analyzeFrame``
     - Retained
     - Full-frame evidence and issue analysis.
   * - ``GetFrameSummaryTool``
     - ``renderdoc_getFrameSummary``
     - Retained
     - Compact frame facts.
   * - ``AnalyzeHotEventTool``
     - ``renderdoc_getHotEvents`` and ``renderdoc_analyzeHotEvent``
     - Retained
     - Ranked timing evidence plus per-event analysis.
   * - ``GetPassGraphTool``
     - ``renderdoc_getPassGraph``
     - Retained
     - Pass/resource dependency graph.
   * - ``FindDrawsByShaderTool``
     - ``renderdoc_findDrawsByShader``
     - Retained
     - Reverse lookup.
   * - ``FindDrawsByTextureTool``
     - ``renderdoc_findDrawsByTexture``
     - Retained
     - Reverse lookup.
   * - ``FindDrawsByResourceIdTool``
     - ``renderdoc_findDrawsByResourceId``
     - Retained
     - Reverse lookup.
   * - ``TraceResourceUsageTool``
     - ``renderdoc_traceResourceUsage``
     - Retained
     - Producer/consumer and usage evidence.
   * - ``ResourceMemoryAuditTool``
     - ``renderdoc_resourceMemoryAudit``
     - Retained
     - Capture-footprint audit.
   * - ``ResourceLifetimeTool``
     - ``renderdoc_resourceLifetime``
     - Retained
     - Lifetime evidence.
   * - ``FindUnusedResourcesTool``
     - ``renderdoc_findUnusedResources``
     - Retained
     - Heuristic conclusions remain labelled.
   * - ``FindResourceLeaksTool``
     - ``renderdoc_findResourceLeaks``
     - Retained
     - Candidate leaks are heuristic, not proof.
   * - ``DiffPipelineStateTool``
     - ``renderdoc_diffPipelineState``
     - Retained
     - Same-capture, cross-event comparison.
   * - ``GeneratePerformanceReportTool``
     - ``renderdoc_generatePerformanceReport``
     - Retained
     - Structured evidence report.
   * - ``ExportInvestigationReportTool``
     - ``renderdoc_buildInvestigationReport`` and ``renderdoc_exportPerformanceReport``
     - Retained
     - Use ``renderdoc_exportPerformanceReport`` for report file export where applicable.
   * - ``BuildEventBrowserContextTool``
     - ``renderdoc_buildEventBrowserContext``
     - Retained
     - AI context; it does not replace the native event browser UI.
   * - ``AddBookmarkTool`` and ``ListBookmarksTool`` ``UpdateBookmarkTool`` and ``RemoveBookmarkTool``
     - ``renderdoc_listBookmarks``, ``renderdoc_bookmarksExtended``, ``renderdoc_updateBookmark``, and ``renderdoc_removeBookmark``
     - Retained
     - Native sidecar records support extended investigation fields.


Cross-capture, source, and external tooling
--------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 24 36 14 26

   * - Extension capability
     - Native replacement
     - Status
     - Migration note
   * - ``CompareResourceMemoryTool``
     - Regression snapshot/diff workflow
     - Retained
     - Matches resources using normalised capture keys, rather than unstable resource IDs.
   * - ``CompareEventTimingsTool``
     - Regression snapshot/diff workflow
     - Retained
     - Requires independently collected replay timing evidence for each snapshot.
   * - ``CompareCapturesTool``
     - Regression snapshot/diff workflow
     - Retained
     - Covers aggregate, memory, pass, event, pipeline, and shader regressions.
   * - ``FindProjectImplementationTool``
     - ``renderdoc_mapProjectSource``
     - Degraded
     - Maps source references against explicit project/include roots; it does not search VS Code workspaces or infer C# semantics.
   * - Mali inspector integration
     - ``renderdoc_getMaliToolStatus`` and ``renderdoc_analyzeMaliShader``
     - Retained
     - Availability is platform/configuration dependent and shader export is sensitive.
   * - ``DiagnoseEnvironmentTool``
     - ``renderdoc_getEnvironmentDiagnostics``
     - Degraded
     - Reports native MCP, privacy, source-mapping, and Mali readiness; it does not probe ADB or install a VS Code bridge.


Shader-specific migration decisions
-----------------------------------

.. list-table::
   :header-rows: 1
   :widths: 24 36 14 26

   * - Extension capability
     - Native replacement
     - Status
     - Migration note
   * - ``FindShaderVariantsTool``
     - ``renderdoc_getResources`` and ``renderdoc_findDrawsByShader``
     - Degraded
     - Filter shader resources client-side, then use reverse lookup for bindings.
   * - ``CompareShadersTool``
     - ``renderdoc_diffPipelineState`` plus ``renderdoc_getShaderInfo``
     - Degraded
     - Compares bound shader/pipeline snapshots; no compiler-level semantic equivalence.
   * - ``GetShaderCompileDiagnosticsTool``
     - qrenderdoc shader editor
     - Retained (UI)
     - Native UI exposes compiler diagnostics when compiling edits; MCP exposes captured shader metadata only.
   * - ``ValidateShaderEditTool``
     - qrenderdoc shader editor
     - Retained (UI)
     - Compile validation is an explicit local UI action, not an MCP side effect.
   * - ``ApplyShaderEditTool``
     - qrenderdoc shader editor
     - Retained (UI)
     - Replacement remains local to replay and is intentionally not remotely callable.
   * - ``GetCurrentDrawPreviewTool``
     - Texture Viewer and drawcall overlay UI
     - Retained (UI)
     - Image/base64 preview export is not an MCP contract.


Capture, target, and editor workflow exclusions
------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 24 36 14 26

   * - Extension capability
     - Native replacement
     - Status
     - Migration note
   * - ``WaitForLiveTargetTool`` and ``WaitForCaptureTool``
     - Capture Connection UI
     - Deprecated
     - Polling an editor-owned lifecycle state has no stable native MCP equivalent.
   * - ``LaunchApplicationTool`` and ``LaunchWindowsApplicationTool``
     - qrenderdoc capture launch UI
     - Retained (UI)
     - Process launch is excluded from local MCP for safety and explicit user control.
   * - ``CaptureFrameTool`` and ``TriggerRemoteCaptureTool``
     - Capture Connection UI
     - Retained (UI)
     - Live capture trigger remains interactive.
   * - ``ListCaptureTargetsTool`` and extension target refresh/attach
     - Capture Connection UI
     - Retained (UI)
     - Target discovery and attach are existing native workflows, not MCP tools.
   * - ``LaunchRemoteApplicationTool``
     - Android/remote capture UI
     - Retained (UI)
     - No MCP launch or remote deployment tool.
   * - ``CheckAndroidLaunchReadinessTool``
     - Android setup documentation and UI
     - Future
     - A structured, read-only Android readiness probe is a potential future native tool.
   * - ``CloseSessionTool`` and close/cache/bridge commands
     - qrenderdoc window/capture controls
     - Deprecated
     - Editor session ownership, cache cleanup, and bridge download do not transfer to MCP.
   * - VS Code inspector, tree, search thumbnail, preview, and logs
     - qrenderdoc Event Browser, Pipeline State, Texture Viewer, Resource Inspector, logs
     - Retained (UI)
     - These are native visual workflows; MCP returns data rather than driving widgets.
   * - VS Code replay-host prompts
     - qrenderdoc replay-host selection UI
     - Retained (UI)
     - Recommendation prompts remain UI policy.


Client migration rules
----------------------

* Discover native capabilities from the MCP ``tools/list`` response; do not
  infer tool availability from this document or from an extension tool name.
* Treat source, texture, buffer, mesh, shader export, and Mali shader analysis
  as privacy-sensitive. If unavailable, inspect
  ``renderdoc_getEnvironmentDiagnostics`` before asking the user to change the
  qrenderdoc setting.
* Use the native cross-capture regression snapshot/diff workflow for comparison.
  Do not pair resources or events across captures by raw IDs.
* Use the qrenderdoc UI for capture launch, attach, remote targets, shader edit,
  and image visualisation. These exclusions prevent an MCP client from starting
  processes, changing live-target state, or silently applying shader changes.
* Treat every analysis conclusion as evidence-backed or heuristic according to
  the ``limitations`` and confidence fields returned by the native tool.
