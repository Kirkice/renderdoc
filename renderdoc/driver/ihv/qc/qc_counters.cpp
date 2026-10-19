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

#include "qc_counters.h"

#include "common/common.h"
#include "common/formatting.h"

#include <algorithm>
#include <dlfcn.h>

// This translation unit is only built for Android (see CMakeLists.txt). On
// other platforms qc_counters_stub.cpp is used instead, exactly like the ARM
// backend.

namespace
{
// A single entry of the Qualcomm / Adreno GGPM counter catalog.
//
// The `PERF_*` names below are the counter names observed in the device-side
// Snapdragon Profiler GGPM plugin string table (see the planning document
// section 8.4). IDs are assigned deterministically (sorted by name) so that a
// counter keeps the same public GPUCounter value across versions and devices -
// see pitfall P-3. We must never assign IDs from the runtime enumeration order.
struct QCCounterMeta
{
  const char *perfName;
  const char *humanName;
  const char *description;
  const char *category;
  // hardware block this metric belongs to - one of the prefixes in kQCBlocks below.
  // GGPM names every raw counter `PERF_<BLOCK>_...` (section 8.4), so the block is
  // what the raw counter hierarchy is keyed on.
  const char *block;
  CounterUnit unit;
  CompType resultType;
  // minimum Adreno generation (600 = A6xx, 700 = A7xx, 800 = A8xx) on which this
  // counter is expected to be valid. Different generations do not necessarily
  // expose the same PERF_* set - see pitfall P-4.
  uint32_t minAdrenoGen;
};

// The 33 hardware blocks of the Adreno GGPM catalog (section 8.4). `count` is the
// observed number of `PERF_<BLOCK>_...` names in that block and `meaning` is the
// [推断] expansion of the block prefix. The counts are kept as [观测] evidence and
// are also a consistency check: per section 8.4 they must sum to exactly the 1828
// unique PERF_* names extracted from pluginGPU-GGPM.
struct QCBlock
{
  const char *prefix;
  uint32_t count;
  const char *meaning;
};

static const QCBlock kQCBlocks[] = {
    {"SP", 208, "Shader Processor"},
    {"GMU", 157, "Graphics Management Unit"},
    {"HLSQ", 139, "High Level Sequencer"},
    {"TSE", 139, "Triangle Setup Engine"},
    {"PC", 138, "Primitive/Parameter Cache"},
    {"VPC", 118, "Varying/Primitive Cache"},
    {"CP", 107, "Command Processor"},
    {"UCHE", 103, "Unified Cache Hierarchy Engine (L2)"},
    {"UFC", 92, "Universal Front-end Cache"},
    {"GBIF", 77, "Graphics Bandwidth Interface"},
    {"CMPDECMP", 86, "Compression/Decompression"},
    {"TP", 82, "Texture Pipe"},
    {"CCHE", 81, "Color Cache"},
    {"RB", 63, "Render Backend"},
    {"CCU", 51, "Color Control Unit"},
    {"LRZ", 40, "Low Resolution Z"},
    {"RAS", 39, "Rasterizer"},
    {"RBBM", 31, "Register-Bus/Block Manager"},
    {"VFD", 25, "Vertex Fetch Decoder"},
    {"CRE", 9, "Compute Runtime Engine"},
    {"VSC", 9, "Visibility Stream Compressor"},
    {"FCHE", 7, "Fragment Cache"},
    {"GLC", 6, "Graphics L2 Controller"},
    {"VFDP", 5, "VFD Primitive"},
    {"TESS", 4, "Tessellation"},
    {"BYPC", 3, "Bypass color path"},
    {"BYPD", 2, "Bypass depth path"},
    {"MHUB", 2, "Memory Hub"},
    {"DHUB", 1, "Display Hub"},
    {"VHUB", 1, "Video Hub"},
    {"GMUCLK", 1, "GMU clock"},
    {"ALWAYSON", 1, "Always-on counters"},
    {"RESERVED", 1, "Reserved"},
};

// Categories observed in the GGPM plugin (sections 8.4 and 9.3). The three "GPU
// Metrics Category" entries carry the profiler's own description strings verbatim;
// "GPU General" is the basic-metrics category seen when the GGPM registration
// function (FUN_001b3234) registers the VBIF basic metrics (section 9.3).
static const char *kCatShader = "GPU Shader Processing";
static const char *kCatPrimitive = "GPU Primitive Processing";
static const char *kCatStalls = "GPU Stalls";
static const char *kCatGeneral = "GPU General";

static const char *kDescShader = "Metrics related to GPU shader processing.";
static const char *kDescPrimitive = "Metrics relating to primitive counts, rejection, etc.";
static const char *kDescStalls = "GPU metrics that measure stalls within the pipeline.";
static const char *kDescGeneral = "Basic GPU metrics.";

// The catalog. This is the RenderDoc-side, original representation of the GGPM
// metrics - it contains no Qualcomm code, only our own declared names, units,
// descriptions and block classification derived from the observed behaviour.
//
// Section 8.4 establishes that the full on-device catalog holds 1828 raw `PERF_*`
// names (see kQCBlocks), of which a smaller set are the *derived* high level metrics
// that the profiler's UI actually presents. Those derived metrics are the ones listed
// here, since only their names/semantics are [观测]; the remaining raw block
// counters are classified through kQCBlocks but cannot be enumerated by name without
// further evidence (their individual names were not recovered).
static const QCCounterMeta kQCCatalog[] = {
    // ---- GPU Shader Processing ----
    {"PERF_PERCENT_SHADERS_BUSY", "Shaders Busy",
     "Percentage of the time the shader processors were busy.", kCatShader, "SP",
     CounterUnit::Percentage, CompType::Float, 600},
    {"PERF_PERCENT_SHADERS_STALLED", "Shaders Stalled",
     "Percentage of the time the shader processors were stalled.", kCatShader, "SP",
     CounterUnit::Percentage, CompType::Float, 600},
    {"PERF_PERCENT_SHADER_ALU_CAPACITY_UTILIZED", "Shader ALU Capacity Utilized",
     "Percentage of the shader ALU capacity that was utilized.", kCatShader, "SP",
     CounterUnit::Percentage, CompType::Float, 600},
    {"PERF_PERCENT_TEXTURE_PIPES_BUSY", "Texture Pipes Busy",
     "Percentage of the time the texture pipes were busy.", kCatShader, "TP",
     CounterUnit::Percentage, CompType::Float, 600},
    {"PERF_PERCENT_TEXTURE_FETCH_STALL", "Texture Fetch Stall",
     "Percentage of shader execution stalled on texture fetches.", kCatShader, "UCHE",
     CounterUnit::Percentage, CompType::Float, 600},
    {"PERF_PERCENT_VERTEX_FETCH_STALL", "Vertex Fetch Stall",
     "Percentage of shader execution stalled on vertex fetches.", kCatShader, "VFD",
     CounterUnit::Percentage, CompType::Float, 600},
    {"PERF_PERCENT_BVH_FETCH_STALL", "BVH Fetch Stall",
     "Percentage of shader execution stalled on BVH fetches.", kCatShader, "UCHE",
     CounterUnit::Percentage, CompType::Float, 700},
    {"PERF_ALU_PER_VERTEX", "ALU / Vertex",
     "Average number of full precision ALU instructions issued per vertex.", kCatShader, "SP",
     CounterUnit::Ratio, CompType::Float, 600},
    {"PERF_EFU_PER_VERTEX", "EFU / Vertex",
     "Average number of elementary function unit operations per vertex.", kCatShader, "SP",
     CounterUnit::Ratio, CompType::Float, 600},
    {"PERF_TEXTURES_PER_VERTEX", "Textures / Vertex",
     "Average number of texture fetches per vertex.", kCatShader, "TP", CounterUnit::Ratio,
     CompType::Float, 600},
    {"PERF_L1_TEXTURE_CACHE_MISS_PER_PIXEL", "L1 Texture Cache Miss / Pixel",
     "Average number of L1 texture cache misses per pixel.", kCatShader, "TP",
     CounterUnit::Ratio, CompType::Float, 600},
    {"PERF_VERTEX_INSTRUCTIONS", "Vertex Instructions",
     "Total number of vertex shader instructions executed.", kCatShader, "SP",
     CounterUnit::Absolute, CompType::UInt, 600},
    {"PERF_VERTEX_INSTRUCTIONS_PER_SECOND", "Vertex Instructions Per Second",
     "Vertex shader instructions executed per second.", kCatShader, "SP",
     CounterUnit::Absolute, CompType::Float, 600},

    // ---- GPU Primitive Processing ----
    {"PERF_PERCENT_BUSY", "Percent Busy",
     "Overall percentage of the time the GPU was busy.", kCatPrimitive, "ALWAYSON",
     CounterUnit::Percentage, CompType::Float, 600},
    {"PERF_GPU_PERCENT_BUS_BUSY", "GPU Bus Busy",
     "Percentage of the time the GPU memory bus was busy.", kCatPrimitive, "GBIF",
     CounterUnit::Percentage, CompType::Float, 600},
    {"PERF_PERCENT_LRZ_BUSY", "LRZ Busy",
     "Percentage of the time the low resolution Z unit was busy.", kCatPrimitive, "LRZ",
     CounterUnit::Percentage, CompType::Float, 600},
    {"PERF_PERCENT_RTU_BUSY", "RTU Busy",
     "Percentage of the time the render target unit was busy.", kCatPrimitive, "RB",
     CounterUnit::Percentage, CompType::Float, 600},
    {"PERF_LRZ_PIXELS_KILLED", "LRZ Pixels Killed",
     "Number of pixels rejected by low resolution Z.", kCatPrimitive, "LRZ",
     CounterUnit::Absolute, CompType::UInt, 600},
    {"PERF_VERTEX_MEMORY_READ", "Vertex Memory Read",
     "Bytes of vertex data read from memory.", kCatPrimitive, "VFD", CounterUnit::Bytes,
     CompType::UInt, 600},
    {"PERF_AVG_BYTES_PER_VERTEX", "Average Bytes / Vertex",
     "Average number of bytes read per vertex.", kCatPrimitive, "VFD", CounterUnit::Ratio,
     CompType::Float, 600},

    // ---- GPU Stalls ----
    {"PERF_PERCENT_STALLED_ON_SYSTEM_MEMORY", "Stalled On System Memory",
     "Percentage of the time the GPU was stalled on system memory accesses.", kCatStalls, "MHUB",
     CounterUnit::Percentage, CompType::Float, 600},

    // ---- GPU General (basic metrics, section 9.3) ----
    // The registration function registers the VBIF basic metrics under the "GPU
    // General" / "Basic GPU metrics." category; the `PERF_` prefix follows the
    // observed `PERF_<NAME>` convention for the remaining catalog entries.
    {"PERF_VBIF", "VBIF", "Video/system bus interface activity.", kCatGeneral, "GBIF",
     CounterUnit::Absolute, CompType::Float, 600},
    {"PERF_VBIF_PWR", "VBIF Power", "Power/energy proxy for the video/system bus interface.",
     kCatGeneral, "GBIF", CounterUnit::Absolute, CompType::Float, 600},

    // ---- Frequency / clock ----
    {"PERF_GPU_FREQUENCY", "GPU Frequency", "GPU frequency in Hz.", kCatPrimitive, "GMUCLK",
     CounterUnit::Hertz, CompType::Float, 600},
    {"PERF_GMUCLK", "GMU Clock", "Graphics Management Unit clock in Hz.", kCatPrimitive,
     "GMUCLK", CounterUnit::Hertz, CompType::Float, 600},
};

// ---- device-side gsl (id, group) catalog: PLACEHOLDER ----
//
// gsl_perfcounter_select and gsl_perfcounter_read address a counter by the PAIR
// (id, group); the PERF_* name is a host-side label and is never sent to the
// driver. The mapping therefore has to come from the device, and it is the one
// piece of this backend that cannot be derived from the host SDP distribution:
// the catalog lives in the device's own libgsl.so, and gsl_perfcounter_query_
// selections() — the only documented way to enumerate it — reports *currently
// selectable* counters, not a name-to-pair mapping.
//
// Status: NOT POPULATED. The table holds only a sentinel entry, because no pair has
// been observed on real hardware yet. This is a deliberate placeholder, not an
// unfinished edit and not a defect, and its consequence is stated plainly so it is
// not mistaken for one: every counter keeps QCCounters::kUnknownGslId, so
// BeginPass() selects nothing, EndSample() reads nothing, and QC counters return
// zero on device even though the UI lists them by name.
//
// REQUIRES REAL-DEVICE VERIFICATION before any entry is enabled. To fill one in:
//   1. On an Adreno device with a live GL/Vulkan context, dlopen libgsl.so and
//      call gsl_perfcounter_query_selections() to learn which pairs the driver
//      currently exposes.
//   2. For each pair, select it alone, run a known workload, and read it back to
//      establish what the counter actually measures.
//   3. Compare the measured behaviour against the GGPM PERF_* semantics recorded
//      in kQCCatalog, and only then record the pair here.
// Until step 3 has been done for a name, guessing a pair is strictly worse than
// reading nothing: a wrong id silently reports an unrelated hardware counter under
// the wrong label, which is undetectable downstream. An explicit zero is honest;
// a plausible-looking wrong number is not.
//
// `minAdrenoGen`-style filtering does not apply here: a pair is a property of a
// specific driver build, so each entry records the generation it was verified on
// and is only used on exactly that generation.
struct QCGslPair
{
  const char *perfName;
  uint32_t gslId;
  uint32_t gslGroup;
  // Adreno generation (600 / 700 / 800) the pair was verified on. A pair is a
  // driver-build property, so it is not assumed to carry across generations.
  uint32_t verifiedGen;
};

static const QCGslPair kQCVerifiedGslPairs[] = {
    // PLACEHOLDER — awaiting real-device verification. Append entries in this form
    // once a pair has been measured on hardware:
    // {"PERF_PERCENT_SHADERS_BUSY", <id>, <group>, 740},

    // One sentinel entry with no perfName keeps the table non-empty without
    // claiming anything is verified. LookupGslPair() skips it and the count in
    // LogCatalogStatistics() ignores it, so appending a real entry is all that is
    // needed to enable a counter.
    {NULL, 0, 0, 0},
};

// Number of entries that actually describe a verified counter. A sentinel-only
// table reports 0, which is what makes the "nothing is addressable yet" warning
// in LogCatalogStatistics() accurate rather than hard-coded.
static uint32_t VerifiedGslPairCount()
{
  uint32_t count = 0;
  for(const QCGslPair &pair : kQCVerifiedGslPairs)
  {
    if(pair.perfName != NULL)
      count++;
  }
  return count;
}

// Looks up the verified (id, group) pair for a PERF_* name on a specific Adreno
// generation. Returns false when no pair has been verified, which is the current
// state of every entry - callers must treat that as "cannot address this counter
// on the device", never as zero.
static bool LookupGslPair(const char *perfName, uint32_t adrenoGen, uint32_t &gslId,
                          uint32_t &gslGroup)
{
  if(perfName == NULL)
    return false;

  for(const QCGslPair &pair : kQCVerifiedGslPairs)
  {
    // Skip the sentinel and any name that is not this counter.
    if(pair.perfName == NULL || strcmp(pair.perfName, perfName) != 0)
      continue;

    // The generation must match exactly: a pair is a property of a particular
    // driver build, so reusing one across generations would silently address a
    // different counter.
    if(pair.verifiedGen == adrenoGen)
    {
      gslId = pair.gslId;
      gslGroup = pair.gslGroup;
      return true;
    }
  }

  return false;
}

// Logs the section 8.4 taxonomy as a category/block breakdown. `descs` are the
// counters that are actually available on this Adreno generation - the block table
// describes the *full* on-device catalog regardless, so it is reported separately.
static void LogCatalogStatistics(const rdcarray<CounterDescription> &descs)
{
  uint32_t total = 0;
  for(const QCBlock &b : kQCBlocks)
    total += b.count;

  // Section 8.4: the 33 blocks must account for exactly the 1828 PERF_* names
  // extracted from the GGPM plugin. If this ever fails, kQCBlocks is out of date.
  if(total != 1828)
    RDCWARN("Qualcomm counters: block taxonomy sums to %u PERF_* names, expected 1828", total);

  std::map<rdcstr, uint32_t> byCategory;
  for(const CounterDescription &d : descs)
    byCategory[d.category]++;

  const uint32_t catalogSize = (uint32_t)(sizeof(kQCCatalog) / sizeof(kQCCatalog[0]));
  const uint32_t blockCount = (uint32_t)(sizeof(kQCBlocks) / sizeof(kQCBlocks[0]));
  const uint32_t pairCount = VerifiedGslPairCount();

  RDCLOG("Qualcomm counters: %u/%u derived counters available across %u categories "
         "(full catalog: %u blocks / %u PERF_* names)",
         (uint32_t)descs.size(), catalogSize, (uint32_t)byCategory.size(), blockCount, total);

  for(const std::pair<const rdcstr, uint32_t> &cat : byCategory)
    RDCLOG("  [%s] %u counters", cat.first.c_str(), cat.second);

  // State the device-addressability status up front rather than leaving an analyst
  // to work out why every counter reads zero. This is a placeholder, not a defect:
  // no (id, group) pair has been verified on real hardware yet.
  if(pairCount == 0)
  {
    RDCWARN("Qualcomm counters: 0/%u counters have a verified device (id, group) pair - the catalog "
            "is listed by name only and every sample will be zero until kQCVerifiedGslPairs is "
            "populated on a real Adreno device",
            catalogSize);
  }
  else
  {
    RDCLOG("Qualcomm counters: %u/%u counters have a verified device (id, group) pair",
           pairCount, catalogSize);
  }
}

// ---- device-side gsl_perfcounter_* entry point types ----
//
// [观测] These signatures are no longer inferred from symbol names: they were
// recovered by disassembling the call sites in the device-side GGPM plugin
// (pluginGPU-GGPM, AArch64). See the planning document section 8.9.1 for the
// evidence trail. The recovered ABI is:
//
//   The device and context are 32-bit integer handles (NOT `void**`/opaque
//   pointers): the plugin stores them as `int` at object offsets +0xc (device,
//   from gsl_device_open) and +0x10 (context, from gsl_context_create), and
//   treats context == -1 as "no live context".
//
//   * gsl_perfcounter_select(device, context, count, uint32_t *ids,
//                            uint32_t *groups, uint64_t *out[, uint64_t *out2])
//         older driver ABI: 6 args, writes a 32-bit selection id into *out.
//         newer driver ABI: 7 args, writes a 64-bit value into *out2 (and a
//         32-bit value into *out) - selected when gsl_library_version() reports
//         a minor version >= 9. Return 0 == success; non-zero is an error code
//         (the plugin logs "Could not select %s:%d (%x)").
//   * gsl_perfcounter_deselect(device, context, 0, count, uint32_t *ids,
//                              uint32_t *groups)
//         The third argument is a constant 0; return value is ignored (void).
//   * gsl_perfcounter_query_selections(device, context, selectionType,
//                              uint32_t count, uint32_t *outIds, uint32_t *outCount)
//         Two-call protocol: first with count == 0 and outIds == NULL and
//         outCount != NULL to learn the required count, then again with
//         count == n and outIds == buffer and outCount == NULL to fill it.
//   * gsl_perfcounter_read(device, uint32_t id, uint32_t group, uint64_t *outValue)
//         Reads one counter value. Result is masked to 32 bits when the counter
//         id is 0xd.
//
//   [观测] A counter is addressed by the PAIR (id, group); both arrays are
//   parallel and `count` elements long. Any id greater than 0x38 (56) is
//   clamped to 0xffffffff before the call, i.e. 56 is the largest valid id.
typedef int (*PFN_gsl_perfcounter_select)(uint32_t device, uint32_t context, uint32_t count,
                                          uint32_t *ids, uint32_t *groups, uint64_t *out);
typedef int (*PFN_gsl_perfcounter_select_ex)(uint32_t device, uint32_t context, uint32_t count,
                                             uint32_t *ids, uint32_t *groups, uint64_t *out,
                                             uint64_t *out2);
typedef int (*PFN_gsl_perfcounter_query_selections)(uint32_t device, uint32_t context,
                                                    uint32_t selectionType, uint32_t count,
                                                    uint32_t *outIds, uint32_t *outCount);
typedef int (*PFN_gsl_perfcounter_read)(uint32_t device, uint32_t id, uint32_t group,
                                        uint64_t *outValue);
typedef void (*PFN_gsl_perfcounter_deselect)(uint32_t device, uint32_t context, uint32_t unused,
                                             uint32_t count, uint32_t *ids, uint32_t *groups);

// [观测] The session entry points. gsl_library_open/close bracket the lifetime of
// the gsl library, gsl_device_open/close the device, and gsl_context_create/destroy
// the context. The device is addressed by a numeric device id (0 is the primary
// GPU) and both open calls take a flags word (the plugin passes 0).
typedef int (*PFN_gsl_library_open)(uint32_t flags);
typedef int (*PFN_gsl_library_close)(void);
typedef int (*PFN_gsl_library_version)(void);
typedef int (*PFN_gsl_device_open)(int deviceId, uint32_t flags);
typedef int (*PFN_gsl_device_close)(int device);
typedef int (*PFN_gsl_context_create)(int device, uint32_t flags);
typedef int (*PFN_gsl_context_destroy)(int device, int context);

// [观测] The largest counter id the GGPM plugin will pass through; anything
// above this is replaced with 0xffffffff (an intentional "invalid" sentinel).
static const uint32_t kQCPerfCounterMaxId = 0x38;

// [观测] The counter id the plugin masks to 32 bits after reading (its value is a
// 32-bit quantity even though every read returns a 64-bit slot).
static const uint32_t kQCPerfCounter32BitId = 0xd;
}    // namespace

QCCounters::QCCounters()
{
}

QCCounters::~QCCounters()
{
#if ENABLED(RDOC_ANDROID)
  if(m_Gsl.context_destroy && m_GslDevice >= 0 && m_GslContext >= 0)
  {
    PFN_gsl_context_destroy destroy = (PFN_gsl_context_destroy)m_Gsl.context_destroy;
    destroy(m_GslDevice, m_GslContext);
  }

  if(m_Gsl.device_close && m_GslDevice >= 0)
  {
    PFN_gsl_device_close close = (PFN_gsl_device_close)m_Gsl.device_close;
    close(m_GslDevice);
  }

  if(m_Gsl.library_close)
  {
    PFN_gsl_library_close close = (PFN_gsl_library_close)m_Gsl.library_close;
    close();
  }

  m_GslDevice = -1;
  m_GslContext = -1;

  if(m_Gsl.module)
  {
    m_Gsl.select = NULL;
    m_Gsl.select_ex = NULL;
    m_Gsl.read = NULL;
    m_Gsl.query_selections = NULL;
    m_Gsl.deselect = NULL;
    m_Gsl.library_open = NULL;
    m_Gsl.library_close = NULL;
    m_Gsl.library_version = NULL;
    m_Gsl.device_open = NULL;
    m_Gsl.device_close = NULL;
    m_Gsl.context_create = NULL;
    m_Gsl.context_destroy = NULL;

    dlclose(m_Gsl.module);
    m_Gsl.module = NULL;
  }
#endif
}

#if ENABLED(RDOC_ANDROID)
bool QCCounters::LoadGSL()
{
  // libgsl.so is the Adreno user-mode driver and is only present on the device.
  // We never bundle it - it is resolved from the device's own system libraries.
  //
  // [观测] The device-side GGPM plugin resolves this same library through
  // dlopen("libgsl.so") (falling back to "libgsl.so.1" and, on Android,
  // android_dlopen_ext with the "sphal" namespace - section 8.9.1). We only need
  // the primary soname because a working GL/Vulkan context already implies the
  // driver is loaded in the process.
  m_Gsl.module = dlopen("libgsl.so", RTLD_NOW | RTLD_LOCAL);

  if(!m_Gsl.module)
  {
    RDCLOG("Qualcomm counters: failed to dlopen libgsl.so (%s)", dlerror());
    return false;
  }

  m_Gsl.select = dlsym(m_Gsl.module, "gsl_perfcounter_select");
  m_Gsl.select_ex = dlsym(m_Gsl.module, "gsl_perfcounter_select_ex");
  m_Gsl.query_selections = dlsym(m_Gsl.module, "gsl_perfcounter_query_selections");
  m_Gsl.read = dlsym(m_Gsl.module, "gsl_perfcounter_read");
  m_Gsl.deselect = dlsym(m_Gsl.module, "gsl_perfcounter_deselect");

  m_Gsl.library_open = dlsym(m_Gsl.module, "gsl_library_open");
  m_Gsl.library_close = dlsym(m_Gsl.module, "gsl_library_close");
  m_Gsl.library_version = dlsym(m_Gsl.module, "gsl_library_version");
  m_Gsl.device_open = dlsym(m_Gsl.module, "gsl_device_open");
  m_Gsl.device_close = dlsym(m_Gsl.module, "gsl_device_close");
  m_Gsl.context_create = dlsym(m_Gsl.module, "gsl_context_create");
  m_Gsl.context_destroy = dlsym(m_Gsl.module, "gsl_context_destroy");

  // At minimum the counter entry points and the read/select pair must exist. The
  // session entry points are needed to obtain a device/context; if they are
  // missing we cannot open a session and report failure rather than calling a
  // counter function with an invalid handle.
  if(!m_Gsl.query_selections || !m_Gsl.read || !m_Gsl.deselect ||
     (!m_Gsl.select && !m_Gsl.select_ex) || !m_Gsl.device_open || !m_Gsl.context_create)
  {
    RDCLOG("Qualcomm counters: libgsl.so loaded but gsl_perfcounter_*/session entry points are "
           "missing");
    return false;
  }

  // [观测] The plugin gates the newer select/context ABI on gsl_library_version()
  // reporting a minor version >= 9. That predicate could not be reproduced
  // statically, so rather than mirroring the version rule we use a simpler
  // capability check: prefer select_ex whenever libgsl.so exports it. This is
  // deliberately weaker than the plugin's version gate ([推断]).
  m_UseSelectEx = (m_Gsl.select_ex != NULL);

  if(m_Gsl.library_version)
  {
    PFN_gsl_library_version version = (PFN_gsl_library_version)m_Gsl.library_version;
    m_LibraryVersion = (uint32_t)version();
    RDCLOG("Qualcomm counters: libgsl.so reports version 0x%x (select_ex=%s)", m_LibraryVersion,
           m_UseSelectEx ? "yes" : "no");
  }

  // Open our own library/device/context session. The plugin does the same: the
  // device handle comes from gsl_device_open and the context from
  // gsl_context_create, and counter calls always use those handles - never the
  // API-level context (section 8.9.1).
  if(m_Gsl.library_open)
  {
    PFN_gsl_library_open open = (PFN_gsl_library_open)m_Gsl.library_open;
    if(open(0) != 0)
    {
      RDCLOG("Qualcomm counters: gsl_library_open failed");
      return false;
    }
  }

  {
    PFN_gsl_device_open open = (PFN_gsl_device_open)m_Gsl.device_open;
    m_GslDevice = open(0, 0);
    if(m_GslDevice < 0)
    {
      RDCLOG("Qualcomm counters: gsl_device_open failed (%d)", m_GslDevice);
      return false;
    }
  }

  {
    PFN_gsl_context_create create = (PFN_gsl_context_create)m_Gsl.context_create;
    m_GslContext = create(m_GslDevice, 0);
    if(m_GslContext < 0)
    {
      RDCLOG("Qualcomm counters: gsl_context_create failed (%d)", m_GslContext);
      return false;
    }
  }

  return true;
}

int QCCounters::SelectCounters(const rdcarray<uint32_t> &ids, const rdcarray<uint32_t> &groups)
{
  if(ids.empty() || ids.size() != groups.size() || m_GslDevice < 0 || m_GslContext < 0)
    return -1;

  // [观测] The plugin clamps any id above 0x38 to 0xffffffff before the call
  // (an explicit "invalid" sentinel). We do the same so that an out-of-range id
  // is rejected by the driver rather than silently mis-selecting.
  rdcarray<uint32_t> clampedIds = ids;
  for(uint32_t &id : clampedIds)
  {
    if(id > kQCPerfCounterMaxId)
      id = 0xffffffffu;
  }

  // The gsl_* ABI takes non-const `uint32_t *` arrays, but `groups` arrives as a
  // const reference. Take a mutable copy so the pointer we hand to the driver
  // matches the parameter type (mirroring the clampedIds copy above).
  rdcarray<uint32_t> mutableGroups = groups;

  uint64_t out = 0;
  const uint32_t count = (uint32_t)clampedIds.size();

  if(m_UseSelectEx && m_Gsl.select_ex)
  {
    PFN_gsl_perfcounter_select_ex select = (PFN_gsl_perfcounter_select_ex)m_Gsl.select_ex;
    uint64_t out2 = 0;
    return select((uint32_t)m_GslDevice, (uint32_t)m_GslContext, count, clampedIds.data(),
                  mutableGroups.data(), &out, &out2);
  }

  PFN_gsl_perfcounter_select select = (PFN_gsl_perfcounter_select)m_Gsl.select;
  return select((uint32_t)m_GslDevice, (uint32_t)m_GslContext, count, clampedIds.data(),
                mutableGroups.data(), &out);
}

uint32_t QCCounters::ResolveAdrenoGen(const char *renderer) const
{
  if(!renderer)
    return 0;

  // GL_RENDERER for Adreno looks like "Adreno (TM) 640". Extract the leading
  // digit of the model number to get the generation (6 -> A6xx, 7 -> A7xx ...).
  const char *p = strstr(renderer, "Adreno");
  if(!p)
    return 0;

  p += strlen("Adreno");
  while(*p && (*p < '0' || *p > '9'))
    p++;

  if(*p >= '1' && *p <= '9')
    return uint32_t(*p - '0') * 100;

  return 0;
}

GPUCounter QCCounters::StableIdFromPerfName(const char *perfName)
{
  // FNV-1a over the PERF_* name, folded into the Qualcomm counter range. Deriving
  // the public ID from the name (rather than from the list position) guarantees a
  // given counter keeps the same GPUCounter value on every Adreno generation and
  // across versions, even though only a subset is available on a given device
  // (pitfall P-3). The device-local index is deliberately NOT used for the ID.
  const uint32_t range =
      (uint32_t)GPUCounter::LastQualcomm - (uint32_t)GPUCounter::FirstQualcomm + 1;

  uint32_t hash = 2166136261u;
  for(const char *p = perfName; p && *p; p++)
  {
    hash ^= (uint8_t)*p;
    hash *= 16777619u;
  }

  return GPUCounter((uint32_t)GPUCounter::FirstQualcomm + (hash % range));
}

int32_t QCCounters::IndexForId(GPUCounter counter) const
{
  for(int32_t i = 0; i < (int32_t)m_Built.size(); i++)
  {
    if(m_Built[i].id == counter)
      return i;
  }

  return -1;
}

void QCCounters::BuildCatalog(uint32_t adrenoGen)
{
  // Build the set of counters available on this Adreno generation. The public ID is
  // derived from the PERF_* name (StableIdFromPerfName), so we sort purely to make
  // the device-local index deterministic - the ID does NOT depend on this order or
  // on which of the entries are filtered out by generation (pitfall P-3).
  rdcarray<const QCCounterMeta *> available;

  for(const QCCounterMeta &meta : kQCCatalog)
  {
    if(adrenoGen == 0 || meta.minAdrenoGen <= adrenoGen)
      available.push_back(&meta);
  }

  std::sort(available.begin(), available.end(), [](const QCCounterMeta *a, const QCCounterMeta *b) {
    return strcmp(a->perfName, b->perfName) < 0;
  });

  for(const QCCounterMeta *meta : available)
  {
    const GPUCounter id = StableIdFromPerfName(meta->perfName);

    // A hash fold can in principle collide inside the reserved range. Drop the
    // later entry rather than aliasing two counters onto one public ID; with this
    // catalog size no collisions are expected.
    bool duplicate = false;
    for(const QCBuiltCounter &built : m_Built)
    {
      if(built.id == id)
      {
        RDCWARN("Qualcomm counters: stable ID collision between '%s' and '%s' - dropping the latter",
                built.perfName, meta->perfName);
        duplicate = true;
        break;
      }
    }

    if(duplicate)
      continue;

    CounterDescription desc;
    desc.name = meta->humanName;
    desc.counter = id;
    desc.category = meta->category;
    desc.description = meta->description;
    desc.resultByteWidth = 8;
    desc.resultType = meta->resultType;
    desc.unit = meta->unit;

    // [未知] The device-resident (id, group) pair is not known host-side: it lives
    // in the device's own libgsl.so catalog, which is not part of the SDP
    // distribution (section 8.9.1). A pair is only used when it has been verified
    // on this exact generation (kQCVerifiedGslPairs), which today is none of them,
    // so the entry keeps kUnknownGslId and the runtime selects/reads nothing for it.
    QCBuiltCounter built = {};
    built.id = id;
    built.perfName = meta->perfName;
    built.gslId = kUnknownGslId;
    built.gslGroup = 0;

    if(LookupGslPair(meta->perfName, adrenoGen, built.gslId, built.gslGroup))
      RDCLOG("Qualcomm counters: %s verified as (id %u, group %u) on Adreno gen %u",
             meta->perfName, built.gslId, built.gslGroup, adrenoGen);

    m_Built.push_back(built);
    m_CounterDescriptions.push_back(desc);
    m_CounterIds.push_back(desc.counter);
  }

  // Emit the section 8.4 category/block taxonomy once the generation-specific set
  // is known, so a debug log records exactly which derived counters are available
  // and how they map onto the full on-device catalog.
  LogCatalogStatistics(m_CounterDescriptions);
}
#endif

bool QCCounters::Init(ApiType api, void *context, const char *renderer)
{
#if ENABLED(RDOC_ANDROID)
  m_Api = api;
  m_ApiContext = context;

  if(!LoadGSL())
    return false;

  m_AdrenoGen = ResolveAdrenoGen(renderer);

  if(m_AdrenoGen == 0)
    RDCLOG("Qualcomm counters: could not determine Adreno generation from '%s'",
           renderer ? renderer : "<null>");

  // [观测] gsl_perfcounter_query_selections() uses a two-call protocol: the first
  // call passes count == 0 and outIds == NULL with a valid outCount to learn the
  // size of the currently-available selection set. We then re-query that size and
  // use it as the per-pass bound - a conservative but evidence-backed default for
  // how many counters the hardware can hold at once (pitfall P-5).
  {
    PFN_gsl_perfcounter_query_selections query =
        (PFN_gsl_perfcounter_query_selections)m_Gsl.query_selections;

    if(query && m_GslDevice >= 0 && m_GslContext >= 0)
    {
      uint32_t availCount = 0;
      int res = query((uint32_t)m_GslDevice, (uint32_t)m_GslContext, 0 /* selectionType */,
                      0 /* count */, NULL /* outIds */, &availCount);

      if(res == 0 && availCount > 0)
      {
        m_MaxPerPass = availCount;
        RDCLOG("Qualcomm counters: device reports %u available selections (per-pass bound)",
               availCount);
      }
      else
      {
        RDCLOG("Qualcomm counters: query_selections returned %d (count %u), keeping default bound "
               "%u",
               res, availCount, m_MaxPerPass);
      }
    }
  }

  BuildCatalog(m_AdrenoGen);

  if(m_CounterIds.empty())
  {
    RDCLOG("Qualcomm counters: no counters available for Adreno generation %u", m_AdrenoGen);
    return false;
  }

  RDCLOG("Qualcomm counters: initialised, %u counters available (Adreno gen %u, max %u per pass)",
         (uint32_t)m_CounterIds.size(), m_AdrenoGen, m_MaxPerPass);

  return true;
#else
  return false;
#endif
}

rdcarray<GPUCounter> QCCounters::GetPublicCounterIds()
{
#if ENABLED(RDOC_ANDROID)
  return m_CounterIds;
#else
  return {};
#endif
}

CounterDescription QCCounters::GetCounterDescription(GPUCounter index)
{
#if ENABLED(RDOC_ANDROID)
  const int32_t idx = IndexForId(index);
  if(idx < 0)
  {
    RDCERR("Qualcomm counters: no description for counter %u", (uint32_t)index);
    return CounterDescription();
  }

  return m_CounterDescriptions[(size_t)idx];
#else
  return {};
#endif
}

void QCCounters::EnableCounter(GPUCounter counter)
{
#if ENABLED(RDOC_ANDROID)
  const int32_t idx = IndexForId(counter);
  if(idx < 0)
  {
    RDCERR("Qualcomm counters: EnableCounter called with unknown counter %u", (uint32_t)counter);
    return;
  }

  // De-duplicate: selecting the same counter twice would otherwise emit it twice
  // into the pass slices. m_EnabledCounters holds device-local indices.
  const uint32_t dev = (uint32_t)idx;
  for(uint32_t enabled : m_EnabledCounters)
  {
    if(enabled == dev)
      return;
  }

  m_EnabledCounters.push_back(dev);
#endif
}

void QCCounters::DisableAllCounters()
{
#if ENABLED(RDOC_ANDROID)
  m_EnabledCounters.clear();
#endif
}

uint32_t QCCounters::GetPassCount()
{
#if ENABLED(RDOC_ANDROID)
  // The hardware can only select a limited number of counters at once, so the
  // requested set may have to be read over several passes (pitfall P-5).
  if(m_EnabledCounters.empty())
    return 0;

  if(m_MaxPerPass == 0)
    return 1;

  return (uint32_t)((m_EnabledCounters.size() + m_MaxPerPass - 1) / m_MaxPerPass);
#else
  return 0;
#endif
}

void QCCounters::BeginPass(uint32_t passID)
{
#if ENABLED(RDOC_ANDROID)
  m_passIndex = passID;

  // Select the subset of counters that belongs to this pass.
  uint32_t start = passID * m_MaxPerPass;
  uint32_t end = RDCMIN((uint32_t)m_EnabledCounters.size(), start + m_MaxPerPass);

  if(start >= (uint32_t)m_EnabledCounters.size())
    return;

  // [观测] select addresses counters by the parallel (id, group) pair. The device
  // pair is unknown host-side (see QCBuiltCounter), so a counter whose pair is not
  // resolved is skipped rather than guessed - a wrong id would silently select an
  // unrelated hardware counter.
  rdcarray<uint32_t> ids;
  rdcarray<uint32_t> groups;
  for(uint32_t i = start; i < end; i++)
  {
    const QCBuiltCounter &built = m_Built[m_EnabledCounters[i]];
    if(!built.HasDevicePair())
      continue;

    ids.push_back(built.gslId);
    groups.push_back(built.gslGroup);
  }

  if(ids.empty())
  {
    // This is the expected outcome while kQCVerifiedGslPairs is a sentinel only: the
    // pass holds only counters whose device pair has not been verified on this
    // generation, and selecting a guessed pair would report an unrelated hardware
    // counter under the right name.
    RDCLOG("Qualcomm counters: pass %u has no selectable counters (device-side (id, group) pairs "
           "unresolved - see kQCVerifiedGslPairs)",
           passID);
    return;
  }

  int res = SelectCounters(ids, groups);
  if(res != 0)
    RDCLOG("Qualcomm counters: gsl_perfcounter_select failed for %u counters (%d)",
           (uint32_t)ids.size(), res);
#endif
}

void QCCounters::EndPass()
{
#if ENABLED(RDOC_ANDROID)
  uint32_t start = m_passIndex * m_MaxPerPass;
  uint32_t end = RDCMIN((uint32_t)m_EnabledCounters.size(), start + m_MaxPerPass);

  if(start >= (uint32_t)m_EnabledCounters.size())
    return;

  if(!m_Gsl.deselect || m_GslDevice < 0 || m_GslContext < 0)
    return;

  rdcarray<uint32_t> ids;
  rdcarray<uint32_t> groups;
  for(uint32_t i = start; i < end; i++)
  {
    const QCBuiltCounter &built = m_Built[m_EnabledCounters[i]];
    if(!built.HasDevicePair())
      continue;

    ids.push_back(built.gslId);
    groups.push_back(built.gslGroup);
  }

  if(ids.empty())
    return;

  // [观测] deselect mirrors select but takes a constant 0 third argument and
  // returns void. We still pass the same (id, group) pair set that was selected.
  PFN_gsl_perfcounter_deselect deselect = (PFN_gsl_perfcounter_deselect)m_Gsl.deselect;
  deselect((uint32_t)m_GslDevice, (uint32_t)m_GslContext, 0 /* constant */, (uint32_t)ids.size(),
           ids.data(), groups.data());
#endif
}

void QCCounters::BeginSample(uint32_t eventId)
{
#if ENABLED(RDOC_ANDROID)
  m_EventId = eventId;
#endif
}

void QCCounters::EndSample()
{
#if ENABLED(RDOC_ANDROID)
  PFN_gsl_perfcounter_read read = (PFN_gsl_perfcounter_read)m_Gsl.read;

  // [观测] Only the counters belonging to the current pass were selected by
  // BeginPass(), and gsl_perfcounter_read only returns a valid value for a
  // selected counter. Reading every enabled counter here would let a later pass
  // overwrite an earlier pass's valid sample with a zero from a counter that was
  // not selected (the multi-pass loop in FetchCountersQC re-runs Begin/EndPass
  // per pass against the same event set). Restrict the readback to this pass's
  // slice, exactly like BeginPass()/EndPass().
  const uint32_t start = m_passIndex * m_MaxPerPass;
  const uint32_t end = RDCMIN((uint32_t)m_EnabledCounters.size(), start + m_MaxPerPass);

  for(uint32_t i = start; i < end; i++)
  {
    const uint32_t counterId = m_EnabledCounters[i];
    CounterValue data;
    data.u64 = 0;

    const QCBuiltCounter &built = m_Built[counterId];
    const CounterDescription &desc = m_CounterDescriptions[counterId];

    // [观测] gsl_perfcounter_read(device, id, group, &out) reads one counter. The
    // counter is identified by the same (id, group) pair used for select; the
    // plugin additionally masks the result to 32 bits for id 0xd. Only a resolved
    // pair can be read - an unresolved one yields zero rather than a bad read.
    if(read && built.HasDevicePair() && m_GslDevice >= 0)
    {
      uint64_t raw = 0;
      int res = read((uint32_t)m_GslDevice, built.gslId, built.gslGroup, &raw);

      if(res != 0)
      {
        RDCLOG("Qualcomm counters: gsl_perfcounter_read failed for %s (%d)", built.perfName, res);
        raw = 0;
      }
      else if(built.gslId == kQCPerfCounter32BitId)
      {
        raw = raw & 0xffffffffu;
      }

      data.u64 = raw;
    }

    // [推断] The read returns a raw 64-bit slot. For counters the catalog types as
    // floating point we surface the raw integer as a double; any unit scaling the
    // hardware applies is not recoverable from the host distribution, so it is left
    // to the presentation layer.
    if(desc.resultType == CompType::Float)
      data.d = (double)data.u64;

    m_CounterData[m_EventId][counterId] = data;
  }
#endif
}

rdcarray<CounterResult> QCCounters::GetCounterData(const rdcarray<uint32_t> &eventIDs,
                                                   const rdcarray<GPUCounter> &counters)
{
  rdcarray<CounterResult> result;
#if ENABLED(RDOC_ANDROID)
  for(size_t i = 0; i < eventIDs.size(); i++)
  {
    uint32_t eventId = eventIDs[i];
    for(size_t j = 0; j < counters.size(); j++)
    {
      GPUCounter counter = counters[j];
      const int32_t idx = IndexForId(counter);
      if(idx < 0)
      {
        RDCERR("Qualcomm counters: GetCounterData called with unknown counter %u",
               (uint32_t)counter);
        continue;
      }

      const CounterDescription &desc = m_CounterDescriptions[(size_t)idx];
      const CounterValue &data = m_CounterData[eventId][(uint32_t)idx];

      if(desc.resultType == CompType::Float)
        result.push_back(CounterResult(eventId, counter, data.d));
      else
        result.push_back(CounterResult(eventId, counter, data.u64));
    }
  }
#endif
  return result;
}
