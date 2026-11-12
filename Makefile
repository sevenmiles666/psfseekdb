CXX ?= c++
CXXFLAGS ?= -O2 -g -std=c++20 -Wall -Wextra -Wpedantic
CPPFLAGS += -Ithird_party
ifeq ($(shell uname -s),Darwin)
SDKROOT := /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
CPPFLAGS += -isysroot $(SDKROOT)
endif

build/minivectordb: minivectordb/main.cpp minivectordb/engine.hpp minivectordb/sql.hpp minivectordb/storage.hpp third_party/json.hpp
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) minivectordb/main.cpp -o $@

.PHONY: public clean
public: build/minivectordb
	python3 protocol_check/run.py --command '["$(CURDIR)/build/minivectordb","--data-dir","{data_dir}"]' --output "protocol-results/public-$$(date +%Y%m%d-%H%M%S)"
clean:
	rm -f build/minivectordb
