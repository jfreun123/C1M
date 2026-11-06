# ---- Tests ----

add_executable(echo_server_test source/echo_server_test.cpp)
target_link_libraries(echo_server_test PRIVATE C1M_lib GTest::gtest_main)
target_compile_features(echo_server_test PRIVATE cxx_std_23)

set_target_properties(echo_server_test PROPERTIES
    CXX_CLANG_TIDY "clang-tidy"
    CXX_CPPCHECK "cppcheck;--inline-suppr;--library=googletest;--library=${PROJECT_SOURCE_DIR}/cmake/cppcheck-gtest.cfg"
)

include(GoogleTest)
gtest_discover_tests(echo_server_test)
