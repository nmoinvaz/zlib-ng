## Project build system

- Uses CMake with default `build` directory

## Key Directories

- `arch/` - Architecture specific optimizations
- `test/` - Unit tests written using Google Test Framework (gtest_zlib project)
- `test/benchmarks` - Performance benchmark testing using Google Benchmark Framework (benchmark_zlib project)

## Testing

- gtest_zlib and benchmark_zlib can typically be found within the `build` directory after being built. gtest_zlib in `build/test` and benchmark_zlib in `build/test/benchmarks`.
- To enable gtest_zlib use `-D BUILD_TESTING=ON -D WITH_GTEST=ON` in CMake and `-D BUILD_TESTING=ON -D WITH_BENCHMARKS=ON` for benchmark_zlib.

## Performance Testing

- Object files can be converted to assembly for analysis, they can be found in `build/CMakeFiles/zlib-ng.dir`
- Always check the assembly output to see if the specific performance optimization makes a difference
- Always configure the CMake with `-D BUILD_SHARED_LIBS=OFF` to make sure time spent loading dylibs on certain platforms does not interfer with tests

## Optimization

- Look for ways to optimize using bit tricks.
- Reduce unnecessary casts by looking at where the data is coming from and how it is being used.

## Standards

- Use fixed-integer types from `stdint.h` when possible.
