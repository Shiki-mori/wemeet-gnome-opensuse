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


find_package(X11 REQUIRED)

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
endfunction()

# wayland screencast hook

add_library(hook SHARED hook.cpp payload.cpp plthook_elf.c)
set_property(TARGET hook PROPERTY POSITION_INDEPENDENT_CODE ON)
add_compile_options_for(hook)
find_package(Threads REQUIRED)
target_link_libraries(hook PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
install(TARGETS hook DESTINATION lib/wemeet/)
