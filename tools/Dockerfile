# The vendor SDK tools are x86_64. Retain their loader/libraries while running
# host tests and QEMU natively on the Docker engine's architecture.
ARG EC_BUILD_PLATFORM=linux/amd64
FROM --platform=linux/amd64 ubuntu:24.04@sha256:78d4744ead1f5b19b04d0df558ab182a5300903900755e515b2ca4d10eb2044d AS sdk-host
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential libgmp10 libmpc3 libmpfr6 libzstd1 zlib1g \
    && rm -rf /var/lib/apt/lists/*

# QEMU 8.2 (Ubuntu 24.04) rejects PR_SET_CHILD_SUBREAPER. This pinned native
# static emulator supports the same process supervision used on the reader.
FROM --platform=${EC_BUILD_PLATFORM} tonistiigi/binfmt@sha256:400a4873b838d1b89194d982c45e5fb3cda4593fbfd7e08a02e76b03b21166f0 AS emulator

FROM --platform=${EC_BUILD_PLATFORM} ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake libarchive-tools libssl-dev \
    openssh-client python3 git curl qemu-user e2fsprogs squashfs-tools util-linux pkg-config \
    && rm -rf /var/lib/apt/lists/*
COPY --from=emulator /usr/bin/qemu-arm /usr/local/bin/qemu-arm
COPY --from=sdk-host /usr/lib/x86_64-linux-gnu/ /usr/lib/x86_64-linux-gnu/
RUN mkdir -p /lib64 && ln -sfn /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 /lib64/ld-linux-x86-64.so.2
ENV LANG=C.UTF-8
WORKDIR /project
CMD ["bash", "tools/build-docker.sh"]
