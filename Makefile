BUILD = build

.PHONY: all build test bench verify clean

all: build

build:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j

test: verify

verify:
	bash scripts/verify.sh

bench:
	bash scripts/benchmark.sh

clean:
	rm -rf $(BUILD)
