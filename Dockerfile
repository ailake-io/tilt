# syntax=docker/dockerfile:1.7

FROM ubuntu:24.04 AS build

ARG TILT_VERSION=0.2.0-beta.2
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential \
       ca-certificates \
       cmake \
       curl \
       libcurl4-openssl-dev \
       ninja-build \
       python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B /tmp/tilt-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DTILT_VERSION="${TILT_VERSION}" \
    && cmake --build /tmp/tilt-build --target tilt --parallel \
    && DESTDIR=/tmp/tilt-root cmake --install /tmp/tilt-build

FROM ubuntu:24.04 AS runtime

ARG TILT_VERSION=0.2.0-beta.2
LABEL org.opencontainers.image.title="Tilt" \
      org.opencontainers.image.description="Linguagem Tilt para dados, IA e serviços" \
      org.opencontainers.image.source="https://github.com/ailake-io/tilt" \
      org.opencontainers.image.version="${TILT_VERSION}" \
      org.opencontainers.image.licenses="MIT"

ENV DEBIAN_FRONTEND=noninteractive \
    TILT_VERSION="${TILT_VERSION}"

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       ca-certificates \
       curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 --shell /usr/sbin/nologin tilt

COPY --from=build /tmp/tilt-root/usr/local/ /usr/local/

WORKDIR /workspace
VOLUME ["/workspace"]
USER tilt

HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
  CMD ["tilt", "versao"]

ENTRYPOINT ["tilt"]
