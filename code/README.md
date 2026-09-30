# AINaturalizer — C++ prototype

Offline batch research tool that modifies AI-generated music so DAAT's
implemented heuristic feature set produces a lower synthetic-likelihood score.
See `../DESIGN.md` for the design target and evaluation protocol.

## Layout

- `src/main.cpp` — CLI (`analyze` / naturalize modes)
- `src/naturalizer/Oracle.{h,cpp}` — mirrors DAAT's three-scale factory-profile
  measurement/scoring path
- `src/naturalizer/Perturb.{h,cpp}` — 8 counter-perturbation operators
- `src/naturalizer/Optimize.{h,cpp}` — greedy coordinate descent with a
  heuristic dose budget, feature-interaction guard, sample-safety checks, and
  refusal rule
- `third_party/daat/` — vendored DAAT measurement sources plus the compatibility
  patch documented in `third_party/daat/PATCHES.md`
- `tests/make_fixture.py` — synthesizes a deliberately "AI-sounding" fixture

## Build

Requires CMake ≥ 3.24, a C++17 compiler, and internet access (JUCE is fetched
at configure time, pinned to release 8.0.6).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

## Run

```sh
# Inspect the current DAAT-mirroring oracle result:
./build/naturalizer analyze tests/ai_like.wav

# Naturalize it. Default target is DAAT factory "Unlikely" boundary: 0.28.
./build/naturalizer tests/ai_like.wav out.wav \
    --target 0.28 --max-iters 12 --budget 6.0 --seed 1234
```

Exit code 0 = target reached and output written.
Exit code 2 = refused and **no output is written**.
Invalid arguments/read/write failures return 1.

## Correctness changes after the 2026-09-29 audit

The original prototype oracle used only 8-second medium windows, confidence-
weighted feature aggregation, and linear resampling. That was not equivalent
to DAAT's production analysis path.

The corrected oracle now:
- evaluates short, medium, and long windows using DAAT's per-feature scale masks;
- aggregates feature suspicion the same way as DAAT's `finalizeResult`;
- uses JUCE `LagrangeInterpolator` in the same direction/ratio as DAAT;
- supplies duration, source sample rate, clipping, RMS, channel count, and
  minimum-duration information to DAAT's confidence calculation;
- exposes scorable state and DAAT verdict so insufficient audio is not mistaken
  for a successful low score.

The optimizer now rejects candidate moves that:
- create NaN/Inf samples;
- introduce new sample clipping beyond the input's existing peak allowance; or
- increase any tracked feature suspicion by more than 0.10.

The transient-humanization operator now derives its timing from the actual
sample rate rather than assuming 48 kHz.

## Important current limits

- DAAT currently has **10 implemented heuristic features**. Vocal, fingerprint,
  and model groups exist architecturally but have no implemented evidence path.
- The oracle now mirrors DAAT much more closely, but it is still duplicated
  scoring plumbing. A direct equivalence regression harness against DAAT's
  production `AnalysisEngine` remains the next correctness milestone.
- The dose budget is still a heuristic search cost, **not** a calibrated
  perceptual safety metric. True-peak, loudness, mono compatibility, spectral
  deviation, and perceptual-distance guards remain to be implemented.
- Transfer to independent detectors is untested.
- Reproducibility with the same seed is expected within the same build/toolchain;
  cross-standard-library bit identity is not promised because standard hashing
  and distribution details can vary.

## Historical prototype results

The earlier fixture results (0.568 → 0.277 → 0.120) were produced with the
pre-audit single-scale oracle. They are retained in repository history only and
must **not** be treated as current validation numbers. Re-run the fixture after
the direct DAAT-equivalence harness passes.

For ongoing engineering status and cross-model review, see `../chats.md`.
