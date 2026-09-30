# AINaturalizer — C++ prototype

Offline batch tool that modifies AI-generated music so heuristic detectors
score it as human-made. See `../DESIGN.md` for the full design.

## Layout

- `src/main.cpp` — CLI (`analyze` / naturalize modes)
- `src/naturalizer/Oracle.{h,cpp}` — drives DAAT's real `FeatureExtractor` +
  feature evaluators + `DetectionEngine` as the loss function
- `src/naturalizer/Perturb.{h,cpp}` — 8 counter-perturbation operators
- `src/naturalizer/Optimize.{h,cpp}` — greedy coordinate descent with
  perceptual budget + refusal rule
- `third_party/daat/` — **unmodified** DAAT AI Audio Inspector sources
  (measurement oracle only; no UI, no plugin shell)
- `tests/make_fixture.py` — synthesizes a deliberately "AI-sounding" fixture

## Build

Requires CMake ≥ 3.24, a C++17 compiler, and internet access (JUCE is
fetched at configure time, pinned release).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

## Run

```sh
# Inspect what DAAT thinks of a file:
./build/naturalizer_artefacts/naturalizer analyze tests/ai_like.wav

# Naturalize it:
./build/naturalizer_artefacts/naturalizer tests/ai_like.wav out.wav \
    --target 0.35 --max-iters 12 --budget 6.0 --seed 1234
```

Exit code 0 = target reached, 2 = refused (see reason in output).

## Notes

- Kept as a separate tool from DAAT on purpose: DAAT stays an honest
  forensics instrument; this is the adversarial counterpart.
- Evasion is measured against DAAT's own 11 features. Transfer to
  independent detectors is *not* claimed — that needs the evaluation
  protocol in `DESIGN.md` §5.

## Verified prototype results (2026-09-29)

Fixture: `tests/make_fixture.py` synthesizes a 24 s "AI-sounding" track
(perfect 4 s loop, 1.5 dB crest, dual mono, constant −72 dBFS noise floor).

| run | likelihood | confidence | steps |
|---|---|---|---|
| input | 0.568 | 0.34 | — |
| `--target 0.35` | **0.277** | 0.41 | stereoWiden@0.7 |
| `--target 0.15` | **0.120** | 0.57 | stereoWiden@0.7, transientHumanize@1.0 ×2 |

- Suspicion cleared: stereoCorrelation 1.0→0, midSideRatio 1.0→0,
  crestFactor 1.0→0, noiseFloorStationarity 0.53→0; microRepetition
  0.97→0.70 (partial — exact loop survives, audibly weakened).
- Audio delta (deep run): peak −1.8 dBFS, RMS −9.7 dBFS; level-normalized
  RMS delta −15.7 dBFS — the stereo widening is clearly audible as a
  mastering-style change, not transparent. The perceptual dose model is
  still crude; treat `costPerUnit` as a placeholder.
- Deterministic: identical seed ⇒ bit-identical output (md5-verified).
- Refusal rule verified: `--budget 0.1` exits 2 with
  "no candidate fit the remaining perceptual budget".

Known prototype limitations: greedy search only (no lookahead), 8 s
medium-scale windows only (DAAT uses 3 scales), no loudness
normalization between trials, dose model uncalibrated, transfer to other
detectors untested.
