# TetherDesk relay: a tiny C++ server; the image also carries the web viewer
# and the QuickSupport downloads it serves (staged by relay/deploy.sh).
FROM debian:bookworm-slim AS build
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
COPY --from=build /src/build/tetherdesk-relay /usr/local/bin/tetherdesk-relay
COPY relay/public /app/public
USER relay
WORKDIR /app
ENV PORT=8080
EXPOSE 8080
CMD ["tetherdesk-relay", "--web-root", "/app/public"]
