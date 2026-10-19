/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2020-2026 Baldur Karlsson
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

#include <map>
#include "api/replay/data_types.h"
#include "api/replay/renderdoc_replay.h"
#include "api/replay/replay_enums.h"
#include "common/common.h"

// Qualcomm / Adreno (GGPM) hardware counter backend.
//
// This backend is the RenderDoc-side, original implementation of the counter
// behaviour observed on device in Qualcomm's Snapdragon Profiler (see the
// integration planning document for the evidence trail). It does NOT link,
// bundle or redistribute any Qualcomm binary - the device-side access goes
// through the Adreno user-mode driver's own `gsl_perfcounter_*` entry points,
// which are resolved at runtime with dlopen/dlsym on the device.
//
// On non-Android builds no counters are available and the stub implementation
// is compiled instead (qc_counters_stub.cpp), mirroring the ARM backend.

class QCCounters
{
public:
  enum class ApiType
  {
    Ogl,
    Vk,
  };

  QCCounters();
  ~QCCounters();

  // establish the device-side counter session. `renderer` is the GL_RENDERER /
  // device name string used to resolve the Adreno generation (A6xx/A7xx/A8xx).
  bool Init(ApiType api, void *context, const char *renderer);
  rdcarray<GPUCounter> GetPublicCounterIds();
  CounterDescription GetCounterDescription(GPUCounter index);

  void EnableCounter(GPUCounter counter);
  void DisableAllCounters();

  uint32_t GetPassCount();

  void BeginPass(uint32_t passID);
  void EndPass();

  void BeginSample(uint32_t eventId);
  void EndSample();

  rdcarray<CounterResult> GetCounterData(const rdcarray<uint32_t> &eventIDs,
                                         const rdcarray<GPUCounter> &counters);

private:
#if ENABLED(RDOC_ANDROID)
  // The sentinel that marks "this counter's device (id, group) pair is unknown".
  // Declared here so QCBuiltCounter and its consumers share one definition.
  static const uint32_t kUnknownGslId = 0xffffffffu;

  struct GslApi
  {
    void *module;

    // [观测] Resolved gsl_* entry points. The parameter layouts were recovered by
    // disassembling the call sites in the device-side GGPM plugin (pluginGPU-GGPM,
    // AArch64) - see the PFN_gsl_* types in qc_counters.cpp and planning document
    // section 8.9.1. Each slot is cast to the matching PFN type at the call site.
    //
    // `select_ex` is the newer 7-argument select ABI; the plugin picks between the
    // two forms using a driver-reported capability byte. We resolve both and prefer
    // `select_ex` when libgsl.so exports it - that preference is [推断] because the
    // plugin's own predicate could not be reproduced statically.
    void *select;             // 6-arg select (writes a 32-bit selection id)
    void *select_ex;          // 7-arg select (writes a 64-bit value)
    void *query_selections;   // two-call selection enumeration
    void *read;               // read one (id, group) counter value
    void *deselect;           // void, third argument is a constant 0

    // [观测] Session entry points used to obtain the gsl device/context handles.
    // The plugin opens its own device + context rather than reusing the API's (see
    // planning document section 8.9.1); we mirror that lifecycle.
    void *library_open;
    void *library_close;
    void *library_version;
    void *device_open;
    void *device_close;
    void *context_create;
    void *context_destroy;
  };

  bool LoadGSL();

  // Issue a select for a pass, using whichever select ABI the device exposes.
  // `ids` and `groups` are parallel, `count`-long arrays.
  int SelectCounters(const rdcarray<uint32_t> &ids, const rdcarray<uint32_t> &groups);

  uint32_t ResolveAdrenoGen(const char *renderer) const;
  void BuildCatalog(uint32_t adrenoGen);

  // A catalog entry resolved to its stable public identity. The public GPUCounter
  // ID is derived from a hash of the PERF_* name, so it is identical on every
  // Adreno generation and does not depend on which counters happen to be available
  // on a given device - see pitfall P-3.
  struct QCBuiltCounter
  {
    GPUCounter id;
    const char *perfName;

    // [未知] The device-resident (id, group) pair that gsl_perfcounter_select and
    // gsl_perfcounter_read actually address. A counter is identified on device by
    // that pair, NOT by our PERF_* name, so the mapping genuinely requires the
    // device's own libgsl.so catalog. That catalog is not part of the host SDP
    // distribution, so until a pair has been verified on a real device this holds
    // kUnknownGslId and passes containing only such counters issue no select at all
    // rather than a guess. See kQCVerifiedGslPairs in qc_counters.cpp.
    //
    // kUnknownGslId is a static const member of QCCounters; that is safe here because
    // it is only ever compared and assigned by value, never address-taken, so it is
    // never odr-used and needs no out-of-line definition.
    uint32_t gslId = kUnknownGslId;
    uint32_t gslGroup = 0;

    // Whether this counter can be addressed on the device at all. The three call
    // sites that select, deselect and read all need exactly this question answered;
    // keeping it beside the sentinel means they cannot drift apart.
    bool HasDevicePair() const { return gslId != kUnknownGslId; }
  };

  // Deterministically map a PERF_* name into the Qualcomm ID range.
  static GPUCounter StableIdFromPerfName(const char *perfName);

  // Device index (into m_CounterDescriptions / m_CounterIds) for a public ID, or -1
  // if the ID does not correspond to a counter available on this device.
  int32_t IndexForId(GPUCounter counter) const;

  // [推断] Maximum number of counters that can be selected in a single hardware
  // pass. gsl_perfcounter_query_selections() reports the device's currently
  // available selections, and we use the size of that set as the per-pass bound;
  // the exact relation between the two is not proven by disassembly, so a
  // conservative default is kept when the query is unavailable (pitfall P-5).
  uint32_t m_MaxPerPass = 4;

  GslApi m_Gsl = {};

  // [观测] gsl device/context handles are 32-bit integers, NOT pointers: the
  // plugin stores them as `int` at object offsets +0xc (device) and +0x10
  // (context) and treats -1 as "no live context". We open our own session in
  // Init() and close it in the destructor.
  int32_t m_GslDevice = -1;
  int32_t m_GslContext = -1;
  bool m_UseSelectEx = false;
  uint32_t m_LibraryVersion = 0;

  ApiType m_Api;
  // The API-level context passed to Init(), retained for diagnostics only - it is
  // NOT a gsl handle and is never passed to a gsl_* call.
  void *m_ApiContext = NULL;

  uint32_t m_AdrenoGen = 0;
  uint32_t m_EventId = 0;
  uint32_t m_passIndex = 0;

  rdcarray<uint32_t> m_EnabledCounters;
  rdcarray<CounterDescription> m_CounterDescriptions;
  rdcarray<GPUCounter> m_CounterIds;
  rdcarray<QCBuiltCounter> m_Built;
  std::map<uint32_t, std::map<uint32_t, CounterValue>> m_CounterData;
#endif
};
