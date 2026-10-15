/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2017-2026 Baldur Karlsson
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

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

struct ICaptureContext;

namespace MCP
{
class QueryAdapter;
}

// P1-9 investigation report UI
// ----------------------------
// A small, code-only dialog (no .ui file, so nothing extra has to be added to the
// build manifests beyond the header/source) that drives the
// renderdoc_buildInvestigationReport capability from the native application.
//
// It composes the same artefact the MCP tool builds - capture metadata, the P1-8
// bookmarks, the P1-6 performance report and the P1-4 resource-memory audit - and
// shows the rendered document in a read-only preview. The user picks the rendering
// format, the row limit and whether to attach the resource audit, refreshes the
// preview, and can save the rendered content to disk. The save is the only side
// effect and is owned by this dialog: the adapter/tool stays strictly read-only so
// an MCP client can never be tricked into writing files.
class InvestigationReportDialog : public QDialog
{
  Q_OBJECT

public:
  explicit InvestigationReportDialog(ICaptureContext &ctx, MCP::QueryAdapter *adapter,
                                     QWidget *parent = 0);
  ~InvestigationReportDialog();

private slots:
  void on_format_changed();
  void on_options_changed();
  void on_refresh_clicked();
  void on_export_clicked();
  void on_copy_clicked();

private:
  // Ask the adapter to rebuild the report with the current option set. Returns
  // false (and leaves m_Content empty) when the report cannot be built yet - e.g.
  // no capture is loaded - so the preview can explain why instead of showing a
  // stale document.
  bool refresh();

  // Save the currently previewed content. Uses the native save dialog so the
  // renderer/extension stays consistent with the rest of qrenderdoc.
  void exportContent();

  QString selectedFormat() const;

  ICaptureContext &m_Ctx;
  MCP::QueryAdapter *m_Adapter;

  QComboBox *m_Format;
  QSpinBox *m_Limit;
  QCheckBox *m_IncludeResources;

  QPlainTextEdit *m_Preview;
  QLabel *m_Status;

  QPushButton *m_Refresh;
  QPushButton *m_Export;
  QPushButton *m_Copy;

  // The rendered document currently shown in the preview, kept so the export and
  // copy actions never re-run the (potentially expensive) build.
  QString m_Content;
  QString m_SuggestedName;
};
