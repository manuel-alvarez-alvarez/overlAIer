FROM public.ecr.aws/docker/library/debian:trixie-slim AS build

ARG DEBIAN_FRONTEND=noninteractive

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
    && rm -rf /var/lib/apt/lists/*

# libyuv: build from source
RUN git clone --depth 1 https://chromium.googlesource.com/libyuv/libyuv /tmp/libyuv && \
    cd /tmp/libyuv && mkdir build && cd build && \
    cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_SHARED_LIBS=ON && \
    make -j$(nproc) && make install && \
    rm -rf /tmp/libyuv

# librga: Rockchip RGA userspace library
RUN git clone --depth 1 https://github.com/tsukumijima/librga-rockchip.git /tmp/librga && \
    cd /tmp/librga && \
    meson setup build --prefix=/usr && \
    ninja -C build && ninja -C build install && \
    rm -rf /tmp/librga

WORKDIR /src
COPY . .

RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    cmake --build . -j$(nproc) --target deploy

# Output stage: just the built artifacts
FROM scratch AS artifacts
COPY --from=build /src/build/ /
