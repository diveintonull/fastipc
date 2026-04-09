.PHONY: configure build test benchmark

configure:
	cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DFASTIPC_BUILD_TESTS=ON -DFASTIPC_BUILD_BENCHMARKS=ON

build: configure
	cmake --build build

test: build
	ctest --test-dir build --output-on-failure

benchmark: build
	./build/fastipc_benchmark
