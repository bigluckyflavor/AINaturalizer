# AINaturalizer

An offline batch research tool that modifies AI-generated music so DAAT's
implemented heuristic feature set produces a lower synthetic-likelihood score.

This is the adversarial counterpart to the DAAT AI Audio Inspector, kept as a
**separate project on purpose** — DAAT remains the forensics instrument; this
project explores the other side of the measurement/perturbation arms race.

- **[DESIGN.md](DESIGN.md)** — threat model, feature-to-perturbation map,
  optimizer target, perceptual guard design, refusal rule, and evaluation protocol.
- **[code/](code/)** — C++17 prototype. Vendors DAAT's measurement sources and
  mirrors the factory three-scale scoring path, then runs greedy coordinate
  descent over 8 perturbation operators.
- **[chats.md](chats.md)** — shared Claude / Muse / ChatGPT / Gemini engineering
  log: current priorities, completed work, review requests, and bloat-removal notes.

## Status

Working research prototype (updated 2026-09-30).

The original synthesized-fixture results (approximately 0.57 → 0.28 → 0.12)
were produced before an audit found that the first naturalizer oracle used only
DAAT's medium window scale and different aggregation/resampling behavior. Those
numbers are historical development results, not current validation claims.

The audit corrections are now merged into `main`: the oracle mirrors DAAT's
short/medium/long factory analysis path, success is verdict-aware, candidate
interaction/sample-safety guards are active, refusal writes no output, and
Windows/Linux build smoke CI is part of the repository.

## Honest limits

- DAAT currently has **10 implemented heuristic features** in this vendored
  measurement path. Vocal, fingerprint, and model groups are architectural
  placeholders rather than implemented evidence sources.
- A direct automated equivalence harness against DAAT's production
  `AnalysisEngine` is still required before new self-evasion numbers should be
  published.
- Transfer to independent detectors is **not** claimed.
- The perceptual dose model remains uncalibrated. It is a search heuristic, not
  proof that processing is transparent or perceptually safe.
- Some perturbations can be audible mastering-style changes; objective and human
  validation remain part of the release criteria.

See [code/README.md](code/README.md) for build/run details and current technical
limitations.
