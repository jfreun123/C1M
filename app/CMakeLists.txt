# ---- Echo server ----

add_executable(echo_server source/echo_server.cpp)
target_link_libraries(echo_server PRIVATE C1M_lib)
target_compile_features(echo_server PRIVATE cxx_std_23)

if(C1M_DEVELOPER_MODE)
  set_target_properties(echo_server PROPERTIES
      CXX_CLANG_TIDY "clang-tidy"
  )
endif()
