# bullet

[English](README.md) · [日本語](README.ja.md) · **简体中文** · [한국어](README.ko.md)

bullet 是一款将 YouTube 和 Twitch 聊天回放烧录到视频上的命令行工具。文字、PNG/JPEG/WebP 图片表情和动态 GIF 会从画面上滚过。

## 开始使用

按照下文构建 bullet，并将 `bullet.exe`、[FFmpeg](https://ffmpeg.org/download.html) 和 ffprobe 加入 `PATH`。下载视频使用 [yt-dlp](https://github.com/yt-dlp/yt-dlp/releases)，下载 Twitch 聊天记录还使用 [TwitchDownloaderCLI](https://github.com/lay295/TwitchDownloader/releases)。将这些工具加入 `PATH`，或通过 `YT_DLP` 和 `TWITCH_DOWNLOADER_CLI` 指定可执行文件路径。

```sh
bullet download "https://www.twitch.tv/videos/123456789"
bullet render data/v123456789.mp4
```

生成的视频位于 `data/v123456789.bullet.mp4`。对于 YouTube，下载视频后，将 `download` 显示的视频路径传给 `render`。

```sh
bullet download "https://www.youtube.com/watch?v=VIDEO_ID"
bullet render data/VIDEO_ID.mp4
```

`render VIDEO` 会查找视频旁的聊天文件，输出 `<名称>.bullet.mp4`。也可以自行指定输入和输出路径。

```sh
bullet render VIDEO CHAT --output OUTPUT.mp4
bullet render VIDEO --start 60 --duration 15 --output clip.mp4
```

使用 `--font` 指定字体、`--force` 替换已有输出。运行 `bullet --help` 查看所有选项。Windows 默认使用 Meiryo Bold。获取的表情图片存放在聊天文件旁的 `assets/` 中；音频流直接复制到输出视频。输出采用原视频的平均帧率，生成恒定帧率的视频。

表情验证使用输出文件旁的专用临时文件。新缓存条目保留经过验证的原始编码数据，并通过 `assets/` 内的专用临时目录完成写入，因此缓存和输出可以位于不同的文件系统。读取已有缓存不需要写入缓存，也支持只读缓存。

为减少整像素取整导致的抖动，滚动文字、图片和 GIF 采用子像素定位。显示时间和横穿速度保持不变。

## 构建

Windows x64 构建使用 CMake 3.24+、C 编译器和 [vcpkg](https://github.com/microsoft/vcpkg)。设置 `VCPKG_ROOT`，在 Visual Studio Developer PowerShell 中运行：

```powershell
cmake -S . -B build "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

可执行文件位于 `build/Release/bullet.exe`。测试使用 FFmpeg 和 ffprobe。Windows CI 构建可执行文件，并将发行所需的依赖库许可声明收集到 `build/notices/`。开发约定见 [AGENTS.md](AGENTS.md)。

## 许可证

bullet 使用 [MIT 许可证](LICENSE)。
