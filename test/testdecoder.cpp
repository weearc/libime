/*
 * SPDX-FileCopyrightText: 2017-2017 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <cstddef>
#include <bit>
#include <cstdint>
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <ostream>
#include <fcitx-utils/log.h>
#include "libime/core/decoder.h"
#include "libime/core/decoder_v2_p.h"
#include "libime/core/languagemodel.h"
#include "libime/core/lattice.h"
#include "libime/core/segmentgraph.h"
#include "libime/pinyin/pinyindecoder.h"
#include "libime/pinyin/pinyindictionary.h"
#include "libime/pinyin/pinyinencoder.h"
#include "testdir.h"
#include "testutils.h"

using namespace libime;

void testBuildScoredDag(PinyinDecoder &decoder, const char *pinyin,
                        PinyinFuzzyFlags flags) {
    for (const auto beamSize : {size_t(0), size_t(1), size_t(3)}) {
        auto graph = PinyinEncoder::parseUserPinyin(pinyin, flags);
        Lattice lattice;
        FCITX_ASSERT(decoder.decode(
            lattice, graph, 5, decoder.model()->nullState(),
            std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(), beamSize,
            Decoder::frameSizeDefault, nullptr));
        const SegmentGraphNode *intermediateNode = nullptr;
        size_t intermediateCount = 0;
        graph.bfs(&graph.start(), [&](const SegmentGraphBase &,
                                     const SegmentGraphNode *node) {
            if (node != &graph.start() && node != &graph.end()) {
                size_t count = 0;
                for (const auto &unused : lattice.nodes(node)) {
                    (void)unused;
                    count++;
                }
                if (count > intermediateCount) {
                    intermediateNode = node;
                    intermediateCount = count;
                }
            }
            return true;
        });
        FCITX_ASSERT(intermediateNode != nullptr);
        FCITX_ASSERT(intermediateCount >= 2);
        bool invariantFailure = false;
        const auto dag = decoder_v2::buildScoredDag(
            graph, lattice, beamSize, invariantFailure);
        FCITX_ASSERT(!invariantFailure);
        FCITX_ASSERT(dag.bos == 0);
        FCITX_ASSERT(dag.eos == dag.nodes.size() - 1);
        FCITX_ASSERT(dag.nodes[dag.bos].incomingCount == 0);
        FCITX_ASSERT(dag.nodes[dag.eos].outgoingCount == 0);
        FCITX_ASSERT(dag.nodes[dag.eos].bestPrevEdge !=
                     decoder_v2::InvalidEdgeId);
        FCITX_ASSERT(dag.nodes[dag.eos].bestPrevEdge < dag.edges.size());
        const auto &bestEdge = dag.edges[dag.nodes[dag.eos].bestPrevEdge];
        FCITX_ASSERT(bestEdge.to == dag.eos);
        FCITX_ASSERT(dag.nodes[bestEdge.from].latticeNode->to() ==
                     dag.nodes[dag.eos].latticeNode->from());
        for (const auto &node : dag.nodes) {
            if (node.latticeNode->from() == intermediateNode) {
                FCITX_ASSERT(node.incomingCount >= 1);
                if (beamSize == 0) {
                    FCITX_ASSERT(node.incomingCount >= 2);
                } else {
                    FCITX_ASSERT(node.incomingCount <= beamSize);
                }
            }
        }
    }
}

void testDecoderV2Differential(PinyinDecoder &decoder) {
    const std::array<std::pair<const char *, PinyinFuzzyFlags>, 6> cases = {{
        {"xian", PinyinFuzzyFlag::Inner},
        {"xiian", PinyinFuzzyFlag::Inner},
        {"tanan", PinyinFuzzyFlag::Inner},
        {"jin'an", PinyinFuzzyFlag::Inner},
        {"anqilaibufangbian", PinyinFuzzyFlag::Inner},
        {"zhizuoxujibianchengleshunshuituizhoudeshiqing",
         PinyinFuzzyFlag::Inner},
    }};
    for (const auto &[pinyin, flags] : cases) {
        for (const auto nbest : {size_t(1), size_t(2), size_t(5)}) {
            const std::array<std::pair<float, float>, 4> limits = {{
                {std::numeric_limits<float>::max(),
                 -std::numeric_limits<float>::max()},
                {2.0F, -std::numeric_limits<float>::max()},
                {0.0F, -std::numeric_limits<float>::max()},
                {std::numeric_limits<float>::max(), -0.2F},
            }};
            for (const auto beamSize : {size_t(0), size_t(1),
                                        Decoder::beamSizeDefault}) {
                for (const auto &[maxDistance, minPath] : limits) {
                    auto graph = PinyinEncoder::parseUserPinyin(pinyin, flags);
                    Lattice lattice;
                    FCITX_ASSERT(decoder.decode(
                        lattice, graph, nbest, decoder.model()->nullState(),
                        maxDistance, minPath, beamSize,
                        Decoder::frameSizeDefault, nullptr));
                    FCITX_ASSERT(lattice.sentenceSize() > 0);
                    bool invariantFailure = false;
                    auto dag = decoder_v2::buildScoredDag(
                        graph, lattice, beamSize, invariantFailure);
                    FCITX_ASSERT(!invariantFailure);
                    decoder_v2::Counters counters;
                    State edgeState;
                    const auto scoreProvider = [&](LatticeNode &from,
                                                   const LatticeNode &to) {
                        return decoder.model()->score(from.state(), to,
                                                      edgeState) + to.cost();
                    };
                    const auto results = decoder_v2::enumerate(
                        dag, lattice.sentence(0), nbest, maxDistance, minPath,
                        scoreProvider, counters);
                    FCITX_ASSERT(!counters.invariantFailure);
                    FCITX_ASSERT(results.size() == lattice.sentenceSize());
                    for (size_t i = 0; i < results.size(); i++) {
                        const auto &legacy = lattice.sentence(i);
                        FCITX_ASSERT(results[i].toString() == legacy.toString());
                        FCITX_ASSERT(
                            std::bit_cast<uint32_t>(results[i].score()) ==
                            std::bit_cast<uint32_t>(legacy.score()));
                    }
                }
            }
        }
    }
}

void testTime(PinyinDictionary & /*unused*/, Decoder &decoder,
              const char *pinyin, PinyinFuzzyFlags flags, int nbest = 1) {
    auto printTime = [](int t) {
        std::cout << "Time: " << t / 1000000.0 << " ms" << std::endl;
    };
    ScopedNanoTimer timer(printTime);
    auto graph = PinyinEncoder::parseUserPinyin(pinyin, flags);
    Lattice lattice;
    decoder.decode(lattice, graph, nbest, decoder.model()->nullState(),
                   std::numeric_limits<float>::max(),
                   -std::numeric_limits<float>::max(), Decoder::beamSizeDefault,
                   Decoder::frameSizeDefault, nullptr);
    for (size_t i = 0, e = lattice.sentenceSize(); i < e; i++) {
        const auto &sentence = lattice.sentence(i);
        for (const auto &p : sentence.sentence()) {
            std::cout << p->word() << " ";
        }
        std::cout << sentence.score() << std::endl;
    }
}

int main() {
    PinyinDictionary dict;
    dict.load(PinyinDictionary::SystemDict, LIBIME_BINARY_DIR "/data/sc.dict",
              PinyinDictFormat::Binary);
    LanguageModel model(LIBIME_BINARY_DIR "/data/sc.lm");
    PinyinDecoder decoder(&dict, &model);
    testBuildScoredDag(decoder, "xianshi", PinyinFuzzyFlag::Inner);
    testDecoderV2Differential(decoder);
    testTime(dict, decoder, "wojiushixiangceshi", PinyinFuzzyFlag::None);
    testTime(dict, decoder, "xian", PinyinFuzzyFlag::Inner);
    testTime(dict, decoder, "xiian", PinyinFuzzyFlag::Inner);
    testTime(dict, decoder, "tanan", PinyinFuzzyFlag::Inner);
    testTime(dict, decoder, "jin'an", PinyinFuzzyFlag::Inner);
    testTime(dict, decoder, "sh'a", PinyinFuzzyFlag::Inner);
    testTime(dict, decoder, "xiian", PinyinFuzzyFlag::Inner);
    testTime(dict, decoder, "anqilaibufangbian", PinyinFuzzyFlag::Inner);
    testTime(dict, decoder, "zhizuoxujibianchengleshunshuituizhoudeshiqing",
             PinyinFuzzyFlag::Inner, 2);
    testTime(dict, decoder, "xi'ian", PinyinFuzzyFlag::Inner);
    testTime(dict, decoder, "zuishengmengsi'''", PinyinFuzzyFlag::Inner);
    testTime(dict, decoder, "yongtiechuichuidanchuibupo",
             PinyinFuzzyFlag::Inner);
    testTime(dict, decoder, "feibenkerenyuanbunengrunei",
             PinyinFuzzyFlag::Inner);
    testTime(dict, decoder, "feibenkerenyuanbuderunei", PinyinFuzzyFlag::Inner);
    testTime(dict, decoder, "yongtiechuichuidanchuibupo",
             PinyinFuzzyFlag::Inner, 2);
    testTime(dict, decoder, "feibenkerenyuanbuderunei", PinyinFuzzyFlag::Inner,
             2);
    testTime(dict, decoder, "tashiyigehaoren", PinyinFuzzyFlag::Inner, 3);
    testTime(dict, decoder, "xianshi", PinyinFuzzyFlag::Inner, 20);
    testTime(dict, decoder, "xianshi", PinyinFuzzyFlag::Inner, 1);
    testTime(dict, decoder, "'xianshi", PinyinFuzzyFlag::Inner, 1);
    testTime(dict, decoder, "zhuoyand", PinyinFuzzyFlag::Inner, 1);
    testTime(dict, decoder, "nd", PinyinFuzzyFlag::Inner, 1);
    testTime(dict, decoder, "zhzxjbchlshshtzhdshq", PinyinFuzzyFlag::Inner, 1);
    testTime(dict, decoder, "tashini", PinyinFuzzyFlag::Inner, 2);
    testTime(dict, decoder, "'''", PinyinFuzzyFlag::Inner, 2);
    // testTime(dict, decoder, "n", PinyinFuzzyFlag::Inner);

    auto printTime = [](int t) {
        std::cout << "Time: " << t / 1000000.0 << " ms" << std::endl;
    };

    SegmentGraph graph;
    {
        ScopedNanoTimer timer(printTime);
        std::cout << "Parse Pinyin ";
        graph = PinyinEncoder::parseUserPinyin("sdfsdfsdfsdfsdfsdfsdf",
                                               PinyinFuzzyFlag::None);
    }
    {
        // try do nothing
        ScopedNanoTimer timer(printTime);
        std::cout << "Pure Match ";
        dict.matchPrefix(graph, [](const SegmentGraphPath &, WordNode &, float,
                                   std::unique_ptr<LatticeNodeData>) {});
    }
    testTime(dict, decoder, "sdfsdfsdfsdfsdfsdfsdf", PinyinFuzzyFlag::None, 2);
    testTime(dict, decoder, "ceshiyixiayebuhuichucuo", PinyinFuzzyFlag::None,
             2);
    return 0;
}
