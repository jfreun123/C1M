# ---- Load generator ----

add_executable(load_gen source/load_gen.cpp)
target_link_libraries(load_gen PRIVATE C1M_lib)
target_compile_features(load_gen PRIVATE cxx_std_23)

set_target_properties(load_gen PROPERTIES
    CXX_CLANG_TIDY "clang-tidy"
)
