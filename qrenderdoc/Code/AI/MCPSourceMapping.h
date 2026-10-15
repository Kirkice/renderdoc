/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2026 Baldur Karlsson
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

// P2-4 project source mapping
// ----------------------------
// A capture can preserve source file names in debug information, but the absolute
// path usually belongs to the machine that created it. This module maps that
// recorded reference onto a local project source tree. It deliberately keeps the
// matching policy separate from capture/replay and UI code so a client can inspect
// why a match was selected and tests can cover every confidence transition.
//
// The resolution order is intentionally conservative:
//
//   1. A manual override wins and is reported as Manual.
//   2. An exact normalized absolute path is Confirmed.
//   3. A root-relative path or unique filename is Inferred.
//   4. A suffix or fuzzy filename match is Low confidence.
//   5. Ties are never guessed: they are returned as ambiguous candidates.
//
// IndexSourceMappingPaths() performs only local, bounded recursive filesystem
// enumeration. All matching/serialization functions remain deterministic over
// plain data, so callers may build the index at a time appropriate for their UI.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <stdint.h>

#include "MCPAnalysisModel.h"

namespace MCP
{
enum class SourceMatchKind
{
  None,
  Manual,
  ExactPath,
  RelativePath,
  Filename,
  Suffix,
  Fuzzy,
};

QString SourceMatchKindName(SourceMatchKind kind);

struct SourceMappingOptions
{
  // Project roots are searched recursively. INCLUDE/search paths are searched
  // with the same rules, but are kept distinct in the result so callers know
  // whether a source came from the project or an external dependency.
  QStringList projectRoots;
  QStringList includePaths;

  // File extensions are case-insensitive and include the common shader and C/C++
  // source formats by default. An empty list permits all regular files.
  QStringList extensions = {QStringLiteral("c"),    QStringLiteral("cc"),
                            QStringLiteral("cpp"),  QStringLiteral("cxx"),
                            QStringLiteral("h"),    QStringLiteral("hpp"),
                            QStringLiteral("hlsl"), QStringLiteral("hlsli"),
                            QStringLiteral("glsl"), QStringLiteral("vert"),
                            QStringLiteral("frag"), QStringLiteral("comp"),
                            QStringLiteral("geom"), QStringLiteral("tesc"),
                            QStringLiteral("tese"), QStringLiteral("metal")};

  // Recursive scans are bounded to avoid turning a mapping request into a whole
  // disk crawl when a configuration entry is too broad.
  int maxFiles = 20000;

  // Candidate similarity threshold in [0, 1]. A fuzzy result below this value is
  // omitted entirely instead of being presented as a plausible source location.
  double fuzzyThreshold = 0.78;
};

struct SourceFileEntry
{
  QString absolutePath;
  QString rootPath;
  QString relativePath;
  bool fromIncludePath = false;
};

struct SourceMappingIndex
{
  SourceMappingOptions options;
  QVector<SourceFileEntry> files;
  QStringList skippedRoots;
  bool truncated = false;
};

struct SourceMappingOverride
{
  // Recorded source reference as it appears in capture/debug data. Comparisons
  // are path-normalized and case-insensitive to make Windows and POSIX captures
  // portable across hosts.
  QString sourceReference;
  QString localPath;
};

struct SourceMappingCandidate
{
  SourceFileEntry file;
  SourceMatchKind kind = SourceMatchKind::None;
  Confidence confidence = Confidence::ToVerify;
  double score = 0.0;
};

struct SourceMappingResult
{
  QString sourceReference;
  SourceMappingCandidate selected;
  QVector<SourceMappingCandidate> candidates;
  bool ambiguous = false;
  QStringList limitations;
};

// Canonicalizes separators, dot segments and case for comparison only. The value
// is not used as an output path, so it never changes the user's chosen spelling.
QString NormalizeSourcePath(const QString &path);

// Computes a stable character similarity in [0, 1]. It is exposed for focused
// tests and for future UI filtering, but SourceMappingResult always records the
// actual selected score as evidence.
double SourceFilenameSimilarity(const QString &a, const QString &b);

// Enumerate files below configured roots. Missing/unreadable roots are retained
// in skippedRoots rather than treated as a fatal error, enabling partial mappings.
SourceMappingIndex IndexSourceMappingPaths(const SourceMappingOptions &options);

// Resolve one recorded source path against an existing index. Manual mappings are
// considered first and only accepted when their target is an existing regular file.
SourceMappingResult ResolveSourceMapping(const SourceMappingIndex &index,
                                         const QString &sourceReference,
                                         const QVector<SourceMappingOverride> &overrides = {});

QJsonObject SourceMappingOptionsToJson(const SourceMappingOptions &options);
QJsonObject SourceFileEntryToJson(const SourceFileEntry &file);
QJsonObject SourceMappingIndexToJson(const SourceMappingIndex &index);
QJsonObject SourceMappingCandidateToJson(const SourceMappingCandidate &candidate);
QJsonObject SourceMappingResultToJson(const SourceMappingResult &result);
}    // namespace MCP
