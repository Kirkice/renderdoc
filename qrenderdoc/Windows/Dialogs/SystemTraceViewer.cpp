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

#include "SystemTraceViewer.h"

#include <math.h>

#include <algorithm>
#include <functional>
#include <vector>

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Code/QRDUtils.h"

namespace
{
// The trace declares a time unit but the module works in seconds throughout; this is display-only.
QString FormatSeconds(double seconds)
{
  const double abs = fabs(seconds);

  if(abs < 1.0e-3)
    return QObject::tr("%1 us").arg(seconds * 1.0e6, 0, 'f', 1);
  if(abs < 1.0)
    return QObject::tr("%1 ms").arg(seconds * 1.0e3, 0, 'f', 2);
  return QObject::tr("%1 s").arg(seconds, 0, 'f', 3);
}

QString FormatValue(double value)
{
  return QString::number(value, 'g', 4);
}

QString ConfidenceName(ClockDomainConfidence c)
{
  switch(c)
  {
    case ClockDomainConfidence::Unknown:
      return QObject::tr("unknown");
    case ClockDomainConfidence::EstimatedFromFrameTiming:
      return QObject::tr("estimated");
    case ClockDomainConfidence::AssumedShared:
      return QObject::tr("shared");
  }

  return QObject::tr("unknown");
}

// Distinct, reasonably colour-blind friendly line colours for the overlay curves.
QColor SeriesColour(int index)
{
  static const QColor palette[] = {
      QColor(0x2e, 0x86, 0xde), QColor(0xd9, 0x53, 0x4f), QColor(0x3c, 0xa8, 0x6a),
      QColor(0xe6, 0x9f, 0x00), QColor(0x8e, 0x44, 0xad), QColor(0x00, 0x99, 0x99),
      QColor(0xb0, 0x6a, 0x00), QColor(0x66, 0x66, 0x66),
  };

  return palette[index % (sizeof(palette) / sizeof(palette[0]))];
}
}    // namespace

// ---------------------------------------------------------------------------------------------
// SystemTraceChart — a hand-painted overlay. There is no QtCharts module in this project, so the
// frame timeline, the trace curves and the action markers are all drawn directly with QPainter.
//
// Layout: the plot area is split into one horizontal band per trace series so that curves with
// unrelated units (e.g. bandwidth and occupancy) are never forced onto a shared axis.
// ---------------------------------------------------------------------------------------------
class SystemTraceChart : public QWidget
{
public:
  SystemTraceChart(QWidget *parent) : QWidget(parent)
  {
    setMinimumHeight(320);
    setMouseTracking(false);
    setFocusPolicy(Qt::ClickFocus);
  }

  void SetData(const SystemTraceChartData &data)
  {
    m_Data = data;
    m_HasData = true;
    update();
  }

  void Clear()
  {
    m_Data = SystemTraceChartData();
    m_HasData = false;
    update();
  }

  // Called when the user clicks a draw/dispatch marker. Returns the event id, or 0 if the click
  // did not land near a marker.
  std::function<void(uint32_t)> OnSelect;

protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(0x25, 0x25, 0x25));

    if(!m_HasData || m_Data.trace == NULL)
    {
      p.setPen(QColor(0xa0, 0xa0, 0xa0));
      p.drawText(rect(), Qt::AlignCenter,
                 QObject::tr("No system trace imported.\nUse \"Open Trace...\" to load one."));
      return;
    }

    const SystemTrace &trace = *m_Data.trace;

    const int marginLeft = 96;
    const int marginRight = 14;
    const int marginTop = 12;
    const int marginBottom = 34;

    m_PlotRect = QRectF(marginLeft, marginTop, qMax(10.0, width() - marginLeft - marginRight),
                        qMax(10.0, height() - marginTop - marginBottom));

    const double span = trace.SpanSec() > 0.0 ? trace.SpanSec() : 1.0;

    const auto mapTime = [&](double t) {
      return m_PlotRect.left() + (t / span) * m_PlotRect.width();
    };

    // --- Grid and time axis -----------------------------------------------------------------
    p.setPen(QPen(QColor(0x3a, 0x3a, 0x3a), 1));
    for(int i = 0; i <= 4; i++)
    {
      const double x = m_PlotRect.left() + (m_PlotRect.width() * i) / 4.0;
      p.drawLine(QPointF(x, m_PlotRect.top()), QPointF(x, m_PlotRect.bottom()));
    }
    p.drawRect(m_PlotRect);

    p.setPen(QColor(0xb0, 0xb0, 0xb0));
    for(int i = 0; i <= 4; i++)
    {
      const double x = m_PlotRect.left() + (m_PlotRect.width() * i) / 4.0;
      const QString label = FormatSeconds(span * i / 4.0);
      p.drawText(QRectF(x - 40, m_PlotRect.bottom() + 4, 80, 20), Qt::AlignHCenter | Qt::AlignTop,
                 label);
    }

    // --- Series bands ----------------------------------------------------------------------
    const int seriesCount = qMax(1, trace.series.size());
    const double bandHeight = m_PlotRect.height() / seriesCount;

    for(int i = 0; i < trace.series.size(); i++)
    {
      const TraceSeries &series = trace.series[i];

      const QRectF band(m_PlotRect.left(), m_PlotRect.top() + bandHeight * i, m_PlotRect.width(),
                        bandHeight);

      // Alternating background so adjacent bands are distinguishable.
      if(i % 2 == 1)
      {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x2b, 0x2b, 0x2b));
        p.drawRect(band);
        p.setBrush(Qt::NoBrush);
      }

      p.setPen(QPen(QColor(0x3a, 0x3a, 0x3a), 1));
      p.drawLine(QPointF(band.left(), band.bottom()), QPointF(band.right(), band.bottom()));

      double vmin = series.MinValue();
      double vmax = series.MaxValue();
      if(vmax <= vmin)
        vmax = vmin + 1.0;

      const auto mapValue = [&](double v) {
        return band.bottom() - ((v - vmin) / (vmax - vmin)) * band.height();
      };

      // Axis labels for this band.
      p.setPen(QColor(0x90, 0x90, 0x90));
      p.drawText(QRectF(2, band.top(), marginLeft - 6, 14), Qt::AlignRight | Qt::AlignTop,
                 FormatValue(vmax));
      p.drawText(QRectF(2, band.bottom() - 14, marginLeft - 6, 14), Qt::AlignRight | Qt::AlignBottom,
                 FormatValue(vmin));

      p.setPen(SeriesColour(i));
      p.drawText(QRectF(band.left() + 6, band.top() + 2, band.width() - 12, 16),
                 Qt::AlignLeft | Qt::AlignTop,
                 series.unit.isEmpty() ? series.name
                                       : QObject::tr("%1 (%2)").arg(series.name, series.unit));

      QPolygonF poly;
      poly.reserve(series.samples.size());
      for(const TraceSample &sample : series.samples)
        poly << QPointF(mapTime(sample.timeSec), mapValue(sample.value));

      p.setPen(QPen(SeriesColour(i), 1.4));
      p.setBrush(Qt::NoBrush);
      p.drawPolyline(poly);
    }

    // --- Frame window and action overlay ---------------------------------------------------
    const bool overlay = m_Data.aligned && m_Data.frameEndSec > m_Data.frameStartSec;

    if(overlay)
    {
      const double t0 = qBound(0.0, m_Data.frameStartSec, span);
      const double t1 = qBound(0.0, m_Data.frameEndSec, span);

      if(t1 > t0)
      {
        const double x0 = mapTime(t0);
        const double x1 = mapTime(t1);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x2e, 0x86, 0xde, 32));
        p.drawRect(QRectF(x0, m_PlotRect.top(), x1 - x0, m_PlotRect.height()));
        p.setBrush(Qt::NoBrush);

        p.setPen(QPen(QColor(0x2e, 0x86, 0xde), 1, Qt::DashLine));
        p.drawLine(QPointF(x0, m_PlotRect.top()), QPointF(x0, m_PlotRect.bottom()));
        p.drawLine(QPointF(x1, m_PlotRect.top()), QPointF(x1, m_PlotRect.bottom()));

        p.setPen(QColor(0x8f, 0xc4, 0xff));
        p.drawText(QRectF(x0, m_PlotRect.top(), qMax(60.0, x1 - x0), 16),
                   Qt::AlignLeft | Qt::AlignTop, QObject::tr("Frame %1").arg(m_Data.frameNumber));

        // Each action is a tick at its own start time inside the frame window.
        const double frameSeconds = m_Data.frameEndSec - m_Data.frameStartSec;
        for(const TraceActionRef &action : m_Data.actions)
        {
          if(!action.hasGpuDuration)
            continue;

          const double fraction = action.timeSec / frameSeconds;
          if(fraction < 0.0 || fraction > 1.0)
            continue;

          const double x = x0 + fraction * (x1 - x0);

          if(action.eventId == m_Data.highlightEvent)
          {
            p.setPen(QPen(QColor(0xff, 0xc8, 0x3c), 2));
            p.drawLine(QPointF(x, m_PlotRect.top()), QPointF(x, m_PlotRect.bottom()));

            p.setPen(QColor(0xff, 0xc8, 0x3c));
            p.drawText(QRectF(x + 3, m_PlotRect.bottom() - 18, 220, 16),
                       Qt::AlignLeft | Qt::AlignBottom,
                       QObject::tr("EID %1 %2").arg(action.eventId).arg(action.name));
          }
          else
          {
            p.setPen(QPen(QColor(0xff, 0xff, 0xff, 90), 1));
            p.drawLine(QPointF(x, m_PlotRect.top() + 4), QPointF(x, m_PlotRect.bottom() - 4));
          }
        }
      }
    }
    else
    {
      // Unaligned: the trace is shown from time zero, clearly labelled so the curves are never
      // mistaken for being placed against the capture.
      p.setPen(QColor(0xe0, 0x6c, 0x6c));
      p.drawText(QRectF(m_PlotRect.left() + 6, m_PlotRect.top() + 16, m_PlotRect.width() - 12, 18),
                 Qt::AlignLeft | Qt::AlignTop,
                 QObject::tr("Trace not aligned to the frame (clock domains unrelated)"));
    }
  }

  void mousePressEvent(QMouseEvent *event) override
  {
    if(!m_HasData || m_Data.trace == NULL || !OnSelect)
      return;

    if(!m_PlotRect.contains(event->pos()))
      return;

    const SystemTrace &trace = *m_Data.trace;
    const double span = trace.SpanSec() > 0.0 ? trace.SpanSec() : 1.0;
    const double fraction = (event->pos().x() - m_PlotRect.left()) / m_PlotRect.width();
    const double clickedTraceTime = fraction * span;

    // Pick the action whose absolute trace time is closest to the click. Only actions with real
    // timing can be placed, so an action without a duration is never selected here.
    double bestDistance = 0.0;
    uint32_t bestEvent = 0;

    const bool overlay = m_Data.aligned && m_Data.frameEndSec > m_Data.frameStartSec;
    if(overlay)
    {
      for(const TraceActionRef &action : m_Data.actions)
      {
        if(!action.hasGpuDuration)
          continue;

        const double actionTraceTime = m_Data.frameStartSec + action.timeSec;
        const double distance = fabs(actionTraceTime - clickedTraceTime);

        if(bestEvent == 0 || distance < bestDistance)
        {
          bestDistance = distance;
          bestEvent = action.eventId;
        }
      }
    }

    if(bestEvent != 0)
      OnSelect(bestEvent);
  }

private:
  SystemTraceChartData m_Data;
  bool m_HasData = false;
  QRectF m_PlotRect;
};

SystemTraceViewer::SystemTraceViewer(ICaptureContext &ctx, QWidget *parent)
    : QDialog(parent), m_Ctx(ctx)
{
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("System Trace Correlation"));
  resize(1000, 720);

  m_TracePath = new QLineEdit(this);
  m_TracePath->setReadOnly(true);
  m_TracePath->setPlaceholderText(tr("No trace loaded"));

  QPushButton *browse = new QPushButton(tr("Open Trace..."), this);
  QPushButton *reload = new QPushButton(tr("Reload"), this);
  QPushButton *close = new QPushButton(tr("Close"), this);

  connect(browse, &QPushButton::clicked, this, [this] { browseForTrace(); });
  connect(reload, &QPushButton::clicked, this, [this] { reloadTrace(); });
  connect(close, &QPushButton::clicked, this, &QDialog::close);

  QHBoxLayout *fileRow = new QHBoxLayout();
  fileRow->addWidget(new QLabel(tr("Trace:"), this));
  fileRow->addWidget(m_TracePath, 1);
  fileRow->addWidget(browse);
  fileRow->addWidget(reload);

  m_Chart = new SystemTraceChart(this);
  m_Chart->OnSelect = [this](uint32_t eventId) { selectEvent(eventId); };

  m_Series = new QListWidget(this);
  m_Frames = new QTableWidget(this);
  m_Frames->setColumnCount(2);
  m_Frames->setHorizontalHeaderLabels({tr("Frame"), tr("Trace start")});
  m_Frames->horizontalHeader()->setStretchLastSection(true);
  m_Frames->verticalHeader()->setVisible(false);
  m_Frames->setEditTriggers(QAbstractItemView::NoEditTriggers);

  m_Warnings = new QListWidget(this);

  QTabWidget *sideTabs = new QTabWidget(this);
  sideTabs->addTab(m_Series, tr("Series"));
  sideTabs->addTab(m_Frames, tr("Frames"));
  sideTabs->addTab(m_Warnings, tr("Warnings"));
  sideTabs->setMaximumWidth(300);

  QHBoxLayout *middle = new QHBoxLayout();
  middle->addWidget(m_Chart, 1);
  middle->addWidget(sideTabs);

  m_Alignment = new QLabel(this);
  m_Alignment->setWordWrap(true);

  m_Selection = new QLabel(this);
  m_Selection->setWordWrap(true);

  m_Status = new QLabel(this);
  m_Status->setWordWrap(true);

  QHBoxLayout *buttonRow = new QHBoxLayout();
  buttonRow->addStretch(1);
  buttonRow->addWidget(close);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addLayout(fileRow);
  layout->addLayout(middle, 1);
  layout->addWidget(m_Alignment);
  layout->addWidget(m_Selection);
  layout->addWidget(m_Status);
  layout->addLayout(buttonRow);

  m_Ctx.AddCaptureViewer(this);

  updateStatusText();
  updateHighlight();
}

SystemTraceViewer::~SystemTraceViewer()
{
  m_Ctx.RemoveCaptureViewer(this);
}

void SystemTraceViewer::OnCaptureLoaded()
{
  rebuild();
}

void SystemTraceViewer::OnCaptureClosed()
{
  m_Correlation = TraceCorrelation();
  m_FrameEventTimes.clear();
  m_FrameDuration = 0.0;
  m_CorrelatedFrame = 0;

  if(m_Chart)
    m_Chart->Clear();

  updateStatusText();
  updateHighlight();
}

void SystemTraceViewer::OnSelectedEventChanged(uint32_t)
{
  updateHighlight();
}

void SystemTraceViewer::OnEventChanged(uint32_t)
{
  updateHighlight();
}

void SystemTraceViewer::browseForTrace()
{
  const QString path = RDDialog::getOpenFileName(
      this, tr("Select a system trace to correlate"), m_Ctx.Config().LastCaptureFilePath,
      tr("Trace files (*.jsonl *.json *.txt);;All Files (*)"));

  if(path.isEmpty())
    return;

  loadTraceFrom(path);
}

void SystemTraceViewer::reloadTrace()
{
  if(m_TracePath->text().isEmpty())
  {
    browseForTrace();
    return;
  }

  loadTraceFrom(m_TracePath->text());
}

void SystemTraceViewer::loadTraceFrom(const QString &path)
{
  m_Trace = ImportSystemTraceJsonLines(path);
  m_TracePath->setText(path);
  m_Loaded = true;

  // Fill the informational side panels. These are static for a given import, so they are not
  // touched on every frame change.
  m_Series->clear();
  for(const TraceSeries &series : m_Trace.series)
  {
    m_Series->addItem(series.unit.isEmpty()
                          ? tr("%1 (%2 samples)").arg(series.name).arg(series.samples.size())
                          : tr("%1 [%2] (%3 samples)")
                                .arg(series.name, series.unit)
                                .arg(series.samples.size()));
  }

  m_Frames->setRowCount(m_Trace.frameBoundaries.size());
  for(int i = 0; i < m_Trace.frameBoundaries.size(); i++)
  {
    const TraceFrameBoundary &b = m_Trace.frameBoundaries[i];

    QTableWidgetItem *frameItem = new QTableWidgetItem(QString::number(b.frameNumber));
    QTableWidgetItem *timeItem = new QTableWidgetItem(FormatSeconds(b.timeSec));

    m_Frames->setItem(i, 0, frameItem);
    m_Frames->setItem(i, 1, timeItem);
  }

  m_Warnings->clear();
  for(const QString &warning : m_Trace.warnings)
    m_Warnings->addItem(warning);

  rebuild();
  updateStatusText();
}

void SystemTraceViewer::rebuild()
{
  if(!m_Ctx.IsCaptureLoaded() || !traceUsable())
  {
    // Nothing to correlate: keep whatever informational panels we have but clear the overlay.
    m_FrameEventTimes.clear();
    m_FrameDuration = 0.0;
    m_CorrelatedFrame = 0;
    m_Correlation = TraceCorrelation();

    if(m_Chart)
      m_Chart->Clear();

    updateHighlight();
    return;
  }

  const uint32_t frame = m_Ctx.FrameInfo().frameNumber;

  // The per-event timing is a RenderDoc replay query, so it must run on the replay thread. The
  // anchor's timeSec is 0 because RenderDoc exposes no absolute frame-origin timestamp: action
  // times are already relative to the frame origin (see ClockDomainAlignment).
  m_Ctx.Replay().AsyncInvoke([this, frame](IReplayController *r) {
    const rdcarray<CounterResult> results = r->FetchCounters({GPUCounter::EventGPUDuration});

    // Prefix-sum the per-event durations in event-id order to obtain each event's start time
    // relative to the frame origin. Event ids increase in submission order, so this ordering is
    // the same one RenderDoc's own event browser uses.
    std::vector<CounterResult> sorted;
    sorted.reserve((size_t)results.size());
    for(const CounterResult &cr : results)
      sorted.push_back(cr);
    std::sort(sorted.begin(), sorted.end(),
              [](const CounterResult &a, const CounterResult &b) { return a.eventId < b.eventId; });

    QHash<uint32_t, double> starts;
    starts.reserve(int(sorted.size()));

    double total = 0.0;
    for(const CounterResult &cr : sorted)
    {
      starts.insert(cr.eventId, total);
      total += cr.value.d;
    }

    // GUIInvoke::call guards against the dialog being closed before the result arrives.
    GUIInvoke::call(this, [this, frame, starts, total]() {
      m_FrameEventTimes = starts;
      m_FrameDuration = total;
      m_CorrelatedFrame = frame;

      updateHighlight();
      updateStatusText();
    });
  });
}

void SystemTraceViewer::updateHighlight()
{
  if(m_Chart == NULL)
    return;

  if(!traceUsable() || !m_Ctx.IsCaptureLoaded())
  {
    m_Chart->Clear();
    m_Alignment->setText(QString());
    m_Selection->setText(QString());
    return;
  }

  const uint32_t frame =
      m_CorrelatedFrame != 0 ? m_CorrelatedFrame : m_Ctx.FrameInfo().frameNumber;

  QVector<TraceFrameAnchor> anchors;
  TraceFrameAnchor anchor;
  anchor.frameNumber = frame;
  anchor.timeSec = 0.0;
  anchors.push_back(anchor);

  const QVector<TraceActionRef> actions =
      FlattenActionsForCorrelation(m_Ctx.CurRootActions(), frame, m_FrameEventTimes);

  m_Correlation = CorrelateSystemTrace(m_Trace, actions, anchors);

  SystemTraceChartData data;
  data.trace = &m_Trace;
  data.actions = m_Correlation.actions;
  data.alignment = m_Correlation.alignment;
  data.aligned = m_Correlation.aligned();
  data.frameStartSec = m_Correlation.alignment.offsetSec;
  data.frameEndSec = data.frameStartSec + m_FrameDuration;
  data.frameNumber = frame;
  data.highlightEvent = m_Ctx.CurSelectedEvent();

  m_Chart->SetData(data);

  m_Alignment->setText(tr("Clock alignment: %1%2  |  %3")
                           .arg(ConfidenceName(m_Correlation.alignment.confidence))
                           .arg(m_Correlation.alignment.toleranceSec > 0.0
                                    ? tr(" (tolerance %1)")
                                          .arg(FormatSeconds(m_Correlation.alignment.toleranceSec))
                                    : QString())
                           .arg(m_Correlation.alignment.description));

  // Describe the selected event in terms of the system counters, if we could place it.
  QString selectionText;
  const uint32_t selected = m_Ctx.CurSelectedEvent();
  for(const TraceActionRef &action : m_Correlation.actions)
  {
    if(action.eventId != selected)
      continue;

    selectionText = tr("Selected EID %1: %2").arg(action.eventId).arg(action.name);

    if(m_Correlation.aligned() && m_FrameDuration > 0.0)
    {
      const double traceTime = m_Correlation.alignment.offsetSec + action.timeSec;

      QStringList readings;
      for(const TraceSeries &series : m_Trace.series)
      {
        double value = 0.0;
        if(series.SampleAt(traceTime, value))
        {
          readings << (series.unit.isEmpty() ? tr("%1=%2").arg(series.name, FormatValue(value))
                                             : tr("%1=%2 %3").arg(series.name,
                                                                  FormatValue(value), series.unit));
        }
      }

      if(!readings.isEmpty())
        selectionText += QStringLiteral("  |  ") + readings.join(QStringLiteral(", "));
      else
        selectionText += QStringLiteral("  |  ") + tr("no samples near this instant");
    }
    else if(!action.hasGpuDuration)
    {
      selectionText += QStringLiteral("  |  ") + tr("no GPU timing: cannot place on the trace");
    }

    break;
  }

  m_Selection->setText(selectionText);
}

void SystemTraceViewer::selectEvent(uint32_t eventId)
{
  if(eventId == 0)
    return;

  // Exclude ourselves so the selection change does not bounce straight back into updateHighlight.
  m_Ctx.SetEventID({this}, eventId, eventId);
}

void SystemTraceViewer::updateStatusText()
{
  if(m_Status == NULL)
    return;

  if(!m_Loaded)
  {
    m_Status->setText(tr("No trace loaded. Use \"Open Trace...\" to import a system trace, then "
                         "click a draw or dispatch marker to select it in RenderDoc."));
    return;
  }

  if(!m_Trace.valid)
  {
    m_Status->setText(tr("Could not import the trace: %1")
                          .arg(m_Trace.warnings.isEmpty() ? tr("unknown error")
                                                          : m_Trace.warnings.join(QLatin1Char(' '))));
    return;
  }

  m_Status->setText(tr("Trace: %1  |  %2 series, %3 frame boundaries, %4 matched to this capture")
                        .arg(m_Trace.source.isEmpty() ? tr("(unnamed)") : m_Trace.source)
                        .arg(m_Trace.series.size())
                        .arg(m_Trace.frameBoundaries.size())
                        .arg(m_Correlation.matchedFrames));
}
