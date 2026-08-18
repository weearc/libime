# Decoder V2 Design

## 1. Motivation

The existing decoder is reliable, but its path-centric backward search can
repeat the same suffix work for many prefixes. Decoder V2 investigates a
private alternative for reducing that pathological recomputation.

## 2. Existing Decoder Overview

The current pipeline builds a lattice, performs forward scoring, and expands
predecessor paths through a global priority queue to produce N-best results.
Lattice order and all decoder parameters are observable semantics.

## 3. Problem Statement

Repeated predecessor expansion makes latency grow sharply when many prefixes
share suffixes. A score cache alone reduces repeated model calls but does not
remove repeated path composition.

## 4. Design Goals

V2 aims to preserve result text, order, scores, filtering, and limits while
sharing suffix enumeration work and materializing transition scores only when
needed.

## 5. Non-Goals

This proposal does not change language-model scoring, beam defaults, topology,
adaptive dispatch, public ABI, or the Legacy production path.

## 6. Semantic Invariants

For an identical scored lattice, V2 must preserve result count, complete text,
ordering, exact score bits, path composition, deduplication, maxDistance,
minPath, beam, and N-best behavior in the normal regime before Legacy's global
backward-search budget is exhausted. Exact-score tie ordering is intentionally
not claimed equivalent: Legacy has no explicit secondary comparator, while V2
uses an explicit deterministic order.

## 7. Search-Graph Properties

The forward-scored lattice is represented as a directed acyclic graph. Nodes
retain their lattice-node identity. Edges connect every permitted predecessor
to a target, in the same predecessor order used by forward search.

## 8. Transition-Score Semantics

An edge score is the language-model transition from the predecessor state to the
target node plus the target cost. A not-yet-materialized edge contains NaN as a
private sentinel; NaN is never exposed as a result score. NaN is reserved for
this internal state. A provider that returns NaN is treated as an invalid
scoring state and causes `invariantFailure`. This is stricter than Legacy's
incidental floating-point propagation; production integration must decide
whether to keep this policy or use an explicit materialization flag.

## 9. V2 Architecture Overview

The private implementation consists of a scored DAG, a lazy edge-score
provider, and a per-node lazy suffix enumerator. The Legacy decoder remains the
only production return path in this RFC branch.

## 10. Scored DAG Representation

Nodes store lattice identity, best prefix score, incoming/outgoing ranges, and
the edge selected by forward search. Edges store source, target, transition
score, and a stable ordinal used for deterministic ties.

## 11. Lazy Transition-Score Materialization

When a candidate first enters a node heap, its edge score is requested. The
materialized value is retained for the remainder of that enumeration, so each
edge is scored at most once.

The overload that omits an `EdgeScoreProvider` is a pre-scored-DAG test helper.
Its caller must ensure every edge needed by enumeration already contains a
materialized, non-NaN score. It is not a production scoring path.

## 12. Per-Node Lazy Suffix Enumeration

Each node owns a memoized ranked suffix stream. Its heap initially contains the
best rank of each outgoing child, then advances only the child stream that was
just consumed.

```text
ensure(node, rank):
  initialize outgoing child rank 0 lazily
  repeatedly pop the best candidate
  request the next rank from the consumed child
  memoize the resulting suffix handle
```

## 13. N-Best Merge Strategy

The root stream is queried in rank order. Complete paths are materialized only
for requested ranks, avoiding a global expansion of all partial paths.

## 14. Deduplication

Deduplication is performed on the complete sentence string. Distinct lattice
paths that produce the same string therefore have the same externally visible
behavior as the existing decoder.

## 15. maxDistance / minPath Handling

Candidates whose distance from the forward-best EOS score exceeds maxDistance
are discarded. minPath is applied to non-BOS transitions; the BOS transition
retains the existing exemption.

## 16. Beam and Search-Budget Semantics

The DAG builder includes only the beam-sized predecessor prefix for each target.
No new search budget is introduced. Existing caller values are passed through
unchanged by the test integration.

Legacy also has a global `MAX_BACKWARD_SEARCH_SIZE` expansion cap. Its
observable effect is tied to the path-centric priority queue and has no direct
one-to-one counter in the lazy DAG enumerator. V2 does not emulate that
implementation-specific cutoff; equivalence claims therefore apply before
Legacy budget exhaustion. A future V2 resource bound must use V2's own work
and memory model rather than a copied queue-push number.

## 17. Tie Handling and Determinism

Equal scores are ordered by edge ordinal and then child rank. Ordinals follow
the lattice predecessor order, making repeated V2 runs deterministic. For
strictly ordered scores this is expected to agree with Legacy. Legacy does not
define a stable secondary order for exact ties, so byte-for-byte tie ordering
equivalence is not a contract.

## 18. Lifetime and Ownership

The DAG borrows lattice-node pointers and is valid only while its source lattice
is alive and unchanged. Lazy state is owned by one enumeration call and is not
shared between decoder invocations.

## 19. Complexity Analysis

For E permitted edges and K requested results, edge scoring is O(E) worst case.
Suffix composition is proportional to the ranks actually requested, rather
than all prefix/suffix combinations. Heap operations add logarithmic factors.

## 20. Memory Characteristics

The DAG uses linear storage in nodes and edges. Per-node heaps and memoized
handles grow with the requested suffix ranks. This trades memory for reduced
repeated path work.

## 21. Correctness Strategy

The private unit tests cover lazy merge, ties, complete-string deduplication,
distance and path thresholds, non-finite values, one-time edge materialization,
randomized differential enumeration, and repeated-run tie determinism. A host
test also compares Legacy and V2 on the same scored lattice across varied
inputs, beam values, and limits.

## 22. Known Trade-offs

V2 strongly reduces pathological N-Best tail latency, but can perform more
topology work on low-ambiguity inputs. Legacy remains faster on some
low/medium-complexity inputs.

## 23. Alternatives Considered

Exact per-decode transition-score memoization preserves Legacy search order and
improves repeated model scoring. It materially improves tails, but does not
remove suffix recombination and therefore does not fully match V2 in
pathological cases.

## 24. Integration Strategy

This RFC keeps the implementation private and test-only. It adds no public API,
does not make V2 the default, and leaves the existing decoder source intact.
An upstream review should decide whether a future production integration is
warranted.

## 25. Open Questions / Future Work

Reviewers should assess acceptable temporary DAG memory, lazy-rank memo growth,
appropriate global and per-node work limits, and the ownership/lifetime
boundary for borrowed `LatticeNode` pointers. They should also review failure
behavior for invalid or non-finite scoring state and whether deterministic
exact-tie ordering should remain a V2 property. A production V2 integration
still needs a resource bound, potentially based on total memoized ranks, lazy
heap pushes, materialized edges, temporary bytes, per-node ranks, or elapsed
decode work. No policy is selected here. Adaptive selection, new optimizations,
and production dispatch are intentionally deferred.
