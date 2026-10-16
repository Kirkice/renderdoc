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

#include "SystemTraceCorrelation.h"

#include <math.h>

#include <algorithm>

// Only these leaves are worth placing individually on a system trace timeline.
static const uint32_t CorrelatableFlags = (uint32_t)ActionFlags::Drawcall |
                                          (uint32_t)ActionFlags::Dispatch |
                                          (uint32_t)ActionFlags::MeshDispatch |
                                          (uint32_t)ActionFlags::Clear;

// A best-effort display name when the action has no custom name. We deliberately do not reach
// for the structured-data file here: this module stays independent of the capture core, and a
// missing name is cosmetic only.
static QString FlagsToName(ActionFlags flags)
{
  const uint32_t f = (uint32_t)flags;

  if(f & (uint32_t)ActionFlags::Clear)
    return QStringLiteral("Clear");
  if(f & (uint32_t)ActionFlags::MeshDispatch)
    return QStringLiteral("Mesh Dispatch");
  if(f & (uint32_t)ActionFlags::Dispatch)
    return QStringLiteral("Dispatch");
  if(f & (uint32_t)ActionFlags::Drawcall)
    return QStringLiteral("Draw");
  if(f & (uint32_t)ActionFlags::MultiAction)
    return QStringLiteral("Multi-action");
  if(f & (uint32_t)ActionFlags::Present)
    return QStringLiteral("Present");
  if(f & (uint32_t)ActionFlags::Copy)
    return QStringLiteral("Copy");

  return QStringLiteral("Action");
}

static void FlattenRecurse(const rdcarray<ActionDescription> &actions, uint32_t frameNumber,
                           const QHash<uint32_t, double> &eventStartTimesSec,
                           QVector<TraceActionRef> &out)
{
  for(const ActionDescription &action : actions)
  {
    const uint32_t f = (uint32_t)action.flags;

    // Leaves we can place on the timeline: a draw / dispatch / mesh-dispatch / clear with no
    // children. Pure markers, command lists and multi-actions are containers and are recursed.
    const bool correlatable = (f & CorrelatableFlags) != 0;
    const bool isLeaf = action.children.isEmpty();

    if(correlatable && isLeaf)
    {
      TraceActionRef ref;
      ref.eventId = action.eventId;
      ref.name = action.customName.isEmpty() ? FlagsToName(action.flags) : QString(action.customName);
      ref.frameNumber = frameNumber;

      auto it = eventStartTimesSec.constFind(action.eventId);
      if(it != eventStartTimesSec.constEnd())
      {
        ref.timeSec = it.value();
        ref.hasGpuDuration = true;
      }

      out.push_back(ref);
    }

    if(!action.children.isEmpty())
      FlattenRecurse(action.children, frameNumber, eventStartTimesSec, out);
  }
}

QVector<TraceActionRef> FlattenActionsForCorrelation(const rdcarray<ActionDescription> &rootActions,
                                                      uint32_t frameNumber,
                                                      const QHash<uint32_t, double> &eventStartTimesSec)
{
  QVector<TraceActionRef> out;
  FlattenRecurse(rootActions, frameNumber, eventStartTimesSec, out);
  return out;
}

TraceCorrelation CorrelateSystemTrace(const SystemTrace &trace, const QVector<TraceActionRef> &actions,
                                      const QVector<TraceFrameAnchor> &frameAnchors)
{
  TraceCorrelation result;
  result.actions = actions;

  // A trace we could not parse carries no timing: say so rather than implying alignment.
  if(!trace.valid)
  {
    result.alignment.confidence = ClockDomainConfidence::Unknown;
    result.alignment.description = QStringLiteral("Trace is empty or invalid: cannot align.");
    return result;
  }

  // Pair up trace frame boundaries with RenderDoc anchors by frame number (the primary key).
  // Each pair (t_trace, t_renderdoc) implies an offset t_renderdoc - t_trace.
  QVector<double> offsets;
  for(const TraceFrameBoundary &boundary : trace.frameBoundaries)
  {
    for(const TraceFrameAnchor &anchor : frameAnchors)
    {
      if(anchor.frameNumber == boundary.frameNumber)
      {
        // traceTime(filtered) = renderdocFrameRelativeTime + offset, so offset is the trace-clock
        // time at the correlated frame's origin.
        offsets.push_back(boundary.timeSec - anchor.timeSec);
        result.matchedFrames++;
        break;
      }
    }
  }

  // The source declared its timestamps already share the capture's clock domain.
  if(trace.sharedClock)
  {
    result.alignment.confidence = ClockDomainConfidence::AssumedShared;
    result.alignment.offsetSec = 0.0;
    result.alignment.toleranceSec = 0.0;

    if(result.matchedFrames > 0)
      result.alignment.description =
          QStringLiteral("Trace declared a shared clock domain (%1 frame(s) cross-checked).")
              .arg(result.matchedFrames);
    else
      result.alignment.description = QStringLiteral("Trace declared a shared clock domain.");

    return result;
  }

  if(offsets.isEmpty())
  {
    // Secondary key: when the capture only has a single frame there is no frame number to match
    // on, so fall back to absolute time using the same frame as the anchor.
    if(frameAnchors.size() == 1 && !trace.frameBoundaries.isEmpty())
    {
      result.alignment.confidence = ClockDomainConfidence::EstimatedFromFrameTiming;
      result.alignment.offsetSec = trace.frameBoundaries.first().timeSec - frameAnchors[0].timeSec;
      result.alignment.toleranceSec = 0.0;
      result.alignment.description =
          QStringLiteral("Single-frame capture: aligned by absolute time against the trace's "
                         "first frame boundary.");
      return result;
    }

    // Nothing matched. Record that explicitly instead of guessing an offset.
    result.alignment.confidence = ClockDomainConfidence::Unknown;
    result.alignment.offsetSec = 0.0;
    result.alignment.toleranceSec = 0.0;

    if(trace.frameBoundaries.isEmpty())
      result.alignment.description = QStringLiteral(
          "Trace has no frame boundaries: clock domains could not be related.");
    else if(frameAnchors.isEmpty())
      result.alignment.description =
          QStringLiteral("Capture has no frame timing: clock domains could not be related.");
    else
      result.alignment.description =
          QStringLiteral("No trace frame boundary matched a captured frame (by number or time): "
                         "clock domains could not be related.");

    return result;
  }

  // Average the per-frame offsets, then report the worst-case residual as the tolerance. This is
  // an estimate fitted from observed frame timing, not an assumed constant.
  double sum = 0.0;
  for(double o : offsets)
    sum += o;
  const double mean = sum / double(offsets.size());

  double worst = 0.0;
  for(double o : offsets)
    worst = fabs(o - mean) > worst ? fabs(o - mean) : worst;

  // A single constant offset is only a sound model when the per-frame offsets agree. If they
  // trend (a clock-rate difference rather than a fixed skew) the constant offset fits the mean
  // but drifts at the ends, so measure the linear drift separately and surface it instead of
  // letting the mean imply alignment that does not hold across the whole range.
  double drift = 0.0;
  if(offsets.size() >= 3)
  {
    const double n = double(offsets.size());
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    // `i` is int (not size_t) so it matches QVector::operator[]'s index type; a size_t index
    // would be a narrowing conversion (C4267).
    for(int i = 0; i < offsets.size(); i++)
    {
      const double x = double(i);
      const double y = offsets[i];
      sx += x;
      sy += y;
      sxx += x * x;
      sxy += x * y;
    }

    const double denom = n * sxx - sx * sx;
    if(denom != 0.0)
    {
      const double slope = (n * sxy - sx * sy) / denom;
      drift = fabs(slope) * (n - 1.0);
    }
  }

  result.alignment.confidence = ClockDomainConfidence::EstimatedFromFrameTiming;
  result.alignment.offsetSec = mean;
  // Both a spread about the mean and an accumulated linear drift are ways the constant offset can
  // be wrong at a given frame, so report the larger of the two as the tolerance.
  result.alignment.toleranceSec = drift > worst ? drift : worst;

  if(offsets.size() == 1)
    result.alignment.description = QStringLiteral(
        "Estimated from a single matched frame; tolerance is not measurable from one sample.");
  else if(drift > worst)
    result.alignment.description =
        QStringLiteral("Estimated from %1 matched frames: offsets trend linearly (drift %2 s "
                       "across the matched range), so a single constant offset is only approximate.")
            .arg(offsets.size())
            .arg(drift, 0, 'g', 6);
  else
    result.alignment.description = QStringLiteral("Estimated from %1 matched frames (worst-case "
                                                  "residual %2 s).")
                                       .arg(offsets.size())
                                       .arg(worst, 0, 'g', 6);

  return result;
}
