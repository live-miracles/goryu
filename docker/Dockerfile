# Goryu runtime image: installs a published release and nothing else, so the image
# runs exactly the binary and libraries that the release archive ships. The base is
# the one the release is built on (scripts/goryu.Dockerfile), because the archive
# links against its glibc. linux/amd64 only, like the release.
#
#   docker build -t goryu docker
#   docker build --build-arg GORYU_VERSION=v3.3.0 -t goryu:v3.3.0 docker
#
# GORYU_VERSION is "latest" or a tag (vX.Y.Z). Pin a tag for anything long-lived.
ARG BASE=ubuntu:22.04@sha256:0a9887931b0e38af2e3487ed39e29436b0535c4f3f52e57745baaf79d1898956

FROM ${BASE} AS fetch
ARG GORYU_VERSION=latest
RUN apt-get update -q \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /dl
RUN set -eux; \
    if [ "$GORYU_VERSION" = latest ]; then base=https://github.com/live-miracles/goryu/releases/latest/download; \
    else base=https://github.com/live-miracles/goryu/releases/download/$GORYU_VERSION; fi; \
    curl -fsSL -o goryu-linux-x86_64.tar.gz "$base/goryu-linux-x86_64.tar.gz"; \
    curl -fsSL -o checksums.sha256 "$base/checksums.sha256"; \
    sha256sum --check checksums.sha256; \
    mkdir -p /opt/goryu; tar -xzf goryu-linux-x86_64.tar.gz -C /opt/goryu

FROM ${BASE}
COPY --from=fetch /opt/goryu /opt/goryu
RUN useradd --system --no-create-home --uid 10001 goryu
ENV LD_LIBRARY_PATH=/opt/goryu/lib
USER goryu
# 10081/udp is the default input_port; the status API stays on 127.0.0.1 inside the
# container, so run with host networking to reach it (docs/1-getting-started/2-install.md#docker).
EXPOSE 10081/udp
ENTRYPOINT ["/opt/goryu/bin/goryu"]
CMD ["/etc/goryu/goryu.json"]
