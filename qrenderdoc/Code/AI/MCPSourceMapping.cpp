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

#include "MCPSourceMapping.h"

#include <algorithm>
#include <cmath>

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>

namespace MCP
{
namespace
{
QString NormalizedExtension(const QString &path)
{
  return QFileInfo(path).suffix().toCaseFolded();
}

bool ExtensionAllowed(const QString &path, const QStringList &extensions)
{
  if(extensions.isEmpty())
    return true;

  const QString extension = NormalizedExtension(path);
  for(const QString &allowed : extensions)
  {
    QString normalized = allowed;
    if(normalized.startsWith(QLatin1Char('.')))
      normalized.remove(0, 1);
    if(extension == normalized.toCaseFolded())
      return true;
  }

  return false;
}

QStringList PathComponents(const QString &path)
{
  return NormalizeSourcePath(path).split(QLatin1Char('/'), QString::SkipEmptyParts);
}

int CommonSuffixComponents(const QString &a, const QString &b)
{
  const QStringList componentsA = PathComponents(a);
  const QStringList componentsB = PathComponents(b);
  int matched = 0;

  while(matched < componentsA.size() && matched < componentsB.size() &&
        componentsA[componentsA.size() - matched - 1] == componentsB[componentsB.size() - matched - 1])
  {
    ++matched;
  }

  return matched;
}

QString SourceBasename(const QString &path)
{
  return QFileInfo(path).fileName().toCaseFolded();
}

QString FileStem(const QString &path)
{
  return QFileInfo(path).completeBaseName().toCaseFolded();
}

bool IsBetterCandidate(const SourceMappingCandidate &a, const SourceMappingCandidate &b)
{
  if(a.score != b.score)
    return a.score > b.score;
  if(a.kind != b.kind)
    return int(a.kind) < int(b.kind);
  return NormalizeSourcePath(a.file.absolutePath) < NormalizeSourcePath(b.file.absolutePath);
}

bool SameRank(const SourceMappingCandidate &a, const SourceMappingCandidate &b)
{
  return a.kind == b.kind && std::fabs(a.score - b.score) < 0.000001;
}

SourceMappingCandidate CandidateFor(const SourceFileEntry &file, SourceMatchKind kind, Confidence confidence,
                                    double score)
{
  SourceMappingCandidate candidate;
  candidate.file = file;
  candidate.kind = kind;
  candidate.confidence = confidence;
  candidate.score = score;
  return candidate;
}

QJsonArray StringListToJson(const QStringList &values)
{
  QJsonArray array;
  for(const QString &value : values)
    array.push_back(value);
  return array;
}
}    // namespace

QString SourceMatchKindName(SourceMatchKind kind)
{
  switch(kind)
  {
    case SourceMatchKind::None: return QStringLiteral("none");
    case SourceMatchKind::Manual: return QStringLiteral("manual");
    case SourceMatchKind::ExactPath: return QStringLiteral("exactPath");
    case SourceMatchKind::RelativePath: return QStringLiteral("relativePath");
    case SourceMatchKind::Filename: return QStringLiteral("filename");
    case SourceMatchKind::Suffix: return QStringLiteral("suffix");
    case SourceMatchKind::Fuzzy: return QStringLiteral("fuzzy");
  }

  return QStringLiteral("none");
}

QString NormalizeSourcePath(const QString &path)
{
  QString normalized = QDir::fromNativeSeparators(path.trimmed());
  if(normalized.isEmpty())
    return normalized;

  normalized = QDir::cleanPath(normalized);
  return normalized.toCaseFolded();
}

double SourceFilenameSimilarity(const QString &a, const QString &b)
{
  const QString left = FileStem(a);
  const QString right = FileStem(b);
  if(left.isEmpty() || right.isEmpty())
    return 0.0;
  if(left == right)
    return 1.0;

  // Levenshtein similarity is deterministic, inexpensive for a filename, and
  // behaves predictably for the short typo/rename cases source mapping sees.
  QVector<int> previous(right.size() + 1);
  QVector<int> current(right.size() + 1);
  for(int j = 0; j <= right.size(); ++j)
    previous[j] = j;

  for(int i = 1; i <= left.size(); ++i)
  {
    current[0] = i;
    for(int j = 1; j <= right.size(); ++j)
    {
      const int substitution = previous[j - 1] + (left[i - 1] == right[j - 1] ? 0 : 1);
      current[j] = std::min(std::min(previous[j] + 1, current[j - 1] + 1), substitution);
    }
    previous.swap(current);
  }

  const int maximum = std::max(left.size(), right.size());
  return maximum == 0 ? 1.0 : 1.0 - double(previous[right.size()]) / double(maximum);
}

SourceMappingIndex IndexSourceMappingPaths(const SourceMappingOptions &options)
{
  SourceMappingIndex index;
  index.options = options;
  const int maxFiles = std::max(1, options.maxFiles);
  QSet<QString> seenFiles;

  auto indexRoots = [&](const QStringList &roots, bool includePath) {
    for(const QString &configuredRoot : roots)
    {
      const QFileInfo rootInfo(configuredRoot);
      if(!rootInfo.exists() || !rootInfo.isDir())
      {
        index.skippedRoots.push_back(configuredRoot);
        continue;
      }

      const QString rootPath = QDir::cleanPath(rootInfo.absoluteFilePath());
      QDirIterator it(rootPath, QDir::Files | QDir::Readable | QDir::NoSymLinks,
                      QDirIterator::Subdirectories);
      while(it.hasNext())
      {
        if(index.files.size() >= maxFiles)
        {
          index.truncated = true;
          return;
        }

        const QString absolutePath = QDir::cleanPath(it.next());
        if(!ExtensionAllowed(absolutePath, options.extensions))
          continue;

        const QString normalized = NormalizeSourcePath(absolutePath);
        if(seenFiles.contains(normalized))
          continue;
        seenFiles.insert(normalized);

        SourceFileEntry entry;
        entry.absolutePath = absolutePath;
        entry.rootPath = rootPath;
        entry.relativePath = QDir(rootPath).relativeFilePath(absolutePath);
        entry.fromIncludePath = includePath;
        index.files.push_back(entry);
      }
    }
  };

  indexRoots(options.projectRoots, false);
  if(!index.truncated)
    indexRoots(options.includePaths, true);

  std::sort(index.files.begin(), index.files.end(), [](const SourceFileEntry &a, const SourceFileEntry &b) {
    return NormalizeSourcePath(a.absolutePath) < NormalizeSourcePath(b.absolutePath);
  });
  return index;
}

SourceMappingResult ResolveSourceMapping(const SourceMappingIndex &index, const QString &sourceReference,
                                         const QVector<SourceMappingOverride> &overrides)
{
  SourceMappingResult result;
  result.sourceReference = sourceReference;
  const QString reference = NormalizeSourcePath(sourceReference);
  if(reference.isEmpty())
  {
    result.limitations.push_back(QStringLiteral("The source reference is empty."));
    return result;
  }

  for(const SourceMappingOverride &overrideEntry : overrides)
  {
    if(NormalizeSourcePath(overrideEntry.sourceReference) != reference)
      continue;

    QFileInfo localInfo(overrideEntry.localPath);
    if(localInfo.exists() && localInfo.isFile())
    {
      SourceFileEntry file;
      file.absolutePath = QDir::cleanPath(localInfo.absoluteFilePath());
      file.rootPath = QString();
      file.relativePath = localInfo.fileName();
      result.selected = CandidateFor(file, SourceMatchKind::Manual, Confidence::Confirmed, 1.0);
      result.candidates.push_back(result.selected);
      return result;
    }

    result.limitations.push_back(
        QStringLiteral("A manual mapping exists but its local target no longer exists: %1")
            .arg(overrideEntry.localPath));
  }

  QVector<SourceMappingCandidate> candidates;
  const QString referenceBasename = SourceBasename(sourceReference);

  for(const SourceFileEntry &file : index.files)
  {
    const QString absolute = NormalizeSourcePath(file.absolutePath);
    const QString relative = NormalizeSourcePath(file.relativePath);
    SourceMatchKind kind = SourceMatchKind::None;
    Confidence confidence = Confidence::ToVerify;
    double score = 0.0;

    if(absolute == reference)
    {
      kind = SourceMatchKind::ExactPath;
      confidence = Confidence::Confirmed;
      score = 1.0;
    }
    else if(relative == reference || reference.endsWith(QLatin1Char('/') + relative))
    {
      kind = SourceMatchKind::RelativePath;
      confidence = Confidence::Inferred;
      score = 0.98;
    }
    else if(SourceBasename(file.absolutePath) == referenceBasename)
    {
      kind = SourceMatchKind::Filename;
      confidence = Confidence::Inferred;
      score = 0.90;
    }
    else
    {
      const int suffix = CommonSuffixComponents(sourceReference, file.absolutePath);
      if(suffix >= 2)
      {
        kind = SourceMatchKind::Suffix;
        confidence = Confidence::Low;
        score = std::min(0.89, 0.70 + 0.06 * double(suffix));
      }
      else
      {
        const double similarity = SourceFilenameSimilarity(sourceReference, file.absolutePath);
        if(similarity >= index.options.fuzzyThreshold)
        {
          kind = SourceMatchKind::Fuzzy;
          confidence = Confidence::Low;
          score = similarity;
        }
      }
    }

    if(kind != SourceMatchKind::None)
      candidates.push_back(CandidateFor(file, kind, confidence, score));
  }

  std::sort(candidates.begin(), candidates.end(), IsBetterCandidate);
  result.candidates = candidates;
  if(candidates.isEmpty())
  {
    result.limitations.push_back(QStringLiteral("No indexed local source file matched the capture reference."));
  }
  else
  {
    result.selected = candidates[0];
    if(candidates.size() > 1 && SameRank(candidates[0], candidates[1]))
    {
      result.ambiguous = true;
      result.selected.confidence = Confidence::ToVerify;
      result.limitations.push_back(
          QStringLiteral("Multiple local source files have the same best match; choose a manual mapping."));
    }
  }

  if(index.truncated)
    result.limitations.push_back(QStringLiteral("The source index reached its configured file limit."));
  return result;
}

QJsonObject SourceMappingOptionsToJson(const SourceMappingOptions &options)
{
  QJsonObject json;
  json[QStringLiteral("projectRoots")] = StringListToJson(options.projectRoots);
  json[QStringLiteral("includePaths")] = StringListToJson(options.includePaths);
  json[QStringLiteral("extensions")] = StringListToJson(options.extensions);
  json[QStringLiteral("maxFiles")] = options.maxFiles;
  json[QStringLiteral("fuzzyThreshold")] = options.fuzzyThreshold;
  return json;
}

QJsonObject SourceFileEntryToJson(const SourceFileEntry &file)
{
  QJsonObject json;
  json[QStringLiteral("absolutePath")] = file.absolutePath;
  json[QStringLiteral("rootPath")] = file.rootPath;
  json[QStringLiteral("relativePath")] = file.relativePath;
  json[QStringLiteral("fromIncludePath")] = file.fromIncludePath;
  return json;
}

QJsonObject SourceMappingIndexToJson(const SourceMappingIndex &index)
{
  QJsonObject json;
  json[QStringLiteral("options")] = SourceMappingOptionsToJson(index.options);
  QJsonArray files;
  for(const SourceFileEntry &file : index.files)
    files.push_back(SourceFileEntryToJson(file));
  json[QStringLiteral("files")] = files;
  json[QStringLiteral("fileCount")] = index.files.size();
  json[QStringLiteral("skippedRoots")] = StringListToJson(index.skippedRoots);
  json[QStringLiteral("truncated")] = index.truncated;
  return json;
}

QJsonObject SourceMappingCandidateToJson(const SourceMappingCandidate &candidate)
{
  QJsonObject json = SourceFileEntryToJson(candidate.file);
  json[QStringLiteral("matchKind")] = SourceMatchKindName(candidate.kind);
  json[QStringLiteral("confidence")] = ConfidenceName(candidate.confidence);
  json[QStringLiteral("score")] = candidate.score;
  return json;
}

QJsonObject SourceMappingResultToJson(const SourceMappingResult &result)
{
  QJsonObject json;
  json[QStringLiteral("sourceReference")] = result.sourceReference;
  json[QStringLiteral("ambiguous")] = result.ambiguous;
  if(result.selected.kind != SourceMatchKind::None)
    json[QStringLiteral("selected")] = SourceMappingCandidateToJson(result.selected);

  QJsonArray candidates;
  for(const SourceMappingCandidate &candidate : result.candidates)
    candidates.push_back(SourceMappingCandidateToJson(candidate));
  json[QStringLiteral("candidates")] = candidates;
  json[QStringLiteral("limitations")] = StringListToJson(result.limitations);
  return json;
}
}    // namespace MCP
