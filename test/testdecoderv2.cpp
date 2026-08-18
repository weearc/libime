/*
 * SPDX-FileCopyrightText: 2026 weearc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <vector>
#include <fcitx-utils/log.h>
#include "libime/core/decoder_v2_p.h"
#include "libime/core/segmentgraph.h"

using namespace libime;
using namespace libime::decoder_v2;

namespace {

struct Fixture {
    static SegmentGraph makeGraph() {
        SegmentGraph graph{"ab"};
        graph.addNext(0, 1);
        graph.addNext(1, 2);
        return graph;
    }

    SegmentGraph graph = makeGraph();
    State state{};
    LatticeNode bos{"", 0, {nullptr, &graph.start()}, state};
    LatticeNode a{"a", 1, {&graph.start(), &graph.node(1)}, state};
    LatticeNode b;
    LatticeNode x{"x", 3, {&graph.node(1), &graph.end()}, state};
    LatticeNode y{"y", 4, {&graph.node(1), &graph.end()}, state};
    LatticeNode eos{"", 5, {&graph.end(), nullptr}, state};
    ScoredDag dag;

    explicit Fixture(std::string bWord = "b")
        : b(bWord, 2, {&graph.start(), &graph.node(1)}, state) {
        dag.nodes = {{&bos}, {&a}, {&b}, {&x}, {&y}, {&eos}};
        dag.bos = 0;
        dag.eos = 5;
    }

    void edge(SearchNodeId from, SearchNodeId to, float score) {
        dag.edges.push_back(
            {from, to, score, static_cast<uint32_t>(dag.edges.size())});
    }

    void finish(float eosBestScore) {
        dag.eosBestScore = eosBestScore;
        std::vector<std::vector<SearchEdgeId>> incoming(dag.nodes.size());
        std::vector<std::vector<SearchEdgeId>> outgoing(dag.nodes.size());
        for (SearchEdgeId edge = 0; edge < dag.edges.size(); edge++) {
            incoming[dag.edges[edge].to].push_back(edge);
            outgoing[dag.edges[edge].from].push_back(edge);
        }
        for (SearchNodeId node = 0; node < dag.nodes.size(); node++) {
            dag.nodes[node].incomingBegin = dag.incomingEdges.size();
            dag.nodes[node].incomingCount = incoming[node].size();
            dag.incomingEdges.insert(dag.incomingEdges.end(),
                                     incoming[node].begin(),
                                     incoming[node].end());
            dag.nodes[node].outgoingBegin = dag.outgoingEdges.size();
            dag.nodes[node].outgoingCount = outgoing[node].size();
            dag.outgoingEdges.insert(dag.outgoingEdges.end(),
                                     outgoing[node].begin(),
                                     outgoing[node].end());
        }
    }
};

void assertScore(float actual, float expected) {
    FCITX_ASSERT(std::bit_cast<uint32_t>(actual) ==
                 std::bit_cast<uint32_t>(expected));
}

std::unique_ptr<Fixture> layeredFixture(float axTail = -0.1F,
                                        std::string bWord = "b") {
    auto fixture = std::make_unique<Fixture>(std::move(bWord));
    fixture->edge(0, 1, -0.1F);
    fixture->edge(0, 2, -0.2F);
    fixture->edge(1, 3, -0.2F);
    fixture->edge(1, 4, -0.4F);
    fixture->edge(2, 3, -0.1F);
    fixture->edge(2, 4, -0.5F);
    fixture->edge(3, 5, axTail);
    fixture->edge(4, 5, -0.1F);
    fixture->finish(-0.4F);
    return fixture;
}

void testLazyMergeAndTie() {
    auto fixture = layeredFixture();
    const float ax = -0.1F + (-0.2F + (-0.1F + 0.0F));
    SentenceResult forward{{&fixture->a, &fixture->x}, ax};
    Counters counters;
    const auto results =
        enumerate(fixture->dag, forward, 3, std::numeric_limits<float>::max(),
                  -std::numeric_limits<float>::max(), counters);
    FCITX_ASSERT(results.size() == 4);
    FCITX_ASSERT(results[0].toString() == "ax");
    FCITX_ASSERT(results[1].toString() == "bx");
    FCITX_ASSERT(results[2].toString() == "ay");
    FCITX_ASSERT(results[3].toString() == "by");
    assertScore(results[1].score(), -0.2F + (-0.1F + (-0.1F + 0.0F)));
    FCITX_ASSERT(counters.nbestLmScoreCalls == 0);
    FCITX_ASSERT(!counters.invariantFailure);
}

void testCompleteStringDedup() {
    auto fixture = layeredFixture(-0.1F, "a");
    const float ax = -0.1F + (-0.2F + (-0.1F + 0.0F));
    SentenceResult forward{{&fixture->a, &fixture->x}, ax};
    Counters counters;
    const auto results =
        enumerate(fixture->dag, forward, 2, std::numeric_limits<float>::max(),
                  -std::numeric_limits<float>::max(), counters);
    FCITX_ASSERT(results.size() == 2);
    FCITX_ASSERT(results[0].toString() == "ax");
    FCITX_ASSERT(results[1].toString() == "ay");
    FCITX_ASSERT(counters.completePathDedupRejects >= 2);
}

void testPartialMaxDistance() {
    auto fixture = layeredFixture(-10.0F);
    fixture->dag.edges[7].transitionScore = -10.0F;
    fixture->dag.eosBestScore = -1.0F;
    SentenceResult forward{{&fixture->a, &fixture->y}, -0.6F};
    Counters counters;
    const auto results =
        enumerate(fixture->dag, forward, 3, 1.0F,
                  -std::numeric_limits<float>::max(), counters);
    FCITX_ASSERT(results.size() == 1);
}

void testFiniteMinPathAndBosExemption() {
    auto fixture = layeredFixture();
    SentenceResult forward{{&fixture->a, &fixture->x}, -0.4F};
    Counters counters;
    const auto results =
        enumerate(fixture->dag, forward, 3, std::numeric_limits<float>::max(),
                  -0.15F, counters);
    FCITX_ASSERT(results.size() == 2);
    FCITX_ASSERT(results[1].toString() == "bx");

    auto bosFixture = layeredFixture();
    bosFixture->dag.edges[0].transitionScore = -100.0F;
    bosFixture->dag.eosBestScore = -100.3F;
    SentenceResult bosForward{{&bosFixture->a, &bosFixture->x}, -100.3F};
    Counters bosCounters;
    const auto bosResults =
        enumerate(bosFixture->dag, bosForward, 2,
                  std::numeric_limits<float>::max(), -0.45F, bosCounters);
    FCITX_ASSERT(bosResults.size() >= 2);
}

void testInfinityAndNan() {
    auto fixture = layeredFixture();
    fixture->dag.edges[0].transitionScore =
        std::numeric_limits<float>::infinity();
    SentenceResult forward{{&fixture->a, &fixture->x}, 0.0F};
    Counters infinityCounters;
    const auto infinityResults =
        enumerate(fixture->dag, forward, 2, std::numeric_limits<float>::max(),
                  -std::numeric_limits<float>::max(), infinityCounters);
    FCITX_ASSERT(!infinityResults.empty());
    FCITX_ASSERT(!infinityCounters.invariantFailure);

    auto nanFixture = layeredFixture();
    nanFixture->dag.edges[0].transitionScore =
        std::numeric_limits<float>::quiet_NaN();
    Counters nanCounters;
    enumerate(nanFixture->dag, forward, 2, std::numeric_limits<float>::max(),
              -std::numeric_limits<float>::max(), nanCounters);
    FCITX_ASSERT(nanCounters.invariantFailure);
}

void testLazyEdgeMaterializedOnce() {
    auto fixture = layeredFixture();
    std::array<float, 8> scores;
    std::array<size_t, 8> calls{};
    for (size_t i = 0; i < fixture->dag.edges.size(); i++) {
        scores[i] = fixture->dag.edges[i].transitionScore;
        fixture->dag.edges[i].transitionScore =
            std::numeric_limits<float>::quiet_NaN();
    }
    const EdgeScoreProvider provider = [&](LatticeNode &from,
                                           const LatticeNode &to) {
        for (size_t i = 0; i < fixture->dag.edges.size(); i++) {
            const auto &edge = fixture->dag.edges[i];
            if (fixture->dag.nodes[edge.from].latticeNode == &from &&
                fixture->dag.nodes[edge.to].latticeNode == &to) {
                calls[i]++;
                return scores[i];
            }
        }
        return std::numeric_limits<float>::quiet_NaN();
    };
    SentenceResult forward{{&fixture->a, &fixture->x}, -0.4F};
    Counters counters;
    const auto results = enumerate(
        fixture->dag, forward, 3, std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(), provider, counters);
    FCITX_ASSERT(results.size() == 4);
    FCITX_ASSERT(std::ranges::all_of(calls,
                                    [](size_t count) { return count <= 1; }));
    FCITX_ASSERT(counters.lazyEdgeScoreCalls ==
                 counters.lazyEdgeScoreCacheMisses);
    FCITX_ASSERT(counters.lazyEdgeScoreCalls ==
                 static_cast<size_t>(std::ranges::count(calls, size_t{1})));
    FCITX_ASSERT(counters.lazyEdgeScoreCacheHits > 0);
    FCITX_ASSERT(!counters.invariantFailure);
}

void testDeterministicRandomizedDifferential() {
    std::mt19937 generator(0xdec0deU);
    std::uniform_int_distribution<int> score(-9, -1);
    for (size_t iteration = 0; iteration < 200; iteration++) {
        Fixture fixture;
        for (SearchNodeId from : {SearchNodeId{0}}) {
            fixture.edge(from, 1, static_cast<float>(score(generator)));
            fixture.edge(from, 2, static_cast<float>(score(generator)));
        }
        for (SearchNodeId from : {SearchNodeId{1}, SearchNodeId{2}}) {
            fixture.edge(from, 3, static_cast<float>(score(generator)));
            fixture.edge(from, 4, static_cast<float>(score(generator)));
        }
        fixture.edge(3, 5, static_cast<float>(score(generator)));
        fixture.edge(4, 5, static_cast<float>(score(generator)));

        using Expected =
            std::tuple<float, std::string, SentenceResult::Sentence>;
        std::vector<Expected> expected;
        for (SearchNodeId first : {SearchNodeId{1}, SearchNodeId{2}}) {
            for (SearchNodeId second : {SearchNodeId{3}, SearchNodeId{4}}) {
                const auto findScore = [&](SearchNodeId from, SearchNodeId to) {
                    return std::ranges::find_if(fixture.dag.edges,
                                                [&](const auto &edge) {
                                                    return edge.from == from &&
                                                           edge.to == to;
                                                })
                        ->transitionScore;
                };
                const float total =
                    findScore(0, first) +
                    (findScore(first, second) + (findScore(second, 5) + 0.0F));
                SentenceResult::Sentence sentence{
                    fixture.dag.nodes[first].latticeNode,
                    fixture.dag.nodes[second].latticeNode};
                expected.emplace_back(total,
                                      sentence[0]->word() + sentence[1]->word(),
                                      std::move(sentence));
            }
        }
        std::stable_sort(expected.begin(), expected.end(),
                         [](const auto &lhs, const auto &rhs) {
                             return std::get<0>(lhs) > std::get<0>(rhs);
                         });
        fixture.finish(std::get<0>(expected.front()));
        SentenceResult forward{std::get<2>(expected.front()),
                               std::get<0>(expected.front())};
        Counters counters;
        const auto actual = enumerate(
            fixture.dag, forward, 3, std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(), counters);
        FCITX_ASSERT(actual.size() == expected.size());
        for (size_t i = 0; i < actual.size(); i++) {
            FCITX_ASSERT(actual[i].toString() == std::get<1>(expected[i]));
            assertScore(actual[i].score(), std::get<0>(expected[i]));
        }
        FCITX_ASSERT(counters.nbestLmScoreCalls == 0);
        FCITX_ASSERT(!counters.invariantFailure);
    }
}

} // namespace

int main() {
    testLazyMergeAndTie();
    testCompleteStringDedup();
    testPartialMaxDistance();
    testFiniteMinPathAndBosExemption();
    testInfinityAndNan();
    testLazyEdgeMaterializedOnce();
    testDeterministicRandomizedDifferential();
    return 0;
}
