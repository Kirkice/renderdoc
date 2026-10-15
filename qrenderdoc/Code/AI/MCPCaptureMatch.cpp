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

#include "MCPCaptureMatch.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <algorithm>
#include <limits.h>

namespace MCP
{
//
// ---- Normalization --------------------------------------------------------
//

QJsonObject NormalizationOptionsToJson(const NormalizationOptions &options)
{
  QJsonObject j;
  j[QStringLiteral("caseInsensitive")] = options.caseInsensitive;
  j[QStringLiteral("collapseWhitespace")] = options.collapseWhitespace;
  j[QStringLiteral("stripExtension")] = options.stripExtension;
  j[QStringLiteral("stripPath")] = options.stripPath;
  j[QStringLiteral("stripInstanceSuffix")] = options.stripInstanceSuffix;
  j[QStringLiteral("stripResolutionSuffix")] = options.stripResolutionSuffix;
  j[QStringLiteral("stripNumericSuffix")] = options.stripNumericSuffix;
  j[QStringLiteral("replaceEmptyWithAnon")] = options.replaceEmptyWithAnon;
  return j;
}

// Peel one trailing instance marker: "Albedo (3)", "Albedo[3]" or "Albedo 3".
// Only pure-decimal payloads count, so "(0x1a)" and "(depth)" are left alone.
// Returns the new string and sets `changed` when a marker was removed.
static QString StripInstanceSuffixOnce(const QString &s, bool &changed)
{
  changed = false;

  const int n = s.size();
  if(n < 3)
    return s;

  const QChar last = s.at(n - 1);

  if(last == QLatin1Char(')') || last == QLatin1Char(']'))
  {
    const QChar open = (last == QLatin1Char(')')) ? QLatin1Char('(') : QLatin1Char('[');
    const int at = s.lastIndexOf(open);
    if(at < 0)
      return s;

    const QString inner = s.mid(at + 1, n - at - 2).trimmed();
    if(inner.isEmpty())
      return s;

    for(int i = 0; i < inner.size(); i++)
    {
      if(!inner.at(i).isDigit())
        return s;
    }

    changed = true;
    return s.left(at).trimmed();
  }

  // Trailing whitespace-separated decimal run.
  int i = n;
  while(i > 0 && s.at(i - 1).isDigit())
    i--;

  if(i < n && i > 0 && s.at(i - 1).isSpace())
  {
    changed = true;
    return s.left(i).trimmed();
  }

  return s;
}

// Replace "123x456" / "123x456x78" shaped tokens with a fixed marker. The two
// leading digit runs must each be at least two characters so a hex literal such
// as "0x1a" is not mistaken for a resolution.
static QString StripResolutionTokens(const QString &s)
{
  const int n = s.size();
  QString out;
  out.reserve(n);

  int i = 0;
  while(i < n)
  {
    int j = i;
    while(j < n && s.at(j).isDigit())
      j++;

    if(j - i >= 2 && j < n && (s.at(j) == QLatin1Char('x') || s.at(j) == QLatin1Char('X')))
    {
      int k = j + 1;
      int d2 = k;
      while(d2 < n && s.at(d2).isDigit())
        d2++;

      if(d2 - k >= 2)
      {
        int end = d2;
        if(end < n && (s.at(end) == QLatin1Char('x') || s.at(end) == QLatin1Char('X')))
        {
          int d3 = end + 1;
          int m3 = d3;
          while(m3 < n && s.at(m3).isDigit())
            m3++;

          if(m3 > d3)
            end = m3;
        }

        out += QStringLiteral("res");
        i = end;
        continue;
      }
    }

    out += s.at(i);
    i++;
  }

  return out;
}

// Collapse every maximal decimal run to one '#', so "albedo0"/"albedo1" both
// normalize to "albedo#" while their multiplicity is preserved by the matcher.
static QString CollapseDigitRuns(const QString &s)
{
  QString out;
  out.reserve(s.size());

  bool inDigit = false;
  for(int i = 0; i < s.size(); i++)
  {
    const QChar c = s.at(i);
    if(c.isDigit())
    {
      if(!inDigit)
      {
        out += QLatin1Char('#');
        inDigit = true;
      }
    }
    else
    {
      out += c;
      inDigit = false;
    }
  }

  return out;
}

QString NormalizeName(const QString &name, const NormalizationOptions &options)
{
  QString s = options.collapseWhitespace ? name.simplified() : name.trimmed();

  if(options.stripPath)
  {
    const int slash = qMax(s.lastIndexOf(QLatin1Char('/')), s.lastIndexOf(QLatin1Char('\\')));
    if(slash >= 0)
      s = s.mid(slash + 1);
  }

  if(options.caseInsensitive)
    s = s.toLower();

  if(options.stripInstanceSuffix)
  {
    // Loop so stacked markers ("Albedo (3) (2)") are all removed.
    for(int guard = 0; guard < 8; guard++)
    {
      bool changed = false;
      const QString next = StripInstanceSuffixOnce(s, changed);
      if(!changed)
        break;
      s = next;
    }
  }

  if(options.stripExtension)
  {
    const int dot = s.lastIndexOf(QLatin1Char('.'));
    const int tail = s.size() - dot - 1;
    if(dot > 0 && tail >= 1 && tail <= 5)
    {
      bool alpha = true;
      for(int i = dot + 1; i < s.size(); i++)
      {
        if(!s.at(i).isLetterOrNumber())
        {
          alpha = false;
          break;
        }
      }

      if(alpha)
        s = s.left(dot);
    }
  }

  if(options.stripResolutionSuffix)
    s = StripResolutionTokens(s);

  if(options.stripNumericSuffix)
    s = CollapseDigitRuns(s);

  if(s.isEmpty() && options.replaceEmptyWithAnon)
    s = QStringLiteral("<anon>");

  return s;
}

static QString NamePartOfPassId(const QString &id)
{
  const int at = id.indexOf(QLatin1Char('@'));
  return at > 0 ? id.left(at) : id;
}

QString NormalizePassKey(const PassInfo &pass, const NormalizationOptions &options)
{
  QString name = pass.name;
  if(name.isEmpty())
    name = NamePartOfPassId(pass.id);

  return NormalizeName(name, options);
}

QString NormalizeResourceKey(const ResourceMemoryRow &row, const NormalizationOptions &options)
{
  if(!row.name.isEmpty())
    return NormalizeName(row.name, options);

  // Anonymous target: identify it by kind (and texture sub-kind) so two captures
  // that both leave a resource unnamed still pair it up.
  QString placeholder = row.type;
  if(!row.textureType.isEmpty())
    placeholder += QStringLiteral("/") + row.textureType;

  return NormalizeName(placeholder, options);
}

QString NormalizeResourceKey(const ResourceLifetimeRow &row, const NormalizationOptions &options)
{
  return NormalizeName(row.name.isEmpty() ? row.type : row.name, options);
}

QString NormalizeResourceKey(const UnusedResource &row, const NormalizationOptions &options)
{
  return NormalizeName(row.name.isEmpty() ? row.type : row.name, options);
}

QString NormalizeResourceKey(const LeakCandidate &row, const NormalizationOptions &options)
{
  return NormalizeName(row.name.isEmpty() ? row.type : row.name, options);
}

QString NormalizeEventKey(const QString &eventId, const NormalizationOptions &options)
{
  QString s = options.collapseWhitespace ? eventId.simplified() : eventId.trimmed();
  if(s.isEmpty())
    return s;

  const QStringList parts = s.split(QLatin1Char('-'));
  QStringList out;
  for(int i = 0; i < parts.size(); i++)
  {
    const QString t = parts[i].trimmed();
    if(t.isEmpty())
    {
      out.push_back(t);
      continue;
    }

    bool digits = true;
    for(int c = 0; c < t.size(); c++)
    {
      if(!t.at(c).isDigit())
      {
        digits = false;
        break;
      }
    }

    if(digits)
    {
      // Drop presentation-only leading zeros ("0412" -> "412").
      int start = 0;
      while(start < t.size() - 1 && t.at(start) == QLatin1Char('0'))
        start++;
      out.push_back(t.mid(start));
    }
    else
    {
      out.push_back(NormalizeName(t, options));
    }
  }

  return out.join(QLatin1Char('-'));
}

bool ParseEventRange(const QString &eventId, uint32_t &first, uint32_t &last)
{
  const QString key = NormalizeEventKey(eventId);
  if(key.isEmpty())
    return false;

  const QStringList parts = key.split(QLatin1Char('-'));
  if(parts.isEmpty() || parts.size() > 2)
    return false;

  bool ok = false;
  const uint32_t a = (uint32_t)parts[0].toUInt(&ok);
  if(!ok)
    return false;

  uint32_t b = a;
  if(parts.size() == 2)
  {
    b = (uint32_t)parts[1].toUInt(&ok);
    if(!ok)
      return false;
  }

  first = a;
  last = b;
  return true;
}

QString EventMatchKey(uint32_t eventId, const QString &name, const NormalizationOptions &options)
{
  return NormalizeName(name, options) + QStringLiteral("@") + QString::number(eventId);
}

QString UnifyCaptureKey(const QString &api, const QString &capturePath,
                        const QString &frameDiscriminator, const NormalizationOptions &options)
{
  // The capture's own identity across machines: the API plus the file stem. The
  // full path is deliberately not used, so the same capture copied to a different
  // directory still shares a cache key.
  const QString stem = QFileInfo(capturePath).completeBaseName();

  QString key = NormalizeName(api, options) + QStringLiteral("|") + NormalizeName(stem, options);
  if(!frameDiscriminator.isEmpty())
    key += QStringLiteral("|") + NormalizeName(frameDiscriminator, options);

  return key;
}

//
// ---- Canonical events -----------------------------------------------------
//

QString EventCanonicalKindName(EventCanonicalKind kind)
{
  switch(kind)
  {
    case EventCanonicalKind::RealEffect: return QStringLiteral("realEffect");
    case EventCanonicalKind::Marker: return QStringLiteral("marker");
    case EventCanonicalKind::Other: break;
  }
  return QStringLiteral("other");
}

QJsonObject EventCanonicalKeyToJson(const EventCanonicalKey &key)
{
  QJsonObject j;
  j[QStringLiteral("key")] = key.key;
  j[QStringLiteral("label")] = key.label;
  j[QStringLiteral("shortLabel")] = key.shortLabel;
  if(!key.markerPath.isEmpty())
    j[QStringLiteral("markerPath")] = key.markerPath;
  if(!key.leafName.isEmpty())
    j[QStringLiteral("leafName")] = key.leafName;
  if(!key.rawName.isEmpty())
    j[QStringLiteral("rawName")] = key.rawName;
  j[QStringLiteral("kind")] = EventCanonicalKindName(key.kind);
  j[QStringLiteral("occurrence")] = (int)key.occurrence;
  return j;
}

void BuildEventCanonicalKeys(const rdcarray<EventNode> &nodes,
                             QHash<uint32_t, EventCanonicalKey> &outByEvent,
                             QHash<QString, QVector<uint32_t>> &outByKey,
                             const NormalizationOptions &options)
{
  outByEvent.clear();
  outByKey.clear();

  // id -> index, so marker ancestry can be resolved by walking parentEventId.
  // FlattenEvents() emits parents before children, so one pre-order pass is enough.
  QHash<uint32_t, int> idToIndex;
  idToIndex.reserve((int)nodes.size());
  for(int i = 0; i < (int)nodes.size(); i++)
  {
    const uint32_t id = nodes[i].eventId;
    if(!idToIndex.contains(id))
      idToIndex.insert(id, i);
  }

  QHash<QString, uint32_t> labelCounts;

  for(int i = 0; i < (int)nodes.size(); i++)
  {
    const EventNode &node = nodes[i];

    EventCanonicalKey ck;
    ck.rawName = node.name;

    if(IsRealEffect(node.flags))
      ck.kind = EventCanonicalKind::RealEffect;
    else if(node.childCount > 0)
      ck.kind = EventCanonicalKind::Marker;
    else
      ck.kind = EventCanonicalKind::Other;

    QString leaf = NormalizeName(node.name, options);
    if(leaf.isEmpty() && options.replaceEmptyWithAnon)
      leaf = QStringLiteral("<anon>");
    ck.leafName = leaf;
    ck.shortLabel = leaf;

    // Walk the ancestors root-first to build the marker path.
    QStringList chain;
    QSet<uint32_t> guard;
    uint32_t parent = node.parentEventId;
    while(parent != 0 && idToIndex.contains(parent) && !guard.contains(parent))
    {
      guard.insert(parent);
      const EventNode &pn = nodes[idToIndex.value(parent)];
      const QString nm = NormalizeName(pn.name, options);
      if(!nm.isEmpty())
        chain.push_back(nm);
      parent = pn.parentEventId;
    }
    std::reverse(chain.begin(), chain.end());
    ck.markerPath = chain.join(QStringLiteral(" > "));

    QString label = ck.leafName;
    if(!ck.markerPath.isEmpty())
      label = label.isEmpty() ? ck.markerPath : (ck.markerPath + QStringLiteral(" > ") + label);
    ck.label = label;

    // The occurrence ordinal is what keeps repeated labels ("Shadow pass" twice)
    // and duplicate draws distinct instead of collapsing into one key.
    uint32_t occurrence = 0;
    if(!label.isEmpty())
      occurrence = labelCounts[label]++;
    ck.occurrence = occurrence;

    if(!label.isEmpty())
      ck.key = label + QStringLiteral("#") + QString::number(occurrence);

    outByEvent.insert(node.eventId, ck);
    if(!ck.key.isEmpty())
      outByKey[ck.key].push_back(node.eventId);
  }
}

EventCanonicalKey BuildEventCanonicalKey(const rdcarray<EventNode> &nodes, int nodeIndex,
                                         const NormalizationOptions &options)
{
  if(nodeIndex < 0 || nodeIndex >= (int)nodes.size())
    return EventCanonicalKey();

  QHash<uint32_t, EventCanonicalKey> byEvent;
  QHash<QString, QVector<uint32_t>> byKey;
  BuildEventCanonicalKeys(nodes, byEvent, byKey, options);

  const uint32_t id = nodes[nodeIndex].eventId;
  return byEvent.value(id);
}

//
// ---- Canonical resources and passes ---------------------------------------
//

QJsonObject ResourceCanonicalKeyToJson(const ResourceCanonicalKey &key)
{
  QJsonObject j;
  j[QStringLiteral("key")] = key.key;
  j[QStringLiteral("displayName")] = key.displayName;
  j[QStringLiteral("type")] = key.type;
  j[QStringLiteral("byteSize")] = QString::number(key.byteSize);
  j[QStringLiteral("index")] = key.index;
  j[QStringLiteral("confidence")] = ConfidenceName(key.confidence);
  return j;
}

QJsonObject PassCanonicalKeyToJson(const PassCanonicalKey &key)
{
  QJsonObject j;
  j[QStringLiteral("key")] = key.key;
  if(!key.id.isEmpty())
    j[QStringLiteral("id")] = key.id;
  j[QStringLiteral("displayName")] = key.displayName;
  j[QStringLiteral("firstEventId")] = (int)key.firstEventId;
  j[QStringLiteral("lastEventId")] = (int)key.lastEventId;
  j[QStringLiteral("index")] = key.index;
  return j;
}

QVector<ResourceCanonicalKey> NormalizeResourceRows(const QVector<ResourceMemoryRow> &rows,
                                                    const NormalizationOptions &options)
{
  QVector<ResourceCanonicalKey> out;
  out.reserve(rows.size());

  for(int i = 0; i < rows.size(); i++)
  {
    const ResourceMemoryRow &row = rows[i];

    ResourceCanonicalKey ck;
    ck.key = NormalizeResourceKey(row, options);
    ck.displayName = row.name.isEmpty() ? row.type : row.name;
    ck.type = row.type;
    ck.byteSize = row.byteSize;
    ck.index = i;
    // A named resource's identity is a fact; an anonymous one is identified by
    // kind alone, which is a heuristic the report must state.
    ck.confidence = row.name.isEmpty() ? Confidence::Inferred : Confidence::Confirmed;

    out.push_back(ck);
  }

  return out;
}

QVector<PassCanonicalKey> NormalizePassRows(const QVector<PassInfo> &passes,
                                            const NormalizationOptions &options)
{
  QVector<PassCanonicalKey> out;
  out.reserve(passes.size());

  for(int i = 0; i < passes.size(); i++)
  {
    const PassInfo &pass = passes[i];

    PassCanonicalKey ck;
    ck.key = NormalizePassKey(pass, options);
    ck.id = pass.id;
    ck.displayName = pass.name.isEmpty() ? pass.id : pass.name;
    ck.firstEventId = pass.eventIdStart;
    ck.lastEventId = pass.eventIdEnd;
    ck.index = i;

    out.push_back(ck);
  }

  return out;
}

//
// ---- Canonical capture (the P2-2 input) -----------------------------------
//

void BuildCanonicalEvents(const rdcarray<EventNode> &nodes, CanonicalCapture &out)
{
  BuildEventCanonicalKeys(nodes, out.events, out.eventsByKey, out.options);
}

void BuildCanonicalResources(const QVector<ResourceMemoryRow> &rows, CanonicalCapture &out)
{
  out.resources = NormalizeResourceRows(rows, out.options);
}

void BuildCanonicalPasses(const QVector<PassInfo> &passes, CanonicalCapture &out)
{
  out.passes = NormalizePassRows(passes, out.options);
}

QJsonObject CanonicalCaptureToJson(const CanonicalCapture &capture)
{
  QJsonObject j;
  j[QStringLiteral("unifyKey")] = capture.unifyKey;
  j[QStringLiteral("capturePath")] = capture.capturePath;
  j[QStringLiteral("api")] = capture.api;
  j[QStringLiteral("frameNumber")] = (int)capture.frameNumber;
  j[QStringLiteral("eventCount")] = capture.events.size();
  j[QStringLiteral("distinctEventKeys")] = capture.eventsByKey.size();
  j[QStringLiteral("resourceCount")] = capture.resources.size();
  j[QStringLiteral("passCount")] = capture.passes.size();
  j[QStringLiteral("normalization")] = NormalizationOptionsToJson(capture.options);
  j[QStringLiteral("confidence")] = ConfidenceName(capture.confidence);
  if(!capture.limitation.isEmpty())
    j[QStringLiteral("limitation")] = capture.limitation;
  return j;
}

//
// ---- Matching -------------------------------------------------------------
//

QString CaptureMatchConfidenceName(CaptureMatchConfidence c)
{
  switch(c)
  {
    case CaptureMatchConfidence::Exact: return QStringLiteral("exact");
    case CaptureMatchConfidence::Normalized: return QStringLiteral("normalized");
    case CaptureMatchConfidence::Fuzzy: return QStringLiteral("fuzzy");
    case CaptureMatchConfidence::Positional: return QStringLiteral("positional");
    case CaptureMatchConfidence::None: break;
  }
  return QStringLiteral("none");
}

QString CaptureMatchPolicy::Describe() const
{
  QStringList active;
  if(normalization.caseInsensitive)
    active.push_back(QStringLiteral("case-fold"));
  if(normalization.collapseWhitespace)
    active.push_back(QStringLiteral("collapse whitespace"));
  if(normalization.stripPath)
    active.push_back(QStringLiteral("strip path"));
  if(normalization.stripExtension)
    active.push_back(QStringLiteral("strip extension"));
  if(normalization.stripInstanceSuffix)
    active.push_back(QStringLiteral("strip instance suffix"));
  if(normalization.stripResolutionSuffix)
    active.push_back(QStringLiteral("collapse resolution"));
  if(normalization.stripNumericSuffix)
    active.push_back(QStringLiteral("collapse digit runs"));

  QStringList rules;
  rules.push_back(QStringLiteral("Normalization: %1")
                      .arg(active.isEmpty() ? QStringLiteral("none")
                                            : active.join(QStringLiteral(", "))));
  rules.push_back(QStringLiteral("Fuzzy threshold: %1")
                      .arg(QString::number(fuzzyThreshold, 'f', 2)));
  rules.push_back(QStringLiteral("Positional fallback: %1")
                      .arg(allowPositionalFallback ? QStringLiteral("on") : QStringLiteral("off")));
  rules.push_back(QStringLiteral("Nearest-event tie-break: %1")
                      .arg(preferNearestEvent ? QStringLiteral("on") : QStringLiteral("off")));

  return rules.join(QStringLiteral("; "));
}

QJsonObject CaptureMatchPolicy::ToJson() const
{
  QJsonObject j;
  j[QStringLiteral("normalization")] = NormalizationOptionsToJson(normalization);
  j[QStringLiteral("fuzzyThreshold")] = fuzzyThreshold;
  j[QStringLiteral("allowPositionalFallback")] = allowPositionalFallback;
  j[QStringLiteral("preferNearestEvent")] = preferNearestEvent;
  j[QStringLiteral("description")] = Describe();
  return j;
}

QJsonObject CaptureMatchPairToJson(const CaptureMatchPair &pair)
{
  QJsonObject j;
  j[QStringLiteral("key")] = pair.key;
  j[QStringLiteral("matched")] = pair.matched;
  j[QStringLiteral("confidence")] = CaptureMatchConfidenceName(pair.confidence);
  j[QStringLiteral("indexA")] = pair.indexA;
  j[QStringLiteral("indexB")] = pair.indexB;
  if(!pair.displayA.isEmpty())
    j[QStringLiteral("displayA")] = pair.displayA;
  if(!pair.displayB.isEmpty())
    j[QStringLiteral("displayB")] = pair.displayB;
  j[QStringLiteral("score")] = pair.score;
  if(!pair.note.isEmpty())
    j[QStringLiteral("note")] = pair.note;
  return j;
}

QJsonObject CaptureMatchResultToJson(const CaptureMatchResult &result)
{
  QJsonObject j;
  j[QStringLiteral("countA")] = result.countA;
  j[QStringLiteral("countB")] = result.countB;
  j[QStringLiteral("matched")] = result.matchedCount();
  j[QStringLiteral("exact")] = result.exactCount;
  j[QStringLiteral("normalized")] = result.normalizedCount;
  j[QStringLiteral("fuzzy")] = result.fuzzyCount;
  j[QStringLiteral("positional")] = result.positionalCount;
  j[QStringLiteral("unmatchedA")] = result.unmatchedA;
  j[QStringLiteral("unmatchedB")] = result.unmatchedB;
  j[QStringLiteral("coverage")] = result.coverage;
  j[QStringLiteral("policy")] = result.policy.ToJson();

  QJsonArray pairs;
  for(const CaptureMatchPair &p : result.pairs)
    pairs.append(CaptureMatchPairToJson(p));
  j[QStringLiteral("pairs")] = pairs;

  return j;
}

// Jaro-Winkler similarity of two canonical strings, in [0,1]. Jaro gives the
// match/transposition ratio; the Winkler prefix bonus rewards shared starts,
// which is what lets "basepass" pair with "gbufferpass" ahead of an unrelated
// name of the same length.
double NameSimilarity(const QString &a, const QString &b)
{
  const int la = a.size();
  const int lb = b.size();

  if(la == 0 && lb == 0)
    return 1.0;
  if(la == 0 || lb == 0)
    return 0.0;
  if(a == b)
    return 1.0;

  int win = qMax(la, lb) / 2 - 1;
  if(win < 0)
    win = 0;

  QVector<bool> matchedA(la, false);
  QVector<bool> matchedB(lb, false);

  int matches = 0;
  for(int i = 0; i < la; i++)
  {
    const int lo = qMax(0, i - win);
    const int hi = qMin(lb - 1, i + win);
    for(int j = lo; j <= hi; j++)
    {
      if(matchedB[j] || a.at(i) != b.at(j))
        continue;
      matchedA[i] = true;
      matchedB[j] = true;
      matches++;
      break;
    }
  }

  if(matches == 0)
    return 0.0;

  int transpositions = 0;
  int k = 0;
  for(int i = 0; i < la; i++)
  {
    if(!matchedA[i])
      continue;
    while(!matchedB[k])
      k++;
    if(a.at(i) != b.at(k))
      transpositions++;
    k++;
  }

  const double m = (double)matches;
  const double jaro =
      (m / (double)la + m / (double)lb + (m - (double)transpositions / 2.0) / m) / 3.0;

  int prefix = 0;
  const int maxPrefix = qMin(4, qMin(la, lb));
  while(prefix < maxPrefix && a.at(prefix) == b.at(prefix))
    prefix++;

  return jaro + (double)prefix * 0.1 * (1.0 - jaro);
}

// The string the fuzzy pass compares: the canonical key, or - when a caller
// supplied no key - the normalized display name.
static QString FuzzyText(const CaptureMatchEntry &e, const NormalizationOptions &options)
{
  if(!e.key.isEmpty())
    return e.key;
  return NormalizeName(e.displayName, options);
}

CaptureMatchResult MatchEntries(const QVector<CaptureMatchEntry> &baseline,
                                const QVector<CaptureMatchEntry> &candidate,
                                const CaptureMatchPolicy &policy)
{
  CaptureMatchResult result;
  result.policy = policy;
  result.countA = baseline.size();
  result.countB = candidate.size();

  const int n = baseline.size();
  const int m = candidate.size();

  QVector<bool> usedA(n, false);
  QVector<bool> usedB(m, false);

  // ---- Pass 1: identical canonical key (exact or normalized) --------------
  QHash<QString, QVector<int>> byBKey;
  for(int j = 0; j < m; j++)
    byBKey[candidate[j].key].push_back(j);

  for(int i = 0; i < n; i++)
  {
    if(usedA[i] || baseline[i].key.isEmpty())
      continue;

    const QVector<int> &cands = byBKey[baseline[i].key];

    int chosen = -1;
    if(policy.preferNearestEvent)
    {
      qint64 bestDiff = -1;
      for(int c = 0; c < cands.size(); c++)
      {
        const int j = cands[c];
        if(usedB[j])
          continue;
        const qint64 diff = qAbs(qint64(baseline[i].position) - qint64(candidate[j].position));
        if(bestDiff < 0 || diff < bestDiff)
        {
          bestDiff = diff;
          chosen = j;
        }
      }
    }
    else
    {
      for(int c = 0; c < cands.size(); c++)
      {
        if(!usedB[cands[c]])
        {
          chosen = cands[c];
          break;
        }
      }
    }

    if(chosen < 0)
      continue;

    usedA[i] = true;
    usedB[chosen] = true;

    CaptureMatchPair p;
    p.key = baseline[i].key;
    p.matched = true;
    p.indexA = i;
    p.indexB = chosen;
    p.displayA = baseline[i].displayName;
    p.displayB = candidate[chosen].displayName;
    p.score = 1.0;

    if(p.displayA == p.displayB)
    {
      p.confidence = CaptureMatchConfidence::Exact;
      result.exactCount++;
    }
    else
    {
      p.confidence = CaptureMatchConfidence::Normalized;
      result.normalizedCount++;
      p.note = QStringLiteral("names differ but normalized key '%1' agreed")
                   .arg(baseline[i].key);
    }

    result.pairs.push_back(p);
  }

  // ---- Pass 2: fuzzy, greedy by descending similarity ---------------------
  if(policy.fuzzyThreshold < 1.0)
  {
    struct FuzzyCandidate
    {
      double score;
      int i;
      int j;
    };

    QVector<FuzzyCandidate> fuzzy;
    for(int i = 0; i < n; i++)
    {
      if(usedA[i])
        continue;

      const QString sa = FuzzyText(baseline[i], policy.normalization);
      if(sa.isEmpty())
        continue;

      for(int j = 0; j < m; j++)
      {
        if(usedB[j])
          continue;

        const QString sb = FuzzyText(candidate[j], policy.normalization);
        if(sb.isEmpty())
          continue;

        const double score = NameSimilarity(sa, sb);
        if(score >= policy.fuzzyThreshold)
        {
          FuzzyCandidate fc;
          fc.score = score;
          fc.i = i;
          fc.j = j;
          fuzzy.push_back(fc);
        }
      }
    }

    std::sort(fuzzy.begin(), fuzzy.end(), [](const FuzzyCandidate &x, const FuzzyCandidate &y) {
      if(x.score != y.score)
        return x.score > y.score;
      if(x.i != y.i)
        return x.i < y.i;
      return x.j < y.j;
    });

    for(int c = 0; c < fuzzy.size(); c++)
    {
      const int i = fuzzy[c].i;
      const int j = fuzzy[c].j;
      if(usedA[i] || usedB[j])
        continue;

      usedA[i] = true;
      usedB[j] = true;

      CaptureMatchPair p;
      p.key = baseline[i].key;
      p.matched = true;
      p.indexA = i;
      p.indexB = j;
      p.displayA = baseline[i].displayName;
      p.displayB = candidate[j].displayName;
      p.score = fuzzy[c].score;
      p.confidence = CaptureMatchConfidence::Fuzzy;
      p.note = QStringLiteral("fuzzy similarity %1 (threshold %2)")
                   .arg(QString::number(fuzzy[c].score, 'f', 3))
                   .arg(QString::number(policy.fuzzyThreshold, 'f', 2));
      result.fuzzyCount++;
      result.pairs.push_back(p);
    }
  }

  // ---- Pass 3: ordinal fallback when the leftovers line up ----------------
  QVector<int> leftA;
  QVector<int> leftB;
  for(int i = 0; i < n; i++)
  {
    if(!usedA[i])
      leftA.push_back(i);
  }
  for(int j = 0; j < m; j++)
  {
    if(!usedB[j])
      leftB.push_back(j);
  }

  if(policy.allowPositionalFallback && !leftA.isEmpty() && leftA.size() == leftB.size())
  {
    for(int c = 0; c < leftA.size(); c++)
    {
      const int i = leftA[c];
      const int j = leftB[c];
      usedA[i] = true;
      usedB[j] = true;

      CaptureMatchPair p;
      p.key = baseline[i].key;
      p.matched = true;
      p.indexA = i;
      p.indexB = j;
      p.displayA = baseline[i].displayName;
      p.displayB = candidate[j].displayName;
      p.score = NameSimilarity(FuzzyText(baseline[i], policy.normalization),
                               FuzzyText(candidate[j], policy.normalization));
      p.confidence = CaptureMatchConfidence::Positional;
      p.note = QStringLiteral("ordinal pairing of equal leftovers (%1 pairs) - a hypothesis")
                   .arg(leftA.size());
      result.positionalCount++;
      result.pairs.push_back(p);
    }

    leftA.clear();
    leftB.clear();
  }

  // ---- Pass 4: whatever is left exists on one side only -------------------
  for(int i = 0; i < n; i++)
  {
    if(usedA[i])
      continue;

    CaptureMatchPair p;
    p.key = baseline[i].key;
    p.matched = false;
    p.indexA = i;
    p.indexB = -1;
    p.displayA = baseline[i].displayName;
    p.confidence = CaptureMatchConfidence::None;
    p.note = QStringLiteral("present in the baseline only");
    result.unmatchedA++;
    result.pairs.push_back(p);
  }

  for(int j = 0; j < m; j++)
  {
    if(usedB[j])
      continue;

    CaptureMatchPair p;
    p.key = candidate[j].key;
    p.matched = false;
    p.indexA = -1;
    p.indexB = j;
    p.displayB = candidate[j].displayName;
    p.confidence = CaptureMatchConfidence::None;
    p.note = QStringLiteral("present in the candidate only");
    result.unmatchedB++;
    result.pairs.push_back(p);
  }

  // Deterministic output order: by baseline index, then candidate index, with
  // the candidate-only pairs last.
  std::sort(result.pairs.begin(), result.pairs.end(),
            [](const CaptureMatchPair &x, const CaptureMatchPair &y) {
              const int ax = x.indexA < 0 ? INT_MAX : x.indexA;
              const int ay = y.indexA < 0 ? INT_MAX : y.indexA;
              if(ax != ay)
                return ax < ay;
              const int bx = x.indexB < 0 ? INT_MAX : x.indexB;
              const int by = y.indexB < 0 ? INT_MAX : y.indexB;
              return bx < by;
            });

  const int denom = qMax(n, m);
  result.coverage = denom > 0 ? (double)result.matchedCount() / (double)denom : 0.0;

  return result;
}

//
// ---- Entry builders and convenience wrappers ------------------------------
//

QVector<CaptureMatchEntry> EventMatchEntries(const rdcarray<EventNode> &nodes,
                                             const QHash<uint32_t, EventCanonicalKey> &events)
{
  QVector<CaptureMatchEntry> out;
  out.reserve((int)nodes.size());

  for(int i = 0; i < (int)nodes.size(); i++)
  {
    const EventNode &node = nodes[i];
    const EventCanonicalKey ck = events.value(node.eventId);

    CaptureMatchEntry e;
    e.key = ck.key;
    e.displayName = ck.rawName.isEmpty() ? ck.label : ck.rawName;
    e.index = i;
    // Event ids ascend with the pre-order walk, so "nearest position" is a
    // meaningful tie-break for a frame whose passes were re-ordered.
    e.position = node.eventId;

    out.push_back(e);
  }

  return out;
}

QVector<CaptureMatchEntry> ResourceMatchEntries(const QVector<ResourceCanonicalKey> &rows)
{
  QVector<CaptureMatchEntry> out;
  out.reserve(rows.size());

  for(int i = 0; i < rows.size(); i++)
  {
    CaptureMatchEntry e;
    e.key = rows[i].key;
    e.displayName = rows[i].displayName;
    e.index = rows[i].index;
    e.position = (uint32_t)i;
    out.push_back(e);
  }

  return out;
}

QVector<CaptureMatchEntry> PassMatchEntries(const QVector<PassCanonicalKey> &rows)
{
  QVector<CaptureMatchEntry> out;
  out.reserve(rows.size());

  for(int i = 0; i < rows.size(); i++)
  {
    CaptureMatchEntry e;
    e.key = rows[i].key;
    e.displayName = rows[i].displayName;
    e.index = rows[i].index;
    e.position = rows[i].firstEventId;
    out.push_back(e);
  }

  return out;
}

CaptureMatchResult MatchEvents(const rdcarray<EventNode> &nodesA,
                               const QHash<uint32_t, EventCanonicalKey> &eventsA,
                               const rdcarray<EventNode> &nodesB,
                               const QHash<uint32_t, EventCanonicalKey> &eventsB,
                               const CaptureMatchPolicy &policy)
{
  return MatchEntries(EventMatchEntries(nodesA, eventsA), EventMatchEntries(nodesB, eventsB),
                      policy);
}

CaptureMatchResult MatchResources(const QVector<ResourceCanonicalKey> &baseline,
                                  const QVector<ResourceCanonicalKey> &candidate,
                                  const CaptureMatchPolicy &policy)
{
  return MatchEntries(ResourceMatchEntries(baseline), ResourceMatchEntries(candidate), policy);
}

CaptureMatchResult MatchPasses(const QVector<PassCanonicalKey> &baseline,
                               const QVector<PassCanonicalKey> &candidate,
                               const CaptureMatchPolicy &policy)
{
  return MatchEntries(PassMatchEntries(baseline), PassMatchEntries(candidate), policy);
}

}    // namespace MCP
