FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git ca-certificates python3 \
    libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev \
    libgl1-mesa-dev libegl-dev libwayland-dev libxkbcommon-dev wayland-protocols \
    libasound2-dev libpulse-dev libudev-dev libvulkan-dev \
    mesa-vulkan-drivers xvfb xauth && rm -rf /var/lib/apt/lists/*
WORKDIR /workspace
