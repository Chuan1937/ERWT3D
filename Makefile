BUILD = build

.PHONY: all build test test-hdd test-ssd clean

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
		./erwt3d run $$f; \
	done

test-ssd:
	@echo "=== SSD 测试 ==="
	@for f in tests/ssd/*.sh; do \
		echo ""; \
		echo "--- $$f ---"; \
		./erwt3d run $$f; \
	done

clean:
	rm -rf $(BUILD)
