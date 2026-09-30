# bullet

**English** · [日本語](README.ja.md) · [简体中文](README.zh-CN.md) · [한국어](README.ko.md)

bullet adds replay chat to YouTube and Twitch videos. Messages scroll across the video as text, PNG/JPEG/WebP emotes, and animated GIFs.

## Getting started

Build bullet using the instructions below. Add `bullet.exe`, [FFmpeg](https://ffmpeg.org/download.html), and ffprobe to your `PATH`. Downloads use [yt-dlp](https://github.com/yt-dlp/yt-dlp/releases); Twitch chat downloads also use [TwitchDownloaderCLI](https://github.com/lay295/TwitchDownloader/releases). Put those tools on `PATH`, or set `YT_DLP` and `TWITCH_DOWNLOADER_CLI` to their executable paths.

```sh
bullet download "https://www.twitch.tv/videos/123456789"
bullet render data/v123456789.mp4
```

The result is `data/v123456789.bullet.mp4`. For YouTube, download a video URL and pass the video path printed by `download` to `render`:

```sh
bullet download "https://www.youtube.com/watch?v=VIDEO_ID"
bullet render data/VIDEO_ID.mp4
```

`render VIDEO` finds its chat file beside the video and writes `<name>.bullet.mp4`. You can also choose the input and output paths yourself:

```sh
bullet render VIDEO CHAT --output OUTPUT.mp4
bullet render VIDEO --start 60 --duration 15 --output clip.mp4
```

Use `--font` to select a font, `--force` to replace an existing output, and `bullet --help` for all options. Windows uses Meiryo Bold by default. bullet saves downloaded emotes in `assets/` next to the chat file; it copies the audio stream into the output. The source video's average frame rate determines the output's constant frame rate.

Scrolling text, images and GIFs use subpixel positioning to reduce jitter from whole-pixel rounding. Their timing and crossing speed are unchanged.

## Building

Windows x64 builds use CMake 3.24+, a C compiler, and [vcpkg](https://github.com/microsoft/vcpkg). Set `VCPKG_ROOT` and run these commands in a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable is `build/Release/bullet.exe`. Tests use FFmpeg and ffprobe. The Windows workflow builds the executable and collects dependency license notices in `build/notices/` for distribution. Development conventions are in [AGENTS.md](AGENTS.md).

## License

bullet is [MIT licensed](LICENSE).
