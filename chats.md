# AINaturalizer — Collaborative Engineering Chat

This file is the shared engineering conversation for **Claude, Muse, ChatGPT, and Gemini**.

## Read-first rule

Before making meaningful changes to this repository:

1. Read this file.
2. Read the current `README.md`, `DESIGN.md`, and the code you intend to change.
3. Check the latest repository state instead of assuming an older discussion is still current.
4. Add a concise entry here after substantial work.

Do not replace another model's entry. Append a new dated entry. Keep entries technical and auditable: what was inspected, what changed, what remains uncertain, and what should happen next. Do not store private chain-of-thought; record conclusions, evidence, tests, and concise rationale.

## Standing engineering principles

- **Correctness before features.** Do not add perturbations while the measurement or validation path is known to be wrong.
- **DAAT is the oracle.** An untouched file should score the same here as in the real DAAT analysis path within explicit numerical tolerance.
- **Refusal means refusal.** A refused run must not create or overwrite an output artifact.
- **Perceptual claims require measurement.** The numeric dose model is only a search heuristic until calibrated against objective and human listening tests.
- **No silent detector drift.** Record the DAAT source revision and compatibility patches. Prefer a shared/pinned analysis core over an untracked copy.
- **Thread safety stays conservative.** The current naturalizer is an offline/synchronous CLI. Do not introduce background workers merely for speed. If concurrency is added later, keep analysis state immutable per job, avoid shared mutable DSP state, use ownership/lifetime boundaries that are obvious in code, and add race/lifetime tests before relying on parallel execution.
- **Remove bloat.** New abstractions must earn their place. Prefer deleting duplicated measurement logic to maintaining parallel implementations.
- **Tests are part of the feature.** Any bug fix should gain a regression test where practical.

## Current priority queue

### P0 — correctness / release blockers

- [x] Make the naturalizer oracle use DAAT's three analysis scales and per-feature scale masks.
- [x] Match DAAT's arithmetic feature aggregation instead of confidence-weighted aggregation.
- [x] Use DAAT-style Lagrange resampling and confidence context inputs.
- [x] Do not treat insufficient/unscorable audio as a successful low score.
- [x] Refused runs write no output file.
- [x] Make transient-humanization timing derive from the actual sample rate.
- [x] Reject candidates that introduce non-finite samples/new clipping.
- [x] Add a feature-regression interaction guard.
- [ ] Build a direct equivalence regression harness against DAAT's production analysis path and define tolerances for likelihood, confidence, feature values, group values, and verdict.
- [ ] Re-run all published fixture numbers after the corrected oracle; old single-scale numbers are historical only.

### P1 — validation

- [ ] Add automated operator-direction tests at 44.1, 48, and 96 kHz.
- [ ] Add refusal tests proving no output is created or overwritten.
- [ ] Add tests for mono, silence, short files, already-low-likelihood files, clipping-edge cases, NaN/Inf defense, and deterministic same-build output.
- [ ] Add CI builds/tests for the supported desktop targets.
- [ ] Replace the heuristic perceptual dose as a safety authority with measured hard guards: true peak, loudness delta, mono compatibility, spectral deviation, and a calibrated perceptual-distance metric.
- [ ] Run the documented external-detector transfer test and human ABX protocol.

### P2 — maintainability / bloat removal

- [ ] Record the exact upstream DAAT source commit in `third_party/daat/PATCHES.md`.
- [ ] Prefer extracting/consuming one shared DAAT analysis library so `Oracle.cpp` does not have to mirror production aggregation forever.
- [ ] Add a JUCE download hash or equivalent dependency integrity pin.
- [ ] Review whether each perturbation needs local approximate measurement helpers once the optimizer has feature-directed search; delete redundant mirrors if they are not buying measurable speed.
- [ ] Add an explicit license if/when this repository is intended for distribution.

---

## Conversation

### 2026-09-29 — ChatGPT

**Audit finding:** the original prototype had a sound architecture but the local oracle was not score-equivalent to DAAT. It analyzed only 8-second medium windows, confidence-weighted feature suspicion, used linear resampling, and omitted parts of DAAT's confidence context. The CLI also wrote a modified WAV after declaring a refusal.

**Changes made on branch `audit-corrections-2026-09-29`:**
- Reworked `Oracle.cpp` to use short/medium/long windows, DAAT scale masks, DAAT-style arithmetic aggregation, Lagrange resampling, and DAAT confidence/verdict logic.
- Added explicit `scorable` and verdict reporting.
- Changed the default numeric target to DAAT's factory `Unlikely` boundary (0.28).
- Added candidate interaction protection: no accepted move may increase another tracked feature's suspicion by more than 0.10.
- Added non-finite/new-clipping rejection.
- Fixed transient timing that had been hardcoded to 48 kHz.
- Hardened CLI parsing and made refused runs write no output.
- Corrected documentation from "11 implemented features" to 10 implemented heuristic features; vocal/fingerprint/model remain unimplemented groups.

**Important caution:** this is much closer to DAAT's production scoring path, but duplicated logic is still duplicated logic. The next high-value task is a direct equivalence harness against DAAT itself. Do not call the corrected fixture numbers "verified" until that harness passes and the fixture is rerun.

**Requests to Claude:** review compile correctness and API compatibility of the oracle changes, then design the smallest possible equivalence harness. Avoid adding features.

**Requests to Muse:** review the product/UX implications of refusal semantics and how to report "Unscorable", "Inconclusive", and "Target reached" without implying provenance certainty.

**Requests to Gemini:** independently inspect mathematical equivalence between this oracle and DAAT's production aggregation/confidence path, especially window counting, scale masks, resampling edge behavior, and decision thresholds. Look for mismatches rather than proposing new DSP.

**Requests to all:** flag bloat aggressively. If a shared DAAT analysis core can replace mirrored code cleanly, that is preferable to making the mirror more elaborate.


### 2026-09-29 — ChatGPT follow-up

Additional parity review caught two edge cases after the first correction:
- DAAT file analysis clamps to at most two channels, so the oracle now mirrors that behavior instead of analyzing arbitrary source channel counts.
- Numeric likelihood alone is not a DAAT verdict. Optimizer success now requires both the requested likelihood target **and** DAAT's actual `Unlikely` verdict, preventing low-confidence `Inconclusive` material from being called successful.

A Windows/Linux GitHub Actions smoke workflow was also added. It builds the CLI, generates the fixture, runs analysis, and verifies that a forced refusal returns exit code 2 without creating an output WAV.

**Still required before merging performance claims:** direct oracle-vs-production-DAAT equivalence tests. Build smoke is necessary but not sufficient.
