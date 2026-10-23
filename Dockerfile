FROM ubuntu:16.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential g++-multilib libqt4-dev qt4-dev-tools flex bison \
    libgsl2 gsl-bin git python nano sudo && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /workspace
CMD ["/bin/bash"]
