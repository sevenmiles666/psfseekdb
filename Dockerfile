FROM gcc:13.4.0-bookworm@sha256:3617a214e52a25bde5375dc9503b5e67f01b6c7322a30137e2790aa8e6db5d1f AS build
WORKDIR /src
COPY src/ ./
RUN g++ -std=c++17 -O3 -DNDEBUG -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Wpedantic -static \
    main.cpp request.cpp sql.cpp journal.cpp database.cpp -o /minivectordb

FROM scratch
COPY --from=build /minivectordb /minivectordb
VOLUME ["/data"]
ENTRYPOINT ["/minivectordb"]
CMD ["--data-dir", "/data"]
