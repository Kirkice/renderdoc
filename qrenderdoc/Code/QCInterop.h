/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2018-2026 Baldur Karlsson
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

#include <QDebug>
#include <QStringList>
#include <QVariantList>
#include "Code/QRDUtils.h"
#include "renderdoc_replay.h"

class QTcpServer;
class QTcpSocket;

// Qualcomm Snapdragon Profiler interop.
//
// The structural shape of this file mirrors RGPInterop.{h,cpp} - one listener, one command
// vocabulary, one connect/select event model - so the two vendor-tool integrations share a mental
// model. See the planning document sections 3 and 4.
//
// IMPORTANT EVIDENCE DISCIPLINE (planning document section 8.9.2) - CLOSED UNKNOWN:
//   - The RGP wire protocol is defined by AMD and is implemented to match RGP exactly.
//   - The Snapdragon Profiler interop wire protocol is now established to NOT EXIST as modelled.
//     An exhaustive scan of every string in the SDP distribution (.cache/sdp-scan/ports.txt) finds
//     ZERO occurrences of `QCInterop`, `QCInteropVersion`, `interop_version`, `interoplinearid`,
//     `cmdbufid`, `interop_name`, the `command=` / `startcommand=` / `endcommand=` envelope, or
//     ports 29100 / 29000 / 29500. SDP's real host transport is ZeroMQ over `tcp:localhost:65100`
//     (Vulkan) and `tcp:localhost:65101` (D3D12), spoken only by SDP's own `SDPCore.NetUtils`
//     peers - there is no documented or discoverable text-envelope interop surface for a third
//     party to dial.
//   - Consequently the classes below remain as an OPTIMISTIC, self-consistent placeholder rather
//     than a claim about SDP. They are kept so that:
//       (a) `QCSupportsInterop` always returns false on a real SDP binary (the marker it probes is
//           absent), which makes the caller take the documented fallback; and
//       (b) if Qualcomm ever ships a text-envelope interop, the shape is already here.
//     The fallback is the supported path: export a trace from Snapdragon Profiler and open it as a
//     plain file (OpenQCProfile still records the path and lets the rest of the UI run) rather than
//     failing hard.
//   - We deliberately do NOT bundle or ship any Qualcomm binary. The profiler is an external
//     process launched from a user-configured path (ExternalTool_SnapdragonProfiler).

// macros to help define encode/decode functions
#define VARIANT_ENCODE(p) lit(#p), p
#define VARIANT_DECODE(p)                \
  if(paramName == lit(#p))               \
  {                                      \
    p = paramValue.value<decltype(p)>(); \
    continue;                            \
  }

enum class QCCommand
{
  Initialize,
  SetEvent,
  Terminate,
};

struct QCInteropInit
{
  int32_t interop_version = 0;
  QString interop_name = QStringLiteral("RenderDoc");

  QVariantList toParams(uint32_t version) const
  {
    return {VARIANT_ENCODE(interop_version), VARIANT_ENCODE(interop_name)};
  }

  void fromParams(uint32_t version, QVariantList list)
  {
    for(int i = 0; i + 1 < list.size(); i += 2)
    {
      QString paramName = list[i].toString();
      QVariant paramValue = list[i + 1];
      VARIANT_DECODE(interop_version);
      VARIANT_DECODE(interop_name);

      qWarning() << "Unrecognised param" << paramName;
    }
  }
};

// An event in Snapdragon Profiler mapped to a RenderDoc event. The conceptual fields match
// RGPInteropEvent: a monotonically increasing linear id assigned by the profiler, an id of the
// command buffer / submission the event belongs to, and the human-readable event name.
struct QCInteropEvent
{
  uint32_t interoplinearid = 0;
  uint32_t cmdbufid = 0;
  QString eventname;

  QVariantList toParams(uint32_t version) const
  {
    return {VARIANT_ENCODE(interoplinearid), VARIANT_ENCODE(cmdbufid), VARIANT_ENCODE(eventname)};
  }

  void fromParams(uint32_t version, QVariantList list)
  {
    for(int i = 0; i + 1 < list.size(); i += 2)
    {
      QString paramName = list[i].toString();
      QVariant paramValue = list[i + 1];
      VARIANT_DECODE(interoplinearid);
      VARIANT_DECODE(cmdbufid);
      VARIANT_DECODE(eventname);

      qWarning() << "Unrecognised param" << paramName;
    }
  }
};

struct QCInteropTerminate
{
  QVariantList toParams(uint32_t version) const { return QVariantList(); }
};

class QCInterop : public IQCInterop
{
public:
  // A separate port from RGP (29000) chosen so the two integrations can coexist and be told
  // apart. [观测] This port is NOT used by Snapdragon Profiler: 29100 appears nowhere in the SDP
  // distribution, and SDP's own host transport binds 65100/65101 (section 8.9.2). The value is
  // retained only so the (never-connected) listener has a distinct, non-conflicting address.
  static const int Port = 29100;

  QCInterop(ICaptureContext &ctx);
  ~QCInterop();

  virtual bool HasQCEvent(uint32_t eventId) override;
  virtual bool SelectQCEvent(uint32_t eventId) override;

  DOCUMENT(R"(Return whether the given Snapdragon Profiler executable appears to support interop.

Best-effort probe of the executable on disk. [观测] The concrete interop capability marker was
established (section 8.9.2) to be absent from every SDP binary, so in practice this always returns
``False`` for a genuine Snapdragon Profiler build and the caller takes the file-based export
fallback. The probe is retained as a hook in case Qualcomm ships a real text-envelope interop: it
looks for a version marker embedded in the binary and treats absence (or a failed read) as 'not
supported'.

:param str path: The path to the Snapdragon Profiler executable.
:return: ``True`` if interop appears to be supported (currently always ``False`` for real SDP).
:rtype: bool
)");
  static bool QCSupportsInterop(const QString &path);

private:
  void InitializeQC();
  void ConnectionEstablished();

  void CreateMapping(const rdcarray<ActionDescription> &actions);
  void CreateMapping(uint32_t version);

  void EventSelected(QCInteropEvent event);

  uint32_t m_Version = 0;

  QTcpServer *m_Server = NULL;
  QTcpSocket *m_Socket = NULL;

  QByteArray m_ReadBuffer;

  ICaptureContext &m_Ctx;
  QStringList m_EventNames;
  rdcarray<QCInteropEvent> m_Event2QC;
  rdcarray<uint32_t> m_QC2Event;
  QString EncodeCommand(QCCommand command, QVariantList params);
  bool DecodeCommand(QString command);
  void ProcessReadBuffer();
};
