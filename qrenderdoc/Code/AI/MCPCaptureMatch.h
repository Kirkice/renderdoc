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

// P2-1 cross-capture normalization and matching policy
// ----------------------------------------------------
// Comparing two captures is not a matter of diffing two JSON blobs. The same
// logical frame usually has different event ids, resource names pick up
// per-instance suffixes ("Albedo (3)") and resolution tokens ("4096x4096"),
// duplicate resources exist (swapchain images, per-frame constant buffers) and
// passes get re-ordered by the driver or a code change. P2-2 therefore needs a
// *stable identity* for every event / resource / pass that survives those
// differences, and an explicit, reviewable policy that records how each pairing
// was decided and how much it may be trusted.
//
// This header defines that vocabulary. Like MCPAnalysisModel.h it is pure data
// plus pure functions: no Qt widgets, no CaptureContext, no IReplayController.
// The pipeline is:
//
//   raw name --NormalizeName()--> canonical name
//            --BuildEventCanonicalKey() / NormalizePassKey() / ...--> key
//            --MatchEntries()--> CaptureMatchPair + CaptureMatchConfidence
//
// The confidence is the honest signal the regression report is built on:
//
//   * Exact      - the two display names were byte-identical;
//   * Normalized - the normalized keys agreed but the display names differed,
//                  so the pairing stands or falls with the normalization rules;
//   * Fuzzy      - the keys differed but the names were similar enough
//                  (Jaro-Winkler similarity >= policy.fuzzyThreshold);
//   * Positional - nothing but the ordinal agreed: a hypothesis, not a fact;
//   * None       - one-sided, i.e. present in one capture only.
//
// Every normalization rule is opt-in through NormalizationOptions and the
// effective policy is echoed in the result JSON, so a regression diff can always
// be explained after the fact. ResourceIds are deliberately NOT used as
// identities: they are creation-order handles and carry no meaning across
// captures, which is exactly the instability this layer exists to absorb.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <stdint.h>

// Pulls in EventNode / PassInfo / ResourceMemoryRow / Confidence / IsRealEffect
// and, through it, rdcarray and the replay type surface.
#include "MCPAnalysisModel.h"

namespace MCP
{
//
// ---- Normalization --------------------------------------------------------
//

// The rule set applied before two names are compared. Every rule is a toggle so
// a caller can trade recall for precision and, more importantly, so the tests
// can pin each rule down in isolation.
struct NormalizationOptions
{
  // Case-fold the canonical name so "GBuffer" and "gbuffer" agree.
  bool caseInsensitive = true;

  // Trim and collapse interior whitespace runs to a single space.
  bool collapseWhitespace = true;

  // Drop a trailing extension-looking token (".png", ".bin", ".dds", 1-5 chars).
  bool stripExtension = true;

  // Reduce "Shaders/Opaque.hlsl" to its last segment so the same asset referenced
  // with a different directory prefix (or a different path separator) matches.
  bool stripPath = true;

  // Peel trailing instance suffixes: "Albedo (12)", "Albedo[3]", "Albedo 12".
  bool stripInstanceSuffix = true;

  // Replace a "WxH" / "WxHxD" token with a fixed marker. The shape of a target is
  // a property, not part of its identity: a resolution change should surface as a
  // change on one matched resource, not as two unrelated resources.
  bool stripResolutionSuffix = true;

  // Collapse every run of digits to a single marker. This absorbs swapchain
  // indices and dynamic-resource indices ("Albedo0"/"Albedo1" -> "albedo#"), which
  // is the "duplicate resource" case: the multiplicity is preserved because the
  // matcher pairs same-key entries as ordered lists, never merging them.
  bool stripNumericSuffix = true;

  // An empty canonical name is a valid result ("it normalized away"). This flag
  // only controls whether an empty name is re-labelled "<anon>" (true) or kept
  // empty (false, which makes the entry unmatchable except positionally).
  bool replaceEmptyWithAnon = true;

  // Number of path segments kept when stripPath is false - unused when stripPath
  // is set, kept so a policy can be described completely.
  int contextPrefixSegments = 2;
};

QJsonObject NormalizationOptionsToJson(const NormalizationOptions &options);

// Normalize one name into a cross-capture canonical form.
QString NormalizeName(const QString &name,
                      const NormalizationOptions &options = NormalizationOptions());

// Normalize a pass identity. Uses pass.name and falls back to the name part of
// pass.id (BuildPassId format "<name>@<start>-<end>") when the name is empty.
QString NormalizePassKey(const PassInfo &pass,
                         const NormalizationOptions &options = NormalizationOptions());

// Normalize a resource identity. Uses the resource name when there is one and a
// type/shape placeholder otherwise, so anonymous targets still pair up by kind
// (the matcher resolves the multiplicity by ordinal).
QString NormalizeResourceKey(const ResourceMemoryRow &row,
                             const NormalizationOptions &options = NormalizationOptions());
QString NormalizeResourceKey(const ResourceLifetimeRow &row,
                             const NormalizationOptions &options = NormalizationOptions());
QString NormalizeResourceKey(const UnusedResource &row,
                             const NormalizationOptions &options = NormalizationOptions());
QString NormalizeResourceKey(const LeakCandidate &row,
                             const NormalizationOptions &options = NormalizationOptions());

// Normalize a bookmark/event id ("412", "0412", "410-430"). Event ids are only
// comparable *within* one capture, so this only removes presentation noise
// (leading zeros, whitespace); the event matcher keys on the marker path plus
// the leaf name instead - see BuildEventCanonicalKey.
QString NormalizeEventKey(const QString &eventId,
                          const NormalizationOptions &options = NormalizationOptions());

// Parse a normalized event id / range into its endpoints. Returns false when the
// string is not a plain id or "a-b" range; `first`/`last` are then left at 0.
bool ParseEventRange(const QString &eventId, uint32_t &first, uint32_t &last);

// A single combined identity for an event: normalized name plus the event id.
// This is distinct from the *cross-capture* canonical key below and is used when
// the two captures are known to be the same capture (order-stable comparisons).
QString EventMatchKey(uint32_t eventId, const QString &name,
                      const NormalizationOptions &options = NormalizationOptions());

// A single identity for a whole capture: the graphics API plus the capture file
// stem, optionally disambiguated by a caller-supplied discriminator (frame
// number, build id, ...). Used as the cache key by P2-2 and as the header of the
// report so a diff always names both sides.
QString UnifyCaptureKey(const QString &api, const QString &capturePath,
                        const QString &frameDiscriminator = QString(),
                        const NormalizationOptions &options = NormalizationOptions());

//
// ---- Canonical events -----------------------------------------------------
//

// What kind of node a canonical key describes: the flags read straight from the
// action tree, so a kind is always a fact.
enum class EventCanonicalKind
{
  RealEffect,   // draw / dispatch / mesh dispatch / clear
  Marker,       // has children but is not itself an effect
  Other,        // anything else (present, copy, resolve, a bare container)
};

QString EventCanonicalKindName(EventCanonicalKind kind);

// The normalized identity of one event. `markerPath` is the chain of enclosing
// marker names (root first), `leafName` the event's own normalized name and
// `occurrence` its ordinal among events that share the same label - which is how
// repeated marker names ("Shadow pass" twice) and duplicate draws stay distinct
// instead of collapsing into one key.
struct EventCanonicalKey
{
  QString key;          // the stable identity; empty when nothing could be normalized
  QString label;        // "Marker > Sub > DrawIndexed#", or the leaf name when root
  QString shortLabel;   // the leaf name only (the plugin's per-event dedup label)
  QString markerPath;   // " > "-joined normalized marker names, empty for a root
  QString leafName;     // normalized name of the event itself
  QString rawName;      // the un-normalized display name, for reports
  EventCanonicalKind kind = EventCanonicalKind::Other;
  uint32_t occurrence = 0;   // ordinal among same-label events, 0-based
};

QJsonObject EventCanonicalKeyToJson(const EventCanonicalKey &key);

// Build canonical keys for a whole capture in one pre-order pass.
//
// `nodes` must be the pre-order list produced by FlattenEvents(), because marker
// ancestry is resolved by walking the parentEventId chain. `outByEvent` maps an
// event id to its key; `outByKey` maps a canonical key to every event id that
// carries it (so P2-2 can see how many partners a key has before pairing).
void BuildEventCanonicalKeys(const rdcarray<EventNode> &nodes,
                             QHash<uint32_t, EventCanonicalKey> &outByEvent,
                             QHash<QString, QVector<uint32_t>> &outByKey,
                             const NormalizationOptions &options = NormalizationOptions());

// Convenience single-event overload. Builds a temporary id->index map, so it is
// meant for tests and spot checks; use BuildEventCanonicalKeys for whole frames.
EventCanonicalKey BuildEventCanonicalKey(const rdcarray<EventNode> &nodes, int nodeIndex,
                                         const NormalizationOptions &options = NormalizationOptions());

//
// ---- Canonical resources and passes ---------------------------------------
//

// A normalized resource identity plus the back-pointer and weight a matcher
// needs. `index` indexes the source vector the key was derived from.
struct ResourceCanonicalKey
{
  QString key;
  QString displayName;   // original name, else the type
  QString type;          // ResourceTypeName()
  uint64_t byteSize = 0;   // creation-time size, 0 when unknown
  int index = -1;
  Confidence confidence = Confidence::Confirmed;   // Inferred for anonymous entries
};

struct PassCanonicalKey
{
  QString key;
  QString id;
  QString displayName;      // pass.name, else the id
  uint32_t firstEventId = 0;
  uint32_t lastEventId = 0;
  int index = -1;
};

QVector<ResourceCanonicalKey> NormalizeResourceRows(const QVector<ResourceMemoryRow> &rows,
                                                    const NormalizationOptions &options =
                                                        NormalizationOptions());
QVector<PassCanonicalKey> NormalizePassRows(const QVector<PassInfo> &passes,
                                            const NormalizationOptions &options =
                                                NormalizationOptions());

QJsonObject ResourceCanonicalKeyToJson(const ResourceCanonicalKey &key);
QJsonObject PassCanonicalKeyToJson(const PassCanonicalKey &key);

//
// ---- Canonical capture (the P2-2 input) -----------------------------------
//

// Everything P2-2 needs to compare two captures without re-reading either one:
// the identity of the capture itself plus the normalized event/resource/pass
// tables. Storing this is the "capture snapshot cache" the regression tool reads
// back, so it is deliberately plain data that serialises to JSON.
struct CanonicalCapture
{
  QString unifyKey;
  QString capturePath;
  QString api;
  uint32_t frameNumber = 0;
  NormalizationOptions options;

  QHash<uint32_t, EventCanonicalKey> events;          // by event id
  QHash<QString, QVector<uint32_t>> eventsByKey;      // key -> event ids
  QVector<ResourceCanonicalKey> resources;
  QVector<PassCanonicalKey> passes;

  Confidence confidence = Confidence::Confirmed;
  QString limitation;
};

void BuildCanonicalEvents(const rdcarray<EventNode> &nodes, CanonicalCapture &out);
void BuildCanonicalResources(const QVector<ResourceMemoryRow> &rows, CanonicalCapture &out);
void BuildCanonicalPasses(const QVector<PassInfo> &passes, CanonicalCapture &out);

// A compact summary (counts + unify key), not the full tables: the full tables
// can be thousands of entries and belong in the cached snapshot, not in a
// tool's default response.
QJsonObject CanonicalCaptureToJson(const CanonicalCapture &capture);

//
// ---- Matching -------------------------------------------------------------
//

// How a pairing was decided. Ordered from most to least trustworthy.
enum class CaptureMatchConfidence
{
  Exact,
  Normalized,
  Fuzzy,
  Positional,
  None,
};

QString CaptureMatchConfidenceName(CaptureMatchConfidence c);

// The P2-1 matching policy in one place, so a client (or the P2-2 cache) can
// report exactly which rules produced a pairing.
struct CaptureMatchPolicy
{
  NormalizationOptions normalization;

  // Jaro-Winkler floor for the fuzzy pass. 1.0 disables fuzzy matching; the
  // default is deliberately conservative so a fuzzy pair is always reported as
  // such rather than being silently treated as a match.
  double fuzzyThreshold = 0.82;

  // Pair the remaining entries by ordinal when both sides have the same number
  // left. This is what makes a renamed pass ("GBuffer" -> "BasePass") still line
  // up, at the cost of the pairing being a hypothesis.
  bool allowPositionalFallback = true;

  // When several entries share a key, pair the closest pair by event id rather
  // than the first available one, so a re-ordered frame keeps its partners.
  bool preferNearestEvent = true;

  // Human readable description of the effective rules, for the report's
  // limitations section.
  QString Describe() const;

  QJsonObject ToJson() const;
};

// One comparable item handed to the matcher. Only `key` is used for the
// keyed/fuzzy passes; `position` breaks ties and orders the positional fallback;
// `index` is the caller's back-pointer into its own vector.
struct CaptureMatchEntry
{
  QString key;
  QString displayName;
  int index = -1;
  uint32_t position = 0;
};

// One pairing between the baseline (A) and the candidate (B). `matched` false
// means the entry exists on one side only; `indexA`/`indexB` is then -1 for the
// missing side.
struct CaptureMatchPair
{
  QString key;
  bool matched = false;
  CaptureMatchConfidence confidence = CaptureMatchConfidence::None;
  int indexA = -1;
  int indexB = -1;
  QString displayA;
  QString displayB;
  double score = 0.0;   // 1.0 for exact/normalized, the similarity for fuzzy
  QString note;         // why this pairing was made, for the report
};

// A whole match run plus the counters the regression report needs. `coverage` is
// matched / max(countA, countB), i.e. how much of the larger frame is accounted
// for; it is 0 when both sides are empty.
struct CaptureMatchResult
{
  int countA = 0;
  int countB = 0;
  QVector<CaptureMatchPair> pairs;
  int exactCount = 0;
  int normalizedCount = 0;
  int fuzzyCount = 0;
  int positionalCount = 0;
  int unmatchedA = 0;   // present in the baseline only
  int unmatchedB = 0;   // present in the candidate only
  double coverage = 0.0;
  CaptureMatchPolicy policy;

  // Sum of the four confidence counters: the number of pairings made.
  int matchedCount() const
  {
    return exactCount + normalizedCount + fuzzyCount + positionalCount;
  }
};

QJsonObject CaptureMatchPairToJson(const CaptureMatchPair &pair);
QJsonObject CaptureMatchResultToJson(const CaptureMatchResult &result);

// The generic matcher. Runs four passes, each only touching what the previous
// one left behind:
//   1. exact/normalized - same canonical key, paired nearest-position first;
//   2. fuzzy            - similarity >= threshold, greedily by descending score;
//   3. positional       - only when the leftover counts are equal;
//   4. one-sided        - everything still unmatched is reported as such.
CaptureMatchResult MatchEntries(const QVector<CaptureMatchEntry> &baseline,
                                const QVector<CaptureMatchEntry> &candidate,
                                const CaptureMatchPolicy &policy = CaptureMatchPolicy());

// Entry builders, so callers do not hand-assemble vectors. The event builder
// walks `nodes` in pre-order, which keeps the result deterministic.
QVector<CaptureMatchEntry> EventMatchEntries(const rdcarray<EventNode> &nodes,
                                             const QHash<uint32_t, EventCanonicalKey> &events);
QVector<CaptureMatchEntry> ResourceMatchEntries(const QVector<ResourceCanonicalKey> &rows);
QVector<CaptureMatchEntry> PassMatchEntries(const QVector<PassCanonicalKey> &rows);

// Convenience wrappers that build the entries and run the matcher. These are what
// P2-2 calls for the event/resource/pass halves of a regression diff.
CaptureMatchResult MatchEvents(const rdcarray<EventNode> &nodesA,
                               const QHash<uint32_t, EventCanonicalKey> &eventsA,
                               const rdcarray<EventNode> &nodesB,
                               const QHash<uint32_t, EventCanonicalKey> &eventsB,
                               const CaptureMatchPolicy &policy = CaptureMatchPolicy());
CaptureMatchResult MatchResources(const QVector<ResourceCanonicalKey> &baseline,
                                  const QVector<ResourceCanonicalKey> &candidate,
                                  const CaptureMatchPolicy &policy = CaptureMatchPolicy());
CaptureMatchResult MatchPasses(const QVector<PassCanonicalKey> &baseline,
                               const QVector<PassCanonicalKey> &candidate,
                               const CaptureMatchPolicy &policy = CaptureMatchPolicy());

// Jaro-Winkler similarity of two already-normalized strings, in [0,1]. Exposed
// because the fuzzy threshold is policy, not an implementation detail.
double NameSimilarity(const QString &a, const QString &b);

}    // namespace MCP
