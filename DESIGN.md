# AI Music Naturalizer — Design Document

**Date:** 2026-09-29
**Status:** Design / pre-implementation
**Relationship to DAAT:** Sibling project, deliberately separate. DAAT (the detector in
`bigluckyflavor/AIDetect`) stays an honest forensics instrument. This tool does the inverse:
it modifies AI-generated audio so its measurable characteristics fall inside human-typical
ranges. Separate repo, separate brand, no shared claims.

## 1. What it is

An offline batch processor: feed it an AI-generated track, it outputs a modified track whose
DAAT feature profile scores below the detection threshold — while remaining perceptually
indistinguishable (or at least unobjectionable) to a human listener.

It is **not** a realtime plugin (at least not in v1). The core loop is iterative —
perturb, re-measure, repeat — which needs the whole file and multiple analysis passes.
A plugin version can come later once the perturbation set is proven.

## 2. Honest caveats (read first)

- **It overfits to DAAT's 11 features.** This tool guarantees evasion of the DAAT
  feature set, not of arbitrary detectors. Transfer to other systems is unmeasured and
  should be tested, not assumed.
- **Arms race.** Every perturbation is a signature. If detectors adapt, this tool's
  output becomes *more* detectable, not less. Version the perturbation profiles and
  expect churn.
- **Perceptual budget is the real constraint.** Moving features far enough to matter
  while staying inaudible is the entire engineering problem. If a track needs audible
  damage to pass, the tool should refuse, not comply.
- **Positioning matters.** This is a "humanizer"/"naturalizer" for producers working
  with AI-assisted material. It is not a forensics tool and must never be marketed as one.

## 3. Architecture

```
Input WAV ──► Analyzer ──► Feature vector (per window, per scale)
                    │              │
                    │              ▼
                    │        Optimizer: pick perturbation, apply, re-analyze
                    │              │
                    │              ▼ (likelihood < threshold + margin, or budget exhausted)
                    ▼
Output WAV ◄── Perturbation chain (frozen parameters)
```

**Reuse from DAAT (link as a static library, do not fork the logic):**
- `FeatureExtractor` — computes `WindowRawFeatures` from audio. This is the measurement
  oracle. The naturalizer's loss function is literally DAAT's `combineGroups` output.
- `DetectionEngine` (`computeGroupScores` / `combineGroups` / `computeConfidence`) —
  pure functions, no audio, no threads. Perfect as the objective function.
- `DetectionProfile` JSON — the *target* profile: the naturalizer optimizes toward the
  "human" side of every threshold in the profile it wants to defeat.
- JUCE — audio file I/O only.

**New components:**
- `Perturbation` — a parameterized audio transform with: `apply(buffer, params)`,
  `perceptualCost(params)` estimate, and the set of features it moves.
- `Optimizer` — greedy coordinate descent (see §5).
- `PerceptualGuard` — hard limits per perturbation + a global budget (§6).

## 4. Feature → perturbation map

Derived directly from DAAT's evaluators (`Source/Features/*`). "Suspicious direction"
is what the detector flags; "counter-move" is the naturalizer's response.

| # | Feature (DAAT id) | Suspicious direction | Counter-move | Perceptual guardrail |
|---|---|---|---|---|
| 1 | `spectralCentroid` | outside 400–7000 Hz | Centroid too high → gentle high-shelf cut; too low → subtle air-band lift (tilt EQ) | ±2.5 dB tilt max; re-check after |
| 2 | `spectralFlatness` | far from human mean 0.21 (sd 0.08); hard flags <0.02 / >0.65 | Too tonal (low flatness, typical of clean synth renders) → add shaped dither noise at −60 dBFS shaped to the signal spectrum; too noisy → light harmonic saturation | Noise floor lift ≤ +3 dB in any band |
| 3 | `spectralFlux` | < 0.05 (too static over time) | Slow random timbral modulation: ±1 dB wandering peaking filter, or micro-chorus (4–8 ms, 0.1 Hz) to raise frame-to-frame flux | Modulation depth inaudible as effect; no audible pitch wobble |
| 4 | `highFrequencyRatio` | outside 0.002–0.45 above 12 kHz | AI renders often band-limited → add synthesized "air" (filtered noise following the signal envelope); excess → gentle lowpass | Added HF must track envelope (no constant hiss) |
| 5 | `crestFactor` | < 6 dB (heavily limited) | Upward expansion on peaks: 1.1:1–1.3:1 above −12 dBFS, multiband on the 2–8 kHz band where limiting is most audible | No new clipping; true-peak ≤ −1 dBTP |
| 6 | `transientVariance` | low (uniform transients) | Detect transients, apply small random gain variation (±0.5–1.5 dB) per transient — "humanized" hits | Variation below just-noticeable difference for isolated hits |
| 7 | `stereoCorrelation` | > 0.985 (near-mono / phase-locked) | Decorrelate: independent micro-modulation per channel, Haas micro-delay (≤0.6 ms) on one channel, or slight M/S EQ divergence | Mono-compatibility check: summed mono must not comb-filter audibly |
| 8 | `midSideRatio` | outside 0.02–1.4 | M/S rebalance EQ toward the profile's mid-range | ≤2 dB M/S correction |
| 9 | `noiseFloorStationarity` | > 0.82 (frozen noise floor) | Replace/augment the floor with slowly drifting room tone: shaped noise with wandering level (±2 dB over 10–30 s) and wandering spectrum | Floor stays ≥ 12 dB below program at all times |
| 10 | `microRepetition` | > 0.76 (looped / exact repeats) | Break exact repetition: alternate loop iterations with micro pitch/time jitter (±4 cents, ±0.3 ms), subtle per-iteration EQ variation | Jitter below pitch/time perception thresholds |
| 11 | (vocal/fingerprint/model groups) | *not implemented in DAAT* | **Nothing to do** — but note: a detector with a real vocal or fingerprint group would likely catch everything above. This is the transfer-risk in concrete form. | — |

Two features deserve emphasis:
- **`noiseFloorStationarity` and `microRepetition` are the highest-leverage targets.**
  They flag the two most "digital" artifacts of generative audio (frozen noise, exact
  loops), and both can be moved a long way with nearly inaudible processing.
- **`spectralFlatness` is the most dangerous to touch.** Adding noise to fix flatness
  raises the noise floor, which interacts with feature 9. The optimizer must handle
  feature interactions, not treat them independently.

## 5. Optimization loop

Greedy coordinate descent over perturbation strengths, using DAAT's own scoring as the
loss — the attacker gets the defender's exact test suite:

1. Analyze input with `FeatureExtractor` + `combineGroups` → baseline likelihood L₀
   and per-feature suspicion vector.
2. Rank features by (suspicion × weight). For the top feature, try its counter-move
   at increasing strengths; re-analyze; keep the smallest strength that moves the
   feature's suspicion below 0.3 *without* pushing any other feature's suspicion up
   by more than 0.1 (interaction guard).
3. Repeat until overall likelihood < (threshold − 0.08 margin) or perceptual budget
   exhausted.
4. **Refusal rule:** if the budget is exhausted and likelihood is still above
   threshold, output nothing and report which features could not be moved. Never
   ship audible damage to hit a number.

Complexity is manageable: 11 features, each evaluated on windowed FFTs. A 3-minute
stereo file at 3 scales is seconds per analysis pass on a modern CPU; a full
optimization run should be under 2–3 minutes. Profile the `FeatureExtractor` first —
if it's the bottleneck, cache per-window raw features and only recompute windows
touched by the current perturbation.

## 6. Perceptual guard design

Each `Perturbation` declares hard limits (the right column of the §4 table). On top:
- **Global budget:** e.g. total spectral deviation ≤ 3 dB RMS vs. original, no
  sample clipped, true peak ≤ −1 dBTP, mono-sum correlation ≥ 0.9 of original.
- **Verification:** every accepted parameter set must pass an ABX-style automated
  check — at minimum, a perceptual distance metric (e.g. PEAQ-style or a simple
  masking-model distance) below a calibrated threshold. Human ABX on a validation
  set before any release.
- The guard is **not tunable by the end user** in v1. The refusal rule is a safety
  property, not a preference.

## 7. Evaluation protocol (before calling it done)

1. **Self-evasion:** 50 AI-generated tracks across 3+ generators (music models, not
   one) → % pushed below DAAT's default threshold, with perceptual distance scores.
2. **Transfer test:** run outputs through at least one *independent* AI-audio
   detector. Report the number honestly — expect it to be worse than (1), publish it
   anyway. This is the credibility test.
3. **Human test:** ABX on 20 pairs (original vs. naturalized), n ≥ 10 listeners.
   Target: statistically indistinguishable.
4. **Regression:** human-made control tracks through the tool must not become
   *more* suspicious (the perturbations shouldn't add "AI-ness").

## 8. Build plan

- **Phase 1 — Harness.** Link DAAT's `FeatureExtractor` + `DetectionEngine` as a
  static lib into a CLI tool. CLI: `naturalize input.wav output.wav --profile
  default.json --target 0.5`. Verify it can reproduce DAAT's likelihood numbers
  on unmodified files (sanity: identical scores).
- **Phase 2 — Perturbations.** Implement the §4 counter-moves one at a time, each
  with a unit test showing it moves its target feature's raw measurement in the
  intended direction on synthetic test signals.
- **Phase 3 — Optimizer + guard.** Coordinate descent, interaction guard, refusal
  rule, perceptual budget.
- **Phase 4 — Evaluation.** The §7 protocol. Publish the transfer-test numbers even
  if they're bad.
- **Later:** plugin version, batch folder mode, profile presets per genre
  (thresholds for "human" differ between techno and folk).

## 9. Suggested repo setup

- New repo, e.g. `bigluckyflavor/AINaturalize` — do **not** put this in AIDetect.
- README states plainly what it does and what it doesn't promise (no transfer
  guarantees, arms-race disclaimer).
- DAAT consumed as a git submodule or vendored static lib — single source of truth
  for the feature definitions, so the naturalizer can never silently drift from the
  detector it's measured against.
