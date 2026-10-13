cmake_minimum_required(VERSION 3.24)
project(MouNeru VERSION 1.2.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "MouNeru supports Windows only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "MouNeru must be built for x64 (use: cmake -A x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(mouneru_core
  src/timer_manager.cpp
)
target_include_directories(mouneru_core PUBLIC src)
target_compile_definitions(mouneru_core PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(MouNeru WIN32
  src/main.cpp
  src/sleep_service.cpp
  src/app.rc
)
target_link_libraries(MouNeru PRIVATE mouneru_core PowrProf Advapi32 Comctl32 Dwmapi Gdiplus Oleacc Ole32 Uuid)
target_compile_definitions(MouNeru PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(MouNeru PRIVATE
  MOUNERU_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  MOUNERU_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  MOUNERU_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  MOUNERU_VERSION_STRING="${PROJECT_VERSION}"
)
if(MSVC)
  target_compile_options(MouNeru PRIVATE /W4 /permissive- /utf-8)
  target_link_options(MouNeru PRIVATE /MANIFEST:NO)
  target_compile_options(mouneru_core PRIVATE /W4 /permissive- /utf-8)
  set_property(TARGET MouNeru PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET mouneru_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_testing()
add_executable(mouneru_tests tests/timer_manager_tests.cpp)
target_link_libraries(mouneru_tests PRIVATE mouneru_core)
if(MSVC)
  target_compile_options(mouneru_tests PRIVATE /W4 /permissive- /utf-8)
  set_property(TARGET mouneru_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_test(NAME timer_manager COMMAND mouneru_tests)

add_executable(mouneru_power_service_tests tests/power_service_tests.cpp)
target_compile_definitions(mouneru_power_service_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(mouneru_power_service_tests PRIVATE /W4 /permissive- /utf-8)
  set_property(TARGET mouneru_power_service_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_test(NAME power_service COMMAND mouneru_power_service_tests)

add_executable(mouneru_ui_tests tests/ui_tests.cpp)
target_link_libraries(mouneru_ui_tests PRIVATE mouneru_core Comctl32 Dwmapi Gdiplus Oleacc Ole32 Uuid)
target_compile_definitions(mouneru_ui_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(mouneru_ui_tests PRIVATE /W4 /permissive- /utf-8)
  target_link_options(mouneru_ui_tests PRIVATE /MANIFEST:NO)
  set_property(TARGET mouneru_ui_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_test(NAME quiet_ui COMMAND mouneru_ui_tests)
set_tests_properties(quiet_ui PROPERTIES TIMEOUT 15)

add_custom_command(TARGET MouNeru POST_BUILD
  COMMAND "${CMAKE_COMMAND}"
          -DINPUT_FILE=$<TARGET_FILE:MouNeru>
          -DOUTPUT_FILE=$<TARGET_FILE_DIR:MouNeru>/MouNeru.exe.sha256
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteChecksum.cmake"
  VERBATIM
  COMMENT "Writing SHA-256 checksum"
)

install(TARGETS MouNeru RUNTIME DESTINATION .)
install(FILES packaging/README.md LICENSE DESTINATION .)
install(FILES "$<TARGET_FILE_DIR:MouNeru>/MouNeru.exe.sha256" DESTINATION .)
