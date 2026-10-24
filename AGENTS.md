# TeraConvert contributor instructions

## Local coding skills

Community skills are installed in `skills/`. Read the relevant SKILL.md before matching work, and load referenced files only as needed:

- `skills/c-pro/SKILL.md`: C implementation, ownership, pointer bounds, POSIX error handling, and performance review.
- `skills/meson/SKILL.md`: Meson configuration, dependencies, build and tests.
- `skills/sanitizers/SKILL.md`: runtime memory safety and undefined behavior diagnostics.
- `skills/gdb/SKILL.md`: reproducible crashes, hangs and thread debugging.

Keep the existing GNU C11, GLib/GIO and Meson architecture. Adapt generic Makefile/CMake examples to Meson. Verify compiler flag support before using examples. Report tests actually run and distinguish environmental limitations from passing checks.

## GitHub account

Use the connected GitHub account `lpa261` for this project. Do not select `phucanhle`.
