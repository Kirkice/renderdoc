/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
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

#include "TraceCapture.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>

#include "Code/QRDUtils.h"

namespace
{
// The device directory used for the config and the protobuf. /data/local/tmp is the one path adb
// shell can always write without root, so it is used for both files and removed afterwards.
const char *DeviceTempRoot = "/data/local/tmp";

QString ShellQuote(const QString &path)
{
  return QStringLiteral("'") + path + QStringLiteral("'");
}

void Log(TraceCaptureResult &result, const QString &line)
{
  result.log.append(line);
}

// Reports a failure with the step that produced it. Every failure path goes through here so the
// message always names the step, and the caller never has to reconstruct which command broke.
void Fail(TraceCaptureResult &result, const QString &step, const QString &detail)
{
  if(result.error.isEmpty())
    result.error = detail.isEmpty() ? step : (step + QStringLiteral(": ") + detail);
}

// Trims tool output for display but keeps a bounded amount of it. Command output is evidence, so
// it is included rather than replaced with a summary.
QString Summarise(const QString &text, int maxChars = 4000)
{
  QString trimmed = text.trimmed();
  if(trimmed.size() > maxChars)
    trimmed = trimmed.left(maxChars) + QStringLiteral("\n[... truncated]");
  return trimmed;
}

// Turns an rdcstr into a QString for display. rdcstr is UTF-8 on every platform.
QString ToQStringDisplay(const rdcstr &s)
{
  return QString::fromUtf8(s.c_str(), (int)s.size());
}

// Locates `traceconv`. Returns an empty string when it cannot be found, and never guesses a
// directory: a wrong guess would fail later with a confusing error.
QString FindTraceconv(const TraceCaptureOptions &opts, TraceCaptureResult &result)
{
  if(!opts.traceconvPath.isEmpty())
  {
    if(QFileInfo::exists(opts.traceconvPath))
    {
      Log(result, QStringLiteral("traceconv: using configured path %1").arg(opts.traceconvPath));
      return opts.traceconvPath;
    }

    Log(result,
        QStringLiteral("traceconv: configured path %1 does not exist, falling back to discovery")
            .arg(opts.traceconvPath));
  }

  // [观测] The Snapdragon Profiler distribution bundles Perfetto's converter under several names
  // depending on release and architecture. Only the ones that were actually observed are probed.
  if(!opts.snapdragonProfilerPath.isEmpty())
  {
    const QDir sdpDir(opts.snapdragonProfilerPath);
    const QStringList candidates = {
        QStringLiteral("traceconv-win-amd64.exe"),
        QStringLiteral("traceconv.exe"),
        QStringLiteral("traceconv"),
        QStringLiteral("resources/traceconv-win-amd64.exe"),
        QStringLiteral("Resources/traceconv-win-amd64.exe"),
    };

    for(const QString &candidate : candidates)
    {
      const QString path = sdpDir.absoluteFilePath(candidate);
      if(QFileInfo::exists(path))
      {
        Log(result, QStringLiteral("traceconv: found next to Snapdragon Profiler at %1").arg(path));
        return path;
      }
    }

    Log(result, QStringLiteral("traceconv: no bundled converter found under %1")
                    .arg(opts.snapdragonProfilerPath));
  }

  const QString onPath = QStandardPaths::findExecutable(QStringLiteral("traceconv"));
  if(!onPath.isEmpty())
  {
    Log(result, QStringLiteral("traceconv: found on PATH at %1").arg(onPath));
    return onPath;
  }

  Log(result, QStringLiteral("traceconv: not found on PATH"));
  return QString();
}

// Runs a local tool to completion and returns its combined output. `ok` reports whether it started
// and exited normally with code 0.
QString RunLocalTool(const QString &exe, const QStringList &args, int timeoutMs, bool &ok)
{
  ok = false;

  QProcess process;
  process.setProgram(exe);
  process.setArguments(args);
  process.start();

  if(!process.waitForStarted(10000))
  {
    return QStringLiteral("failed to start %1: %2").arg(exe, process.errorString());
  }

  if(!process.waitForFinished(timeoutMs))
  {
    process.kill();
    process.waitForFinished(5000);
    return QStringLiteral("%1 timed out after %2 ms").arg(exe).arg(timeoutMs);
  }

  const QString stdOut = QString::fromUtf8(process.readAllStandardOutput());
  const QString stdErr = QString::fromUtf8(process.readAllStandardError());

  QString combined;
  if(!stdOut.trimmed().isEmpty())
    combined += stdOut;
  if(!stdErr.trimmed().isEmpty())
  {
    if(!combined.isEmpty())
      combined += QStringLiteral("\n");
    combined += stdErr;
  }

  if(process.exitStatus() != QProcess::NormalExit)
  {
    combined += QStringLiteral("\n%1 crashed").arg(exe);
    return combined;
  }

  if(process.exitCode() != 0)
  {
    combined += QStringLiteral("\n%1 exited with code %2").arg(exe).arg(process.exitCode());
    return combined;
  }

  ok = true;
  return combined;
}

}    // anonymous namespace

QString DefaultPerfettoConfig(int durationMs, uint64_t bufferSizeKb)
{
  // Every data source and event named here is part of the standard Android Perfetto surface, so
  // the config does not assume a vendor-specific probe or a rooted device. The counters chosen are
  // the ones that explain GPU-bound frames: what ran, on which CPU, at what frequency, and how
  // much memory the system had while it ran.
  //
  // NOTE: this is textproto, selected with `perfetto --txt`.
  return QStringLiteral(
             "duration_ms: %1\n"
             "flush_period_ms: 1000\n"
             "buffers: {\n"
             "    size_kb: %2\n"
             "    fill_policy: RING_BUFFER\n"
             "}\n"
             "data_sources: {\n"
             "    config {\n"
             "        name: \"linux.ftrace\"\n"
             "        ftrace_config {\n"
             "            ftrace_events: \"sched/sched_switch\"\n"
             "            ftrace_events: \"sched/sched_wakeup\"\n"
             "            ftrace_events: \"sched/sched_waking\"\n"
             "            ftrace_events: \"power/cpu_frequency\"\n"
             "            ftrace_events: \"power/cpu_idle\"\n"
             "            ftrace_events: \"power/suspend_resume\"\n"
             "            atrace_categories: \"gfx\"\n"
             "            atrace_categories: \"view\"\n"
             "            atrace_categories: \"sched\"\n"
             "            atrace_categories: \"freq\"\n"
             "            atrace_categories: \"idle\"\n"
             "            atrace_categories: \"am\"\n"
             "        }\n"
             "    }\n"
             "}\n"
             "data_sources: {\n"
             "    config {\n"
             "        name: \"linux.sys_stats\"\n"
             "        sys_stats_config {\n"
             "            meminfo_period_ms: 1000\n"
             "            meminfo_counters: MEMINFO_MEM_TOTAL\n"
             "            meminfo_counters: MEMINFO_MEM_FREE\n"
             "            meminfo_counters: MEMINFO_MEM_AVAILABLE\n"
             "            meminfo_counters: MEMINFO_CACHED\n"
             "            vmstat_period_ms: 1000\n"
             "            vmstat_counters: VMSTAT_PGFAULT\n"
             "            vmstat_counters: VMSTAT_PGMAJFAULT\n"
             "            stat_period_ms: 1000\n"
             "            stat_counters: STAT_CPU_TIMES\n"
             "        }\n"
             "    }\n"
             "}\n"
             "data_sources: {\n"
             "    config {\n"
             "        name: \"linux.pslist\"\n"
             "        pslist_config {\n"
             "            scan_interval_ms: 1000\n"
             "        }\n"
             "    }\n"
             "}\n")
      .arg(durationMs)
      .arg(bufferSizeKb);
}

TraceCaptureResult RunTraceCapture(IDeviceProtocolController *protocol, const QString &url,
                                   const TraceCaptureOptions &opts)
{
  TraceCaptureResult result;

  if(protocol == NULL)
  {
    Fail(result, QStringLiteral("capture"),
         QStringLiteral("no device protocol is available for this host"));
    return result;
  }

  // Every command addresses the device through exactly the URL the caller selected it by, so the
  // device identity is never re-derived from a friendly name or an index.
  const rdcstr urlStr = url.toUtf8().data();

  Log(result, QStringLiteral("device: %1 / protocol %2")
                  .arg(url, ToQStringDisplay(protocol->GetProtocolName())));

  // -----------------------------------------------------------------------------------------------
  // Step 1: probe perfetto. A device without it cannot be traced by this path at all, and saying so
  // up front is far more useful than a failure part-way through the pipeline.
  // -----------------------------------------------------------------------------------------------
  {
    rdcstr out;
    const ResultDetails probe = protocol->ExecCommand(
        urlStr, "command -v perfetto || ls -l /system/bin/perfetto", &out);

    if(!probe.OK())
    {
      Log(result, QStringLiteral("perfetto probe failed: %1").arg(ToQStringDisplay(probe.Message())));
      Fail(result, QStringLiteral("probe perfetto"),
           QStringLiteral("this device does not expose `perfetto`, which Android 10+ provides as "
                          "the system tracer. Nothing was recorded.\n%1")
               .arg(ToQStringDisplay(probe.Message())));
      return result;
    }

    Log(result, QStringLiteral("perfetto: %1").arg(Summarise(ToQStringDisplay(out), 200)));
  }

  // -----------------------------------------------------------------------------------------------
  // Step 2: stage the config and the trace under /data/local/tmp. A per-run directory keeps two
  // concurrent or successive captures from overwriting each other's files.
  // -----------------------------------------------------------------------------------------------
  // QUuid::WithoutBraces is newer than the Qt versions this project still builds against, so the
  // braces and separators are stripped from the string form instead.
  QString runSuffix = QUuid::createUuid().toString();
  runSuffix.remove(QLatin1Char('{')).remove(QLatin1Char('}')).remove(QLatin1Char('-'));

  const QString runId = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
                        QStringLiteral("-") + runSuffix.left(8);

  const QString deviceDir =
      QStringLiteral("%1/rdoc_trace_%2").arg(QLatin1String(DeviceTempRoot), runId);
  const QString deviceConfig = deviceDir + QStringLiteral("/config.txtproto");
  const QString deviceTrace = deviceDir + QStringLiteral("/capture.perfetto-trace");

  result.deviceTracePath = deviceTrace;

  {
    rdcstr out;
    const ResultDetails mk = protocol->ExecCommand(
        urlStr, QStringLiteral("mkdir -p %1").arg(ShellQuote(deviceDir)), &out);

    if(!mk.OK())
    {
      Log(result, QStringLiteral("mkdir failed: %1").arg(ToQStringDisplay(mk.Message())));
      Fail(result, QStringLiteral("create device staging directory"),
           ToQStringDisplay(mk.Message()));
      return result;
    }

    Log(result, QStringLiteral("staging directory: %1").arg(deviceDir));
  }

  // The local output directory is deliberately NOT a QTemporaryDir: the converted JSON is the
  // artefact the user asked for, so it must survive this function. The path is reported back and
  // logged, which makes it findable without the caller having to remember it.
  const QString localDir = QDir(QDir::tempPath())
                               .absoluteFilePath(QStringLiteral("rdoc-trace-%1").arg(runId));
  if(!QDir().mkpath(localDir))
  {
    Fail(result, QStringLiteral("create local output directory"),
         QStringLiteral("could not create %1").arg(localDir));
    return result;
  }

  const QString localConfig = QDir(localDir).absoluteFilePath(QStringLiteral("config.txtproto"));
  const QString localTrace = QDir(localDir).absoluteFilePath(QStringLiteral("capture.perfetto-trace"));

  result.localTracePath = localTrace;
  Log(result, QStringLiteral("local output directory: %1").arg(localDir));

  // -----------------------------------------------------------------------------------------------
  // Step 3: write and push the config. The config text is caller-supplied when present, so a user
  // who knows which counters they want is not limited to the default surface.
  // -----------------------------------------------------------------------------------------------
  {
    const QString configText = opts.configText.isEmpty()
                                   ? DefaultPerfettoConfig(opts.durationMs, opts.bufferSizeKb)
                                   : opts.configText;

    QFile configFile(localConfig);
    if(!configFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
      Fail(result, QStringLiteral("write local config"),
           QStringLiteral("could not write %1: %2").arg(localConfig, configFile.errorString()));
      return result;
    }

    const QByteArray utf8 = configText.toUtf8();
    if(configFile.write(utf8) != utf8.size())
    {
      Fail(result, QStringLiteral("write local config"),
           QStringLiteral("short write to %1").arg(localConfig));
      return result;
    }
    configFile.close();

    const ResultDetails push = protocol->PushFile(urlStr, localConfig.toUtf8().data(),
                                                  deviceConfig.toUtf8().data());
    if(!push.OK())
    {
      Log(result, QStringLiteral("push config failed: %1").arg(ToQStringDisplay(push.Message())));
      Fail(result, QStringLiteral("push perfetto config"), ToQStringDisplay(push.Message()));
      return result;
    }

    Log(result, QStringLiteral("pushed %1 byte config to %2").arg(utf8.size()).arg(deviceConfig));
  }

  // -----------------------------------------------------------------------------------------------
  // Step 4: record. `perfetto --txt` records synchronously until the config's duration_ms expires,
  // which is why this call blocks for that long and why cancellation cannot cut it short: the
  // device protocol serialises commands onto one thread, so there is no channel to send a stop
  // signal while this command is in flight.
  // -----------------------------------------------------------------------------------------------
  {
    const QString command = QStringLiteral("perfetto --txt -c %1 -o %2")
                                .arg(ShellQuote(deviceConfig), ShellQuote(deviceTrace));

    Log(result, QStringLiteral("recording: %1 (blocking for the configured duration)")
                    .arg(command));

    rdcstr out;
    const ResultDetails rec = protocol->ExecCommand(urlStr,
                                                    command.toUtf8().data(), &out);

    const QString recOut = ToQStringDisplay(out);
    if(!recOut.trimmed().isEmpty())
      Log(result, QStringLiteral("perfetto output: %1").arg(Summarise(recOut)));

    if(!rec.OK())
    {
      Log(result, QStringLiteral("recording failed: %1").arg(ToQStringDisplay(rec.Message())));
      Fail(result, QStringLiteral("record trace on device"), ToQStringDisplay(rec.Message()));
      return result;
    }

    // perfetto can exit successfully while writing nothing when the config is rejected for a
    // non-fatal reason, so the existence and size of the output are checked rather than assumed.
    rdcstr stat;
    const ResultDetails ls = protocol->ExecCommand(
        urlStr, QStringLiteral("ls -l %1").arg(ShellQuote(deviceTrace)), &stat);

    if(!ls.OK())
    {
      Log(result, QStringLiteral("trace stat failed: %1").arg(ToQStringDisplay(ls.Message())));
      Fail(result, QStringLiteral("verify recorded trace"),
           QStringLiteral("perfetto reported success but no trace was produced.\n%1")
               .arg(ToQStringDisplay(ls.Message())));
      return result;
    }

    Log(result, QStringLiteral("recorded: %1").arg(Summarise(ToQStringDisplay(stat), 300)));
  }

  // -----------------------------------------------------------------------------------------------
  // Step 5: pull the protobuf back. It is kept even if conversion fails, so a recording is never
  // lost to a converter problem.
  // -----------------------------------------------------------------------------------------------
  {
    const ResultDetails pull = protocol->PullFile(
        urlStr, deviceTrace.toUtf8().data(), localTrace.toUtf8().data());

    if(!pull.OK())
    {
      Log(result, QStringLiteral("pull failed: %1").arg(ToQStringDisplay(pull.Message())));
      Fail(result, QStringLiteral("pull trace from device"), ToQStringDisplay(pull.Message()));
      return result;
    }

    const QFileInfo info(localTrace);
    Log(result, QStringLiteral("pulled %1 bytes to %2")
                    .arg(info.size())
                    .arg(localTrace));

    if(info.size() == 0)
    {
      Fail(result, QStringLiteral("pull trace from device"),
           QStringLiteral("the trace file transferred empty: %1").arg(localTrace));
      return result;
    }
  }

  // The device copy is no longer needed. Failure to remove it is logged but is not fatal: the
  // capture itself already succeeded, and /data/local/tmp is periodically cleaned by the platform.
  {
    rdcstr ignored;
    const ResultDetails rm = protocol->ExecCommand(
        urlStr, QStringLiteral("rm -rf %1").arg(ShellQuote(deviceDir)), &ignored);
    if(!rm.OK())
      Log(result, QStringLiteral("cleanup of %1 failed (non-fatal): %2")
                      .arg(deviceDir, ToQStringDisplay(rm.Message())));
    else
      Log(result, QStringLiteral("removed device staging directory"));
  }

  // -----------------------------------------------------------------------------------------------
  // Step 6: convert to Chrome Trace Event Format. `perfetto` itself writes a protobuf, which the
  // importer does not parse, so the conversion is required rather than optional.
  // -----------------------------------------------------------------------------------------------
  result.traceconvPath = FindTraceconv(opts, result);

  if(result.traceconvPath.isEmpty())
  {
    Fail(result, QStringLiteral("locate traceconv"),
         QStringLiteral("The trace was recorded and pulled to:\n%1\n\n"
                        "Converting it needs Perfetto's `traceconv`. Set its path in the capture "
                        "dialog, install it on PATH, or point RenderDoc at a Snapdragon Profiler "
                        "installation that bundles it.")
             .arg(localTrace));
    return result;
  }

  const QString jsonPath = QDir(localDir).absoluteFilePath(QStringLiteral("capture.json"));
  {
    bool ok = false;
    const QString toolOut = RunLocalTool(result.traceconvPath,
                                         {QStringLiteral("json"), localTrace, jsonPath}, 10 * 60 * 1000,
                                         ok);

    if(!toolOut.trimmed().isEmpty())
      Log(result, QStringLiteral("traceconv output: %1").arg(Summarise(toolOut)));

    const qint64 jsonSize = QFileInfo::exists(jsonPath) ? QFileInfo(jsonPath).size() : -1;

    if(!ok || jsonSize <= 0)
    {
      // An older or newer `traceconv` may expect different arguments. The tool's own output is
      // reported verbatim so that mismatch is visible instead of presenting a generic error.
      Fail(result, QStringLiteral("convert trace to Chrome Trace Event Format"),
           QStringLiteral("`traceconv json` did not produce %1.\n%2\n\nThe recorded protobuf is "
                          "retained at:\n%3")
               .arg(jsonPath, Summarise(toolOut), localTrace));
      return result;
    }

    Log(result, QStringLiteral("converted %1 bytes to %2").arg(jsonSize).arg(jsonPath));
  }

  result.localJsonPath = jsonPath;
  result.success = true;
  Log(result, QStringLiteral("capture complete"));
  return result;
}

void TraceCaptureJob::Start(QObject *owner, IDeviceProtocolController *protocol, const QString &url,
                            const TraceCaptureOptions &opts, Callback done)
{
  QPointer<QObject> guard(owner);

  LambdaThread *thread = new LambdaThread([protocol, url, opts, done, guard]() {
    const TraceCaptureResult result = RunTraceCapture(protocol, url, opts);

    // The caller's widget may have been destroyed while the device was recording, in which case
    // the result still exists (it is local evidence) but there is nobody to deliver it to.
    if(!guard)
      return;

    GUIInvoke::call(guard, [done, result]() { done(result); });
  });

  thread->setName(lit("TraceCapture"));
  thread->selfDelete(true);
  thread->start();
}
