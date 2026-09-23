# syntax=docker/dockerfile:1

# ---- build & test -----------------------------------------------------------
FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake ninja-build libcurl4-openssl-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGEMINI_WERROR=ON -DGEMINI_BUILD_EXAMPLES=OFF \
 && cmake --build build \
 && ctest --test-dir build --output-on-failure -j"$(nproc)" \
 && cmake --install build --prefix /opt/gemini-cpp

# ---- runtime ----------------------------------------------------------------
# The eval harness compiles model-written code, so the runtime image ships a compiler.
# Model output runs as an unprivileged user inside the container; run with
# `--network none` for eval-only workloads that talk to a mock, or restrict egress.
FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ libcurl4t64 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --create-home --shell /usr/sbin/nologin runner
COPY --from=build /opt/gemini-cpp /opt/gemini-cpp
ENV PATH=/opt/gemini-cpp/bin:$PATH
WORKDIR /opt/gemini-cpp/share/gemini-cpp
USER runner
ENTRYPOINT ["gemini-cpp"]
CMD ["--help"]
