# Development

bullet is a C99 CLI. Keep the application in `bullet.c`, native tests in `test.c`, and build files at the repository root. Build artifacts belong in `build/`; downloaded media and emote caches belong in `data/`.

## Code

- Use tabs and `.clang-format`. Make internal functions `static` and keep ownership and time units explicit.
- Render text, images and animated GIFs from the same microsecond clock onto one RGBA layer. Preserve each message's time and crossing speed, including when lanes overlap.
- Use SDL3, SDL3_image, SDL3_ttf, cJSON, libcurl and the existing FFmpeg/download tools for their respective tasks.
- Check sizes, arithmetic, process exits and file operations. Create temporary files in an exclusively owned directory and preserve source files and existing output on failure.
- Keep `download` and `render` usable from the CLI. Place default behavior and resource limits near the top of `bullet.c`.

## Checks

Build with warnings as errors. After changes, run the Release build and `ctest --test-dir build -C Release --output-on-failure`, then check `clang-format --dry-run --Werror bullet.c test.c`. Run the sanitizer build for C changes. `test.c` provides local media fixtures and fake downloaders; use those fixtures for destructive cases. Keep user media and caches intact.

Update all four READMEs when changing commands, dependencies, or behavior that users need to know.
