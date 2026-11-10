cmake_minimum_required(VERSION 3.24)
project(jsfav_checker VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(LLVM_CONFIG NAMES llvm-config llvm-config-22 llvm-config-21 llvm-config-20 llvm-config-19 llvm-config-18)
if(LLVM_CONFIG)
  execute_process(COMMAND "${LLVM_CONFIG}" --includedir OUTPUT_VARIABLE LLVM_INCLUDEDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND "${LLVM_CONFIG}" --libdir OUTPUT_VARIABLE LLVM_LIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h
  HINTS "$ENV{LLVM_HOME}/include" "${LLVM_INCLUDEDIR}" "C:/Program Files/LLVM/include")
find_library(LIBCLANG_LIBRARY NAMES libclang clang
  HINTS "$ENV{LLVM_HOME}/lib" "${LLVM_LIBDIR}" "C:/Program Files/LLVM/lib")
if(NOT LIBCLANG_INCLUDE_DIR OR NOT LIBCLANG_LIBRARY)
  message(FATAL_ERROR "libclang development files are required; set LLVM_HOME or LIBCLANG_INCLUDE_DIR and LIBCLANG_LIBRARY")
endif()
find_program(CLANG_BINARY NAMES clang clang-22 clang-21 clang-20 clang-19 clang-18
  HINTS "$ENV{LLVM_HOME}/bin" "C:/Program Files/LLVM/bin")
if(NOT CLANG_BINARY)
  message(FATAL_ERROR "clang executable is required to locate builtin resource headers")
endif()
execute_process(COMMAND "${CLANG_BINARY}" --print-resource-dir
  OUTPUT_VARIABLE CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE CLANG_RESOURCE_RESULT)
file(TO_CMAKE_PATH "${CLANG_RESOURCE_DIR}" CLANG_RESOURCE_DIR)
if(NOT CLANG_RESOURCE_RESULT EQUAL 0 OR NOT EXISTS "${CLANG_RESOURCE_DIR}/include/stddef.h")
  message(FATAL_ERROR "Could not locate Clang builtin resource headers")
endif()

add_executable(jsfav src/main.cpp src/server.cpp)
target_include_directories(jsfav PRIVATE include "${LIBCLANG_INCLUDE_DIR}" third_party/cpp-httplib)
target_link_libraries(jsfav PRIVATE "${LIBCLANG_LIBRARY}")
if(WIN32)
  target_link_libraries(jsfav PRIVATE ole32 shell32)
endif()
target_compile_definitions(jsfav PRIVATE JSFAV_VERSION="${PROJECT_VERSION}")
add_custom_command(TARGET jsfav POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:jsfav>/data"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/data/rules.json" "$<TARGET_FILE_DIR:jsfav>/data/rules.json"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/web" "$<TARGET_FILE_DIR:jsfav>/web"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CLANG_RESOURCE_DIR}/include" "$<TARGET_FILE_DIR:jsfav>/clang-resource/include")

if(UNIX AND NOT APPLE)
  set_target_properties(jsfav PROPERTIES INSTALL_RPATH "$ORIGIN")
  install(TARGETS jsfav RUNTIME_DEPENDENCY_SET jsfav_runtime RUNTIME DESTINATION .)
  install(RUNTIME_DEPENDENCY_SET jsfav_runtime DESTINATION .
    PRE_EXCLUDE_REGEXES
      "^ld-linux.*" "^libc\\.so.*" "^libm\\.so.*" "^libpthread\\.so.*"
      "^libdl\\.so.*" "^librt\\.so.*" "^libstdc\\+\\+\\.so.*" "^libgcc_s\\.so.*")
else()
  install(TARGETS jsfav RUNTIME DESTINATION .)
endif()
install(FILES data/rules.json DESTINATION data)
install(DIRECTORY web/ DESTINATION web)
install(DIRECTORY "${CLANG_RESOURCE_DIR}/include/" DESTINATION clang-resource/include)
install(FILES README.md AGENTS.md DESTINATION .)
install(FILES docs/agent-usage.md DESTINATION docs)
if(WIN32)
  install(FILES start-jsfav-ui.bat DESTINATION .)
else()
  install(PROGRAMS start-jsfav-ui.sh DESTINATION .)
endif()
install(FILES include/nlohmann/LICENSE.MIT DESTINATION licenses/nlohmann)
install(FILES licenses/LLVM-LICENSE.TXT DESTINATION licenses/llvm)
install(FILES third_party/cpp-httplib/LICENSE DESTINATION licenses/cpp-httplib)
if(WIN32)
  find_file(LIBCLANG_RUNTIME NAMES libclang.dll HINTS "$ENV{LLVM_HOME}/bin" "C:/Program Files/LLVM/bin")
  if(LIBCLANG_RUNTIME)
    add_custom_command(TARGET jsfav POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${LIBCLANG_RUNTIME}" "$<TARGET_FILE_DIR:jsfav>/libclang.dll")
    install(FILES "${LIBCLANG_RUNTIME}" DESTINATION .)
  else()
    message(FATAL_ERROR "libclang.dll is required for a portable Windows package")
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_cli.py" "$<TARGET_FILE:jsfav>" "${CMAKE_SOURCE_DIR}")
  add_test(NAME local_web_server COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_server.py" "$<TARGET_FILE:jsfav>")
  set_tests_properties(local_web_server PROPERTIES TIMEOUT 90)
endif()

set(CPACK_PACKAGE_NAME "jsfav-checker")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_GENERATOR "ZIP;TGZ")
include(CPack)
