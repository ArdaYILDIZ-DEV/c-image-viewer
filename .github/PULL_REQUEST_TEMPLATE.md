## Description
Summary of the changes introduced by this pull request.

## Type of Change
- [ ] Bug fix (non-breaking change fixing an issue)
- [ ] New feature (non-breaking change adding functionality)
- [ ] Performance improvement
- [ ] Refactoring / Code hygiene
- [ ] Documentation update
- [ ] Test suite addition or update

## Motivation and Context
Why is this change required? What problem does it solve? If it fixes an open issue, link it here (e.g. Fixes #123).

## Architecture & Quality Checklist
- [ ] Strictly follows the Pitchfork Layout standard (source in `src/`, headers in `include/`, vendor in `external/`, tests in `tests/`, assets in `assets/`).
- [ ] Out-of-source build: all artifacts reside in `build/`.
- [ ] Compiles cleanly with `gcc -O2 -Wall -Wextra -Wpedantic -std=c11` without warnings.
- [ ] All tests pass via `make test` under AddressSanitizer and UndefinedBehaviorSanitizer.
- [ ] Every exported function has purpose-driven documentation blocks conforming to AGENTS.md.
- [ ] No emojis in comments or commit messages.
- [ ] Conventional Commit format used.
