# OverlAIer

Real-time audio/video overlay processing pipeline. Receives A/V input, processes it through pluggable analyzers that produce overlay primitives, and outputs the composited result via DRM/KMS.

## Build

- **Build system**: CMake (minimum 4.3), C23 standard
- **Target platform**: Linux aarch64 (RK3588) — cross-compile from macOS using zig via `cmake/toolchain-aarch64-linux.cmake`
- **Native build**: `cmake -B build -S . && cmake --build build`
- **Cross build**: `cmake -B build-aarch64 -S . -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-linux.cmake && cmake --build build-aarch64`

## Architecture

The system is a pipeline with four module types:

```
[Receiver] → [Converter*] → [Processor(s)] → [Converter*] → [Encoder/Output]
                                  ↓
                           overlay primitives
                           + optional audio
```

`*` Converters are inserted only when format negotiation between adjacent stages requires it.

### 1. Receiver (`src/receiver/`)

Captures raw audio and video from hardware inputs.

- Uses **V4L2** to negotiate and receive video (format, resolution, framerate)
- Captures audio from ALSA or equivalent
- Outputs raw frames and audio buffers via DMA-accessible memory
- Exposes the negotiated format/resolution/framerate so downstream modules can adapt

### 2. Encoder/Output (`src/encoder/`)

Sends the final composited video and audio to display/speakers.

- Uses **DRM/KMS** for video output
- Negotiates output format and resolution with the display
- Participates in format negotiation with the receiver — ideally both agree on a common format to avoid unnecessary conversion
- Reads composited frames from DMA buffers
- Mixes and outputs audio

### 3. Processors (`src/processor/`)

Pluggable analysis modules that inspect video/audio and produce overlays.

**Processors are language- and runtime-agnostic.** A processor can be:
- A **C function** linked directly into the pipeline (lowest latency)
- A **separate process** in any language (Python, Go, Rust, etc.) communicating over a defined IPC protocol
- A **CUDA/OpenCL kernel** for GPU-accelerated analysis
- A **remote service** accessed over the network

The pipeline treats all processors uniformly through a common interface/protocol regardless of how they are implemented. This allows mixing high-performance native processors with rapid-prototype Python scripts or GPU-accelerated models in the same pipeline.

Each processor:
- Declares a list of **supported input formats and resolutions**
- Receives frames already converted to one of its supported formats (conversion is handled externally by converters)
- **Does not modify the original frame** — instead returns:
  - A list of **overlay primitives** (SVG-style: rects, text, lines, paths, etc.)
  - Optional **audio** to be mixed into the output
- Multiple processors can run in parallel on the same frame

Examples: object detection bounding boxes, text overlay, audio alerts, motion detection, face recognition.

### 4. Converters (`src/converter/`)

Handle format and resolution translation between pipeline stages.

- Convert between supported pixel formats (e.g. YUYV ↔ NV12 ↔ RGB)
- Scale between resolutions
- Composite overlay primitives onto frames (rasterize SVG-style primitives into the video)
- Mix additional audio into the output audio stream
- Should leverage hardware acceleration on the RK3588 where available (RGA, MPP)

## Format Negotiation

The pipeline performs format negotiation at startup:

1. Receiver queries V4L2 for supported formats
2. Encoder queries DRM/KMS for supported formats
3. The intersection is computed — if a direct match exists, no conversion is needed
4. Processors declare their required input formats
5. Converters are inserted where format/resolution mismatches exist

The goal is to minimize conversions. The ideal path is zero-copy from receiver to encoder with processors receiving converted copies only when needed.

## Key Design Principles

- **Zero-copy where possible**: use DMA buffers shared between V4L2 and DRM/KMS
- **Processors are pure analyzers**: they read frames and emit primitives, never mutate the video
- **Processors are anything**: from an in-process C function to a Python script to a CUDA kernel — the pipeline doesn't care
- **Overlays are primitives, not pixels**: processors output vector-style drawing commands, converters rasterize them
- **Hardware acceleration**: leverage RK3588 RGA2 for scaling/conversion and MPP for encode/decode when applicable
- **Modular**: each module type has a well-defined interface; new processors/converters can be added independently
