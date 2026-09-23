# Validation

## Evidence model

Validation is layered. Compilation proves source compatibility; automated tests prove their covered contracts; integration and hardware tests prove only the environments actually exercised.

## Automated gates

- .github/workflows/portable-ci.yml
- .github/workflows/windows-wdk-ci.yml
- .github/workflows/hardware-submission-ci.yml
- .github/workflows/publish-release.yml

Portable tests exercise image parsing and mutation contracts; Windows tests cover host readiness, installer contracts, filesystem contracts and Driver Verifier support.

## Manual/environment-dependent evidence

Production Microsoft signing, Secure Boot loading and destructive filesystem mutation require real Windows/hardware evidence beyond portable CI.

A simulated, fixture-driven or hosted result must not be described as proof of a physical-device, destructive-media or boot-path result.

## Release criterion

The exact release revision must pass its required gates, and generated assets must correspond to that revision. Known unsupported or failing behaviour remains documented as such.

## Regression rule

Reproducible defects should gain permanent regression coverage at the narrowest layer that captures the original failure. Validation documentation should distinguish automatic release blockers from optional, manual or milestone evidence.
