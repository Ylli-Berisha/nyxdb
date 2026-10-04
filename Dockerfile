FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git openssl libssl-dev libnuma-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target nyxdb_server -j$(nproc) \
    && mkdir -p /stage/bin /stage/lib \
    && cp build/src/nyxdb_server /stage/bin/ \
    && find build -name 'libmsquic.so*' -exec cp {} /stage/lib/ \;

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        openssl libssl3 libnuma1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /stage/bin/nyxdb_server /usr/local/bin/
COPY --from=builder /stage/lib/             /usr/local/lib/
RUN ldconfig

VOLUME /data
EXPOSE 4433/udp

ENTRYPOINT ["nyxdb_server"]
CMD ["--data-dir", "/data", "--token", "changeme", "--port", "4433"]
