# ---- Benchmark ----

add_executable(benchmark source/benchmark.cpp)
target_link_libraries(benchmark PRIVATE cpp-starter_lib)
target_compile_features(benchmark PRIVATE cxx_std_23)

set_target_properties(benchmark PROPERTIES
    CXX_CLANG_TIDY "clang-tidy"
)
