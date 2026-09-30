# Vendored DAAT sources — porting notes

Copied verbatim from the DAAT AI Audio Inspector `Source/` tree on
2026-09-29, except for the compatibility patches listed below. Only the
measurement path is vendored (feature extractor, feature evaluators,
detection engine, profiles, registry); the plugin shell, worker, and UI
are not needed by the naturalizer.

## Patches

1. `Analysis/FeatureExtractor.h` — removed the `= {}` default argument from
   `FeatureExtractor::prepare`. A default argument of `{}` for a *nested*
   struct with default member initializers is ill-formed C++ (the NSDMIs are
   not complete until the end of the enclosing class; GCC ≥ 11 and Clang
   reject it — MSVC historically accepted it, which is presumably how the
   original compiled). All in-tree callers pass `Settings` explicitly, so
   nothing else changes.
