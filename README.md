# Appeal FFmpeg Video plugin

Working decoder for Appeal_Module_Video using the FFmpeg shared C libraries. See the [module build/API guide](../README.md) and [plugin SDK](../src/Video_Plugin_API.h). Install the SDK with `python ../Packaging/Install_FFmpeg.py` and build with `make -f Makefile`.

The local FFmpeg 8.1 SDK lives under `.deps/ffmpeg`. Its license, full SDK docs/headers, and source URL/SHA-256 manifest are preserved there. Runtime DLLs must remain beside Appeal.exe when using this Windows build. The build does not launch ffmpeg.exe during playback; it directly links avformat/avcodec/swscale/swresample. ffmpeg.exe is used only to generate test fixtures.
