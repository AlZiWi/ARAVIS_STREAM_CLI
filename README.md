# ARAVIS_STREAM_CLI

macOS CLI for streaming and recording GenICam cameras through the
[Aravis](https://aravis-project.discourse.group/) library. It supports device
listing, camera selection by model/serial/device ID, resolution, frame rate,
exposure, JPEG or raw frame recording, binary frame streaming, diagnostics, and an
optional OpenCV preview. Trigger control is intentionally not implemented.

## Requirements

- macOS
- Xcode Command Line Tools
- CMake 3.16 or newer
- Homebrew

Install Aravis with:

```sh
brew install aravis
```

JPEG output also requires libjpeg:

```sh
brew install jpeg
```

The preview is optional:

```sh
brew install opencv
```

## Build

```sh
cmake -S . -B build
cmake --build build
```

To include the preview:

```sh
cmake -S . -B build-preview -DARAVIS_ENABLE_PREVIEW=ON
cmake --build build-preview
```

The resulting executable is `build/aravis_stream_cli` or
`build-preview/aravis_stream_cli`.

## Usage

List cameras:

```sh
./build/aravis_stream_cli -list
```

Capture a camera by its model, serial number, or device ID:

```sh
./build/aravis_stream_cli "Camera Name" -resx=1920 -resy=1080 \
  -fps=30 -exposure=1000 -out=frames
```

Output defaults to JPEG-compressed frames. Select raw payloads explicitly:

```sh
./build/aravis_stream_cli "Camera Name" -format=raw -out=frames
```

The `-format=mjpeg` and `-format=raw` choices apply to both `-out` files and
`-stream` records. JPEG files use the `.jpg` extension, while raw files use
`.raw`. JPEG encoding supports Mono8, RGB8 packed, and BGR8 packed camera
formats; unsupported formats fail explicitly.

The exposure value uses 0.0001-second units, matching the original UVC CLI;
Aravis receives the converted microsecond value. Use `-exposure auto` for
continuous automatic exposure. `-stream` writes an `ARVF` binary record for
each frame to standard output. The record's `frame_bytes` and payload contain
the selected representation; `frame_format` contains the Aravis pixel-format
value for raw records or the `MJPG` fourcc (`0x47504a4d`) for JPEG records.

When built with preview support:

```sh
./build-preview/aravis_stream_cli "Camera Name" -preview
```

Press Q to quit the preview.
