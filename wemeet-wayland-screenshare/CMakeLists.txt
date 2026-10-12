cmake_minimum_required(VERSION 3.23)

project(wemeet-wayland-screencast LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find PkgConfig
find_package(PkgConfig REQUIRED)

# Find the gio-2.0 package
pkg_check_modules(GIO REQUIRED gio-2.0)
# libportal
pkg_check_modules(LIBPORTAL REQUIRED libportal)
# pipewire
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)


find_package(X11 REQUIRED COMPONENTS Xdamage Xfixes)

function(add_compile_options_for TARGET_NAME)
  target_link_libraries(${TARGET_NAME} PRIVATE ${GIO_LIBRARIES})
  target_include_directories(${TARGET_NAME} PRIVATE ${GIO_INCLUDE_DIRS})

  target_link_libraries(${TARGET_NAME} PRIVATE ${LIBPORTAL_LIBRARIES})
  target_include_directories(${TARGET_NAME} PRIVATE ${LIBPORTAL_INCLUDE_DIRS})

  target_link_libraries(${TARGET_NAME} PRIVATE ${PIPEWIRE_LIBRARIES})
  target_include_directories(${TARGET_NAME} PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
  target_compile_options(${TARGET_NAME} PRIVATE ${PIPEWIRE_CFLAGS_OTHER})

  target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  target_link_libraries(${TARGET_NAME} PRIVATE X11::X11)
  target_link_libraries(${TARGET_NAME} PRIVATE X11::Xrandr)
  target_link_libraries(${TARGET_NAME} PRIVATE X11::Xdamage X11::Xfixes)
endfunction()

# wayland screencast hook

add_library(hook SHARED hook.cpp payload.cpp plthook_elf.c)
set_property(TARGET hook PROPERTY POSITION_INDEPENDENT_CODE ON)
add_compile_options_for(hook)
find_package(Threads REQUIRED)
target_link_libraries(hook PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
install(TARGETS hook DESTINATION lib/wemeet/)

option(WEMEET_BUILD_TESTS "Build forwarding and isolated portal regression checks" OFF)
if(WEMEET_BUILD_TESTS)
  enable_testing()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
  add_executable(capture-lifecycle tests/capture-lifecycle.cpp payload.cpp)
  add_compile_options_for(capture-lifecycle)
  target_link_libraries(capture-lifecycle PRIVATE Threads::Threads)
  add_test(NAME portal-cancel-retry COMMAND ${DBUS_RUN_SESSION} --
    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/mock-portal.py
    $<TARGET_FILE:capture-lifecycle>)
  set_tests_properties(portal-cancel-retry PROPERTIES TIMEOUT 20)
  add_executable(x11-forwarding tests/x11-forwarding.cpp)
  target_link_libraries(x11-forwarding PRIVATE X11::X11 X11::Xext X11::Xdamage)
  add_test(NAME x11-original-forwarding COMMAND ${CMAKE_COMMAND} -E env
    LD_PRELOAD=$<TARGET_FILE:hook> $<TARGET_FILE:x11-forwarding>)
  set_tests_properties(x11-original-forwarding PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 10)
  add_executable(frame-injection tests/frame-injection.cpp)
  add_compile_options_for(frame-injection)
  target_link_libraries(frame-injection PRIVATE hook)
  add_test(NAME first-frame-and-colors COMMAND frame-injection)
  set_tests_properties(first-frame-and-colors PROPERTIES TIMEOUT 10)
  add_library(screen-share-fixture SHARED tests/screen-share-fixture.cpp)
  set_target_properties(screen-share-fixture PROPERTIES OUTPUT_NAME screen_share_module
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
  add_executable(module-routing tests/module-routing.cpp)
  target_link_libraries(module-routing PRIVATE ${CMAKE_DL_LIBS})
  add_test(NAME screen-share-module-routing COMMAND ${CMAKE_COMMAND} -E env
    LD_PRELOAD=$<TARGET_FILE:hook> $<TARGET_FILE:module-routing> $<TARGET_FILE:screen-share-fixture>)
  set_tests_properties(screen-share-module-routing PROPERTIES TIMEOUT 10)
  add_test(NAME native-portal-module-routing COMMAND ${CMAKE_COMMAND} -E env
    WEMEET_CAPTURE_MODE=native-portal LD_PRELOAD=$<TARGET_FILE:hook>
    $<TARGET_FILE:module-routing> $<TARGET_FILE:screen-share-fixture> native)
  set_tests_properties(native-portal-module-routing PROPERTIES TIMEOUT 10)
  add_test(NAME native-portal-diagnostics COMMAND ${DBUS_RUN_SESSION} --
    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/native-monitor.py)
  set_tests_properties(native-portal-diagnostics PROPERTIES TIMEOUT 15)
  find_program(XVFB Xvfb)
  if(XVFB)
    add_executable(frame-damage tests/frame-damage.cpp)
    target_include_directories(frame-damage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(frame-damage PRIVATE X11::X11 X11::Xdamage X11::Xfixes)
    add_test(NAME pipewire-frame-damage COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/with-xvfb.py ${XVFB} $<TARGET_FILE:frame-damage>)
    set_tests_properties(pipewire-frame-damage PROPERTIES TIMEOUT 15)
    add_test(NAME native-portal-no-x11-injection COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/with-xvfb.py ${XVFB}
      ${CMAKE_COMMAND} -E env WEMEET_CAPTURE_MODE=native-portal LD_PRELOAD=$<TARGET_FILE:hook>
      $<TARGET_FILE:x11-forwarding> large)
    set_tests_properties(native-portal-no-x11-injection PROPERTIES TIMEOUT 15)
  endif()
endif()
