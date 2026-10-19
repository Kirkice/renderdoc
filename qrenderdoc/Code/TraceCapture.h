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

#pragma once

#include <stdint.h>

#include <functional>

#include <QString>
#include <QStringList>

#include "renderdoc_replay.h"

class QObject;

// ---------------------------------------------------------------------------------------------
// P1 — on-device system trace capture.
//
// This is the capture half of the trace-correlation feature. The importer in SystemTrace.h reads
// a Chrome Trace Event Format file; this driver produces one by driving the device itself:
//
//   1. probe `perfetto` on the device (every Android 10+ device ships it as the system tracer),
//   2. push a Perfetto textproto config,
//   3. record to a protobuf trace on the device,
//   4. pull that trace back,
//   5. convert it to Chrome Trace Event Format with Perfetto's own `traceconv`,
//   6. return the path of the importable JSON.
//
// It reaches the device only through IDeviceProtocolController::ExecCommand / PushFile /
// PullFile, so it works for any protocol that implements them (currently `adb`) and never links
// against an engine or vendor binary. [观测] Neither `perfetto` nor `traceconv` is bundled or
// redistributed by RenderDoc: the device binary is the platform's own, and `traceconv` is located
// on the user's machine (the Snapdragon Profiler distribution ships `traceconv-win-amd64.exe`, and
// a standalone build may be on PATH).
//
// Limits that are real and therefore stated rather than worked around:
//   * A device protocol serialises commands onto one command thread (AndroidController::Invoke
//     spins until the queued command finishes). A second ExecCommand therefore CANNOT interrupt a
//     recording that is already running, so a capture is always bounded by the config's
//     `duration_ms` instead of being stoppable on demand. Cancelling only stops the *pipeline*
//     between steps.
//   * The exact `traceconv` sub-command spelling has varied between Perfetto releases. The probe
//     and conversion results are reported verbatim in TraceCaptureResult::log so a mismatch is
//     visible as evidence instead of being smoothed over.
// ---------------------------------------------------------------------------------------------

struct TraceCaptureOptions
{
  // Perfetto textproto config. When empty, DefaultPerfettoConfig() is used.
  QString configText;
  // Recording length and ring buffer size baked into the default config. Ignored when
  // `configText` is supplied, because then the config owns those values.
  int durationMs = 5000;
  uint64_t bufferSizeKb = 16384;
  // Explicit path to `traceconv`. When empty, discovery is attempted (Snapdragon Profiler install
  // directory, then PATH).
  QString traceconvPath;
  // Directory of the Snapdragon Profiler installation, used only to look for its bundled
  // `traceconv`. Empty disables that discovery step.
  QString snapdragonProfilerPath;
};

struct TraceCaptureResult
{
  bool success = false;
  // The pulled Perfetto protobuf. Retained even when conversion fails, so the recording is not
  // lost and the user can convert it later.
  QString localTracePath;
  // The Chrome Trace Event Format JSON. This is the file ImportChromeTraceEventFormat() consumes.
  QString localJsonPath;
  // The path the trace had while on the device, kept for diagnostics.
  QString deviceTracePath;
  QString traceconvPath;
  // Non-empty only on failure, and always names the step that failed plus the tool's own output.
  QString error;
  // Every command issued and every tool result, in order. The UI shows this so a capture failure
  // is diagnosable from what actually ran rather than from a generic message.
  QStringList log;
};

// A default Perfetto config covering the counters this feature correlates against: scheduler
// switches, per-CPU frequency and idle residency, the atrace `gfx`/`sched` categories, and Android
// system memory/CPU statistics. Every event name here is part of the standard Android ftrace
// surface, so the config does not assume an Adreno- or vendor-specific probe exists.
QString DefaultPerfettoConfig(int durationMs, uint64_t bufferSizeKb);

// Runs the capture pipeline. Owns no state and must be called off the UI thread, because every
// device command blocks until it completes. Never throws: every failure is returned in `error`
// with the failing step recorded in `log`.
TraceCaptureResult RunTraceCapture(IDeviceProtocolController *protocol, const QString &url,
                                   const TraceCaptureOptions &opts);

// Convenience wrapper: runs RunTraceCapture() on a worker thread and delivers the result back on
// `owner`'s thread. `done` is invoked exactly once. The returned thread self-deletes.
class TraceCaptureJob
{
public:
  typedef std::function<void(const TraceCaptureResult &)> Callback;

  static void Start(QObject *owner, IDeviceProtocolController *protocol, const QString &url,
                    const TraceCaptureOptions &opts, Callback done);
};
