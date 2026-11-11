FROM ubuntu:24.04 AS build-deps
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake python3 python3-numpy zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src

FROM build-deps AS build
COPY CMakeLists.txt ./
COPY src/ ./src/
COPY tests/ ./tests/
COPY tools/benchmark_cpu.cpp ./tools/benchmark_cpu.cpp
COPY mysqltest/ ./mysqltest/
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --target mvdb_local -j2

FROM build AS test
RUN cmake --build /build -j2 \
    && ctest --test-dir /build --output-on-failure

FROM ubuntu:24.04 AS runtime
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends libstdc++6 libgcc-s1 zlib1g \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /data
COPY --from=build /build/mvdb_local /usr/local/bin/minivectordb
WORKDIR /data
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/minivectordb"]
CMD ["--data-dir", "/data"]
