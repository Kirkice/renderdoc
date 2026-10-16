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

#include <QHash>
#include <QString>
#include <QVector>

#include "Code/SystemTrace.h"
#include "renderdoc_replay.h"

// A single RenderDoc action (draw / dispatch / mesh-dispatch / clear) reduced to the timing keys
// used for correlation. `timeSec` is the action's start time on the GPU timeline in RenderDoc's
// clock; `hasGpuDuration` is false when no timing was available, in which case the action can
// still be correlated by frame number but cannot be placed precisely on the timeline.
struct TraceActionRef
{
  uint32_t eventId = 0;
  QString name;
  uint32_t frameNumber = 0;
  double timeSec = 0.0;
  bool hasGpuDuration = false;
};

// The timing of one RenderDoc frame, used as a correlation anchor. `timeSec` is the frame's
// start time in RenderDoc's own clock.
struct TraceFrameAnchor
{
  uint32_t frameNumber = 0;
  double timeSec = 0.0;
};

// Result of correlating an imported trace against RenderDoc's frame timing.
//
// Correlation keys, in priority order (as specified by the planning document):
//   1. Frame number  — the primary key. A trace frame boundary with the same frameNumber as a
//                      RenderDoc frame is treated as a correspondence.
//   2. Absolute time — the secondary key. When the trace declares that it shares RenderDoc's
//                      clock domain (or only a single frame exists) the offset is taken directly.
// The clock-domain offset and its tolerance are always recorded, even when correlation
// entirely fails (confidence == Unknown).
struct TraceCorrelation
{
  ClockDomainAlignment alignment;

  // Actions that could be placed on the trace timeline (those with a GPU duration).
  QVector<TraceActionRef> actions;

  // Number of trace frame boundaries that corresponded to a RenderDoc frame.
  int matchedFrames = 0;

  // The offset used to render. When confidence is Unknown this is 0 and the UI draws the
  // trace unshifted, clearly labelled as unaligned.
  double offsetSeconds() const { return alignment.offsetSec; }

  bool aligned() const
  {
    return alignment.confidence != ClockDomainConfidence::Unknown;
  }
};

// Build a correlation between an imported trace and the supplied per-frame anchors. Anchors may
// be given in any order; they are matched to trace frame boundaries by frame number. The action
// list must be sorted by (frameNumber, eventId).
//
// Evidence discipline: an offset is only produced from data that actually matched. If nothing
// matched, or the trace declared a shared clock, the confidence says so explicitly rather than
// inventing an alignment. This function performs no I/O and never asserts.
TraceCorrelation CorrelateSystemTrace(const SystemTrace &trace,
                                      const QVector<TraceActionRef> &actions,
                                      const QVector<TraceFrameAnchor> &frameAnchors);

// Convenience: flatten a set of root actions into frame-ordered TraceActionRefs for a single
// frame, keeping only draw / dispatch / mesh-dispatch / clear leaves. `eventStartTimesSec` maps
// an event id to its GPU start time (seconds) in RenderDoc's clock; actions present in the map
// are emitted with hasGpuDuration == true, the rest with hasGpuDuration == false so that
// frame-number correlation can still use them. Leaf actions are emitted with their own eventId;
// pure marker/push/pop groups are recursed and not emitted.
QVector<TraceActionRef> FlattenActionsForCorrelation(const rdcarray<ActionDescription> &rootActions,
                                                      uint32_t frameNumber,
                                                      const QHash<uint32_t, double> &eventStartTimesSec);
