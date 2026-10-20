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

// Native capture tools. Target-control and remote-server handles have one worker
// owner; CaptureContext is accessed only through QueryAdapter's UI boundary.
#include "MCPSessionTools.h"
#include "MCPQueryAdapter.h"
#include "Code/Interface/QRDInterface.h"
#include "Code/Interface/PersistentConfig.h"
#include "Code/QRDUtils.h"
#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <cmath>
#include <climits>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace MCP
{
namespace
{
QString Str(const rdcstr &s) { return QString::fromUtf8(s.c_str(), int(s.size())); }
rdcstr Rdc(const QString &s) { return rdcstr(s.toUtf8().constData()); }
QString Arg(const QJsonObject &a, const char *key) { return a.value(QString::fromUtf8(key)).toString(); }
ToolResult Fail(ErrorCode code, const char *message)
{
  return ToolResult::Failure(Error(code, message));
}
ToolResult Status(bool ok, const char *code, const QString &message)
{
  return ToolResult::Success(QJsonObject{{QStringLiteral("ok"), ok},
      {QStringLiteral("code"), QString::fromUtf8(code)}, {QStringLiteral("message"), message},
      {QStringLiteral("recoverable"), !ok}});
}
void Pause(int ms)
{
  if(QCoreApplication::instance() && GUIInvoke::onUIThread())
  {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
  }
  else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
ToolResult Interrupted(CallContext &call)
{
  return Fail(call.IsCancelled() ? ErrorCode::Cancelled : ErrorCode::Timeout,
              "Request interrupted; submitted native work may still finish. Inspect application session state.");
}
QJsonArray Targets()
{
  QJsonArray targets;
  rdcarray<rdcstr> protocols;
  RENDERDOC_GetSupportedDeviceProtocols(&protocols);
  for(const rdcstr &protocol : protocols)
  {
    IDeviceProtocolController *controller = RENDERDOC_GetDeviceProtocolController(protocol);
    if(!controller) continue;
    const QString p = Str(controller->GetProtocolName());
    for(const rdcstr &device : controller->GetDevices())
    {
      const QString url = p + QStringLiteral("://") + Str(device);
      const QString friendly = Str(controller->GetFriendlyName(Rdc(url)));
      targets.append(QJsonObject{{QStringLiteral("protocol"), p}, {QStringLiteral("id"), Str(device)},
          {QStringLiteral("url"), url}, {QStringLiteral("name"), friendly.isEmpty() ? Str(device) : friendly},
          {QStringLiteral("supported"), controller->IsSupported(Rdc(url))},
          {QStringLiteral("supportsMultiplePrograms"), controller->SupportsMultiplePrograms(Rdc(url))}});
    }
  }
  return targets;
}
ToolResult SelectAndroid(const QJsonArray &targets, const QJsonObject &args)
{
  QJsonArray matches;
  const QString exact = Arg(args, "targetUrl"), query = Arg(args, "targetQuery");
  for(const QJsonValue &v : targets)
  {
    const QJsonObject t = v.toObject();
    if(t.value(QStringLiteral("protocol")).toString() != QStringLiteral("adb") &&
       t.value(QStringLiteral("protocol")).toString() != QStringLiteral("android")) continue;
    if(!t.value(QStringLiteral("supported")).toBool()) continue;
    if(!exact.isEmpty() && t.value(QStringLiteral("url")).toString() != exact) continue;
    if(exact.isEmpty() && !query.isEmpty() &&
       !t.value(QStringLiteral("url")).toString().contains(query, Qt::CaseInsensitive) &&
       !t.value(QStringLiteral("id")).toString().contains(query, Qt::CaseInsensitive) &&
       !t.value(QStringLiteral("name")).toString().contains(query, Qt::CaseInsensitive)) continue;
    matches.append(t);
  }
  if(matches.size() != 1)
    return ToolResult::Failure(Error(ErrorCode::InvalidParams,
        QStringLiteral("Select exactly one supported Android target using targetUrl or targetQuery."),
        QJsonObject{{QStringLiteral("targets"), targets}, {QStringLiteral("matches"), matches}}));
  return ToolResult::Success(matches[0].toObject());
}
ToolResult AndroidReadiness(const QJsonObject &args, const QJsonObject &target)
{
  const QString package = Arg(args, "packageName");
  // These values become device shell arguments. Android package/activity grammar
  // excludes shell metacharacters; never interpolate arbitrary caller text.
  if(!QRegularExpression(QStringLiteral("^[A-Za-z0-9_]+(?:\\.[A-Za-z0-9_]+)+$")).match(package).hasMatch())
    return Fail(ErrorCode::InvalidParams, "Invalid Android package name");
  const QString url = target.value(QStringLiteral("url")).toString();
  IDeviceProtocolController *controller = RENDERDOC_GetDeviceProtocolController(
      Rdc(target.value(QStringLiteral("protocol")).toString()));
  rdcstr output;
  ResultDetails result = controller->ExecCommand(Rdc(url), Rdc(QStringLiteral("pm path ") + package), &output);
  QJsonObject out{{QStringLiteral("ready"), false}, {QStringLiteral("ok"), false},
      {QStringLiteral("target"), target}, {QStringLiteral("packageName"), package},
      {QStringLiteral("packageQueryOutput"), Str(output)}};
  if(!result.OK() || !Str(output).contains(QStringLiteral("package:")))
  {
    out[QStringLiteral("code")] = result.OK() ? QStringLiteral("PACKAGE_NOT_INSTALLED") : QStringLiteral("DEVICE_COMMAND_FAILED");
    out[QStringLiteral("message")] = Str(result.Message());
    return ToolResult::Success(out);
  }
  output.clear();
  result = controller->ExecCommand(Rdc(url), Rdc(QStringLiteral("cmd package resolve-activity --brief -c android.intent.category.LAUNCHER ") + package), &output);
  out[QStringLiteral("activityQueryOutput")] = Str(output);
  QString activity;
  for(const QString &line : Str(output).split(QLatin1Char('\n')))
    if(line.trimmed().startsWith(package + QLatin1Char('/'))) activity = line.trimmed();
  out[QStringLiteral("activity")] = activity;
  out[QStringLiteral("ready")] = result.OK() && !activity.isEmpty();
  out[QStringLiteral("ok")] = result.OK() && !activity.isEmpty();
  out[QStringLiteral("code")] = !result.OK() ? QStringLiteral("DEVICE_COMMAND_FAILED") : activity.isEmpty() ? QStringLiteral("ACTIVITY_NOT_FOUND") : QStringLiteral("READY");
  if(!result.OK()) out[QStringLiteral("message")] = Str(result.Message());
  out[QStringLiteral("deviceTransport")] = QStringLiteral("RenderDoc device protocol (configured adb)");
  // Enumeration/support and these commands establish device transport, package,
  // and launcher readiness, not debuggability or successful injection.
  out[QStringLiteral("limitation")] = QStringLiteral("Injection/debugger readiness is established only by launch; no APK is installed or modified by this check.");
  return ToolResult::Success(out);
}
}

class NativeCaptureSession
{
public:
  NativeCaptureSession() : m_Worker([this]() { Run(); }) {}
  ~NativeCaptureSession()
  {
    m_Stop.store(true);
    m_Wake.notify_all();
    m_Worker.join(); // keep ownership until native blocking calls have settled
  }
  std::future<ToolResult> Submit(const std::function<ToolResult()> &fn,
                               const QSharedPointer<QAtomicInt> &abandoned,
                               const CallContext &call)
  {
    // Never retain a caller's non-owning cancellation pointer or a shared_ptr to
    // this worker in its own queue. The adapter/calling thread owns us and joins
    // on destruction; queued jobs cannot become the last owner on this thread.
    CallContext owned;
    owned.cancellationState = call.cancellationState;
    owned.deadlineMs = call.deadlineMs;
    auto task = std::make_shared<std::packaged_task<ToolResult()>>([fn, owned, abandoned]() mutable {
      if(abandoned->loadAcquire() || owned.IsCancelled() || owned.IsExpired())
        return Fail(ErrorCode::Cancelled, "Request was interrupted before native dispatch");
      try { return fn(); }
      catch(...) { return Fail(ErrorCode::InternalError, "Native session operation threw an exception"); }
    });
    auto future = task->get_future();
    { std::lock_guard<std::mutex> lock(m_QueueLock); m_Jobs.push_back([task]() { (*task)(); }); }
    m_Wake.notify_all();
    return future;
  }
  QJsonObject State() const
  {
    std::lock_guard<std::mutex> lock(m_StateLock);
    return m_State;
  }
  void Set(const QString &key, const QJsonValue &value)
  {
    std::lock_guard<std::mutex> lock(m_StateLock);
    m_State[key] = value;
  }
  ToolResult Close()
  {
    if(m_Target) { m_Target->Shutdown(); m_Target = NULL; }
    if(m_Remote) { m_Remote->ShutdownConnection(); m_Remote = NULL; }
    m_DelayUntil = 0;
    Set(QStringLiteral("phase"), QStringLiteral("closed"));
    Set(QStringLiteral("target"), QJsonValue(QJsonValue::Null));
    return ToolResult::Success(QJsonObject{{QStringLiteral("closed"), true},
        {QStringLiteral("state"), State()}, {QStringLiteral("capturesDeleted"), false},
        {QStringLiteral("applicationTerminated"), false}});
  }
  ToolResult Launch(const QJsonObject &args, const QString &platform)
  {
    if(m_Target || m_Remote)
      return Fail(ErrorCode::CaptureBusy, "An application capture session already exists; close it before launching another");
    QString app = Arg(args, "app");
    if(app.isEmpty()) app = Arg(args, platform == QStringLiteral("windows") ? "executablePath" : "packageActivity");
    QJsonObject selected;
    QString url;
    if(platform == QStringLiteral("android"))
    {
      ToolResult selection = SelectAndroid(Targets(), args);
      if(!selection.ok) return selection;
      selected = selection.structured;
      url = selected.value(QStringLiteral("url")).toString();
      IDeviceProtocolController *controller = RENDERDOC_GetDeviceProtocolController(Rdc(selected.value(QStringLiteral("protocol")).toString()));
      if(!selected.value(QStringLiteral("supportsMultiplePrograms")).toBool() &&
         RENDERDOC_EnumerateRemoteTargets(Rdc(url), 0) != 0)
        return Fail(ErrorCode::CaptureBusy, "Android device already has an injected application");
      if(!app.contains(QLatin1Char('/')))
      {
        ToolResult ready = AndroidReadiness(QJsonObject{{QStringLiteral("packageName"), app}}, selected);
        if(!ready.ok || !ready.structured.value(QStringLiteral("ready")).toBool()) return ready;
        app = ready.structured.value(QStringLiteral("activity")).toString();
      }
      if(!QRegularExpression(QStringLiteral("^[A-Za-z0-9_.]+/[A-Za-z0-9_.$]+$")).match(app).hasMatch())
        return Fail(ErrorCode::InvalidParams, "Invalid Android package/activity");
      // Explicit remote-launch authority includes RenderDoc's native server setup:
      // it can install/update its own APKs and change device capture settings.
      // The requested application APK is not installed or patched here.
      ResultDetails started = controller->StartRemoteServer(Rdc(url));
      if(!started.OK()) return Status(false, "REMOTE_SERVER_FAILED", Str(started.Message()));
      ResultDetails connected = RENDERDOC_CreateRemoteServerConnection(Rdc(url), &m_Remote);
      if(!connected.OK() || !m_Remote)
      { Close(); return Status(false, "REMOTE_CONNECT_FAILED", Str(connected.Message())); }
    }
    else
    {
      if(platform != QStringLiteral("windows")) return Fail(ErrorCode::InvalidParams, "platform must be windows or android");
#if !defined(Q_OS_WIN)
      return Fail(ErrorCode::Unsupported, "Windows launch requires a Windows host");
#endif
      QFileInfo file(app);
      if(!file.isAbsolute() || !file.isFile() || file.suffix().compare(QStringLiteral("exe"), Qt::CaseInsensitive))
        return Fail(ErrorCode::InvalidParams, "Provide an existing absolute Windows .exe path");
    }
    const QString id = QStringLiteral("application-") + QUuid::createUuid().toString().mid(1, 36);
    { std::lock_guard<std::mutex> lock(m_StateLock);
      m_State = QJsonObject{{QStringLiteral("applicationSessionId"), id}, {QStringLiteral("phase"), QStringLiteral("launching")},
          {QStringLiteral("platform"), platform}, {QStringLiteral("application"), app}, {QStringLiteral("lastCapture"), QJsonValue(QJsonValue::Null)}}; }
    m_URL = url;
    QString workingDir = Arg(args, "workingDir");
    if(platform == QStringLiteral("windows") && workingDir.isEmpty()) workingDir = QFileInfo(app).absolutePath();
    const QString directory = QDir::tempPath() + QStringLiteral("/renderdoc-mcp/") + id;
    if(!QDir().mkpath(directory)) { Close(); return Fail(ErrorCode::InternalError, "Cannot create capture output directory"); }
    m_Directory = directory;
    CaptureOptions options;
    // CaptureOptions' constructor supplies the native defaults.
    const rdcarray<EnvironmentModification> env;
    const ExecuteResult launched = m_Remote ? m_Remote->ExecuteAndInject(Rdc(app), Rdc(workingDir), Rdc(Arg(args, "commandLine")), env, options) :
        RENDERDOC_ExecuteAndInject(Rdc(app), Rdc(workingDir), Rdc(Arg(args, "commandLine")), env, Rdc(directory + QStringLiteral("/capture")), options, false);
    if(!launched.result.OK() || !launched.ident)
    { Close(); Set(QStringLiteral("phase"), QStringLiteral("error")); return Status(false, "LAUNCH_FAILED", Str(launched.result.Message())); }
    m_Target = RENDERDOC_CreateTargetControl(Rdc(url), launched.ident, "qrenderdoc MCP", false);
    if(!m_Target || !m_Target->Connected() || !m_Target->GetBusyClient().empty())
    { Close(); Set(QStringLiteral("phase"), QStringLiteral("error")); return Fail(ErrorCode::ReplayUnavailable, "Application launched but target control is unavailable (application was not terminated)"); }
    QJsonObject target{{QStringLiteral("url"), url}, {QStringLiteral("ident"), double(launched.ident)},
        {QStringLiteral("pid"), double(m_Target->GetPID())}, {QStringLiteral("target"), Str(m_Target->GetTarget())},
        {QStringLiteral("api"), Str(m_Target->GetAPI())}, {QStringLiteral("local"), url.isEmpty()}};
    Set(QStringLiteral("target"), target);
    Set(QStringLiteral("phase"), QStringLiteral("ready"));
    QJsonObject out = target;
    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("platform")] = platform;
    out[QStringLiteral("selectedTarget")] = selected;
    if(platform == QStringLiteral("android"))
      out[QStringLiteral("serverSetupEffects")] = QStringLiteral("Native RenderDoc server setup may install/update its server APKs, forward ports and configure device capture settings; the requested application APK is not installed by this tool.");
    out[QStringLiteral("state")] = State();
    return ToolResult::Success(out);
  }
  ToolResult Trigger(const QJsonObject &args, bool remoteOnly)
  {
    if(!m_Target || !m_Target->Connected()) return Fail(ErrorCode::ReplayUnavailable, "Launch an application capture session first");
    if(remoteOnly && m_URL.isEmpty()) return Fail(ErrorCode::InvalidParams, "triggerRemoteCapture requires an Android session; use captureFrame for Windows");
    if(State().value(QStringLiteral("phase")) == QStringLiteral("capturing"))
      return Fail(ErrorCode::CaptureBusy, "A capture is already pending");
    const QString mode = args.contains(QStringLiteral("trigger")) ? Arg(args, "trigger") : QStringLiteral("immediate");
    const double frame = args.value(QStringLiteral("frameNumber")).toDouble(-1);
    const double delay = args.value(QStringLiteral("delaySeconds")).toDouble(0);
    if(mode != QStringLiteral("immediate") && mode != QStringLiteral("frame") && mode != QStringLiteral("delay"))
      return Fail(ErrorCode::InvalidParams, "trigger must be immediate, frame or delay");
    if(mode == QStringLiteral("frame") && (!std::isfinite(frame) || frame < 0 || frame > double(UINT32_MAX) || std::floor(frame) != frame))
      return Fail(ErrorCode::InvalidParams, "frameNumber must fit the native uint32 frame index");
    if(mode == QStringLiteral("delay") && (!std::isfinite(delay) || delay <= 0 || delay >= double((LLONG_MAX - QDateTime::currentMSecsSinceEpoch()) / 1000)))
      return Fail(ErrorCode::InvalidParams, "delaySeconds must be finite, positive and representable as milliseconds");
    { std::lock_guard<std::mutex> lock(m_StateLock);
      m_State[QStringLiteral("captureRequestId")] = QUuid::createUuid().toString().mid(1, 36);
      m_State[QStringLiteral("lastCapture")] = QJsonValue(QJsonValue::Null);
      m_State[QStringLiteral("phase")] = QStringLiteral("capturing"); }
    if(mode == QStringLiteral("frame")) m_Target->QueueCapture(uint32_t(frame), 1);
    else if(mode == QStringLiteral("delay")) m_DelayUntil = QDateTime::currentMSecsSinceEpoch() + qint64(delay * 1000);
    else m_Target->TriggerCapture(1);
    return ToolResult::Success(State());
  }
private:
  void Poll()
  {
    if(!m_Target) return;
    if(m_DelayUntil && QDateTime::currentMSecsSinceEpoch() >= m_DelayUntil)
    { m_DelayUntil = 0; m_Target->TriggerCapture(1); }
    if(!m_Target->Connected()) { Close(); Set(QStringLiteral("phase"), QStringLiteral("disconnected")); return; }
    TargetControlMessage message = m_Target->ReceiveMessage(NULL);
    if(message.type == TargetControlMessageType::NewCapture)
    {
      const NewCaptureData &capture = message.newCapture;
      QString path = Str(capture.path);
      if(!capture.local)
      {
        const QString copy = m_Directory + QStringLiteral("/") + QUuid::createUuid().toString().mid(1, 36) + QStringLiteral(".rdc");
        if(m_Remote) m_Remote->CopyCaptureFromRemote(capture.path, Rdc(copy), NULL);
        else { Set(QStringLiteral("error"), QStringLiteral("No remote connection to copy capture")); return; }
        path = copy;
      }
      if(!QFileInfo(path).isFile() || QFileInfo(path).size() <= 0)
      { Set(QStringLiteral("error"), QStringLiteral("Capture file was not successfully saved")); Set(QStringLiteral("phase"), QStringLiteral("error")); return; }
      QJsonObject saved{{QStringLiteral("capturePath"), path},
          {QStringLiteral("sourcePath"), Str(capture.path)}, {QStringLiteral("captureId"), double(capture.captureId)},
          {QStringLiteral("frameNumber"), double(capture.frameNumber)}, {QStringLiteral("timestamp"), double(capture.timestamp)},
          {QStringLiteral("byteSize"), double(QFileInfo(path).size())}, {QStringLiteral("sourceByteSize"), double(capture.byteSize)},
          {QStringLiteral("api"), Str(capture.api)}, {QStringLiteral("local"), true}, {QStringLiteral("sourceLocal"), capture.local},
          {QStringLiteral("applicationSessionId"), State().value(QStringLiteral("applicationSessionId"))},
          {QStringLiteral("captureRequestId"), State().value(QStringLiteral("captureRequestId"))},
          {QStringLiteral("correlation"), QStringLiteral("Next native capture notification; target protocol does not echo request IDs")}};
      { std::lock_guard<std::mutex> lock(m_StateLock);
        m_State[QStringLiteral("lastCapture")] = saved;
        if(!m_DelayUntil) m_State[QStringLiteral("phase")] = QStringLiteral("completed"); }
    }
    else if(message.type == TargetControlMessageType::Disconnected)
    { Close(); Set(QStringLiteral("phase"), QStringLiteral("disconnected")); }
    if(m_Remote && m_Ping.elapsed() >= 1000)
    { m_Remote->Ping(); m_Ping.restart(); }
  }
  void Run()
  {
    m_Ping.start();
    while(!m_Stop.load())
    {
      std::function<void()> job;
      { std::unique_lock<std::mutex> lock(m_QueueLock);
        if(m_Jobs.empty()) m_Wake.wait_for(lock, std::chrono::milliseconds(10));
        if(!m_Jobs.empty()) { job = m_Jobs.front(); m_Jobs.pop_front(); } }
      if(job) job();
      if(!m_Stop.load())
      {
        try { Poll(); }
        catch(...) { Close(); Set(QStringLiteral("phase"), QStringLiteral("error")); Set(QStringLiteral("error"), QStringLiteral("Native target polling failed")); }
      }
    }
    Close();
  }
  mutable std::mutex m_StateLock;
  QJsonObject m_State{{QStringLiteral("phase"), QStringLiteral("idle")}};
  std::mutex m_QueueLock;
  std::condition_variable m_Wake;
  std::deque<std::function<void()>> m_Jobs;
  std::atomic<bool> m_Stop{false};
  ITargetControl *m_Target = NULL;
  IRemoteServer *m_Remote = NULL;
  QString m_URL, m_Directory;
  qint64 m_DelayUntil = 0;
  QElapsedTimer m_Ping;
  // Must be last: all state is initialised before the worker starts.
  std::thread m_Worker;
};

std::shared_ptr<NativeCaptureSession> CreateNativeCaptureSession()
{
  return std::make_shared<NativeCaptureSession>();
}
QJsonObject QueryAdapter::ApplicationSessionState() const
{
  return m_ApplicationSession ? m_ApplicationSession->State() : QJsonObject{{QStringLiteral("phase"), QStringLiteral("idle")}};
}
namespace
{
ToolResult Await(std::future<ToolResult> &future, CallContext &call,
                 const QSharedPointer<QAtomicInt> &abandoned)
{
  while(future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
  {
    if(call.IsCancelled() || call.IsExpired())
    { abandoned->storeRelease(1); return Interrupted(call); }
    Pause(10);
  }
  return future.get();
}
}
ToolResult QueryAdapter::SessionTool(const QString &name, const QJsonObject &args, CallContext &call)
{
  if(call.IsCancelled() || call.IsExpired()) return Interrupted(call);
  if(name == QStringLiteral("renderdoc_launchApplication") && Arg(args, "platform").isEmpty())
    return Status(false, "PLATFORM_REQUIRED", QStringLiteral("Specify platform windows or android; no application was launched."));
  if(name == QStringLiteral("renderdoc_openCapture"))
  {
    const QString requested = Arg(args, "filePath");
    return WithCaptureUI(call, [requested](ICaptureContext &ctx) {
      QJsonObject out{{QStringLiteral("requestedPath"), requested.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(requested)},
          {QStringLiteral("loadedNow"), false}, {QStringLiteral("candidatePaths"), QJsonArray()}};
      if(ctx.IsCaptureLoading()) return Fail(ErrorCode::CaptureBusy, "Capture is loading");
      if(!requested.isEmpty() && requested != Str(ctx.GetCaptureFilename()))
      {
        QFileInfo file(requested);
        if(!file.isAbsolute() || !file.isFile()) return Fail(ErrorCode::InvalidParams, "filePath must name an existing absolute capture file");
        if(ctx.IsCaptureLoaded() && (ctx.IsCaptureTemporary() || ctx.GetCaptureModifications() != CaptureModifications::NoModifications))
          return Fail(ErrorCode::CaptureBusy, "Save or close the temporary/modified capture in the UI before replacing it");
        ctx.LoadCapture(Rdc(requested), ctx.Config().DefaultReplayOptions, Rdc(requested), false, true);
        out[QStringLiteral("loadedNow")] = ctx.IsCaptureLoaded();
      }
      out[QStringLiteral("captureLoaded")] = ctx.IsCaptureLoaded();
      out[QStringLiteral("requestedCaptureLoaded")] = ctx.IsCaptureLoaded() &&
          (requested.isEmpty() || QFileInfo(requested).absoluteFilePath() == QFileInfo(Str(ctx.GetCaptureFilename())).absoluteFilePath());
      out[QStringLiteral("capturePath")] = ctx.IsCaptureLoaded() ? QJsonValue(Str(ctx.GetCaptureFilename())) : QJsonValue(QJsonValue::Null);
      out[QStringLiteral("message")] = ctx.IsCaptureLoaded() ? QStringLiteral("Native capture is loaded.") : QStringLiteral("No native capture is loaded; supply filePath. VS Code tabs are not consulted.");
      return ToolResult::Success(out);
    });
  }
  const auto session = m_ApplicationSession;
  if(!session) return Fail(ErrorCode::Unsupported, "Native application session owner is unavailable");
  if(name == QStringLiteral("renderdoc_waitForLiveTarget") || name == QStringLiteral("renderdoc_waitForCapture"))
  {
    const bool capture = name == QStringLiteral("renderdoc_waitForCapture");
    const int timeout = args.value(QStringLiteral("timeoutMs")).toInt(10000);
    const int poll = args.value(QStringLiteral("pollMs")).toInt(250);
    if(timeout < 100 || timeout > 60000 || poll < 50 || poll > 2000)
      return Fail(ErrorCode::InvalidParams, "timeoutMs must be 100..60000 and pollMs 50..2000");
    const QString sessionId = session->State().value(QStringLiteral("applicationSessionId")).toString();
    QElapsedTimer timer; timer.start();
    for(;;)
    {
      const QJsonObject state = session->State();
      const QString currentId = state.value(QStringLiteral("applicationSessionId")).toString();
      if(!sessionId.isEmpty() && currentId != sessionId)
        return Fail(ErrorCode::CaptureBusy, "Application session changed while waiting");
      const QString phase = state.value(QStringLiteral("phase")).toString();
      const bool done = capture ? phase == QStringLiteral("completed") && state.value(QStringLiteral("lastCapture")).isObject() :
          state.value(QStringLiteral("target")).isObject() && (phase == QStringLiteral("ready") || phase == QStringLiteral("running") || phase == QStringLiteral("capturing") || phase == QStringLiteral("completed"));
      if(done || timer.elapsed() >= timeout)
        return ToolResult::Success(QJsonObject{{QStringLiteral("ok"), done}, {QStringLiteral("timedOut"), !done}, {QStringLiteral("timeoutMs"), timeout}, {QStringLiteral("state"), state}});
      if(call.IsCancelled() || call.IsExpired()) return Interrupted(call);
      Pause(qMin(poll, int(timeout - timer.elapsed())));
    }
  }
  const QSharedPointer<QAtomicInt> abandoned(new QAtomicInt(0));
  NativeCaptureSession *owner = session.get();
  auto job = session->Submit([owner, name, args]() {
    if(name == QStringLiteral("renderdoc_listCaptureTargets")) return ToolResult::Success(QJsonObject{{QStringLiteral("targets"), Targets()}});
    if(name == QStringLiteral("renderdoc_checkAndroidLaunchReadiness"))
    {
      ToolResult selected = SelectAndroid(Targets(), args);
      if(!selected.ok) return selected;
      return AndroidReadiness(args, selected.structured);
    }
    if(name == QStringLiteral("renderdoc_closeSession")) return owner->Close();
    if(name == QStringLiteral("renderdoc_launchApplication") || name == QStringLiteral("renderdoc_launchWindowsApplication") || name == QStringLiteral("renderdoc_launchRemoteApplication"))
      return owner->Launch(args, name == QStringLiteral("renderdoc_launchApplication") ? Arg(args, "platform") : name == QStringLiteral("renderdoc_launchWindowsApplication") ? QStringLiteral("windows") : QStringLiteral("android"));
    if(name == QStringLiteral("renderdoc_captureFrame") || name == QStringLiteral("renderdoc_triggerRemoteCapture"))
      return owner->Trigger(args, name == QStringLiteral("renderdoc_triggerRemoteCapture"));
    return Fail(ErrorCode::InvalidParams, "Unknown native session tool");
  }, abandoned, call);
  ToolResult result = Await(job, call, abandoned);
  if(!result.ok || (name != QStringLiteral("renderdoc_captureFrame") && name != QStringLiteral("renderdoc_triggerRemoteCapture"))) return result;
  // Trigger submission and completion are distinct. A timeout does not claim the
  // queued frame was cancelled, and the owner continues preserving late captures.
  const QJsonValue sessionId = result.structured.value(QStringLiteral("applicationSessionId"));
  const QJsonValue requestId = result.structured.value(QStringLiteral("captureRequestId"));
  QElapsedTimer captureTimer; captureTimer.start();
  QJsonObject state;
  for(;;)
  {
    state = session->State();
    if(state.value(QStringLiteral("applicationSessionId")) != sessionId ||
       state.value(QStringLiteral("captureRequestId")) != requestId)
      return Fail(ErrorCode::CaptureBusy, "Application session or capture request changed while waiting");
    const QString phase = state.value(QStringLiteral("phase")).toString();
    if(phase == QStringLiteral("completed") && state.value(QStringLiteral("lastCapture")).isObject()) break;
    if(phase == QStringLiteral("error") || phase == QStringLiteral("closed") || phase == QStringLiteral("disconnected"))
      return ToolResult::Failure(Error(ErrorCode::ReplayUnavailable, QStringLiteral("Capture session stopped before completion"), state));
    if(call.IsCancelled() || call.IsExpired()) return Interrupted(call);
    if(captureTimer.elapsed() >= 60000)
      return ToolResult::Success(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("timedOut"), true}, {QStringLiteral("state"), state}});
    Pause(50);
  }
  QJsonObject output = state.value(QStringLiteral("lastCapture")).toObject();
  const QString path = output.value(QStringLiteral("capturePath")).toString();
  ToolResult loaded = SessionTool(QStringLiteral("renderdoc_openCapture"), QJsonObject{{QStringLiteral("filePath"), path}}, call);
  output[QStringLiteral("ok")] = true;
  output[QStringLiteral("state")] = state;
  output[QStringLiteral("loadedIntoInspector")] = loaded.ok && loaded.structured.value(QStringLiteral("requestedCaptureLoaded")).toBool();
  if(!loaded.ok) output[QStringLiteral("loadError")] = loaded.error.toJson();
  output[QStringLiteral("replayStatus")] = SnapshotToJson(Snapshot());
  return ToolResult::Success(output);
}

void RegisterSessionTools(ToolRegistry &registry, QueryAdapter &adapter)
{
  auto string = [](bool nonempty = false) { QJsonObject s{{QStringLiteral("type"), QStringLiteral("string")}}; if(nonempty) s[QStringLiteral("minLength")] = 1; return s; };
  auto integer = [](int min, int max) { return QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), min}, {QStringLiteral("maximum"), max}}; };
  const QJsonObject trigger{{QStringLiteral("trigger"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("immediate"), QStringLiteral("frame"), QStringLiteral("delay")}}}},
      {QStringLiteral("frameNumber"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 0}, {QStringLiteral("maximum"), double(UINT32_MAX)}}},
      {QStringLiteral("delaySeconds"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}};
  auto add = [&](const char *name, const char *description, const QJsonObject &props, const QJsonArray &required, bool readOnly) {
    const QString n = QString::fromUtf8(name);
    if(registry.Contains(n)) return; // other registration owners retain their names
    QJsonObject schema{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), props}, {QStringLiteral("required"), required}, {QStringLiteral("additionalProperties"), false}};
    registry.Add(ToolDefinition{n, n, QString::fromUtf8(description), readOnly ? ToolEffects::ReadOnly : ToolEffects::SideEffect, ToolRequirements::None, schema},
        [&adapter, n](const QJsonObject &a, CallContext &c) { return adapter.SessionTool(n, a, c); });
  };
  const QJsonObject remote{{QStringLiteral("targetUrl"), string()}, {QStringLiteral("targetQuery"), string()}, {QStringLiteral("packageActivity"), string(true)}, {QStringLiteral("commandLine"), string()}};
  QJsonObject launch = remote; launch.remove(QStringLiteral("packageActivity"));
  launch[QStringLiteral("app")] = string(true);
  launch[QStringLiteral("workingDir")] = string();
  launch[QStringLiteral("platform")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("windows"), QStringLiteral("android")}}};
  add("renderdoc_launchApplication", "Launch an explicitly selected Windows/Android application; omitted platform returns PLATFORM_REQUIRED. Android native server setup may install/update RenderDoc server APKs and device capture settings. Application sessions are not MCP transport sessions.", launch, QJsonArray{QStringLiteral("app")}, false);
  add("renderdoc_launchWindowsApplication", "Launch a Windows executable using native RenderDoc injection and retain its target connection.", QJsonObject{{QStringLiteral("executablePath"), string(true)}, {QStringLiteral("workingDir"), string()}, {QStringLiteral("commandLine"), string()}}, QJsonArray{QStringLiteral("executablePath")}, false);
  add("renderdoc_launchRemoteApplication", "Select one Android target, launch package/activity and retain its target connection. Native server setup may install/update RenderDoc server APKs and device capture settings; does not install the requested application's APK.", remote, QJsonArray{QStringLiteral("packageActivity")}, false);
  add("renderdoc_checkAndroidLaunchReadiness", "Check native device transport, supported Android target, installed package and launcher activity without starting an application.", QJsonObject{{QStringLiteral("packageName"), string(true)}, {QStringLiteral("targetQuery"), string()}}, QJsonArray{QStringLiteral("packageName")}, true);
  add("renderdoc_listCaptureTargets", "Enumerate native RenderDoc device protocols and their supported capture targets.", QJsonObject(), QJsonArray(), true);
  add("renderdoc_openCapture", "Resolve the native active capture or load an absolute filePath; refuses to discard temporary or modified UI captures.", QJsonObject{{QStringLiteral("filePath"), string()}}, QJsonArray(), false);
  add("renderdoc_closeSession", "Disconnect the application target and remote connection without terminating the application, deleting files or closing replay. Not MCP transport-session deletion.", QJsonObject(), QJsonArray(), false);
  add("renderdoc_captureFrame", "Trigger one immediate/frame/delayed capture, preserve the saved file and load it into native replay when safe. Timeout does not revoke a submitted frame request.", trigger, QJsonArray(), false);
  add("renderdoc_triggerRemoteCapture", "Capture one frame from the active Android session, copy it locally and load replay when safe.", trigger, QJsonArray(), false);
  const QJsonObject wait{{QStringLiteral("timeoutMs"), integer(100, 60000)}, {QStringLiteral("pollMs"), integer(50, 2000)}};
  add("renderdoc_waitForLiveTarget", "Wait for the current application session target; defaults timeoutMs=10000 and pollMs=250. Does not launch or attach.", wait, QJsonArray(), true);
  add("renderdoc_waitForCapture", "Wait for a completed saved capture in the current application session; defaults timeoutMs=10000 and pollMs=250.", wait, QJsonArray(), true);
}
}
