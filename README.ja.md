# bullet

[English](README.md) · **日本語** · [简体中文](README.zh-CN.md) · [한국어](README.ko.md)

bulletは、YouTube・Twitchの動画にリプレイチャットを焼き込むCLIです。文字・PNG/JPEG/WebPの絵文字・アニメーションGIFを動画上に流します。

## 使い方

下記の手順でbulletをビルドし、`bullet.exe`、[FFmpeg](https://ffmpeg.org/download.html)、ffprobeを`PATH`に置きます。動画の取得には[yt-dlp](https://github.com/yt-dlp/yt-dlp/releases)、Twitchチャットの取得には[TwitchDownloaderCLI](https://github.com/lay295/TwitchDownloader/releases)も使います。これらを`PATH`に置くか、`YT_DLP`と`TWITCH_DOWNLOADER_CLI`に実行ファイルのパスを設定してください。

```sh
bullet download "https://www.twitch.tv/videos/123456789"
bullet render data/v123456789.mp4
```

完成動画は`data/v123456789.bullet.mp4`です。YouTubeでは動画URLを取得して、`download`が表示した動画のパスを`render`に渡します。

```sh
bullet download "https://www.youtube.com/watch?v=VIDEO_ID"
bullet render data/VIDEO_ID.mp4
```

`render VIDEO`は動画と同じ場所にあるチャットを探し、`<名前>.bullet.mp4`を出力します。入力と出力のパスを指定することもできます。

```sh
bullet render VIDEO CHAT --output OUTPUT.mp4
bullet render VIDEO --start 60 --duration 15 --output clip.mp4
```

フォントの指定には`--font`、既存の出力の置換には`--force`を使います。全オプションは`bullet --help`で確認できます。Windowsの既定フォントはメイリオ太字です。取得した絵文字はチャットの隣の`assets/`に保存し、音声はそのままコピーします。出力は元動画の平均フレームレートを使った一定フレームレートです。

絵文字の検証には出力先の隣に専用の一時ファイルを作ります。新しいキャッシュには検証済みの元の符号化データを保存し、`assets/`内の専用一時ディレクトリから確定します。キャッシュと出力先が別のファイルシステムでも使えます。既存のキャッシュの読み込みには書き込みは不要で、読み取り専用でも使えます。

絵文字の幅はURLごとに、チャットのメタデータで最初に現れる有効な縦横比と描画時の絵文字の高さから決まり、丸めた結果を最低1ピクセルにします。同じURLの後続の有効なメタデータでは幅を変更しません。不正な縦横比は同じURLでも拒否します。静止画像とGIFの全フレームを、元の画像サイズにかかわらずこの幅と絵文字の高さに揃えます。表示される絵文字だけをデコードします。

整数ピクセルへの丸めによる揺れを抑えるため、文字・画像・GIFの横移動にはサブピクセル描画を使います。表示時刻と横断速度は変わりません。

## ビルド

Windows x64ではCMake 3.24以上、Cコンパイラー、[vcpkg](https://github.com/microsoft/vcpkg)を使用します。`VCPKG_ROOT`を設定し、Visual Studio Developer PowerShellで実行してください。

```powershell
cmake -S . -B build "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

実行ファイルは`build/Release/bullet.exe`です。テストではFFmpegとffprobeを使います。Windows CIは実行ファイルをビルドし、配布時に使う依存ライブラリのライセンス通知を`build/notices/`に集めます。開発方針は[AGENTS.md](AGENTS.md)にあります。

## ライセンス

bulletは[MITライセンス](LICENSE)で公開しています。
