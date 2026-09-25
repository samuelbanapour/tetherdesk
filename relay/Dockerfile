# TetherDesk relay (Render or any Docker host). Builds, from source:
#   1. the web viewer (C++ -> WebAssembly with Emscripten)
#   2. the relay server (C++)
# and serves the viewer plus the QuickSupport download page.
FROM emscripten/emsdk:3.1.74 AS web
WORKDIR /src
COPY CMakeLists.txt ./
COPY common common
COPY client client
COPY web web
RUN emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build-web -j"$(nproc)"

FROM debian:bookworm-slim AS relay
RUN apt-get update && apt-get install -y --no-install-recommends cmake g++ make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY common common
COPY relay/relay.cpp relay/relay.cpp
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTD_RELAY_ONLY=ON \
    && cmake --build build --target tetherdesk-relay -j"$(nproc)"

FROM debian:bookworm-slim
RUN useradd --system --no-create-home relay
COPY --from=relay /src/build/tetherdesk-relay /usr/local/bin/tetherdesk-relay
COPY --from=web /src/build-web/index.html /src/build-web/index.js /src/build-web/index.wasm /app/public/
COPY relay/get.html /app/public/get.html
USER relay
WORKDIR /app
ENV PORT=10000
EXPOSE 10000
CMD ["tetherdesk-relay", "--web-root", "/app/public"]
