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

#include <functional>

#include <QDialog>
#include <QString>
#include <QVector>

#include "Code/Interface/QRDInterface.h"
#include "Code/SystemTrace.h"
#include "Code/SystemTraceCorrelation.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;

// Forward-declared: implemented entirely in the .cpp as a plain QWidget with a custom paintEvent.
// There is no QtCharts dependency in this project, so the overlay is drawn by hand.
class SystemTraceChart;

// Everything the chart needs to draw a single correlated frame. It is passed by value on each
// rebuild, so the chart never holds a reference into the viewer's mutable state.
struct SystemTraceChartData
{
  // Non-owning; must outlive the chart's current data (it is the viewer's imported trace).
  const SystemTrace *trace = NULL;
  // Flattened actions for the shown frame, with timeSec relative to the frame origin.
  QVector<TraceActionRef> actions;
  ClockDomainAlignment alignment;
  // True when the two clock domains were related, so the action overlay is meaningful.
  bool aligned = false;
  // Trace-time window occupied by the shown frame (only meaningful when aligned).
  double frameStartSec = 0.0;
  double frameEndSec = 0.0;
  uint32_t frameNumber = 0;
  // Event to draw as the highlighted interval, or 0 when nothing is selected.
  uint32_t highlightEvent = 0;
};

// P1-4: frame-interior counter x system-level trace correlation view.
//
// A self-contained, modeless tool window (like MaliAnalysisDialog) that overlays an imported
// system trace (bandwidth / occupancy curves) against a single RenderDoc frame's timeline, and
// lets the user click a draw / dispatch to both select it in RenderDoc and read the system
// counters at that instant.
//
// It is a capture viewer: it follows the current frame and event, and clears itself when the
// capture closes. It never modifies the capture and never bundles a vendor binary.
class SystemTraceViewer : public QDialog, public ICaptureViewer
{
public:
  explicit SystemTraceViewer(ICaptureContext &ctx, QWidget *parent = 0);
  ~SystemTraceViewer();

  // ICaptureViewer
  void OnCaptureLoaded() override;
  void OnCaptureClosed() override;
  void OnSelectedEventChanged(uint32_t eventId) override;
  void OnEventChanged(uint32_t eventId) override;

private:
  void browseForTrace();
  void reloadTrace();
  void loadTraceFrom(const QString &path);

  // Kick off the (async) event-timing fetch and, once it arrives, correlate + redraw.
  void rebuild();
  bool traceUsable() const { return m_Trace.valid && !m_Trace.series.isEmpty(); }

  void updateHighlight();
  void selectEvent(uint32_t eventId);
  void updateStatusText();

  ICaptureContext &m_Ctx;

  SystemTrace m_Trace;
  bool m_Loaded = false;
  TraceCorrelation m_Correlation;

  // Cache so a redraw does not need to re-fetch counters.
  QHash<uint32_t, double> m_FrameEventTimes;
  double m_FrameDuration = 0.0;
  uint32_t m_CorrelatedFrame = 0;

  QLineEdit *m_TracePath = NULL;
  QListWidget *m_Series = NULL;
  QTableWidget *m_Frames = NULL;
  QLabel *m_Alignment = NULL;
  QLabel *m_Selection = NULL;
  QLabel *m_Status = NULL;
  QListWidget *m_Warnings = NULL;
  SystemTraceChart *m_Chart = NULL;
};
