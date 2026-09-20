# Build context: repo root  (docker-compose.yml passes context: .)
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    python3 \
    python3-pip \
    time \
    && rm -rf /var/lib/apt/lists/*

# Python visualisation dependencies
RUN pip3 install --no-cache-dir --break-system-packages matplotlib pandas numpy

WORKDIR /skarn
COPY . .

# Build the Skarn VM from source
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --target skarnvm -j$(nproc) \
    && cp /build/skarnvm /usr/local/bin/skarnvm

# Compile every C++ benchmark
RUN mkdir -p benchmarks/programs/cpp/bin && \
    for src in benchmarks/programs/cpp/*.cpp; do \
        name=$(basename "${src%.cpp}") && \
        g++ -O3 -std=c++20 -o "benchmarks/programs/cpp/bin/$name" "$src" && \
        echo "  compiled $name"; \
    done

RUN mkdir -p benchmarks/results benchmarks/results/charts
RUN chmod +x benchmarks/entrypoint.sh

ENV VM_PATH=/usr/local/bin/skarnvm

# Any extra args passed to `docker compose run benchmark ...` are forwarded
# to run.py (e.g. --lang skarn,cpp --filter arith).
ENTRYPOINT ["bash", "benchmarks/entrypoint.sh"]
