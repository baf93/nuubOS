FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y     bash     bc     bison     build-essential     ca-certificates     cpio     file     flex     git     libelf-dev     libncurses-dev     libssl-dev     locales     make     patch     perl     python3     python3-dev     python3-setuptools     rsync     sed     unzip     wget     xz-utils     && rm -rf /var/lib/apt/lists/*

RUN locale-gen en_US.UTF-8

ENV LANG=en_US.UTF-8
ENV LC_ALL=en_US.UTF-8

WORKDIR /workspace
