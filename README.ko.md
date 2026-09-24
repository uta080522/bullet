# bullet

[English](README.md) · [日本語](README.ja.md) · [简体中文](README.zh-CN.md) · **한국어**

bullet은 YouTube와 Twitch 다시보기 채팅을 동영상에 입히는 CLI 도구입니다. 텍스트, PNG/JPEG/WebP 이모티콘, 애니메이션 GIF가 영상 위를 가로질러 흐릅니다.

## 시작하기

아래 방법으로 bullet을 빌드하고 `bullet.exe`, [FFmpeg](https://ffmpeg.org/download.html), ffprobe를 `PATH`에 추가하세요. 동영상 다운로드에는 [yt-dlp](https://github.com/yt-dlp/yt-dlp/releases)를, Twitch 채팅 다운로드에는 [TwitchDownloaderCLI](https://github.com/lay295/TwitchDownloader/releases)도 사용합니다. 도구를 `PATH`에 추가하거나 `YT_DLP`과 `TWITCH_DOWNLOADER_CLI`에 실행 파일 경로를 설정하세요.

```sh
bullet download "https://www.twitch.tv/videos/123456789"
bullet render data/v123456789.mp4
```

완성된 영상은 `data/v123456789.bullet.mp4`입니다. YouTube는 동영상 URL을 다운로드한 다음 `download`가 표시한 영상 경로를 `render`에 전달하세요.

```sh
bullet download "https://www.youtube.com/watch?v=VIDEO_ID"
bullet render data/VIDEO_ID.mp4
```

`render VIDEO`는 동영상 옆에서 채팅 파일을 찾아 `<이름>.bullet.mp4`를 출력합니다. 입력과 출력 경로를 직접 지정할 수도 있습니다.

```sh
bullet render VIDEO CHAT --output OUTPUT.mp4
bullet render VIDEO --start 60 --duration 15 --output clip.mp4
```

글꼴을 고르려면 `--font`, 기존 출력을 교체하려면 `--force`를 사용하세요. 전체 옵션은 `bullet --help`에서 볼 수 있습니다. Windows 기본 글꼴은 Meiryo Bold입니다. 받은 이모티콘은 채팅 파일 옆 `assets/`에 보관하고 오디오는 그대로 복사합니다. 원본 동영상의 평균 프레임 레이트를 사용해 고정 프레임 레이트로 출력합니다.

## 빌드

Windows x64 빌드에는 CMake 3.24 이상, C 컴파일러, [vcpkg](https://github.com/microsoft/vcpkg)를 사용합니다. `VCPKG_ROOT`를 설정하고 Visual Studio Developer PowerShell에서 실행하세요.

```powershell
cmake -S . -B build "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

실행 파일은 `build/Release/bullet.exe`입니다. 테스트에는 FFmpeg와 ffprobe를 사용합니다. Windows CI는 실행 파일을 빌드하고 배포에 필요한 의존 라이브러리 라이선스 고지문을 `build/notices/`에 모읍니다. 개발 지침은 [AGENTS.md](AGENTS.md)에 있습니다.

## 라이선스

bullet은 [MIT 라이선스](LICENSE)로 배포합니다.
