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
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPen>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "Code/QRDUtils.h"
#include "Code/TraceCapture.h"
#include "Widgets/Extended/RDSplitter.h"

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

// Decides which importer a file needs. Extension alone is not enough: a `traceconv json` export is
// usually named `.json`, but a `.txt` systrace or a bare file can still be the same document. The
// format is therefore identified by looking for the Chrome event array first, and only then
// falling back to the JSON-lines interchange.
bool LooksLikeChromeTrace(const QString &path)
{
  QFile file(path);
  if(!file.open(QIODevice::ReadOnly))
    return false;

  // The event array is at the top level of the document, so a bounded prefix is enough to decide
  // and keeps a multi-hundred-megabyte trace from being read twice.
  const QByteArray prefix = file.read(64 * 1024);

  if(prefix.contains("\"traceEvents\""))
    return true;

  const QByteArray trimmed = prefix.trimmed();
  // A bare top-level array of events; JSON-lines interchange always starts with a '{'.
  return trimmed.startsWith('[');
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

// ---------------------------------------------------------------------------------------------
// Geometry. These follow the numbers Snapdragon Profiler's own theme declares so the panel keeps the
// density of the Trace view it is modelled on: `.lefttrackhbox` is at least 210px wide and
// `button.trackmetricbutton` is at least 30px tall.
//
// The colours are deliberately *not* declared here. This panel paints with QPainter instead of
// style sheets, so it has to read the theme RenderDoc is actually running rather than imposing a
// scheme of its own: RDStyle installs the application palette and the project has no global
// stylesheet, so the widget's palette is the single authority. The accessors live on
// SystemTraceTimeline, mirroring how TimelineBar paints from palette().
// ---------------------------------------------------------------------------------------------
const int kHeaderWidth = 210;
const int kRulerHeight = 26;
const int kRowHeight = 30;
const int kGroupHeight = 20;
const int kScrollBarWidth = 14;
const int kSpanHeight = 14;
const int kSpanStep = 16;

// Mixes two colours, `t` being the weight of `b`. Used for shades the palette has no role for; the
// result follows whichever scheme RDStyle installed instead of being fixed to one.
QColor Blend(const QColor &a, const QColor &b, double t)
{
  const double w = qBound(0.0, t, 1.0);

  return QColor(int(a.red() * (1.0 - w) + b.red() * w),
                int(a.green() * (1.0 - w) + b.green() * w),
                int(a.blue() * (1.0 - w) + b.blue() * w));
}

// A span's colour comes from its own category, so the same category is always the same colour
// within a session without inventing a category table the source never declared.
QColor SpanColour(const QString &category)
{
  if(category.isEmpty())
    return QColor(0x4f, 0x7f, 0xb8);

  const uint hash = qHash(category);
  const int hue = int(hash % 360u);
  const int value = 170 + int((hash >> 9) % 60u);

  return QColor::fromHsv(hue, 105, value);
}

// A round tick step (1, 2 or 5 x 10^n seconds) that keeps ruler labels about 90px apart.
double NiceStep(double span, double pixels)
{
  if(span <= 0.0 || pixels <= 0.0)
    return 1.0;

  const double target = span / qMax(1.0, pixels / 90.0);
  const double magnitude = pow(10.0, floor(log10(target)));
  const double norm = target / magnitude;

  double step = 10.0;
  if(norm <= 1.0)
    step = 1.0;
  else if(norm <= 2.0)
    step = 2.0;
  else if(norm <= 5.0)
    step = 5.0;

  return step * magnitude;
}
}    // namespace

namespace
{
// Everything the timeline draws for one rebuild. Passed by value so the widget never holds a
// reference into the viewer's mutable state; `trace` is non-owning and must outlive the data.
struct SystemTraceTimelineData
{
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
  // False when no capture is open. The lanes are then drawn on their own time scale with no frame
  // overlay, instead of the panel claiming that nothing has been imported.
  bool captureLoaded = true;
};
}    // namespace

// ---------------------------------------------------------------------------------------------
// SystemTraceTimeline — the trace view itself: one lane per process/thread (or per metric), a
// shared zoomable time ruler, and the frame overlay. There is no QtCharts module in this project,
// so every element is drawn directly with QPainter.
//
// The structure and the interactions follow Snapdragon Profiler's Trace view:
//   * a fixed-width caption column (SDP's `.lefttrackhbox`), one row per track, with a group
//     header for each owning process;
//   * counter metrics drawn as curves on their own lane and thread activity drawn as nested span
//     boxes, both against one shared time axis;
//   * three navigation modes — Select (lasso), Pan (drag) and Zoom (drag a region) — matching
//     SDP's "Select, Pan, and Zoom ... switched via the toolbar buttons or the shortcut keys 1, 2,
//     and 3";
//   * wheel zoom, Ctrl+W / Ctrl+S zoom, Ctrl+A / Ctrl+D pan, double-click calipers for measuring a
//     region, and right-click annotations on an element, which add an annotation lane to that
//     element's group.
// ---------------------------------------------------------------------------------------------
class SystemTraceTimeline : public QWidget
{
public:
  SystemTraceTimeline(QWidget *parent) : QWidget(parent)
  {
    setMinimumHeight(260);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    m_Scroll = new QScrollBar(Qt::Vertical, this);
    m_Scroll->setSingleStep(kRowHeight / 2);

    connect(m_Scroll, &QScrollBar::valueChanged, this, [this](int value) {
      if(m_ScrollY == value)
        return;

      m_ScrollY = value;
      update();
    });

    SetNavMode(TraceNavMode::Pan);
  }

  // The parameter is deliberately not named `data`: QWidget declares a member called `data`, so
  // that name would shadow it (C4458).
  void SetData(const SystemTraceTimelineData &newData)
  {
    m_Data = newData;
    RebuildRows();

    // The view window is only reset when the trace changed. A redraw caused by an event or
    // selection change must keep the user's zoom and pan.
    if(m_ResetPending || newData.trace != m_LastTrace)
    {
      m_LastTrace = newData.trace;
      m_ResetPending = false;

      const double total = TotalSpan();
      m_ViewStartSec = 0.0;
      m_ViewSpanSec = total > 0.0 ? total : 1.0;
    }

    ClampView();
    update();
  }

  // Drop everything that was drawn. Annotations are kept: they belong to the imported trace, not
  // to the capture being closed, and are cleared by ResetView() when a different trace is loaded.
  void Clear()
  {
    m_Data = SystemTraceTimelineData();
    m_Rows.clear();
    m_ContentHeight = 0;
    m_ScrollY = 0;
    m_Hover = SpanRef();
    m_Selected = SpanRef();
    UpdateScrollBar();
    update();
  }

  // Discard the annotations, the selection and the calipers, and ask for the full trace on the next
  // data update. The window cannot be set here: the viewer reassigns its trace in place and calls
  // this before handing the new data over, so the extent is not yet known to the timeline.
  void ResetView()
  {
    m_Annotations.clear();
    m_Hover = SpanRef();
    m_Selected = SpanRef();

    m_CaliperASet = false;
    m_CaliperBSet = false;

    m_ResetPending = true;
    m_LastTrace = NULL;

    update();
  }

  void SetNavMode(TraceNavMode mode)
  {
    m_Mode = mode;

    switch(mode)
    {
      case TraceNavMode::Pan: setCursor(Qt::OpenHandCursor); break;
      case TraceNavMode::Select:
      case TraceNavMode::Zoom: setCursor(Qt::CrossCursor); break;
    }

    update();
  }

  TraceNavMode NavMode() const { return m_Mode; }

  // --- theme ---------------------------------------------------------------------------------
  // This widget draws its whole surface with QPainter, so it cannot inherit the theme the way a
  // styled child would. RenderDoc installs its palette application-wide through RDStyle and has no
  // global stylesheet, so the widget's own palette is the authority: these accessors are the same
  // idiom TimelineBar uses (`palette().brush(QPalette::Window)` / `Base` / `Text`), which keeps the
  // panel consistent with the rest of the UI and correct in both the light and dark schemes.
  //
  // The roles are chosen to reproduce the layering the panel needs:
  //   * Base / AlternateBase — the lane surfaces, straight from Qt.
  //   * Window — the gutter and ruler chrome, one step off the data area.
  //   * Text / WindowText — labels.
  // A few shades have no palette role (grid lines, dimmed captions), so they are mixed from the
  // roles above rather than hard-coded; that way they follow the scheme too.
  QColor Surface() const { return palette().color(QPalette::Base); }
  QColor SurfaceAlt() const { return palette().color(QPalette::AlternateBase); }
  QColor Chrome() const { return palette().color(QPalette::Window); }
  QColor Grid() const { return Blend(Chrome(), palette().color(QPalette::WindowText), 0.22); }
  QColor Text() const { return palette().color(QPalette::WindowText); }
  QColor DimText() const { return Blend(Text(), Chrome(), 0.42); }
  // Accent is the interaction colour the rest of RenderDoc uses for selection and focus.
  QColor Accent() const { return palette().color(QPalette::Highlight); }
  QColor AccentHover() const { return palette().color(QPalette::Highlight).darker(130); }
  // The emphasis colour for calipers, annotations and a selected span's outline: the accent lifted
  // far enough to read against the lane surface, instead of the flat yellow SDP used.
  QColor Highlight() const { return palette().color(QPalette::Highlight).lighter(135); }

  // Called when the user clicks a draw/dispatch marker, with the event id to select.
  std::function<void(uint32_t)> OnSelectEvent;
  // Called when the user selects a span, with the span and its lane caption.
  std::function<void(const TraceSpan &, const QString &)> OnSelectSpan;
  // Called when the mode changed from inside the timeline (the 1/2/3 shortcuts), so the toolbar
  // buttons can follow.
  std::function<void(TraceNavMode)> OnNavModeChanged;

protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), Surface());

    const QRectF rows = RowsRect();

    if(m_Data.trace == NULL)
    {
      p.setPen(DimText());
      p.drawText(rect(), Qt::AlignCenter,
                 QObject::tr("No system trace imported.\nUse \"Open Trace...\" to load one."));
      return;
    }

    if(TotalSpan() <= 0.0)
    {
      p.setPen(DimText());
      p.drawText(rect(), Qt::AlignCenter,
                 QObject::tr("The imported trace contains no timed element to draw."));
      return;
    }

    PaintLanes(p, rows);
    PaintCaptions(p, rows);
    PaintRuler(p, rows);
    PaintOverlays(p, rows);

    if(m_Dragging != DragNone)
      PaintDrag(p, rows);
  }

  void resizeEvent(QResizeEvent *event) override
  {
    QWidget::resizeEvent(event);

    const QRectF rows = RowsRect();
    m_Scroll->setGeometry(int(rows.right()) + 4, int(rows.top()), kScrollBarWidth,
                          qMax(10, int(rows.height())));

    UpdateScrollBar();
  }

  void mousePressEvent(QMouseEvent *event) override
  {
    // Take focus so the 1/2/3 shortcuts work straight after a click on the timeline.
    setFocus(Qt::MouseFocusReason);

    if(event->button() == Qt::RightButton)
    {
      AddAnnotationAt(event->pos());
      event->accept();
      return;
    }

    if(event->button() != Qt::LeftButton || m_Data.trace == NULL)
    {
      QWidget::mousePressEvent(event);
      return;
    }

    const QRectF rows = RowsRect();
    if(!rows.contains(event->pos()))
    {
      QWidget::mousePressEvent(event);
      return;
    }

    // Shift-click closes the caliper pair: the first double-click sets one marker, shift-click the
    // other, and the measured interval is drawn with its duration.
    if(event->modifiers() & Qt::ShiftModifier)
    {
      const double t = TimeAtX(event->pos().x());

      if(!m_CaliperASet)
      {
        m_CaliperA = t;
        m_CaliperASet = true;
      }
      else
      {
        m_CaliperB = t;
        m_CaliperBSet = true;
      }

      update();
      event->accept();
      return;
    }

    m_DragStart = event->pos();
    m_DragCurrent = event->pos();
    m_DragSpanStartSec = m_ViewSpanSec;
    m_DragPanStartSec = m_ViewStartSec;

    switch(m_Mode)
    {
      case TraceNavMode::Select: m_Dragging = DragSelect; break;
      case TraceNavMode::Pan: m_Dragging = DragPan; break;
      case TraceNavMode::Zoom: m_Dragging = DragZoom; break;
    }

    event->accept();
  }

  void mouseMoveEvent(QMouseEvent *event) override
  {
    if(m_Dragging == DragPan)
    {
      const double dx = double(event->pos().x() - m_DragStart.x());
      const double range = qMax(1.0, RowsRect().width());

      m_ViewStartSec = m_DragPanStartSec - (dx / range) * m_DragSpanStartSec;
      ClampView();
      update();
      event->accept();
      return;
    }

    if(m_Dragging == DragSelect || m_Dragging == DragZoom)
    {
      m_DragCurrent = event->pos();
      update();
      event->accept();
      return;
    }

    const SpanRef hit = SpanAt(event->pos());
    if(hit.track != m_Hover.track || hit.span != m_Hover.span)
    {
      m_Hover = hit;
      update();
    }

    QWidget::mouseMoveEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent *event) override
  {
    if(event->button() != Qt::LeftButton || m_Dragging == DragNone)
    {
      QWidget::mouseReleaseEvent(event);
      return;
    }

    const DragMode mode = m_Dragging;
    m_Dragging = DragNone;

    const QRect dragRect = QRect(m_DragStart, event->pos()).normalized();
    const bool moved = dragRect.width() > 3 || dragRect.height() > 3;

    if(mode == DragZoom && moved)
    {
      const double ta = TimeAtX(dragRect.left());
      const double tb = TimeAtX(dragRect.right());

      if(tb - ta > 0.0)
      {
        m_ViewStartSec = ta;
        m_ViewSpanSec = tb - ta;
        ClampView();
      }

      update();
    }
    else if(mode == DragSelect && moved)
    {
      SelectInRect(dragRect);
    }
    else if(!moved)
    {
      // A click that did not move is still a selection, whichever mode is active.
      SelectAt(event->pos());
    }
    else
    {
      update();
    }

    event->accept();
  }

  void mouseDoubleClickEvent(QMouseEvent *event) override
  {
    if(event->button() != Qt::LeftButton || m_Data.trace == NULL)
    {
      QWidget::mouseDoubleClickEvent(event);
      return;
    }

    if(!RowsRect().contains(event->pos()))
      return;

    // "Double click on the timeline ... to set markers at the beginning and end of the region you
    // wish to measure": the first double-click places the start marker, the next one the end.
    const double t = TimeAtX(event->pos().x());

    if(!m_CaliperASet || m_CaliperBSet)
    {
      m_CaliperA = t;
      m_CaliperASet = true;
      m_CaliperBSet = false;
    }
    else
    {
      m_CaliperB = t;
      m_CaliperBSet = true;
    }

    update();
    event->accept();
  }

  void wheelEvent(QWheelEvent *event) override
  {
    if(m_Data.trace == NULL)
    {
      event->ignore();
      return;
    }

    const QRectF rows = RowsRect();
    const int x = event->pos().x();

    // Over the caption column there is no time axis to zoom, so the wheel scrolls the lanes.
    if(x < rows.left() || x > rows.right())
    {
      m_Scroll->setValue(m_Scroll->value() - event->delta() / 4);
      event->accept();
      return;
    }

    const double anchor = TimeAtX(x);
    const double total = TotalSpan();

    // Same zoom-response as the rest of this project (see TimelineBar::wheelEvent) so the two
    // timelines feel identical.
    const double mod = 1.0 + event->delta() / 2500.0f;
    const double span = qMin(total, qMax(total * 1.0e-5, m_ViewSpanSec / mod));

    const double fraction = (x - rows.left()) / qMax(1.0, rows.width());
    m_ViewStartSec = anchor - fraction * span;
    m_ViewSpanSec = span;

    ClampView();
    update();
    event->accept();
  }

  void keyPressEvent(QKeyEvent *event) override
  {
    switch(event->key())
    {
      case Qt::Key_1:
        NotifyNavMode(TraceNavMode::Select);
        event->accept();
        return;
      case Qt::Key_2:
        NotifyNavMode(TraceNavMode::Pan);
        event->accept();
        return;
      case Qt::Key_3:
        NotifyNavMode(TraceNavMode::Zoom);
        event->accept();
        return;
      default: break;
    }

    if(event->modifiers() & Qt::ControlModifier)
    {
      switch(event->key())
      {
        case Qt::Key_A:
          PanBy(-0.25);
          event->accept();
          return;
        case Qt::Key_D:
          PanBy(0.25);
          event->accept();
          return;
        case Qt::Key_W:
          ZoomBy(1.5);
          event->accept();
          return;
        case Qt::Key_S:
          ZoomBy(1.0 / 1.5);
          event->accept();
          return;
        default: break;
      }
    }

    QWidget::keyPressEvent(event);
  }

private:
  struct SpanRef
  {
    int track = -1;
    int span = -1;
    bool valid() const { return track >= 0 && span >= 0; }
  };

  struct Row
  {
    // Index into SystemTrace::tracks, -1 for a group header, an annotation row, or a counter row
    // synthesised from a trace that carried no lanes.
    int track = -1;
    // Index into SystemTrace::series for a synthesised counter row, otherwise -1.
    int series = -1;
    int top = 0;
    int height = 0;
    bool header = false;
    bool annotation = false;
    QString group;
    QString annotationGroup;
  };

  struct Annotation
  {
    double beginSec = 0.0;
    QString text;
    // The owning process group, so the annotation lane lands in the group of the element that was
    // annotated instead of appearing somewhere unrelated.
    QString group;
  };

  enum DragMode
  {
    DragNone,
    DragSelect,
    DragPan,
    DragZoom,
  };

  double TotalSpan() const { return m_Data.trace == NULL ? 0.0 : m_Data.trace->SpanSec(); }

  QRectF RowsRect() const
  {
    const double left = kHeaderWidth;
    const double top = kRulerHeight;
    const double w = qMax(40.0, double(width() - kHeaderWidth - kScrollBarWidth - 6));
    const double h = qMax(40.0, double(height() - kRulerHeight - 2));

    return QRectF(left, top, w, h);
  }

  double XForTime(double t) const
  {
    const QRectF rows = RowsRect();
    const double span = qMax(1.0e-9, m_ViewSpanSec);

    return rows.left() + ((t - m_ViewStartSec) / span) * rows.width();
  }

  double TimeAtX(double x) const
  {
    const QRectF rows = RowsRect();
    const double span = qMax(1.0e-9, m_ViewSpanSec);

    return m_ViewStartSec + ((x - rows.left()) / qMax(1.0, rows.width())) * span;
  }

  void ClampView()
  {
    const double total = TotalSpan();

    if(total <= 0.0)
    {
      m_ViewStartSec = 0.0;
      m_ViewSpanSec = 1.0;
      return;
    }

    // Never zoom past a ten-thousandth of the trace: below that the ruler step degenerates.
    m_ViewSpanSec = qMin(total, qMax(total * 1.0e-4, m_ViewSpanSec));
    m_ViewStartSec = qMin(qMax(0.0, m_ViewStartSec), total - m_ViewSpanSec);
  }

  void PanBy(double fraction)
  {
    m_ViewStartSec += fraction * m_ViewSpanSec;
    ClampView();
    update();
  }

  void ZoomBy(double factor)
  {
    const double centre = m_ViewStartSec + m_ViewSpanSec * 0.5;

    m_ViewSpanSec = m_ViewSpanSec / factor;
    m_ViewStartSec = centre - m_ViewSpanSec * 0.5;

    ClampView();
    update();
  }

  void NotifyNavMode(TraceNavMode mode)
  {
    SetNavMode(mode);

    if(OnNavModeChanged)
      OnNavModeChanged(mode);
  }

  QString GroupOf(const TraceTrack &track) const
  {
    return track.group.isEmpty() ? QObject::tr("Ungrouped") : track.group;
  }

  QString CaptionOf(int trackIndex) const
  {
    if(m_Data.trace == NULL || trackIndex < 0 || trackIndex >= m_Data.trace->tracks.size())
      return QString();

    return m_Data.trace->tracks[trackIndex].name;
  }

  bool GroupHasAnnotations(const QString &group) const
  {
    for(const Annotation &a : m_Annotations)
    {
      if(a.group == group)
        return true;
    }

    return false;
  }

  void RebuildRows()
  {
    m_Rows.clear();
    m_ContentHeight = 0;

    if(m_Data.trace == NULL)
    {
      UpdateScrollBar();
      return;
    }

    const SystemTrace &trace = *m_Data.trace;
    int y = 0;

    if(!trace.tracks.isEmpty())
    {
      // Group the lanes by their owning process, keeping the importer's order, so the captions
      // read as SDP's per-process track groups.
      QVector<QString> groupOrder;
      QHash<QString, QVector<int>> groupTracks;

      for(int i = 0; i < trace.tracks.size(); i++)
      {
        const QString group = GroupOf(trace.tracks[i]);

        if(!groupTracks.contains(group))
          groupOrder.push_back(group);

        groupTracks[group].push_back(i);
      }

      for(const QString &group : groupOrder)
      {
        Row header;
        header.header = true;
        header.group = group;
        header.top = y;
        header.height = kGroupHeight;
        m_Rows.push_back(header);
        y += kGroupHeight;

        for(int trackIndex : groupTracks.value(group))
        {
          const TraceTrack &track = trace.tracks[trackIndex];

          int maxDepth = 0;
          for(const TraceSpan &span : track.spans)
            maxDepth = qMax(maxDepth, span.depth);

          Row row;
          row.track = trackIndex;
          row.top = y;
          // A counter lane is a single curve; a span lane is as tall as its nesting needs, capped
          // so one deeply nested lane cannot push every other lane off the screen.
          row.height = track.kind == TraceTrackKind::Counter
                           ? kRowHeight
                           : qMax(kRowHeight, 6 + (qMin(8, maxDepth) + 1) * kSpanStep);
          m_Rows.push_back(row);
          y += row.height;
        }

        if(GroupHasAnnotations(group))
        {
          Row row;
          row.annotation = true;
          row.annotationGroup = group;
          row.top = y;
          row.height = kRowHeight;
          m_Rows.push_back(row);
          y += kRowHeight;
        }
      }
    }
    else
    {
      // A trace that carried only counters (the JSON-lines interchange) has no lanes, so one row
      // per series is synthesised rather than showing an empty timeline.
      for(int i = 0; i < trace.series.size(); i++)
      {
        Row row;
        row.series = i;
        row.top = y;
        row.height = kRowHeight;
        m_Rows.push_back(row);
        y += kRowHeight;
      }
    }

    m_ContentHeight = y;
    UpdateScrollBar();
  }

  void UpdateScrollBar()
  {
    const int viewport = qMax(0, int(RowsRect().height()));
    const int maxValue = qMax(0, m_ContentHeight - viewport);

    m_Scroll->setRange(0, maxValue);
    m_Scroll->setPageStep(qMax(1, viewport));
    m_Scroll->setSingleStep(qMax(1, kRowHeight / 2));
    m_Scroll->setVisible(maxValue > 0);

    m_ScrollY = qBound(0, m_ScrollY, maxValue);

    if(m_Scroll->value() != m_ScrollY)
      m_Scroll->setValue(m_ScrollY);
  }

  SpanRef SpanAt(const QPoint &pos) const
  {
    SpanRef ref;

    if(m_Data.trace == NULL)
      return ref;

    const QRectF rows = RowsRect();
    if(!rows.contains(pos))
      return ref;

    const double t = TimeAtX(pos.x());
    const double contentY = double(pos.y() - rows.top()) + m_ScrollY;

    for(const Row &row : m_Rows)
    {
      if(row.header || row.annotation || row.track < 0)
        continue;

      if(contentY < row.top || contentY >= row.top + row.height)
        continue;

      if(row.track >= m_Data.trace->tracks.size())
        continue;

      const TraceTrack &track = m_Data.trace->tracks[row.track];

      for(int i = 0; i < track.spans.size(); i++)
      {
        const TraceSpan &span = track.spans[i];

        // Spans are sorted by begin time, so nothing further right can contain the cursor.
        if(span.beginSec > t)
          break;

        if(t <= span.endSec)
        {
          // Prefer the innermost span under the cursor: that is the element the user pointed at.
          if(!ref.valid() || span.depth >= m_Data.trace->tracks[ref.track].spans[ref.span].depth)
          {
            ref.track = row.track;
            ref.span = i;
          }
        }
      }
    }

    return ref;
  }

  void SelectAt(const QPoint &pos)
  {
    const SpanRef hit = SpanAt(pos);

    if(hit.valid())
    {
      SelectSpan(hit.track, hit.span);
      return;
    }

    SelectNearestAction(pos);
  }

  void SelectSpan(int trackIndex, int spanIndex)
  {
    m_Selected.track = trackIndex;
    m_Selected.span = spanIndex;
    update();

    if(m_Data.trace == NULL || trackIndex < 0 || trackIndex >= m_Data.trace->tracks.size())
      return;

    const TraceTrack &track = m_Data.trace->tracks[trackIndex];
    if(spanIndex < 0 || spanIndex >= track.spans.size())
      return;

    if(OnSelectSpan)
      OnSelectSpan(track.spans[spanIndex], track.name);
  }

  void SelectInRect(const QRect &widgetRect)
  {
    if(m_Data.trace == NULL)
      return;

    const QRectF rows = RowsRect();
    const QRect plot(int(rows.left()), int(rows.top()), int(rows.width()), int(rows.height()));
    const QRect box = widgetRect.intersected(plot);

    if(box.isEmpty())
      return;

    const double ta = qMin(TimeAtX(box.left()), TimeAtX(box.right()));
    const double tb = qMax(TimeAtX(box.left()), TimeAtX(box.right()));
    const double top = double(box.top() - rows.top()) + m_ScrollY;
    const double bottom = double(box.bottom() - rows.top()) + m_ScrollY;

    SpanRef best;
    double bestDuration = 0.0;

    for(const Row &row : m_Rows)
    {
      if(row.header || row.annotation || row.track < 0)
        continue;

      if(row.top + row.height < top || row.top > bottom)
        continue;

      if(row.track >= m_Data.trace->tracks.size())
        continue;

      const TraceTrack &track = m_Data.trace->tracks[row.track];

      for(int i = 0; i < track.spans.size(); i++)
      {
        const TraceSpan &span = track.spans[i];

        if(span.beginSec > tb)
          break;

        if(span.endSec < ta)
          continue;

        // Of everything the lasso caught, report the smallest element: that is the specific thing
        // the user drew a box around, rather than the process-wide span containing it.
        const double duration = span.endSec - span.beginSec;
        if(!best.valid() || duration < bestDuration)
        {
          best.track = row.track;
          best.span = i;
          bestDuration = duration;
        }
      }
    }

    if(best.valid())
    {
      SelectSpan(best.track, best.span);
      return;
    }

    SelectNearestAction(box.center());
  }

  void SelectNearestAction(const QPoint &pos)
  {
    if(!m_Data.aligned || m_Data.frameEndSec <= m_Data.frameStartSec)
      return;

    const double clicked = TimeAtX(pos.x());

    // Only actions with real timing can be placed, so an action without a duration is never
    // selected here.
    double bestDistance = 0.0;
    uint32_t bestEvent = 0;

    for(const TraceActionRef &action : m_Data.actions)
    {
      if(!action.hasGpuDuration)
        continue;

      const double t = m_Data.frameStartSec + action.timeSec;
      const double distance = fabs(t - clicked);

      if(bestEvent == 0 || distance < bestDistance)
      {
        bestDistance = distance;
        bestEvent = action.eventId;
      }
    }

    if(bestEvent != 0 && OnSelectEvent)
      OnSelectEvent(bestEvent);
  }

  void AddAnnotationAt(const QPoint &pos)
  {
    if(m_Data.trace == NULL)
      return;

    if(!RowsRect().contains(pos))
      return;

    const SpanRef hit = SpanAt(pos);

    QString group = QObject::tr("Ungrouped");
    double t = TimeAtX(pos.x());

    if(hit.valid())
    {
      const TraceTrack &track = m_Data.trace->tracks[hit.track];
      group = GroupOf(track);

      if(hit.span < track.spans.size())
        t = track.spans[hit.span].beginSec;
    }
    else if(!m_Data.trace->tracks.isEmpty())
    {
      group = GroupOf(m_Data.trace->tracks[0]);
    }

    bool ok = false;
    const QString text = QInputDialog::getText(this, QObject::tr("Add annotation"),
                                               QObject::tr("Annotation:"), QLineEdit::Normal,
                                               QString(), &ok);

    if(!ok || text.isEmpty())
      return;

    Annotation annotation;
    annotation.beginSec = t;
    annotation.text = text;
    annotation.group = group;
    m_Annotations.push_back(annotation);

    RebuildRows();
    update();
  }

  // --- painting ---------------------------------------------------------------------------

  void PaintLanes(QPainter &p, const QRectF &rows)
  {
    const SystemTrace &trace = *m_Data.trace;
    const double rightEdge = rows.right();
    const double t0 = m_ViewStartSec;
    const double t1 = t0 + m_ViewSpanSec;

    // Lane backgrounds and group headers. The clip is set before the scroll translation, so it
    // stays in widget space and the lanes never paint over the ruler.
    p.save();
    p.setClipRect(QRectF(0, rows.top(), rightEdge, rows.height()));
    p.translate(0, -m_ScrollY);

    for(int i = 0; i < m_Rows.size(); i++)
    {
      const Row &row = m_Rows[i];
      const QRectF band(0, row.top, rightEdge, row.height);

      if(row.header)
      {
        p.fillRect(band, SurfaceAlt());

        p.setPen(QPen(Grid(), 1));
        p.drawLine(QPointF(0, row.top), QPointF(rightEdge, row.top));

        QFont font = p.font();
        font.setBold(true);
        p.setFont(font);
        p.setPen(Text());
        p.drawText(QRectF(8, row.top, rightEdge - 16, row.height), Qt::AlignLeft | Qt::AlignVCenter,
                   row.group);
        font.setBold(false);
        p.setFont(font);
        continue;
      }

      p.fillRect(band, (i % 2) == 0 ? Surface() : SurfaceAlt());

      p.setPen(QPen(Grid(), 1));
      p.drawLine(QPointF(0, row.top + row.height - 1),
                 QPointF(rightEdge, row.top + row.height - 1));
    }

    p.restore();

    // Vertical time grid, identical in translated and untranslated space because it only depends
    // on the horizontal mapping.
    const double step = NiceStep(m_ViewSpanSec, rows.width());

    p.setPen(QPen(Grid(), 1));
    for(double t = ceil(t0 / step) * step; t <= t1; t += step)
    {
      const double x = XForTime(t);
      p.drawLine(QPointF(x, rows.top()), QPointF(x, rows.bottom()));
    }

    // Lane contents.
    p.save();
    p.setClipRect(QRectF(rows.left(), rows.top(), rows.width(), rows.height()));
    p.translate(0, -m_ScrollY);

    for(const Row &row : m_Rows)
    {
      if(row.header)
        continue;

      if(row.annotation)
      {
        PaintAnnotations(p, row, rows);
        continue;
      }

      if(row.track >= 0 && row.track < trace.tracks.size())
      {
        const TraceTrack &track = trace.tracks[row.track];

        if(track.kind == TraceTrackKind::Counter && track.counterSeries >= 0 &&
           track.counterSeries < trace.series.size())
        {
          PaintCounterLane(p, trace.series[track.counterSeries], row, track.counterSeries, t0, t1);
        }
        else
        {
          PaintSpanLane(p, track, row, t0, t1);
        }

        continue;
      }

      if(row.series >= 0 && row.series < trace.series.size())
        PaintCounterLane(p, trace.series[row.series], row, row.series, t0, t1);
    }

    p.restore();
  }

  void PaintCounterLane(QPainter &p, const TraceSeries &series, const Row &row, int colourIndex,
                        double t0, double t1)
  {
    double vmin = series.MinValue();
    double vmax = series.MaxValue();

    if(vmax <= vmin)
      vmax = vmin + 1.0;

    const double top = double(row.top) + 4.0;
    const double bottom = double(row.top + row.height) - 5.0;
    const double span = qMax(1.0e-9, t1 - t0);

    QPolygonF poly;
    for(const TraceSample &sample : series.samples)
    {
      if(sample.timeSec < t0 - span)
        continue;
      if(sample.timeSec > t1)
        break;

      const double y = bottom - ((sample.value - vmin) / (vmax - vmin)) * (bottom - top);
      poly << QPointF(XForTime(sample.timeSec), y);
    }

    if(poly.size() < 2)
      return;

    p.setPen(QPen(SeriesColour(colourIndex), 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(poly);
  }

  void PaintSpanLane(QPainter &p, const TraceTrack &track, const Row &row, double t0, double t1)
  {
    for(int i = 0; i < track.spans.size(); i++)
    {
      const TraceSpan &span = track.spans[i];

      if(span.beginSec > t1)
        break;

      if(span.endSec < t0)
        continue;

      const double xa = XForTime(span.beginSec);
      const double xb = XForTime(span.endSec);
      const double w = qMax(1.5, xb - xa);

      const QRectF box(xa, double(row.top) + 3.0 + span.depth * kSpanStep, w, double(kSpanHeight));

      const bool selected = (m_Selected.track == row.track && m_Selected.span == i);
      const bool hovered = (m_Hover.track == row.track && m_Hover.span == i);

      QColor base = SpanColour(span.category);
      if(hovered)
        base = base.lighter(118);

      p.setPen(QPen(base.darker(150), 1));
      p.setBrush(selected ? base.lighter(150) : base);
      p.drawRoundedRect(box, 2, 2);

      if(selected || hovered)
      {
        p.setPen(QPen(selected ? Highlight() : AccentHover().lighter(160), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(box.adjusted(-1.0, -1.0, 1.0, 1.0), 2, 2);
      }

      if(w > 34.0)
      {
        const QString label =
            p.fontMetrics().elidedText(span.name, Qt::ElideRight, int(w) - 6);

        p.setPen(QColor(0x12, 0x12, 0x12));
        p.drawText(box.adjusted(3.0, 0.0, -3.0, 0.0), Qt::AlignLeft | Qt::AlignVCenter, label);
      }
    }
  }

  void PaintAnnotations(QPainter &p, const Row &row, const QRectF &rows)
  {
    const double t0 = m_ViewStartSec;
    const double t1 = t0 + m_ViewSpanSec;

    for(const Annotation &a : m_Annotations)
    {
      if(a.group != row.annotationGroup)
        continue;

      if(a.beginSec < t0 || a.beginSec > t1)
        continue;

      const double x = XForTime(a.beginSec);

      p.setPen(QPen(Highlight(), 1));
      p.setBrush(Highlight());

      QPolygonF flag;
      flag << QPointF(x, double(row.top) + 8.0) << QPointF(x + 6.0, double(row.top) + 11.0)
           << QPointF(x, double(row.top) + 14.0);
      p.drawPolygon(flag);

      p.setPen(Highlight());
      p.drawText(QRectF(x + 9.0, double(row.top), qMax(40.0, rows.right() - x - 12.0),
                        double(row.height)),
                 Qt::AlignLeft | Qt::AlignVCenter, a.text);
    }
  }

  void PaintCaptions(QPainter &p, const QRectF &rows)
  {
    const SystemTrace &trace = *m_Data.trace;

    p.save();
    p.setClipRect(QRectF(0, rows.top(), kHeaderWidth, rows.height()));
    p.translate(0, -m_ScrollY);

    for(int i = 0; i < m_Rows.size(); i++)
    {
      const Row &row = m_Rows[i];

      // Group headers are painted by PaintLanes across the full width, label included, and that
      // label starts inside this column. They are left alone so the label survives.
      if(row.header)
        continue;

      // The caption column is its own surface, as SDP's left track header is: each lane's cell
      // gets the same colour PaintLanes used for that lane's band, so the column keeps the plot
      // area's banding instead of reading as one undifferentiated slab.
      p.fillRect(QRectF(0, row.top, double(kHeaderWidth), row.height),
                 (i % 2) == 0 ? Surface() : SurfaceAlt());

      p.setPen(QPen(Grid(), 1));
      p.drawLine(QPointF(0, row.top + row.height - 1),
                 QPointF(double(kHeaderWidth), row.top + row.height - 1));

      const double captionWidth = double(kHeaderWidth) - 30.0;
      QString caption;
      QString detail;
      QColor swatch(0x80, 0x80, 0x80);

      if(row.annotation)
      {
        caption = QObject::tr("Annotations");
        swatch = Highlight();
      }
      else if(row.track >= 0 && row.track < trace.tracks.size())
      {
        const TraceTrack &track = trace.tracks[row.track];
        caption = track.name;

        if(track.kind == TraceTrackKind::Counter)
        {
          swatch = SeriesColour(qMax(0, track.counterSeries));

          if(track.counterSeries >= 0 && track.counterSeries < trace.series.size())
          {
            const TraceSeries &series = trace.series[track.counterSeries];
            detail = series.unit.isEmpty()
                         ? QObject::tr("%1 samples").arg(series.samples.size())
                         : QObject::tr("%1 samples, %2").arg(series.samples.size()).arg(series.unit);
          }
        }
        else
        {
          swatch = SpanColour(track.spans.isEmpty() ? QString() : track.spans[0].category);
          detail = QObject::tr("%1 spans").arg(track.spans.size());
        }
      }
      else if(row.series >= 0 && row.series < trace.series.size())
      {
        const TraceSeries &series = trace.series[row.series];
        caption = series.name;
        swatch = SeriesColour(row.series);
        detail = series.unit.isEmpty() ? QObject::tr("%1 samples").arg(series.samples.size())
                                       : QObject::tr("%1 samples, %2")
                                             .arg(series.samples.size())
                                             .arg(series.unit);
      }

      if(caption.isEmpty())
        continue;

      p.fillRect(QRectF(6.0, double(row.top) + 6.0, 8.0, 8.0), swatch);

      const QString elided = p.fontMetrics().elidedText(caption, Qt::ElideRight, int(captionWidth));

      p.setPen(Text());
      p.drawText(QRectF(20.0, double(row.top) + 3.0, captionWidth, 13.0),
                 Qt::AlignLeft | Qt::AlignVCenter, elided);

      if(!detail.isEmpty())
      {
        p.setPen(DimText());
        p.drawText(QRectF(20.0, double(row.top) + 16.0, captionWidth, 12.0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(detail, Qt::ElideRight, int(captionWidth)));
      }
    }

    p.restore();

    // A single continuous edge from the ruler down to the bottom of the lane area, so the caption
    // column reads as one column rather than as a per-row decoration.
    p.setPen(QPen(Grid(), 1));
    p.drawLine(QPointF(double(kHeaderWidth) - 1.0, rows.top()),
               QPointF(double(kHeaderWidth) - 1.0, rows.bottom()));
  }

  void PaintRuler(QPainter &p, const QRectF &rows)
  {
    const double t0 = m_ViewStartSec;
    const double t1 = t0 + m_ViewSpanSec;

    p.fillRect(QRectF(0.0, 0.0, double(width()), double(kRulerHeight)), SurfaceAlt());

    p.setPen(QPen(Grid(), 1));
    p.drawLine(QPointF(0.0, double(kRulerHeight) - 1.0), QPointF(double(width()), double(kRulerHeight) - 1.0));
    p.drawLine(QPointF(double(kHeaderWidth) - 1.0, 0.0),
               QPointF(double(kHeaderWidth) - 1.0, double(kRulerHeight)));

    p.setPen(DimText());
    p.drawText(QRectF(8.0, 0.0, double(kHeaderWidth) - 16.0, double(kRulerHeight)),
               Qt::AlignLeft | Qt::AlignVCenter,
               m_CaliperASet && m_CaliperBSet
                   ? QObject::tr("dt %1").arg(FormatSeconds(fabs(m_CaliperB - m_CaliperA)))
                   : QObject::tr("Time"));

    const double step = NiceStep(m_ViewSpanSec, rows.width());

    for(double t = ceil(t0 / step) * step; t <= t1; t += step)
    {
      const double x = XForTime(t);

      p.setPen(QPen(DimText(), 1));
      p.drawLine(QPointF(x, double(kRulerHeight) - 7.0), QPointF(x, double(kRulerHeight) - 1.0));

      p.setPen(Text());
      p.drawText(QRectF(x - 50.0, 0.0, 100.0, double(kRulerHeight) - 8.0),
                 Qt::AlignHCenter | Qt::AlignVCenter, FormatSeconds(t));
    }
  }

  void PaintOverlays(QPainter &p, const QRectF &rows)
  {
    p.save();
    p.setClipRect(rows);

    // --- frame window and action markers --------------------------------------------------
    if(m_Data.aligned && m_Data.frameEndSec > m_Data.frameStartSec)
    {
      const double t0 = m_ViewStartSec;
      const double t1 = t0 + m_ViewSpanSec;
      const double fa = qBound(t0, m_Data.frameStartSec, t1);
      const double fb = qBound(t0, m_Data.frameEndSec, t1);

      if(fb > fa)
      {
        const double x0 = XForTime(fa);
        const double x1 = XForTime(fb);

        // The frame window is drawn in the theme's accent so it reads as a selection over the
        // lanes rather than as a fourth unrelated colour on top of them.
        QColor frameTint = Accent();
        frameTint.setAlpha(32);

        p.setPen(Qt::NoPen);
        p.setBrush(frameTint);
        p.drawRect(QRectF(x0, rows.top(), x1 - x0, rows.height()));
        p.setBrush(Qt::NoBrush);

        p.setPen(QPen(Accent(), 1, Qt::DashLine));
        p.drawLine(QPointF(x0, rows.top()), QPointF(x0, rows.bottom()));
        p.drawLine(QPointF(x1, rows.top()), QPointF(x1, rows.bottom()));

        p.setPen(Accent().lighter(145));
        p.drawText(QRectF(x0 + 3.0, rows.top() + 2.0, qMax(80.0, x1 - x0), 14.0),
                   Qt::AlignLeft | Qt::AlignTop,
                   QObject::tr("Frame %1").arg(m_Data.frameNumber));
      }
    }

    const bool overlay = m_Data.aligned && m_Data.frameEndSec > m_Data.frameStartSec;

    if(overlay)
    {
      // Each action is a tick at its own start time inside the frame window.
      const double frameSeconds = m_Data.frameEndSec - m_Data.frameStartSec;

      for(const TraceActionRef &action : m_Data.actions)
      {
        if(!action.hasGpuDuration)
          continue;

        const double fraction = action.timeSec / frameSeconds;

        if(fraction < 0.0 || fraction > 1.0)
          continue;

        const double x = XForTime(m_Data.frameStartSec + action.timeSec);

        if(action.eventId == m_Data.highlightEvent)
        {
          p.setPen(QPen(Highlight(), 2));
          p.drawLine(QPointF(x, rows.top()), QPointF(x, rows.bottom()));

          p.setPen(Highlight());
          p.drawText(QRectF(x + 3.0, rows.bottom() - 18.0, 240.0, 14.0),
                     Qt::AlignLeft | Qt::AlignBottom,
                     QObject::tr("EID %1 %2").arg(action.eventId).arg(action.name));
        }
        else
        {
          p.setPen(QPen(QColor(0xff, 0xff, 0xff, 90), 1));
          p.drawLine(QPointF(x, rows.top() + 4.0), QPointF(x, rows.bottom() - 4.0));
        }
      }
    }
    else if(!m_Data.captureLoaded)
    {
      // No capture open: the lanes are still meaningful on their own, but there is nothing to
      // place them against, so say that instead of implying a failed clock alignment.
      p.setPen(QColor(0xe0, 0xb0, 0x6c));
      p.drawText(QRectF(rows.left() + 6.0, rows.top() + 4.0, rows.width() - 12.0, 16.0),
                 Qt::AlignLeft | Qt::AlignTop,
                 QObject::tr("No capture loaded: trace shown on its own time base"));
    }
    else
    {
      // Unaligned: the trace is shown from time zero, clearly labelled so the lanes are never
      // mistaken for being placed against the capture.
      p.setPen(QColor(0xe0, 0x6c, 0x6c));
      p.drawText(QRectF(rows.left() + 6.0, rows.top() + 4.0, rows.width() - 12.0, 16.0),
                 Qt::AlignLeft | Qt::AlignTop,
                 QObject::tr("Trace not aligned to the frame (clock domains unrelated)"));
    }

    // --- calipers -------------------------------------------------------------------------
    if(m_CaliperASet && m_CaliperBSet)
    {
      const double xa = XForTime(qMin(m_CaliperA, m_CaliperB));
      const double xb = XForTime(qMax(m_CaliperA, m_CaliperB));

      p.setPen(Qt::NoPen);
      p.setBrush(QColor(Highlight().red(), Highlight().green(), Highlight().blue(), 28));
      p.drawRect(QRectF(xa, rows.top(), xb - xa, rows.height()));
      p.setBrush(Qt::NoBrush);
    }

    if(m_CaliperASet)
    {
      const double x = XForTime(m_CaliperA);
      p.setPen(QPen(Highlight(), 1, Qt::DashLine));
      p.drawLine(QPointF(x, rows.top()), QPointF(x, rows.bottom()));
    }

    if(m_CaliperBSet)
    {
      const double x = XForTime(m_CaliperB);
      p.setPen(QPen(Highlight(), 1, Qt::DashLine));
      p.drawLine(QPointF(x, rows.top()), QPointF(x, rows.bottom()));
    }

    p.restore();
  }

  void PaintDrag(QPainter &p, const QRectF &rows)
  {
    const QRect dragRect = QRect(m_DragStart, m_DragCurrent).normalized();
    const QRectF clipped = QRectF(dragRect).intersected(rows);

    if(clipped.isEmpty())
      return;

    if(m_Dragging == DragZoom)
    {
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(Accent().red(), Accent().green(), Accent().blue(), 60));
      p.drawRect(clipped);
      p.setBrush(Qt::NoBrush);
    }

    p.setPen(QPen(m_Dragging == DragSelect ? Highlight() : Accent(), 1, Qt::DashLine));
    p.drawRect(clipped);
  }

  SystemTraceTimelineData m_Data;
  QVector<Row> m_Rows;
  QVector<Annotation> m_Annotations;
  QScrollBar *m_Scroll = NULL;

  // Which trace the current view window belongs to. When the viewer hands over a different trace
  // the window is reset to its full extent; an event-driven redraw leaves the zoom and pan alone.
  const SystemTrace *m_LastTrace = NULL;
  bool m_ResetPending = false;

  TraceNavMode m_Mode = TraceNavMode::Pan;
  DragMode m_Dragging = DragNone;

  QPoint m_DragStart;
  QPoint m_DragCurrent;
  double m_DragSpanStartSec = 0.0;
  double m_DragPanStartSec = 0.0;

  double m_ViewStartSec = 0.0;
  double m_ViewSpanSec = 1.0;

  int m_ScrollY = 0;
  int m_ContentHeight = 0;

  SpanRef m_Hover;
  SpanRef m_Selected;

  bool m_CaliperASet = false;
  bool m_CaliperBSet = false;
  double m_CaliperA = 0.0;
  double m_CaliperB = 0.0;
};

SystemTraceViewer::SystemTraceViewer(ICaptureContext &ctx, QWidget *parent)
    : QFrame(parent), m_Ctx(ctx)
{
  // The title is the docked tab's caption, not a window caption: the tool window manager reads
  // windowTitle() when it builds the tab. The panel is explicitly *not* delete-on-close, because
  // MainWindow keeps the single instance and re-shows it; deleting it here would leave that
  // pointer dangling.
  setWindowTitle(tr("System Trace"));
  setObjectName(lit("systemTraceViewer"));

  m_TracePath = new QLineEdit(this);
  m_TracePath->setReadOnly(true);
  m_TracePath->setPlaceholderText(tr("No trace loaded"));

  QPushButton *browse = new QPushButton(tr("Open Trace..."), this);
  QPushButton *reload = new QPushButton(tr("Reload"), this);

  // Recording controls. The duration is chosen before starting because a device protocol
  // serialises its commands onto one thread: a running `perfetto` cannot be interrupted, so the
  // recording must be bounded up front rather than stopped on demand.
  //
  // The ceiling is 10 seconds because that is the limit Snapdragon Profiler documents for its own
  // trace captures; offering more would only produce a duration the device tooling caps anyway.
  m_CaptureSeconds = new QSpinBox(this);
  m_CaptureSeconds->setRange(1, 10);
  m_CaptureSeconds->setValue(5);
  m_CaptureSeconds->setSuffix(tr(" s"));

  m_Capture = new QPushButton(tr("Capture Trace..."), this);

  connect(browse, &QPushButton::clicked, this, [this] { browseForTrace(); });
  connect(reload, &QPushButton::clicked, this, [this] { reloadTrace(); });
  connect(m_Capture, &QPushButton::clicked, this, [this] { captureTrace(); });

  QHBoxLayout *fileRow = new QHBoxLayout();
  fileRow->addWidget(new QLabel(tr("Trace:"), this));
  fileRow->addWidget(m_TracePath, 1);
  fileRow->addWidget(browse);
  fileRow->addWidget(reload);
  fileRow->addWidget(new QLabel(tr("Record:"), this));
  fileRow->addWidget(m_CaptureSeconds);
  fileRow->addWidget(m_Capture);

  // Navigation modes, matching SDP's Trace window: Select, Pan and Zoom, also reachable with the
  // 1, 2 and 3 keys while the timeline has focus.
  m_ModeSelect = new QToolButton(this);
  m_ModeSelect->setText(tr("Select"));
  m_ModeSelect->setCheckable(true);
  m_ModeSelect->setToolTip(tr("Select (1): drag a box to pick elements; their fields are shown "
                              "below the timeline."));

  m_ModePan = new QToolButton(this);
  m_ModePan->setText(tr("Pan"));
  m_ModePan->setCheckable(true);
  m_ModePan->setToolTip(tr("Pan (2): drag to move the visible time window. Ctrl+A / Ctrl+D also "
                           "pan. This is the default mode."));

  m_ModeZoom = new QToolButton(this);
  m_ModeZoom->setText(tr("Zoom"));
  m_ModeZoom->setCheckable(true);
  m_ModeZoom->setToolTip(tr("Zoom (3): drag a region to zoom into it. The wheel and Ctrl+W / "
                            "Ctrl+S zoom as well."));

  connect(m_ModeSelect, &QToolButton::clicked, this, [this] { setNavMode(TraceNavMode::Select); });
  connect(m_ModePan, &QToolButton::clicked, this, [this] { setNavMode(TraceNavMode::Pan); });
  connect(m_ModeZoom, &QToolButton::clicked, this, [this] { setNavMode(TraceNavMode::Zoom); });

  QLabel *modeHint = new QLabel(tr("Double-click sets a caliper, shift-click the second one; "
                                   "right-click an element to annotate it."),
                                this);
  modeHint->setWordWrap(true);

  QHBoxLayout *modeRow = new QHBoxLayout();
  modeRow->addWidget(new QLabel(tr("Navigation:"), this));
  modeRow->addWidget(m_ModeSelect);
  modeRow->addWidget(m_ModePan);
  modeRow->addWidget(m_ModeZoom);
  modeRow->addWidget(modeHint, 1);

  m_Chart = new SystemTraceTimeline(this);
  m_Chart->OnSelectEvent = [this](uint32_t eventId) { selectEvent(eventId); };
  m_Chart->OnSelectSpan = [this](const TraceSpan &span, const QString &trackName) {
    showElementFields(span, trackName);
  };
  m_Chart->OnNavModeChanged = [this](TraceNavMode mode) { setNavMode(mode); };

  m_Series = new QListWidget(this);

  m_Frames = new QTableWidget(this);
  m_Frames->setColumnCount(2);
  m_Frames->setHorizontalHeaderLabels({tr("Frame"), tr("Trace start")});
  m_Frames->horizontalHeader()->setStretchLastSection(true);
  m_Frames->verticalHeader()->setVisible(false);
  m_Frames->setEditTriggers(QAbstractItemView::NoEditTriggers);

  m_Warnings = new QListWidget(this);

  // The capture log is kept as a tab rather than a transient popup: it holds every device command
  // and every tool result in order, which is the only way a failed conversion is diagnosable.
  m_CaptureLog = new QPlainTextEdit(this);
  m_CaptureLog->setReadOnly(true);
  m_CaptureLog->setPlaceholderText(tr("No capture has been recorded in this window yet."));

  // Element pane. This is the job SDP's Inspector does for a selected trace element: the fields
  // the trace recorded for it, verbatim.
  QWidget *elementPane = new QWidget(this);

  m_ElementCaption = new QLabel(tr("No element selected"), elementPane);
  m_ElementCaption->setWordWrap(true);

  m_Element = new QTableWidget(elementPane);
  m_Element->setColumnCount(2);
  m_Element->setHorizontalHeaderLabels({tr("Field"), tr("Value")});
  m_Element->horizontalHeader()->setStretchLastSection(true);
  m_Element->verticalHeader()->setVisible(false);
  m_Element->setEditTriggers(QAbstractItemView::NoEditTriggers);

  QVBoxLayout *elementLayout = new QVBoxLayout(elementPane);
  elementLayout->addWidget(m_ElementCaption);
  elementLayout->addWidget(m_Element, 1);

  QTabWidget *sideTabs = new QTabWidget(this);
  sideTabs->addTab(m_Series, tr("Tracks"));
  sideTabs->addTab(m_Frames, tr("Frames"));
  sideTabs->addTab(elementPane, tr("Element"));
  sideTabs->addTab(m_Warnings, tr("Warnings"));
  sideTabs->addTab(m_CaptureLog, tr("Capture Log"));

  // The timeline and the side tabs are separated by a real splitter rather than a fixed layout, so
  // the boundary between the two is draggable: the trace plot and the field/log panes compete for
  // the same width and which one needs more depends on what the user is reading. The side pane is
  // still capped (a 2000px-wide "Capture Log" tab would be useless) but the cap is now well above
  // the initial 320px share, so dragging can widen it instead of the cap silently refusing.
  //
  // RDSplitter, not QSplitter: the project's splitter handle paints its own grab dots from the
  // palette's WindowText, so it lands in whatever scheme RDStyle installed without a stylesheet --
  // a bare handle would be an untinted platform default in the middle of a themed panel. The 12px
  // width is the same one BufferViewer and BufferFormatSpecifier use.
  //
  // Stretch factors give the timeline all the slack: widening the window grows the plot, not the
  // side tabs, because the side pane's content is mostly fixed-width tables. Children are not
  // collapsible, so the handle never reduces either pane to nothing and no collapse arrows are drawn.
  RDSplitter *middle = new RDSplitter(Qt::Horizontal, this);
  middle->setHandleWidth(12);
  middle->addWidget(m_Chart);
  middle->addWidget(sideTabs);
  middle->setStretchFactor(0, 1);
  middle->setStretchFactor(1, 0);
  middle->setChildrenCollapsible(false);
  middle->setSizes({640, 320});

  sideTabs->setMinimumWidth(180);
  sideTabs->setMaximumWidth(900);

  m_Alignment = new QLabel(this);
  m_Alignment->setWordWrap(true);

  m_Selection = new QLabel(this);
  m_Selection->setWordWrap(true);

  m_Status = new QLabel(this);
  m_Status->setWordWrap(true);

  // No close button: the panel is closed through its dock tab, and hiding it via the tab keeps
  // the imported trace and the capture log alive for when it is re-opened.
  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addLayout(fileRow);
  layout->addLayout(modeRow);
  layout->addWidget(middle, 1);
  layout->addWidget(m_Alignment);
  layout->addWidget(m_Selection);
  layout->addWidget(m_Status);

  m_Ctx.AddCaptureViewer(this);

  setNavMode(TraceNavMode::Pan);
  updateCaptureControls();
  updateStatusText();
  updateHighlight();
}

SystemTraceViewer::~SystemTraceViewer()
{
  // Only the viewer registration is released here. This panel is not owned by CaptureContext - the
  // MainWindow creates and retains it, and the dock area owns it once added - so it must not call
  // BuiltinWindowClosed(): that would hit the unrecognised-window branch and log a critical for a
  // panel CaptureContext never handed out. Detaching from the capture viewer list is still required,
  // because the context notifies every registered viewer on capture load / close / event change.
  m_Ctx.RemoveCaptureViewer(this);
}

void SystemTraceViewer::OnCaptureLoaded()
{
  updateCaptureControls();
  rebuild();
}

void SystemTraceViewer::OnCaptureClosed()
{
  m_Correlation = TraceCorrelation();
  m_FrameEventTimes.clear();
  m_FrameDuration = 0.0;
  m_CorrelatedFrame = 0;

  updateCaptureControls();

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
  // The capture path always produces a Chrome Trace Event Format document, while the documented
  // interchange is JSON-lines. Both are accepted, and the file itself decides which importer runs:
  // selecting on the extension alone would mis-route a `traceconv json` output named `.txt`.
  m_Trace = LooksLikeChromeTrace(path) ? ImportChromeTraceEventFormat(path)
                                       : ImportSystemTraceJsonLines(path);

  m_TracePath->setText(path);
  m_Loaded = true;

  // Fill the informational side panels. These are static for a given import, so they are not
  // touched on every frame change.
  m_Series->clear();
  for(const TraceTrack &track : m_Trace.tracks)
  {
    if(track.kind == TraceTrackKind::Counter)
    {
      QString unit;
      if(track.counterSeries >= 0 && track.counterSeries < m_Trace.series.size())
        unit = m_Trace.series[track.counterSeries].unit;

      m_Series->addItem(unit.isEmpty()
                            ? tr("Metric: %1").arg(track.name)
                            : tr("Metric: %1 [%2]").arg(track.name, unit));
    }
    else
    {
      m_Series->addItem(tr("Track: %1 (%2 spans)%3")
                            .arg(track.name)
                            .arg(track.spans.size())
                            .arg(track.group.isEmpty() ? QString()
                                                       : tr(" in %1").arg(track.group)));
    }
  }

  // A trace that carried only counters has no lanes, so the list falls back to the series it does
  // have rather than appearing empty.
  if(m_Trace.tracks.isEmpty())
  {
    for(const TraceSeries &series : m_Trace.series)
    {
      m_Series->addItem(series.unit.isEmpty()
                            ? tr("%1 (%2 samples)").arg(series.name).arg(series.samples.size())
                            : tr("%1 [%2] (%3 samples)")
                                  .arg(series.name, series.unit)
                                  .arg(series.samples.size()));
    }
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

  // A newly imported trace starts from its full extent, and any previous annotations and
  // calipers belong to the trace that is gone.
  if(m_Chart)
    m_Chart->ResetView();

  if(m_Element)
    m_Element->setRowCount(0);

  if(m_ElementCaption)
    m_ElementCaption->setText(tr("No element selected"));

  rebuild();
  updateStatusText();
}

void SystemTraceViewer::captureTrace()
{
  if(m_Capturing)
    return;

  // The device is reached through the protocol of the host this session is connected to. When no
  // protocol is active there is nothing this path can talk to, so the button is refused rather
  // than starting a pipeline that would fail at the first command.
  const RemoteHost remote = m_Ctx.Replay().CurrentRemote();
  IDeviceProtocolController *protocol = remote.Protocol();

  if(protocol == NULL)
  {
    RDDialog::warning(this, tr("Cannot capture a trace"),
                      tr("No device protocol is connected. Connect to an Android device (or "
                         "another protocol that supports file transfer) and try again."));
    return;
  }

  TraceCaptureOptions opts;
  opts.durationMs = m_CaptureSeconds->value() * 1000;

  // The only place a `traceconv` binary may be found is the Snapdragon Profiler installation the
  // user already pointed at in Settings; the driver falls back to PATH discovery when this is
  // empty. Nothing is bundled or redistributed with RenderDoc.
  const QString profilerPath =
      QString::fromUtf8(m_Ctx.Config().ExternalTool_SnapdragonProfiler.c_str());
  if(!profilerPath.isEmpty())
    opts.snapdragonProfilerPath = QFileInfo(profilerPath).absolutePath();

  // The hostname IS the device URL the caller selected this device by, so it is passed through
  // unchanged: the driver must never re-derive the device identity.
  const QString url = QString::fromUtf8(remote.Hostname().c_str());

  m_Capturing = true;
  updateCaptureControls();

  m_Status->setText(tr("Recording up to %1 seconds of system trace on %2. The recording is "
                       "bounded by that duration because a device command cannot be interrupted "
                       "once it is running.")
                        .arg(m_CaptureSeconds->value())
                        .arg(url));

  if(m_CaptureLog)
  {
    m_CaptureLog->clear();
    m_CaptureLog->appendPlainText(tr("Starting capture on %1 (duration %2 ms)")
                                      .arg(url)
                                      .arg(opts.durationMs));
  }

  // The driver blocks on every device command, so it runs on a worker thread; TraceCaptureJob
  // delivers the result back on this object's thread and guards against the dialog being closed
  // while the pipeline is still running.
  TraceCaptureJob::Start(this, protocol, url, opts, [this](const TraceCaptureResult &result) {
    m_Capturing = false;
    updateCaptureControls();

    if(m_CaptureLog)
    {
      m_CaptureLog->clear();
      for(const QString &line : result.log)
        m_CaptureLog->appendPlainText(line);

      if(!result.error.isEmpty())
        m_CaptureLog->appendPlainText(tr("\nFAILED: %1").arg(result.error));

      if(!result.localTracePath.isEmpty())
        m_CaptureLog->appendPlainText(tr("\nPerfetto trace: %1").arg(result.localTracePath));
      if(!result.localJsonPath.isEmpty())
        m_CaptureLog->appendPlainText(tr("Importable JSON: %1").arg(result.localJsonPath));
    }

    if(!result.success)
    {
      updateStatusText();
      RDDialog::warning(this, tr("Trace capture failed"),
                        tr("The trace could not be recorded or converted.\n\n%1\n\nEvery command "
                           "that ran is listed in the Capture Log tab.")
                            .arg(result.error.isEmpty() ? tr("unknown error") : result.error));
      return;
    }

    // The captured JSON is the same document the importer accepts, so it is loaded through the
    // normal path: one code path for both captured and manually opened traces.
    loadTraceFrom(result.localJsonPath);
  });
}

void SystemTraceViewer::updateCaptureControls()
{
  if(m_Capture == NULL || m_CaptureSeconds == NULL)
    return;

  const bool haveProtocol = m_Ctx.Replay().CurrentRemote().Protocol() != NULL;

  m_Capture->setEnabled(haveProtocol && !m_Capturing);
  m_CaptureSeconds->setEnabled(!m_Capturing);

  if(m_Capturing)
  {
    m_Capture->setText(tr("Recording..."));
    m_Capture->setToolTip(
        tr("A recording cannot be stopped early: the device protocol serialises commands, so the "
           "run finishes at the duration chosen above."));
  }
  else
  {
    m_Capture->setText(tr("Capture Trace..."));
    m_Capture->setToolTip(haveProtocol ? tr("Record a system trace directly from the connected "
                                            "device with its own perfetto, then import it.")
                                       : tr("Connect to a device that implements file transfer "
                                            "commands to record a trace."));
  }
}

void SystemTraceViewer::setNavMode(TraceNavMode mode)
{
  if(m_Chart)
    m_Chart->SetNavMode(mode);

  // The buttons are kept in step with the timeline, whichever drove the change (a button, or the
  // 1/2/3 shortcut handled inside the timeline).
  if(m_ModeSelect)
    m_ModeSelect->setChecked(mode == TraceNavMode::Select);
  if(m_ModePan)
    m_ModePan->setChecked(mode == TraceNavMode::Pan);
  if(m_ModeZoom)
    m_ModeZoom->setChecked(mode == TraceNavMode::Zoom);
}

void SystemTraceViewer::showElementFields(const TraceSpan &span, const QString &trackName)
{
  if(m_Element == NULL)
    return;

  m_Element->setRowCount(0);

  const auto addField = [this](const QString &name, const QString &value) {
    const int row = m_Element->rowCount();
    m_Element->insertRow(row);
    m_Element->setItem(row, 0, new QTableWidgetItem(name));
    m_Element->setItem(row, 1, new QTableWidgetItem(value));
  };

  if(!trackName.isEmpty())
    addField(tr("Track"), trackName);

  addField(tr("Name"), span.name);

  if(!span.category.isEmpty())
    addField(tr("Category"), span.category);

  addField(tr("Begin"), FormatSeconds(span.beginSec));
  addField(tr("End"), FormatSeconds(span.endSec));
  addField(tr("Duration"), FormatSeconds(span.endSec - span.beginSec));

  if(span.depth > 0)
    addField(tr("Nesting depth"), QString::number(span.depth));

  // The trace's own argument fields, verbatim. Nothing here is reinterpreted: the importer kept
  // them as the source rendered them.
  for(const QString &field : span.fields)
  {
    const int split = field.indexOf(QStringLiteral(" = "));

    if(split > 0)
      addField(field.left(split), field.mid(split + 3));
    else
      addField(tr("Argument"), field);
  }

  m_Element->resizeColumnToContents(0);

  if(m_ElementCaption)
    m_ElementCaption->setText(trackName.isEmpty()
                                  ? tr("Element: %1").arg(span.name)
                                  : tr("Element: %1 on %2").arg(span.name, trackName));
}

void SystemTraceViewer::rebuild()
{
  // Any cached correlation belongs to the capture it was computed for, so drop it unconditionally
  // before deciding what can be drawn.
  m_FrameEventTimes.clear();
  m_FrameDuration = 0.0;
  m_CorrelatedFrame = 0;
  m_Correlation = TraceCorrelation();

  if(!traceUsable())
  {
    if(m_Chart)
      m_Chart->Clear();

    updateHighlight();
    updateStatusText();
    return;
  }

  // The imported lanes do not depend on RenderDoc: keep showing them (on their own time base)
  // even with no capture open. Only the frame overlay and the per-event readout need a capture.
  if(!m_Ctx.IsCaptureLoaded())
  {
    showTraceOnly();
    updateStatusText();
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

  if(!traceUsable())
  {
    m_Chart->Clear();
    m_Alignment->setText(QString());
    m_Selection->setText(QString());
    return;
  }

  if(!m_Ctx.IsCaptureLoaded())
  {
    // The lanes stay visible; the frame overlay and the per-event readout cannot be produced
    // without a capture, so they are cleared instead of showing stale data.
    showTraceOnly();
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

  SystemTraceTimelineData chartData;
  chartData.trace = &m_Trace;
  chartData.actions = m_Correlation.actions;
  chartData.alignment = m_Correlation.alignment;
  chartData.aligned = m_Correlation.aligned();
  chartData.frameStartSec = m_Correlation.alignment.offsetSec;
  chartData.frameEndSec = chartData.frameStartSec + m_FrameDuration;
  chartData.frameNumber = frame;
  chartData.highlightEvent = m_Ctx.CurSelectedEvent();

  m_Chart->SetData(chartData);

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

void SystemTraceViewer::showTraceOnly()
{
  // Trace-only presentation: the lanes are drawn on their own time base and both correlation
  // readouts are emptied, because neither has a meaning without a frame to align to.
  if(m_Chart)
  {
    SystemTraceTimelineData chartData;
    chartData.trace = &m_Trace;
    chartData.aligned = false;
    chartData.captureLoaded = false;

    m_Chart->SetData(chartData);
  }

  if(m_Alignment)
  {
    m_Alignment->setText(
        tr("Clock alignment: not applicable — no capture is loaded, so the trace is shown on its "
           "own time base."));
  }

  if(m_Selection)
    m_Selection->setText(QString());
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
    if(m_Capturing)
    {
      m_Status->setText(tr("Recording a system trace on the connected device..."));
      return;
    }

    const bool haveProtocol = m_Ctx.Replay().CurrentRemote().Protocol() != NULL;

    if(haveProtocol)
    {
      m_Status->setText(tr("No trace loaded. Use \"Capture Trace...\" to record one directly from "
                           "the connected device, or \"Open Trace...\" to import one. Then click a "
                           "lane element to read its fields, or a draw/dispatch marker to select it "
                           "in RenderDoc."));
    }
    else
    {
      m_Status->setText(tr("No trace loaded. Use \"Open Trace...\" to import one, then click a lane "
                           "element to read its fields, or a draw/dispatch marker to select it in "
                           "RenderDoc. Recording a trace needs a connected device, which this "
                           "session does not have."));
    }
    return;
  }

  if(!m_Trace.valid)
  {
    m_Status->setText(tr("Could not import the trace: %1")
                          .arg(m_Trace.warnings.isEmpty() ? tr("unknown error")
                                                          : m_Trace.warnings.join(QLatin1Char(' '))));
    return;
  }

  const QString source = m_Trace.source.isEmpty() ? tr("(unnamed)") : m_Trace.source;

  if(!m_Ctx.IsCaptureLoaded())
  {
    m_Status->setText(tr("Trace: %1  |  %2 tracks, %3 spans, %4 metrics, %5 frame boundaries. No "
                         "capture is loaded, so there is nothing to correlate against yet; open a "
                         "capture and the frame overlay appears here.")
                          .arg(source)
                          .arg(m_Trace.tracks.size())
                          .arg(m_Trace.SpanCount())
                          .arg(m_Trace.series.size())
                          .arg(m_Trace.frameBoundaries.size()));
    return;
  }

  m_Status->setText(tr("Trace: %1  |  %2 tracks, %3 spans, %4 metrics, %5 frame boundaries, %6 "
                       "matched to this capture")
                        .arg(source)
                        .arg(m_Trace.tracks.size())
                        .arg(m_Trace.SpanCount())
                        .arg(m_Trace.series.size())
                        .arg(m_Trace.frameBoundaries.size())
                        .arg(m_Correlation.matchedFrames));
}
