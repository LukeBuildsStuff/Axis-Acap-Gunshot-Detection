# Dockerfile for Gunshot Detector v1.1.91 with LAROD Integration
# Using official Axis audio-capture SDK version with PipeWire support

ARG ARCH=aarch64
ARG VERSION=12.5.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk
ARG CHIP=cv25

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

# Install any additional dependencies
RUN apt-get update && apt-get install -y \
    unzip \
    build-essential \
    cmake \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Build FFTW3 for aarch64
RUN mkdir -p /opt/build && cd /opt/build && \
    wget http://www.fftw.org/fftw-3.3.10.tar.gz && \
    tar -xzf fftw-3.3.10.tar.gz && \
    cd fftw-3.3.10 && \
    . /opt/axis/acapsdk/environment-setup* && \
    ./configure --prefix=/opt/build/fftw3 --enable-single --enable-shared --host=aarch64-linux-gnu && \
    make -j$(nproc) && \
    make install && \
    mkdir -p /opt/app/lib /opt/app/include && \
    cp -r /opt/build/fftw3/lib/* /opt/app/lib/ && \
    cp -r /opt/build/fftw3/include/* /opt/app/include/ && \
    rm -rf /opt/build

# Set working directory
WORKDIR /opt/app

# Copy application files for v1.1.91 - Official SDK Audio + v1.1.78 Model + FFTW3
COPY gunshot_detector_v1192_official.c Makefile LICENSE ./
RUN mv gunshot_detector_v1192_official.c gunshot_detector.c
COPY gunshot_model_real_audio.tflite ./
COPY config.json ./
COPY test_gunshot.wav ./

# Copy CGI script for web interface communication
COPY trigger.cgi ./

# Copy web interface files (v1.1.75 version)
COPY html/ ./html/

# Copy and select appropriate manifest for CV25
COPY manifest.json.cv25 ./manifest.json
COPY package.conf ./
COPY param.conf ./

# Make CGI script executable
RUN chmod +x trigger.cgi

# Copy FFTW libraries and headers to the build directory
RUN mkdir -p ./lib ./include && \
    cp /opt/app/lib/*.so* ./lib/ 2>/dev/null || true && \
    cp /opt/app/include/*.h ./include/ || true

# Set up build environment and compile v1.1.91
RUN . /opt/axis/acapsdk/environment-setup* && \
    echo "Building Gunshot Detector v1.1.91 - Official SDK Audio + Working Model + FFTW3 for CV25..." && \
    echo "Target chip: ${CHIP}" && \
    echo "Architecture: ${ARCH}" && \
    acap-build . -a 'gunshot_model_real_audio.tflite' -a 'config.json' -a 'html/' -a 'test_gunshot.wav' -a 'trigger.cgi' -a 'lib/'

# The built application will be available in the working directory
CMD ["echo", "Gunshot Detector v1.1.91 - Official SDK Audio + Working Model build complete"]