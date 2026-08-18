/*
 * SPDX-FileCopyrightText: 2026 weearc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef _FCITX_LIBIME_CORE_DECODER_V2_P_H_
#define _FCITX_LIBIME_CORE_DECODER_V2_P_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>
#include "lattice.h"
#include "segmentgraph.h"

namespace libime::decoder_v2 {

using SearchNodeId = uint32_t;
using SearchEdgeId = uint32_t;
constexpr SearchNodeId InvalidNodeId = std::numeric_limits<SearchNodeId>::max();
constexpr SearchEdgeId InvalidEdgeId = std::numeric_limits<SearchEdgeId>::max();

struct ScoredEdge {
    SearchNodeId from = InvalidNodeId;
    SearchNodeId to = InvalidNodeId;
    float transitionScore = 0.0F;
    uint32_t ordinal = 0;
};

struct SearchNode {
    LatticeNode *latticeNode = nullptr;
    float bestPrefixScore = 0.0F;
    SearchEdgeId bestPrevEdge = InvalidEdgeId;
    uint32_t incomingBegin = 0;
    uint32_t incomingCount = 0;
    uint32_t outgoingBegin = 0;
    uint32_t outgoingCount = 0;
};

struct ScoredDag {
    std::vector<SearchNode> nodes;
    std::vector<ScoredEdge> edges;
    std::vector<SearchEdgeId> incomingEdges;
    std::vector<SearchEdgeId> outgoingEdges;
    SearchNodeId bos = InvalidNodeId;
    SearchNodeId eos = InvalidNodeId;
    float eosBestScore = 0.0F;
};

struct Counters {
    size_t lazyPathHeapPushes = 0;
    size_t lazyPathHeapPops = 0;
    size_t lazyRankRequests = 0;
    size_t lazyNodeRankCacheHits = 0;
    size_t lazyNodeRankCacheMisses = 0;
    size_t completePathsMaterialized = 0;
    size_t completePathDedupRejects = 0;
    size_t maxLazyHeapSize = 0;
    size_t memoizedLazyRanks = 0;
    size_t completePathHandles = 0;
    size_t nbestLmScoreCalls = 0;
    size_t lazyEdgeScoreCalls = 0;
    size_t lazyEdgeScoreCacheHits = 0;
    size_t lazyEdgeScoreCacheMisses = 0;
    bool invariantFailure = false;
};

using EdgeScoreProvider =
    std::function<float(LatticeNode &, const LatticeNode &)>;

ScoredDag buildScoredDag(const SegmentGraph &graph, const Lattice &lattice,
                         size_t beamSize, bool &invariantFailure);

std::vector<SentenceResult> enumerate(ScoredDag &dag,
                                      const SentenceResult &forwardBest,
                                      size_t nbest, float maxDistance,
                                      float minPath,
                                      const EdgeScoreProvider &scoreProvider,
                                      Counters &counters);

std::vector<SentenceResult> enumerate(ScoredDag &dag,
                                      const SentenceResult &forwardBest,
                                      size_t nbest, float maxDistance,
                                      float minPath, Counters &counters);

} // namespace libime::decoder_v2

#endif // _FCITX_LIBIME_CORE_DECODER_V2_P_H_
