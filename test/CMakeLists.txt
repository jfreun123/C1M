# ---- Tests ----

add_executable(add_test source/add_test.cpp)
target_link_libraries(add_test PRIVATE cpp-starter_lib GTest::gtest_main)
target_compile_features(add_test PRIVATE cxx_std_23)

set_target_properties(add_test PROPERTIES
    CXX_CLANG_TIDY "clang-tidy"
    CXX_CPPCHECK "cppcheck;--inline-suppr"
)

include(GoogleTest)
gtest_discover_tests(add_test)
