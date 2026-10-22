# Bonded test sender: libsrt (the tag and commit pinned in scripts/goryu.Dockerfile, read
# from it so there is one copy) plus tests/e2e/bonded_caller.c and ffmpeg. A demo, not a
# product; see docs/2-usage/2-sending-bonded-srt.md. Build from the repository root.
FROM ubuntu:22.04 AS build
RUN apt-get update -q \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
        build-essential ca-certificates cmake g++ git libssl-dev pkg-config tclsh \
    && rm -rf /var/lib/apt/lists/*
COPY scripts/goryu.Dockerfile /tmp/goryu.Dockerfile
WORKDIR /src/srt
RUN set -eux; \
    tag=$(sed -n 's/^ARG SRT_TAG=//p' /tmp/goryu.Dockerfile); \
    commit=$(sed -n 's/^ARG SRT_COMMIT=//p' /tmp/goryu.Dockerfile); \
    [ -n "$tag" ] && [ -n "$commit" ]; \
    git clone --branch "$tag" --depth 1 https://github.com/Haivision/srt.git .; \
    [ "$(git rev-parse HEAD)" = "$commit" ]; \
    ./configure --prefix=/usr/local --enable-apps=OFF --enable-bonding; \
    make -j"$(nproc)"; make install; ldconfig
COPY tests/e2e/bonded_caller.c /src/bonded_caller.c
RUN gcc -O2 -Wall -Wextra -o /bonded-caller /src/bonded_caller.c \
    $(pkg-config --cflags --libs srt) -lpthread

FROM ubuntu:22.04
RUN apt-get update -q \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends ffmpeg libssl3 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /usr/local/lib/libsrt.so.1.5 /usr/local/lib/
COPY --from=build /bonded-caller /usr/local/bin/bonded-caller
RUN ldconfig
COPY docker/demo-sender/run.sh /usr/local/bin/run-sender
ENTRYPOINT ["/usr/local/bin/run-sender"]
