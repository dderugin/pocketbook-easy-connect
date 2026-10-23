FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake libarchive-tools libssl-dev \
    openssh-client python3 \
    && rm -rf /var/lib/apt/lists/*

ENV LANG=C.UTF-8

WORKDIR /project

CMD ["bash", "tools/build-docker.sh"]
