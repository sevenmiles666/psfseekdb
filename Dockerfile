ARG MVDB_BUILD_BASE=gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818
ARG MVDB_RUNTIME_BASE=debian:bookworm-slim@sha256:734a72b3d6142a71993b4bc580ca01d8ca687f77a2c422d96a961d3376b179bc
FROM ${MVDB_BUILD_BASE} AS builder
WORKDIR /src
COPY Makefile ./
COPY minivectordb/ minivectordb/
COPY third_party/ third_party/
RUN make CXX=g++ CXXFLAGS='-O2 -std=c++20 -Wall -Wextra -Wpedantic -static-libstdc++ -static-libgcc'

FROM ${MVDB_RUNTIME_BASE}
COPY --from=builder /src/build/minivectordb /usr/local/bin/minivectordb
RUN mkdir /data
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/minivectordb"]
CMD ["--data-dir", "/data"]
