# Wayland wlroots video source

## What's this

A small GStreamer plugin for capturing the display on Wayland.

It requires support for the `zwlr_screencopy_manager_v1` (and `zwp_linux_dmabuf_v1` when using DMABuf) protocol(s) to capture.

You can check this with the command wayland-info.

Currently, this is highly experimental. Expect bugs, incomplete features, and unstable behavior.

Written in C++.

## Installation

To build the plugin, you need `wayland-scanner`.

```bash
git clone https://github.com/Callyth/gst-wlrsrc.git
cd gst-wlrsrc
# Set CMAKE_C_COMPILER
cmake -B build -S . -DCMAKE_C_COMPILER=/usr/bin/gcc
cmake --build build
# By default, .so library is installed in /usr/local/lib/gstreamer-1.0/
cmake --install build
```

## Usage

```bash
GST_PLUGIN_PATH=/usr/local/lib/gstreamer-1.0 gst-inspect-1.0 wlrsrc

#Note: You may need to insert queue elements between stages to avoid underflow.
GST_PLUGIN_PATH=/usr/local/lib/gstreamer-1.0 gst-launch-1.0 wlrsrc dmabuf=false show_cursor=false ! queue ! waylandsink

GST_PLUGIN_PATH=/usr/local/lib/gstreamer-1.0 gst-launch-1.0 wlrsrc dmabuf=true show_cursor=true ! video/x-raw,width=1920,height=1080 ! vaapipostproc ! vaapih264enc ! filesink location=test.mp4
```

## TODO

- Fix the drop in the first few frames
- Support multi-display setups
- Output formats other than BGRx
- Fix various bugs

## References

- [screencopy-dmabuf.c](https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/0.16/examples/screencopy-dmabuf.c)
- [wayland document](https://wayland.app)

## Screenshot

![](https://i.imgur.com/Hi8b0VW.png)