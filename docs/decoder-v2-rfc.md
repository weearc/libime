# Decoder V2 RFC Overview

## What changes

This branch adds a private scored-DAG representation, lazy transition scoring,
lazy per-node suffix enumeration, focused unit tests, and a host differential
test.

## Why it exists

The design targets repeated suffix recombination that dominates pathological
N-Best latency in the existing path-centric search.

## What is preserved

Legacy source, public APIs, defaults, lattice construction, score semantics,
and production return behavior are preserved.

## What improves

The measured arm64 sample shows substantially lower N-Best tail latency for V2,
with the largest improvement in pathological inputs.

## What regresses

V2 can do more topology work and lose to Legacy on low or medium complexity
inputs. It also requires additional temporary DAG and lazy-stream memory.

## How it was validated

Self-contained algorithm tests pass. Host differential coverage compares six
inputs across N-Best and threshold settings with zero mismatches and no
invariant failures.

## Feedback requested

Reviewers should focus on ownership, memory bounds, tie semantics, and whether
an internal experiment is valuable enough to justify a future production API.
