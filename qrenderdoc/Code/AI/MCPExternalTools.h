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

// P2-3 Mali Offline Compiler integration
// --------------------------------------
// The VS Code extension ships a "Mali Offline Compiler" action in its shader
// inspector: it takes the *source text* the inspector already has, writes it to a
// temp file, shells out to `malioc` with an optional `-c <profile>` (the compiler's
// core/device selector) and shows the raw stdout in a modal. That is a UI-only,
// best-effort feature and the extension deliberately does NOT expose it over MCP -
// a shader snapshot is not a fact the capture can prove.
//
// The native port keeps the same workflow but splits it into three parts so the
// interesting logic is testable and the process boundary is explicit:
//
//   1. EXPORT     - turn the shader that is actually bound at an event into a file
//                   malioc understands. The capture carries the compiled bytecode
//                   (SPIR-V for Vulkan), not the GLSL source, so the export is the
//                   bytecode with the extension that identifies its encoding. This
//                   is a *fact* read from the capture and is reported as such.
//
//   2. COMMAND    - build an explicit argv vector for the compiler. The extension
//                   built a shell string (`chcp 65001 >nul & "malioc.exe" ...`),
//                   which is fragile on Windows and impossible to reason about
//                   cross-platform. The port builds an argument *vector* with no
//                   shell, so quoting, spaces in paths and non-ASCII paths all work
//                   and nothing is ever interpreted as a shell metacharacter.
//
//   3. PARSE      - turn the compiler's stdout into a structured report (sections
//                   of numeric metrics plus the derived totals) instead of dumping
//                   a wall of text. Every parsed value carries the raw line so a
//                   client can always check the derivation, and unparsable output
//                   degrades to the raw text with Confidence::Low rather than
//                   silently producing zeros.
//
// Everything above is pure data and free functions: no Qt widgets, no capture, no
// IReplayController, no clock. The single process-touching entry point is
// RunMaliAnalysis(), which is isolated at the bottom of this header and takes a
// caller-supplied cancellation predicate so it never outlives a cancelled tool
// call. That keeps the whole module unit-testable without a compiler installed.
//
// Layering (unchanged): MCPServer -> MCPToolRegistry -> MCPQueryAdapter. This
// module is a peer of MCPAnalysisModel.h and MCPCaptureMatch.h and, like them,
// never talks to the UI.

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <stdint.h>
#include "MCPAnalysisModel.h"
#include "MCPQueryAdapter.h"
#include "MCPToolRegistry.h"

namespace MCP
{
//
// ---- Tool configuration and device profiles -------------------------------
//

// Every tunable the integration needs, as plain data. The caller fills this from
// the app config (MCP_MaliCompilerPath / MCP_MaliDevice / MCP_MaliExtraArgs) and
// from the tool arguments, so nothing here reads the config file itself.
struct MaliTargetOptions
{
  // The `-c <profile>` value (malioc's core/device selector; `-c` is the short
  // form of `--core`). Empty means "let malioc pick its own default profile",
  // which is exactly what the extension's "Default profile" option did.
  QString device;

  // Extra arguments appended verbatim after the built-in ones. The caller owns
  // them: the module never adds or removes flags the caller did not ask for, and
  // it never splits them on whitespace.
  QStringList extraArgs;

  // Hard wall-clock limit for one compiler invocation. malioc is a static
  // analysis pass over a shader and normally finishes in well under a second, so
  // a generous default keeps pathological shaders from hanging a tool call.
  int timeoutMs = 30000;
};

// The Mali GPU profiles the plugin offered in its device picker, in the same
// order. Exposed so a client can validate a device name before running anything
// and so the error message can list the valid values.
QStringList MaliDeviceNames();

// True when `device` is one of MaliDeviceNames(). An empty device is "default
// profile", not a known device.
bool IsKnownMaliDevice(const QString &device);

// Returns `device` when it is a known profile and an empty string otherwise, so
// an unknown or misspelled device degrades to the default profile instead of
// being passed to the compiler as-is. Mirrors the extension's
// normalizeMaliOfflineCompilerDevice().
QString NormalizeMaliDevice(const QString &device);

// Turn a user-supplied setting into an executable path. Accepts either the
// executable itself or a directory containing it (the extension's "if the user
// filled in a directory, append malioc[.exe]" convenience), and returns the path
// that should be executed. Returns an empty string when `configured` is empty.
// The file is NOT required to exist: existence is a separate diagnostic so the
// caller can distinguish "not configured" from "configured but missing".
QString ResolveMaliExecutablePath(const QString &configured);

// The config shape serialised for the diagnostic tool, so a client can tell
// "not configured" from "configured but the file does not exist" without running
// anything.
QJsonObject MaliTargetOptionsToJson(const MaliTargetOptions &options);

//
// ---- Shader export --------------------------------------------------------
//

// The result of turning a bound shader into a file the compiler can consume.
// `contents` is the exact byte payload to write; `extension` is chosen from the
// shader encoding so the compiler can infer the language without extra flags.
struct MaliShaderExport
{
  QString stage;              // human readable stage, e.g. "Vertex"
  QString entryPoint;
  uint64_t resourceValue = 0; // the shader ResourceId value, for evidence
  int encoding = 0;           // ShaderEncoding as an int, for diagnostics
  bool binary = true;         // true when `contents` is bytecode, not text
  QString extension;          // ".spv" for SPIR-V
  QString suggestedFileName;  // "<entryPoint>_<stage><extension>", filesystem-safe
  QByteArray contents;

  // SHA-256 of contents. This identifies the exact shader revision submitted to
  // malioc and lets UI clients avoid presenting an analysis for different bytes.
  QString revision;

  // Non-empty when the export could not faithfully represent the shader (e.g. an
  // encoding malioc cannot read), so the caller reports a limitation rather than
  // running the compiler on something it will reject.
  QString limitation;
};

// Whether malioc can read a captured shader with this encoding. Vulkan captures
// carry SPIR-V and OpenGL captures carry either SPIR-V or the GLSL source text;
// all three are things malioc consumes directly (SPIR-V as a binary module, GLSL
// as a `.vert`/`.frag`/... source file). Anything else - DXBC, DXIL, HLSL,
// SPIR-V assembly, Slang - reports a limitation instead of guessing.
bool MaliSupportsEncoding(int encoding);
QString MaliEncodingExtension(int encoding);

// A short, human readable name for a captured shader encoding, used by the UI to
// describe what was actually handed to malioc rather than assuming SPIR-V.
QString MaliEncodingName(int encoding);

// Build the export descriptor for one shader. `rawBytes` are the bytes the
// replay controller returned for the shader; `byteLength` is the length the
// reflection reported, which can exceed rawBytes.size() when the capture
// truncated the payload - that difference is surfaced as a limitation instead of
// being hidden.
MaliShaderExport BuildMaliShaderExport(const QString &stage, const QString &entryPoint,
                                       uint64_t resourceValue, int encoding,
                                       const QByteArray &rawBytes, int64_t byteLength);

//
// ---- Command line ---------------------------------------------------------
//

// Build the exact argv to execute: the executable, the shader path, then `-c
// <profile>` when a device was requested (the extension passed the device as
// `-c "<profile>"`; `-c` is malioc's short form of `--core`) and finally the
// caller's extra arguments. Kept separate from execution so the test suite can
// assert the ordering rules without a compiler on the machine.
QStringList BuildMaliCommandLine(const QString &executable, const QString &shaderPath,
                                 const MaliTargetOptions &options);

//
// ---- Output parsing -------------------------------------------------------
//

// One parsed `name: value` line. `numeric` is false for pure-text entries (a
// description, a hardware name), in which case `value` is 0 and `text` holds the
// payload. `raw` always keeps the original line.
struct MaliMetric
{
  QString name;
  bool numeric = false;
  double value = 0.0;
  QString unit;   // "%", "bytes", ... - empty when the value is unitless
  QString text;   // the value as written, for non-numeric entries
  QString raw;    // the original line
};

// A titled group of metrics. `Shader program for X` starts a section; subsequent
// metrics attach to it until the next title.
struct MaliSection
{
  QString title;
  QVector<MaliMetric> metrics;
};

// The A/LS/V/T breakdown printed by recent malioc versions for total, shortest
// and longest instruction paths. `available` is false when the compiler printed
// an unavailable value such as "N/A".
struct MaliCycleBreakdown
{
  bool available = false;
  double arithmetic = 0.0;
  double loadStore = 0.0;
  double varying = 0.0;
  double texture = 0.0;
  QString bound;
};

// The structured compiler report. `ran` is true as soon as a process was started,
// regardless of exit code, so a client can distinguish "never ran" (not
// configured / tool missing) from "ran and failed".
struct MaliReport
{
  bool ran = false;
  int exitCode = -1;
  QString device;            // the requested device, echoed back
  QString compilerVersion;   // parsed from the banner
  QString hardware;          // parsed from "Hardware: ..."
  QVector<MaliSection> sections;
  QStringList warnings;

  // Derived totals. `hasTotals` is false when the output did not contain the
  // expected total lines, so a consumer never mistakes an absent value for zero.
  bool hasTotals = false;
  double totalCycles = 0.0;
  double totalInstructions = 0.0;
  double registers = 0.0;
  double spillBytes = 0.0;

  // Performance-oriented fields promoted from the raw report. They remain
  // optional because malioc output differs by compiler version and GPU family.
  bool hasWorkRegisters = false;
  double workRegisters = 0.0;
  bool hasUniformRegisters = false;
  double uniformRegisters = 0.0;
  bool hasStackSize = false;
  double stackSize = 0.0;
  bool stackSpillingKnown = false;
  bool stackSpilling = false;
  MaliCycleBreakdown totalPath;
  MaliCycleBreakdown shortestPath;
  MaliCycleBreakdown longestPath;
  QStringList riskSignals;

  QString error;      // non-empty when the run or the parse failed
  QString rawOutput;  // stdout+stderr, always kept so nothing is lost

  // Confirmed  - ran, exit 0, totals parsed.
  // ToVerify   - ran, exit 0, but no totals were found (output shape changed).
  // Low        - ran and failed, or could not run.
  Confidence confidence = Confidence::ToVerify;
  QString limitation;
};

// Parse compiler stdout/stderr into a report. Tolerant by design: unknown lines
// are kept in `rawOutput` and simply do not produce metrics, so a future malioc
// version degrades to fewer fields rather than a hard failure.
MaliReport ParseMaliOutput(const QString &stdoutText, const QString &stderrText, int exitCode);

// The metric with this name in any section, or NULL. Case-insensitive, and
// matches on the whole name so "Total cycles" never matches
// "Total instruction cycles".
const MaliMetric *FindMaliMetric(const MaliReport &report, const QString &name);

QJsonObject MaliReportToJson(const MaliReport &report);

//
// ---- Running the compiler -------------------------------------------------
//

// Why a run did or did not produce a report. This is the field a client branches
// on, and it is deliberately finer-grained than a bool so the diagnostic tool can
// explain what to fix.
enum class MaliRunStatus
{
  NotConfigured,      // no configured, bundled, or PATH-discovered compiler
  ToolMissing,        // configured, but the executable is not there
  UnsupportedShader,  // the shader encoding cannot be compiled
  Timeout,            // the process exceeded MaliTargetOptions::timeoutMs
  Cancelled,          // the caller's cancellation predicate fired
  FailedToStart,      // QProcess could not launch it
  Ran,                // a process ran and finished (see report.exitCode)
};

QString MaliRunStatusName(MaliRunStatus status);

// What to actually run. `shaderPath` is the file the caller wrote from a
// MaliShaderExport; this module never writes files itself, so the temp-file
// lifetime stays with the caller that owns the tool call.
struct MaliRunRequest
{
  QString configuredPath;  // the raw MCP_MaliCompilerPath value
  MaliTargetOptions target;
  QString shaderPath;
  QString workDir;         // empty = inherit the current directory
};

// The full outcome: the resolved command, the report and the one-line reason a
// client should show the user when `status` is not Ran.
struct MaliRunResult
{
  MaliRunStatus status = MaliRunStatus::NotConfigured;
  QString executable;         // resolved path that was (or would be) executed
  QStringList commandLine;    // the argv that was (or would be) used
  QString hint;               // actionable fix for non-Ran statuses
  MaliReport report;
};

// Run the compiler on a shader that has already been written to disk. Blocking:
// the process is given `timeoutMs` and the caller's predicate is polled while it
// runs, so a cancelled tool call returns promptly instead of waiting out the
// timeout. No shell is involved - the argv from BuildMaliCommandLine() is passed
// straight to QProcess, which is what makes paths with spaces and non-ASCII
// characters safe.
MaliRunResult RunMaliAnalysis(const MaliRunRequest &request,
                              const std::function<bool()> &cancelled = std::function<bool()>());

QJsonObject MaliRunResultToJson(const MaliRunResult &result);

// One shader "analysis" as reported to a client: the export descriptor, the run
// outcome and the evidence tying it back to a capture event. This is the shape the
// tool returns, and it is intentionally the same evidence vocabulary every other
// analysis tool uses so a Mali result and a timing result can sit in one report.
struct MaliEventAnalysis
{
  bool available = false;
  uint32_t eventId = 0;
  QString eventName;
  QString capturePath;
  MaliShaderExport shader;
  MaliRunResult run;

  // CaptureEvidence for the shader resource plus TimingEvidence when the caller
  // supplied the event's GPU duration.
  QVector<EvidenceRef> evidence;
  QStringList limitations;
  Confidence confidence = Confidence::ToVerify;
};

// Recompute `limitations` and `confidence` from the export and the run so the two
// can never disagree with `shader.limitation` / `run.report.limitation`.
void FinalizeMaliEventAnalysis(MaliEventAnalysis &analysis);

QJsonObject MaliEventAnalysisToJson(const MaliEventAnalysis &analysis);

// Human readable rendering: the capture/event anchor, the export facts, the
// compiler status and the parsed metrics. Section titles are stable so a client
// written against the extension's modal still finds the numbers.
QString MaliEventAnalysisToText(const MaliEventAnalysis &analysis);

// Registers the P2-3 external-tool MCP tools (Mali shader analysis plus the
// configured-tool diagnostic). `adapter` must outlive the registry. Returns the
// number of tools registered.
int RegisterExternalToolTools(ToolRegistry &registry, QueryAdapter &adapter);

}    // namespace MCP
