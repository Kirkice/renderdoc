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

#include <atomic>

#include <QDialog>
#include <QSharedPointer>

#include "Code/AI/MCPExternalTools.h"

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

struct ICaptureContext;

// Native presentation for a single captured SPIR-V shader analysed with Arm's
// Mali Offline Compiler. The actual compiler invocation and output parsing stay
// in MCPExternalTools, shared with the MCP tool, so the UI never builds a shell
// command or has its own parser.
class MaliAnalysisDialog : public QDialog
{
public:
  explicit MaliAnalysisDialog(ICaptureContext &ctx, const MCP::MaliShaderExport &shader,
                              QWidget *parent = 0);
  ~MaliAnalysisDialog();

private:
  void startAnalysis();
  void cancelAnalysis();
  void configureCompiler();
  void showResult(const MCP::MaliRunResult &result);
  void setRunning(bool running);
  QString cacheKey() const;
  void restoreCachedResult();
  void clearDisplayedResult();

  ICaptureContext &m_Ctx;
  MCP::MaliShaderExport m_Shader;
  QSharedPointer<std::atomic_bool> m_Cancelled;

  QComboBox *m_Device = NULL;
  QLabel *m_ShaderInfo = NULL;
  QLabel *m_Command = NULL;
  QLabel *m_Status = NULL;
  QLabel *m_Summary = NULL;
  QTreeWidget *m_Metrics = NULL;
  QPlainTextEdit *m_Output = NULL;
  QPushButton *m_Configure = NULL;
  QPushButton *m_Run = NULL;
  QPushButton *m_Copy = NULL;
  bool m_Running = false;
};
