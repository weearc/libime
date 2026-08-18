/*
 * SPDX-FileCopyrightText: 2026 weearc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "decoder_v2_p.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <queue>
#include <string>
#include <unordered_set>
#include <utility>
#include <unordered_map>

namespace libime::decoder_v2 {
namespace {

struct RankedPathHandle {
    float suffixScore = 0.0F;
    SearchEdgeId edge = InvalidEdgeId;
    uint32_t childRank = 0;
};

struct LazyHeapEntry {
    SearchEdgeId edge = InvalidEdgeId;
    uint32_t childRank = 0;
    float totalScore = 0.0F;
    uint32_t tieOrdinal = 0;
};

struct LazyHeapEntryLess {
    bool operator()(const LazyHeapEntry &lhs, const LazyHeapEntry &rhs) const {
        if (lhs.totalScore != rhs.totalScore) {
            return lhs.totalScore < rhs.totalScore;
        }
        if (lhs.tieOrdinal != rhs.tieOrdinal) {
            return lhs.tieOrdinal > rhs.tieOrdinal;
        }
        return lhs.childRank > rhs.childRank;
    }
};

using LazyHeap = std::priority_queue<LazyHeapEntry, std::vector<LazyHeapEntry>,
                                     LazyHeapEntryLess>;

struct PendingAdvance {
    SearchEdgeId edge = InvalidEdgeId;
    uint32_t childRank = 0;
};

struct LazyNodeState {
    std::vector<RankedPathHandle> memoizedRanks;
    LazyHeap heap;
    std::optional<PendingAdvance> pending;
    uint32_t initializeOffset = 0;
    bool initialized = false;
    bool exhausted = false;
};

struct Request {
    SearchNodeId node = InvalidNodeId;
    uint32_t rank = 0;
};

class Enumerator {
public:
    Enumerator(ScoredDag &dag, float maxDistance, float minPath,
               const EdgeScoreProvider &scoreProvider, Counters &counters)
        : dag_(dag), maxDistance_(maxDistance), minPath_(minPath),
          scoreProvider_(scoreProvider), counters_(counters),
          states_(dag.nodes.size()) {
        auto &eos = states_[dag_.eos];
        eos.memoizedRanks.push_back({0.0F, InvalidEdgeId, 0});
        eos.initialized = true;
        eos.exhausted = true;
        counters_.memoizedLazyRanks++;
    }

    bool ensureRank(SearchNodeId node, uint32_t rank) {
        std::vector<Request> requests;
        pushRequest(requests, node, rank);
        while (!requests.empty() && !counters_.invariantFailure) {
            const auto request = requests.back();
            auto &state = states_[request.node];
            if (state.memoizedRanks.size() > request.rank || state.exhausted) {
                requests.pop_back();
                continue;
            }

            const auto &searchNode = dag_.nodes[request.node];
            if (!state.initialized) {
                if (state.initializeOffset < searchNode.outgoingCount) {
                    const auto edge =
                        dag_.outgoingEdges[searchNode.outgoingBegin +
                                           state.initializeOffset];
                    const auto child = dag_.edges[edge].to;
                    if (!rankAvailable(child, 0) && !states_[child].exhausted) {
                        pushRequest(requests, child, 0);
                        continue;
                    }
                    state.initializeOffset++;
                    if (rankAvailable(child, 0)) {
                        addCandidate(request.node, edge, 0);
                    }
                    continue;
                }
                state.initialized = true;
            }

            if (state.pending) {
                const auto pending = *state.pending;
                const auto child = dag_.edges[pending.edge].to;
                if (!rankAvailable(child, pending.childRank) &&
                    !states_[child].exhausted) {
                    pushRequest(requests, child, pending.childRank);
                    continue;
                }
                state.pending.reset();
                if (rankAvailable(child, pending.childRank)) {
                    addCandidate(request.node, pending.edge, pending.childRank);
                }
                continue;
            }

            if (state.heap.empty()) {
                state.exhausted = true;
                continue;
            }

            const auto top = state.heap.top();
            state.heap.pop();
            counters_.lazyPathHeapPops++;
            state.memoizedRanks.push_back(
                {top.totalScore, top.edge, top.childRank});
            counters_.memoizedLazyRanks++;
            state.pending = PendingAdvance{top.edge, top.childRank + 1};
        }
        return !counters_.invariantFailure && rankAvailable(node, rank);
    }

    const RankedPathHandle &rank(SearchNodeId node, uint32_t rank) const {
        return states_[node].memoizedRanks[rank];
    }

private:
    void pushRequest(std::vector<Request> &requests, SearchNodeId node,
                     uint32_t rank) {
        counters_.lazyRankRequests++;
        if (rankAvailable(node, rank)) {
            counters_.lazyNodeRankCacheHits++;
        } else {
            counters_.lazyNodeRankCacheMisses++;
        }
        requests.push_back({node, rank});
    }

    bool rankAvailable(SearchNodeId node, uint32_t rank) const {
        return states_[node].memoizedRanks.size() > rank;
    }

    void addCandidate(SearchNodeId source, SearchEdgeId edgeId,
                      uint32_t childRank) {
        auto &edge = dag_.edges[edgeId];
        if (!std::isnan(edge.transitionScore)) {
            counters_.lazyEdgeScoreCacheHits++;
        } else {
            edge.transitionScore = scoreProvider_(
                *dag_.nodes[edge.from].latticeNode,
                *dag_.nodes[edge.to].latticeNode);
            counters_.lazyEdgeScoreCalls++;
            counters_.lazyEdgeScoreCacheMisses++;
            counters_.nbestLmScoreCalls++;
        }
        if (source != dag_.bos && edge.transitionScore < minPath_) {
            return;
        }
        const auto childScore =
            states_[edge.to].memoizedRanks[childRank].suffixScore;
        const float suffixScore = edge.transitionScore + childScore;
        const float distance = dag_.eosBestScore - suffixScore;
        if (std::isnan(edge.transitionScore) || std::isnan(childScore) ||
            std::isnan(suffixScore) || std::isnan(distance)) {
            counters_.invariantFailure = true;
            return;
        }
        if (distance > maxDistance_) {
            return;
        }
        auto &heap = states_[source].heap;
        heap.push({edgeId, childRank, suffixScore, edge.ordinal});
        counters_.lazyPathHeapPushes++;
        counters_.maxLazyHeapSize =
            std::max(counters_.maxLazyHeapSize, heap.size());
    }

    ScoredDag &dag_;
    float maxDistance_;
    float minPath_;
    const EdgeScoreProvider &scoreProvider_;
    Counters &counters_;
    std::vector<LazyNodeState> states_;
};

SentenceResult materialize(const ScoredDag &dag, const Enumerator &enumerator,
                           uint32_t bosRank, Counters &counters) {
    SentenceResult::Sentence sentence;
    auto node = dag.bos;
    auto rank = bosRank;
    float score = dag.nodes[dag.bos].bestPrefixScore;
    while (node != dag.eos) {
        const auto &path = enumerator.rank(node, rank);
        const auto &edge = dag.edges[path.edge];
        node = edge.to;
        rank = path.childRank;
        const auto *latticeNode = dag.nodes[node].latticeNode;
        if (latticeNode->to()) {
            sentence.push_back(latticeNode);
        }
        if (node == dag.bos) {
            counters.invariantFailure = true;
            break;
        }
    }
    score += enumerator.rank(dag.bos, bosRank).suffixScore;
    counters.completePathsMaterialized++;
    counters.completePathHandles += sentence.size();
    return {std::move(sentence), score};
}

} // namespace

ScoredDag buildScoredDag(const SegmentGraph &graph, const Lattice &lattice,
                         size_t beamSize, bool &invariantFailure) {
    ScoredDag dag;
    std::unordered_map<const LatticeNode *, SearchNodeId> nodeIds;
    auto appendUnit = [&](const SegmentGraphNode *graphNode) {
        for (const auto &constNode : lattice.nodes(graphNode)) {
            if (dag.nodes.size() >= InvalidNodeId) {
                invariantFailure = true;
                return;
            }
            // The DAG borrows lattice-node identity; construction does not
            // mutate through this localized cast.
            auto *node = const_cast<LatticeNode *>(&constNode);
            const auto id = static_cast<SearchNodeId>(dag.nodes.size());
            nodeIds.emplace(node, id);
            dag.nodes.push_back({node, node->score()});
        }
    };

    appendUnit(&graph.start());
    if (invariantFailure || dag.nodes.size() != 1) {
        invariantFailure = true;
        return dag;
    }
    dag.bos = 0;
    graph.bfs(&graph.start(), [&](const SegmentGraphBase &,
                                  const SegmentGraphNode *node) {
        if (node != &graph.start()) {
            appendUnit(node);
        }
        return !invariantFailure;
    });
    const auto nodesBeforeEnd = dag.nodes.size();
    appendUnit(nullptr);
    if (invariantFailure || dag.nodes.size() != nodesBeforeEnd + 1) {
        invariantFailure = true;
        return dag;
    }
    dag.eos = static_cast<SearchNodeId>(dag.nodes.size() - 1);
    dag.eosBestScore = dag.nodes[dag.eos].bestPrefixScore;

    std::vector<std::vector<SearchEdgeId>> incoming(dag.nodes.size());
    std::vector<std::vector<SearchEdgeId>> outgoing(dag.nodes.size());
    uint32_t ordinal = 0;
    for (SearchNodeId targetId = 0; targetId < dag.nodes.size(); targetId++) {
        if (targetId == dag.bos) {
            continue;
        }
        auto *target = dag.nodes[targetId].latticeNode;
        size_t traversed = 0;
        for (const auto &constFrom : lattice.nodes(target->from())) {
            if (beamSize && traversed++ >= beamSize) {
                break;
            }
            // The DAG borrows lattice-node identity; scoring uses the
            // existing mutable state API without changing the lattice here.
            auto *from = const_cast<LatticeNode *>(&constFrom);
            const auto fromIter = nodeIds.find(from);
            if (fromIter == nodeIds.end() || from->to() != target->from() ||
                dag.edges.size() >= InvalidEdgeId) {
                invariantFailure = true;
                return dag;
            }
            const auto edgeId = static_cast<SearchEdgeId>(dag.edges.size());
            dag.edges.push_back({fromIter->second, targetId,
                                 std::numeric_limits<float>::quiet_NaN(),
                                 ordinal++});
            incoming[targetId].push_back(edgeId);
            outgoing[fromIter->second].push_back(edgeId);
            if (target->prev() == from) {
                dag.nodes[targetId].bestPrevEdge = edgeId;
            }
        }
        if (dag.nodes[targetId].bestPrevEdge == InvalidEdgeId) {
            invariantFailure = true;
            return dag;
        }
    }
    for (SearchNodeId id = 0; id < dag.nodes.size(); id++) {
        auto &node = dag.nodes[id];
        node.incomingBegin = static_cast<uint32_t>(dag.incomingEdges.size());
        node.incomingCount = static_cast<uint32_t>(incoming[id].size());
        dag.incomingEdges.insert(dag.incomingEdges.end(), incoming[id].begin(),
                                 incoming[id].end());
        node.outgoingBegin = static_cast<uint32_t>(dag.outgoingEdges.size());
        node.outgoingCount = static_cast<uint32_t>(outgoing[id].size());
        dag.outgoingEdges.insert(dag.outgoingEdges.end(), outgoing[id].begin(),
                                 outgoing[id].end());
    }
    return dag;
}

std::vector<SentenceResult> enumerate(ScoredDag &dag,
                                      const SentenceResult &forwardBest,
                                      size_t nbest, float maxDistance,
                                      float minPath,
                                      const EdgeScoreProvider &scoreProvider,
                                      Counters &counters) {
    std::vector<SentenceResult> results;
    results.push_back(forwardBest);
    if (nbest <= 1 || dag.bos == InvalidNodeId || dag.eos == InvalidNodeId) {
        return results;
    }

    Enumerator enumerator(dag, maxDistance, minPath, scoreProvider, counters);
    std::unordered_set<std::string> duplicates;
    duplicates.insert(forwardBest.toString());
    for (uint32_t rank = 0; results.size() <= nbest; rank++) {
        if (!enumerator.ensureRank(dag.bos, rank)) {
            break;
        }
        auto result = materialize(dag, enumerator, rank, counters);
        if (counters.invariantFailure) {
            break;
        }
        auto text = result.toString();
        if (duplicates.contains(text)) {
            counters.completePathDedupRejects++;
            continue;
        }
        const float distance = dag.eosBestScore - result.score();
        if (std::isnan(result.score()) || std::isnan(distance)) {
            counters.invariantFailure = true;
            break;
        }
        if (distance > maxDistance) {
            break;
        }
        duplicates.insert(std::move(text));
        results.push_back(std::move(result));
    }
    return results;
}

std::vector<SentenceResult> enumerate(ScoredDag &dag,
                                      const SentenceResult &forwardBest,
                                      size_t nbest, float maxDistance,
                                      float minPath, Counters &counters) {
    const EdgeScoreProvider noLazyScores =
        [](LatticeNode &, const LatticeNode &) {
            return std::numeric_limits<float>::quiet_NaN();
        };
    return enumerate(dag, forwardBest, nbest, maxDistance, minPath,
                     noLazyScores, counters);
}

} // namespace libime::decoder_v2
