# Decoder V2 Evaluation

## 1. Scope

This document records correctness evidence and supplementary performance
measurements for the private Decoder V2 design.

## 2. Evaluation Questions

The evaluation asks whether V2 preserves decoder semantics and whether lazy
suffix composition improves pathological N-Best latency beyond score caching.

## 3. Correctness Methodology

The host differential test runs Legacy on a lattice, builds a private scored
DAG from that same lattice, and compares V2 results by count, text, order, and
IEEE-754 score bits. It also checks invariant status.

## 4. Test Corpus

The reproducible host cases cover six pinyin inputs, three N-Best requests, and
unbounded, finite-distance, and finite-path thresholds. The broader research
corpus contained 158 deterministic fixtures, but its private paths are not
required by this branch.

## 5. Device / Build Environment

Performance numbers below came from an Android arm64 device benchmark
environment. Correctness is host-level and does not depend on device logs.
The RFC branch is maintained against current upstream `master`.

## 6. Measurement Method

Measurements report decode-stage and N-Best-stage latency percentiles over the
same input set. Legacy, an exact transition-score memo control, and V2 used
identical model and parameter settings.

The arm64 probe boundaries were fixed before collection:

| Probe | Boundary |
| --- | --- |
| `match` | dictionary/prefix matching entry through completion of match callbacks |
| `lattice` | lattice-node admission/insertion after matching |
| `forward` | forward search entry through completion of forward scores and predecessor links |
| `legacy backward` | Legacy backward N-Best entry through Legacy result construction |
| `V2 sidecar` | scored-DAG construction entry through DAG validation |
| `V2 edge scoring` | lazy transition-score provider calls, including model scoring and target cost |
| `V2 N-Best` | lazy root-stream enumeration entry through V2 result materialization |
| `V2 total` | V2 sidecar start through V2 N-Best completion, excluding the subsequently run Legacy oracle |
| `decode total` | decode entry through the selected result path's completion |

All durations were measured in microseconds and converted to milliseconds for
the tables. The probes are measurement-only boundaries from the device
experiment; they are not part of this upstream RFC branch.

## 7. Legacy Baseline

Legacy uses the existing lattice, forward search, and path-centric backward
N-Best search without changing defaults.

## 8. Legacy Transition-Memo Control

The control memoizes exact transition scores for one decode while preserving
Legacy search order. It produced an approximately 92.6% weighted reduction in
repeated language-model scoring overall.

## 9. Decoder V2 Results

V2's principal gain is reduced repeated suffix composition. It is not uniformly
faster: low-ambiguity topology work can cost more than Legacy.

## 10. Aggregate Latency

| Variant | p50 (ms) | p95 (ms) | p99 (ms) | max (ms) |
| --- | ---: | ---: | ---: | ---: |
| Legacy | 8.305 | 33.557 | 72.392 | 83.307 |
| Legacy + exact transition memo | 7.814 | 20.344 | 41.849 | 46.655 |
| Decoder V2 | 7.922 | 14.081 | 16.906 | 19.417 |

## 11. Tail Latency

N-Best stage p95 was 30.977 ms for Legacy, 14.240 ms for the memo control,
and 3.581 ms for V2. This supports a topology-level benefit beyond score reuse.

## 12. Complexity-Bucket Analysis

Representative p50 values were:

| Bucket | Legacy | Memo control | V2 |
| --- | ---: | ---: | ---: |
| Low | 5.369 | 6.919 | 8.286 |
| Medium | 4.424 | 4.468 | 5.292 |
| High | 8.659 | 8.283 | 8.142 |
| Pathological | 14.793 | 11.432 | 8.928 |

Pathological p95 was 63.751 ms, 33.380 ms, and 14.959 ms respectively.

## 13. LM-Call / Edge-Work Analysis

The memo control reduces repeated model calls. V2 additionally reuses ranked
suffix streams and only materializes edges needed by requested results.

The no-provider enumerator overload was validated only with fully pre-scored
test DAGs; callers must not use it with unmaterialized edges.

## 14. Known Regressions

V2 may be slower on low and medium complexity inputs because it builds and
maintains DAG topology that Legacy can traverse cheaply.

The semantic differential campaign intentionally does not cover Legacy's
10,000-expansion budget-exhausted regime. It also does not claim identical
ordering for exact-score ties: V2 is deterministic, while Legacy has no stable
secondary comparator contract.

## 15. Interpretation

V2 improves worst-case and tail behavior, especially for pathological N-Best
requests, while Legacy remains a reasonable default for low ambiguity.

## 16. Limitations

The performance sample is device-specific, and the private 158-fixture corpus
is not included here. Device measurements are supplementary evidence, not a
claim of universal speedup. Semantic differential evidence does not cover
Legacy budget exhaustion or establish exact-score tie-order identity.

## 17. Reproduction Notes

Build the host tests with the repository-local toolchain, run `testdecoderv2`,
`testdecoder`, and `testpinyincontext`, and inspect the differential
assertions. Repeat device measurements only with an explicitly selected
benchmark environment; this RFC does not add a runtime switch.
