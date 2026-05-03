# OverlAIer

Real-time audio/video overlay pipeline with optional USB HID proxying. Video comes in through V4L2, overlays are produced by pluggable processors, output is presented through DRM/KMS, audio is passed through with adaptive resampling, and selected USB HID devices can be forwarded through Linux USB gadget mode.

## Build

- Build system: CMake 3.25+
- Language standard: C23
- Target platform: Linux
- Main binary: `overlAIer`
- Plugin format: shared libraries loaded at runtime via `dlopen`

Primary commands:

```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target deploy
```

Useful targets:

- `deploy`: copies the executable, default config, and processor plugins into repo-local `build/`
- `format`: runs `clang-format` across the source tree if available
- `format-check`: verifies formatting

If `clang-tidy` is installed, the build wires it into the main target automatically.

## Repo Layout

```text
overlaier/
├── CMakeLists.txt
├── Dockerfile
├── README.md
├── AGENTS.md
├── pipeline/
│   ├── CMakeLists.txt
│   ├── main.c
│   ├── common/
│   │   ├── config.c/h
│   │   ├── edid.c/h
│   │   ├── log.h
│   │   ├── options.c/h
│   │   └── pixfmt.c/h
│   ├── receiver/
│   │   ├── alsa_capture.c/h
│   │   ├── alsa_caps.c/h
│   │   ├── receiver.c/h
│   │   ├── v4l2_capture.c/h
│   │   └── v4l2_caps.c/h
│   ├── encoder/
│   │   ├── alsa_playback.c/h
│   │   ├── drm_caps.c/h
│   │   └── drm_output.c/h
│   ├── converter/
│   │   ├── converter.c/h
│   │   ├── converter_rga.c/h
│   │   └── converter_sw.c/h
│   ├── overlay/
│   │   └── overlay.c/h
│   ├── processor/
│   │   ├── processor.h
│   │   └── processor_mgr.c/h
│   └── usb_proxy/
│       ├── gadget_configfs.c/h
│       ├── usb_caps.c/h
│       └── usb_proxy.c/h
├── processors/
│   ├── CMakeLists.txt
│   └── fps_counter/
│       ├── fps_counter.c
│       └── fps_counter.h
└── installer/
    ├── 99-overlAIer.rules
    ├── install.sh
    ├── overlAIer-system.service
    ├── overlAIer-system-config.path
    ├── overlAIer-system-config-reload.service
    ├── overlAIer-web.service
    └── overlAIer.toml.example
```

Note: the installer script emits these unit files directly from embedded templates, so treat `installer/` as both reference artifacts and packaging inputs. The pipeline runs as the system unit `overlAIer-system.service` (with `AmbientCapabilities=CAP_SYS_ADMIN CAP_SYS_RAWIO` and `SupplementaryGroups=input` so it can drive ConfigFS / hidg / evdev without full root); the web UI keeps its user-mode unit. A system-level `.path` unit watches `~/.overlAIer/overlAIer.toml` and triggers a oneshot that restarts the pipeline on change.

## Runtime Architecture

### Video path

1. `receiver/v4l2_capture.c` captures frames from the HDMI or V4L2 source.
2. `common/edid.c` can synthesize and write a passthrough EDID based on the selected output mode.
3. `converter/` inserts software or RGA conversion when the capture and display formats do not match.
4. `processor/processor_mgr.c` fans frames out to plugin threads.
5. `overlay/overlay.c` renders aggregated overlay primitives into an ARGB buffer.
6. `encoder/drm_output.c` presents the video frame on the primary plane and overlay on a separate plane.

### Audio path

1. `receiver/alsa_capture.c` captures PCM from the selected ALSA input.
2. Adaptive resampling tracks clock drift between capture and playback domains.
3. `encoder/alsa_playback.c` writes the corrected stream to the selected ALSA output.

### USB HID proxy path

1. `usb_proxy/usb_caps.c` enumerates USB-backed evdev devices and classifies them as keyboard, mouse, gamepad, or other.
2. `usb_proxy/usb_proxy.c` opens matching evdev devices, converts events into boot-format HID reports, and forwards them to gadget endpoints.
3. `usb_proxy/gadget_configfs.c` creates and tears down HID gadget functions under ConfigFS.
4. Processor plugins may intercept and modify HID reports through `on_hid_report`.

## Processors

Processors are explicit runtime plugins configured in `overlAIer.toml` through `[[processor]]` entries. The program fails startup if a configured plugin cannot be loaded.

Each processor runs in its own thread and receives frames through a mailbox. The current manager behavior matters:

- Frame delivery is non-blocking from the video thread’s point of view.
- Incoming frame data is copied per processor before `on_frame`.
- Overlay primitives from all processors are rendered in registration order.
- `on_flip` is called synchronously when a DRM flip completes if the processor provides it.
- `on_hid_report` forms a sequential filter chain for proxied HID reports.

Current processor interface:

```c
struct ovl_frame_info {
    uint32_t sequence;
    uint64_t timestamp_us;
    uint32_t width, height;
    uint32_t stride;
};

struct ovl_processor_def {
    const char *name;
    void *(*init)(const struct ovl_processor_def *def,
                  uint32_t width, uint32_t height,
                  enum ovl_pixfmt format);
    void (*on_frame)(void *state, const void *frame_data,
                     const struct ovl_frame_info *info,
                     struct ovl_primitive **prims_out, int *count_out);
    void (*on_flip)(void *state, const struct ovl_frame_info *info);
    int (*on_hid_report)(void *state, uint8_t *report, int *report_len,
                         const char *device_name, uint16_t vid, uint16_t pid);
    void (*destroy)(void *state);
};
```

Export symbol:

```c
const struct ovl_processor_def *ovl_processor_register(void);
```

The in-tree `fps_counter` plugin is the reference implementation for video overlays. There is no in-tree HID mutator plugin yet.

## Configuration Model

Config is TOML, loaded from `overlAIer.toml` next to the executable unless `--config` overrides it. CLI options take precedence over config.

Active sections in the current parser:

- `[device]`: `video_in`, `video_out`, `audio_in`, `audio_out`
- `[format]`: `fmt_in`, `fmt_out`, `res_in`, `res_out`, `fps_in`, `fps_out`
- `[general]`: `log_level`, `async_flip`
- `[[processor]]`: `path`
- `[usb]`: `udc`
- `[[usb.device]]`: `vid_pid`

## Query Surface

`overlAIer query` reports:

- V4L2 input capabilities
- DRM/KMS output capabilities
- ALSA capture/playback capabilities
- Converter backends and supported format pairs
- Enumerated USB HID inputs

Plain text and JSON output are both supported through `--format plain|json`.

## Design Constraints

- Favor low-latency paths first; conversion is optional and inserted only when required.
- The overlay system works in normalized coordinates and is rendered separately from the main video plane.
- USB proxying is independent from the video signal path and can be initialized before capture comes up.
- The current USB implementation targets boot keyboard and boot mouse style forwarding, not arbitrary HID descriptor passthrough.
- The project can compile without RGA; software conversion remains available.

## Editing Guidance

- Keep docs aligned with code, especially CLI help in `pipeline/main.c`, config parsing in `pipeline/common/config.c`, and plugin ABI in `pipeline/processor/processor.h`.
- Be careful around `pipeline/usb_proxy/`: this area is currently under active modification in the worktree.
- Do not document processor input negotiation features that do not exist in code yet; the current processor manager always feeds capture-format frames.
