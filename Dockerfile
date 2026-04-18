FROM public.ecr.aws/docker/library/debian:trixie-slim AS build

ARG DEBIAN_FRONTEND=noninteractive
ARG LIBDISPLAY_INFO_VERSION=0.3.0
ARG LIBYUV_VERSION=main
ARG LIBRGA_VERSION=v2.2.0-1-20260121-2cffdf6

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    pkg-config \
    ca-certificates \
    meson \
    ninja-build \
    libasound2-dev \
    libdrm-dev \
    libcairo2-dev \
    libpango1.0-dev \
    libsamplerate0-dev \
    libdisplay-info-dev \
    libusb-1.0-0-dev \
    patchelf \
    && rm -rf /var/lib/apt/lists/*

# libyuv: build from source
RUN git clone --branch $LIBYUV_VERSION --depth 1 https://chromium.googlesource.com/libyuv/libyuv /tmp/libyuv && \
    cd /tmp/libyuv && mkdir build && cd build && \
    cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_SHARED_LIBS=ON && \
    make -j$(nproc) && make install && \
    rm -rf /tmp/libyuv

# librga: Rockchip RGA userspace library
RUN git clone --branch $LIBRGA_VERSION --depth 1 https://github.com/tsukumijima/librga-rockchip.git /tmp/librga && \
    cd /tmp/librga && \
    meson setup build --prefix=/usr && \
    ninja -C build && ninja -C build install && \
    rm -rf /tmp/librga

WORKDIR /src
COPY . .

RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    cmake --build . -j$(nproc) --target deploy

# Bundle shared library dependencies + dynamic linker for full portability
# Layout: overlAIer.bin + ld-linux at root, libs in lib/, processors in processors/
# /proc/self/exe resolves to ld-linux at root, so exe_dir = root, finding processors/ directly
RUN mkdir -p build/lib && \
    for bin in build/overlAIer build/processors/*.so; do \
        ldd "$bin" 2>/dev/null | awk '/=>/ && !/linux-vdso/ {print $3}' ; \
    done | sort -u | while read -r lib; do \
        cp -L "$lib" build/lib/ ; \
    done && \
    cp -L /lib/ld-linux-aarch64.so.1 build/ && \
    patchelf --set-rpath '$ORIGIN/lib' build/overlAIer && \
    for so in build/processors/*.so; do \
        patchelf --set-rpath '$ORIGIN/../lib' "$so" ; \
    done && \
    mv build/overlAIer build/overlAIer.bin && \
    mkdir -p build/bin && \
    printf '#!/bin/sh\nDIR="$(cd "$(dirname "$0")/.." && pwd)"\nexec "$DIR/ld-linux-aarch64.so.1" --library-path "$DIR/lib" "$DIR/overlAIer.bin" "$@"\n' > build/bin/overlAIer && \
    chmod +x build/bin/overlAIer

# Output stage: just the built artifacts
FROM scratch AS artifacts
COPY --from=build /src/build/ /