# syntax=docker/dockerfile:1
#
# Self-contained build of the Unitree G1 WBC deploy controller (x86_64 or aarch64, native build).
#
#   docker build -t wbc-g1-deploy .
#   docker run --rm wbc-g1-deploy --help
#   docker run --rm --name wbc_g1_ctrl --network host --privileged \
#       -v <cfg>:/app/config wbc-g1-deploy --network eth0      # run against a robot (DDS domain 0)
#
# Runtime layout (the same as the images launched by x-tor):
#   /app/bin/wbc_g1_ctrl   entrypoint; the controller resolves its config dir as <bin>/../config
#   /app/config            config.yaml, policy/, clips/, robot_defaults.yaml (repo defaults baked in;
#                          mount a directory over it to run another config)
#   /app/lib               ONNX Runtime, unitree_sdk2 / CycloneDDS shared libraries
#   /app/tools             read_policy_io.py (reader for the policy I/O recording, needs numpy)
#
# Policy I/O recording is ON by default in this image: POLICY_IO_DIR=/app/config/policy_io, i.e. it
# lands in the mounted config directory on the host. Each launch writes
# <POLICY_IO_DIR>/<UTC launch time>/<state>/policy_io.bin. Turn it off with `-e POLICY_IO_DIR=`.

ARG UBUNTU=ubuntu:22.04

FROM ${UBUNTU} AS build

ARG DEBIAN_FRONTEND=noninteractive
# Pinned unitree_sdk2 revision (main on 2026-11-06) for reproducible image builds.
ARG UNITREE_SDK_REF=8e1b927586712edd6a4c29d50dd52dff1e163439
# Parallel compile jobs (default: all CPUs visible to the build).
ARG JOBS=

# Build toolchain + runtime dependencies (see README "Dependencies").
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      git \
      curl \
      ca-certificates \
      libyaml-cpp-dev \
      libboost-program-options-dev \
      libeigen3-dev \
      libspdlog-dev \
      libfmt-dev \
      zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Build + install Unitree SDK2 into /opt/unitree_robotics (checked by the app's
# CMakeLists). This also installs the bundled CycloneDDS shared libraries.
RUN git clone https://github.com/unitreerobotics/unitree_sdk2.git /tmp/unitree_sdk2 \
    && git -C /tmp/unitree_sdk2 checkout --detach "${UNITREE_SDK_REF}" \
    && cmake -S /tmp/unitree_sdk2 -B /tmp/unitree_sdk2/build \
         -DBUILD_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX=/opt/unitree_robotics \
    && cmake --build /tmp/unitree_sdk2/build -j"${JOBS:-$(nproc)}" \
    && cmake --install /tmp/unitree_sdk2/build \
    && rm -rf /tmp/unitree_sdk2 \
    && echo /opt/unitree_robotics/lib > /etc/ld.so.conf.d/unitree.conf \
    && ldconfig

WORKDIR /opt/wbc-g1-deploy
COPY . .

# Fetch ONNX Runtime, then configure and build the controller (Release: no debug logging).
RUN scripts/bootstrap_thirdparty.sh \
    && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"${JOBS:-$(nproc)}"

# Collect what the runtime image needs: the binary, the native ONNX Runtime and the SDK/DDS libs.
RUN set -eux; \
    arch="$(uname -m)"; case "$arch" in x86_64) ort=x64 ;; aarch64) ort=aarch64 ;; *) exit 1 ;; esac; \
    mkdir -p /stage/app/bin /stage/app/lib /stage/app/tools; \
    cp build/wbc_g1_ctrl /stage/app/bin/; \
    cp -a thirdparty/onnxruntime-linux-${ort}-1.22.0/lib/libonnxruntime.so* /stage/app/lib/; \
    cp -a /opt/unitree_robotics/lib/*.so* /stage/app/lib/ 2>/dev/null || true; \
    cp -a config /stage/app/config; \
    cp tools/read_policy_io.py /stage/app/tools/; \
    LD_LIBRARY_PATH=/stage/app/lib ldd /stage/app/bin/wbc_g1_ctrl; \
    ! LD_LIBRARY_PATH=/stage/app/lib ldd /stage/app/bin/wbc_g1_ctrl | grep -q "not found"

FROM ${UBUNTU} AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      libyaml-cpp0.7 \
      libboost-program-options1.74.0 \
      libspdlog1 \
      libfmt8 \
      zlib1g \
      python3-minimal \
      python3-numpy \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /stage/app /app
ENV LD_LIBRARY_PATH=/app/lib
ENV POLICY_IO_DIR=/app/config/policy_io
WORKDIR /app

# Smoke test: the binary starts and every shared library resolves. The exit code is not checked:
# upstream aborts on exit after --help (the Keyboard reader thread is never joined).
RUN /app/bin/wbc_g1_ctrl --help 2>&1 | grep -q -- "--network"

ENTRYPOINT ["/app/bin/wbc_g1_ctrl"]
CMD []
