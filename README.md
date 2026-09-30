# AINaturalizer

An offline batch tool that modifies AI-generated music so heuristic
AI-audio detectors score it as human-made.

This is the adversarial counterpart to the DAAT AI Audio Inspector, kept
as a **separate project on purpose** — DAAT stays an honest forensics
instrument; this tool explores the other side of the arms race.

- **[DESIGN.md](DESIGN.md)** — full design: threat model, per-feature
  counter-perturbation map, optimizer, perceptual budget, refusal rule,
  and the 4-part evaluation protocol.
- **[code/](code/)** — C++17 prototype. Vendors DAAT's real feature
  extractor + 11 feature evaluators + detection engine as the measurement
  oracle (the loss function), then runs greedy coordinate descent over 8
  perturbation operators. See [code/README.md](code/README.md) for build
  instructions and verified results.

## Status

Working prototype (2026-09-29). On a synthesized AI-like fixture:
likelihood 0.57 → 0.28 (1 step) → 0.12 (3 steps), deterministic,
with a verified refusal rule.

## Honest limits

- Evasion is demonstrated against DAAT's own 11 heuristic features.
  Transfer to independent detectors is **not** claimed — see DESIGN.md §5.
- The perceptual dose model is uncalibrated; some operators are audibly
  a mastering-style change, not transparent processing.
