FROM ubuntu:24.04 AS builder
RUN apt-get update && apt-get install -y --no-install-recommends build-essential cmake && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY server/CMakeLists.txt ./server/CMakeLists.txt
COPY server/src ./server/src
COPY config ./config
# Portable release binary; native CPU flags belong in measured local builds.
RUN cmake -S server -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF && cmake --build build -j2
FROM ubuntu:24.04
RUN useradd --uid 10001 --no-create-home --shell /usr/sbin/nologin fastbid
COPY --from=builder /src/build/fastbid /usr/local/bin/fastbid
USER 10001:10001
ENTRYPOINT ["/usr/local/bin/fastbid"]
