FROM ubuntu:22.04@sha256:0a9887931b0e38af2e3487ed39e29436b0535c4f3f52e57745baaf79d1898956

ARG SRT_TAG=v1.5.6
# The commit SRT_TAG must resolve to. A moved or re-pointed tag fails the build.
ARG SRT_COMMIT=c63c311e88aa55e430e3b7d94b89d790994f88c4
ARG RELAY_VERSION=dev

RUN apt-get update -q \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y -q \
        build-essential \
        ca-certificates \
        cmake \
        g++ \
        git \
        libssl-dev \
        pkg-config \
        tclsh \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src/srt
RUN git clone --branch "${SRT_TAG}" --depth 1 https://github.com/Haivision/srt.git . \
    && actual="$(git rev-parse HEAD)" \
    && [ "$actual" = "${SRT_COMMIT}" ] \
    || { echo "libsrt ${SRT_TAG} is ${actual:-unknown}, expected ${SRT_COMMIT}" >&2; exit 1; }
RUN ./configure --prefix=/usr/local --enable-apps=OFF --enable-bonding \
    && make -j"$(nproc)" \
    && make install \
    && ldconfig

WORKDIR /src/app
ARG RELAY_SOURCE_SHA=dev
RUN printf '%s\n' "$RELAY_SOURCE_SHA" > /tmp/relay-source.sha
COPY src/ ./src/
RUN g++ -O2 -DRELAY_VERSION=\"${RELAY_VERSION}\" -o goryu src/*.c \
    $(pkg-config --cflags --libs srt) -lpthread -lssl -lcrypto -lm

RUN set -eux; \
    stage=/package; \
    mkdir -p "$stage/bin" "$stage/lib"; \
    install -m 755 goryu "$stage/bin/goryu"; \
    ldd "$stage/bin/goryu" | awk '/=> \// {print $3}' | while read -r lib; do \
        case "$lib" in \
            /lib/*/libc.so.*|/lib/*/libpthread.so.*|/lib/*/libm.so.*|/lib/*/libdl.so.*|/lib/*/ld-linux-*.so.*) ;; \
            *) cp -v "$lib" "$stage/lib/" ;; \
        esac; \
    done
