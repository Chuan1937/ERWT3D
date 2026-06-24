BUILD = build

.PHONY: all build test test-hdd clean

all: build

build:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j

test: test-hdd

test-hdd:
	@echo "=== HDD 测试 ==="
	@for f in tests/hdd/*.sh; do \
		echo ""; \
		echo "--- $$f ---"; \
		bash $$f; \
	done

clean:
	rm -rf $(BUILD)
