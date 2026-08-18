# Decoder V2 RFC Overview

## What changes

This branch adds a private scored-DAG representation, lazy transition scoring,
lazy per-node suffix enumeration, focused unit tests, and a host differential
test.

## Why it exists

The design targets repeated suffix recombination that dominates pathological
N-Best latency in the existing path-centric search.

## What is preserved

Legacy source, public APIs, defaults, lattice construction, and score semantics
are preserved in the normal regime before Legacy's global backward-search
budget is exhausted. Legacy's implementation-specific expansion cutoff is not
emulated by V2. Exact-score tie ordering is also not claimed identical: V2 has
an explicit deterministic tie breaker, while Legacy has no stable secondary
ordering contract.

## What improves

The measured arm64 sample shows substantially lower N-Best tail latency for V2,
with the largest improvement in pathological inputs.

## What regresses

V2 can do more topology work and lose to Legacy on low or medium complexity
inputs. It also requires additional temporary DAG and lazy-stream memory.

## How it was validated

Self-contained algorithm tests pass, including repeated-run exact-tie
determinism. Host differential coverage compares six inputs across N-Best,
beam, and threshold settings with zero mismatches and no invariant failures in
the normal regime.

## Feedback requested

Reviewers should focus on ownership, memory bounds, exact-tie semantics, NaN
failure policy, the missing Legacy-budget equivalence, and whether an internal
experiment is valuable enough to justify a future production API.
