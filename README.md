# Wayland wlroots video source

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

GST_PLUGIN_PATH=/usr/local/lib/gstreamer-1.0 GST_DEBUG_DUMP_DOT_DIR=tmp gst-launch-1.0 wlrsrc dmabuf=true ! queue ! vapostproc ! vah264enc bitrate=3000 ! vah264dec ! vapostproc ! waylandsink
```

## TODO

- Fix the drop in the first few frames
- Support multi-display setups
- Output formats other than BGRx
- Fix various bugs
- Implement a time stamp

## References

- [screencopy-dmabuf.c](https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/0.16/examples/screencopy-dmabuf.c)
- [wayland document](https://wayland.app)

## Screenshot

![](https://i.imgur.com/Hi8b0VW.png)