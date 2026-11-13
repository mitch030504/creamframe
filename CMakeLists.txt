# My version, use previous version at own risk
CMAKE_MINIMUM_REQUIRED(VERSION 3.9)

PROJECT(Creamlinux)
SET(CMAKE_CXX_STANDARD 14) # no explicit compiler flags if possible
SET(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

link_directories(${CMAKE_SOURCE_DIR}/lib)
link_libraries(${CMAKE_DL_LIBS})

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR})

# CREAM_ANDROID_PROXY builds the libsteam_api.so proxy used inside the Steam
# Frame/Lepton Android container. It uses the NDK toolchain and is not a
# drop-in replacement for the normal Linux shared library.
OPTION(CREAM_ANDROID_PROXY "Build the Android libsteam_api.so proxy" OFF)

ADD_LIBRARY(Creamlinux SHARED
	${CMAKE_SOURCE_DIR}/src/config.cpp
	${CMAKE_SOURCE_DIR}/src/steam_hooks.cpp
	${CMAKE_SOURCE_DIR}/src/export_hooks.cpp
)
message("CMAKE_SIZEOF_VOID_P is ${CMAKE_SIZEOF_VOID_P}")
if(CREAM_ANDROID_PROXY)
    set_target_properties(Creamlinux PROPERTIES
        PREFIX "lib"
        OUTPUT_NAME "steam_api"
    )
    target_compile_definitions(Creamlinux PRIVATE CREAM_ANDROID=1)
    target_compile_options(Creamlinux PRIVATE
        -fvisibility=hidden
        -fvisibility-inlines-hidden
    )
    target_link_options(Creamlinux PRIVATE
        -Wl,-soname,libsteam_api.so
        -Wl,-z,max-page-size=16384
        -Wl,--version-script=${CMAKE_SOURCE_DIR}/android/exports.map
        -Wl,--strip-all
    )
    # The proxy must record DT_NEEDED: libsteam_api_orig.so so the Android
    # linker resolves every non-hooked Steamworks symbol from the renamed
    # original. The stub is link-time only; the APK ships the real original
    # under that name.
    ADD_LIBRARY(steam_api_orig_stub SHARED ${CMAKE_SOURCE_DIR}/android/needed_stub.cpp)
    set_target_properties(steam_api_orig_stub PROPERTIES
        PREFIX "lib"
        OUTPUT_NAME "steam_api_orig"
        SONAME "libsteam_api_orig.so"
    )
    target_link_libraries(Creamlinux PRIVATE "-Wl,--no-as-needed" steam_api_orig_stub)
    target_link_libraries(Creamlinux PRIVATE android log)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # 64 bits
    set(CMAKE_SHARED_LIBRARY_PREFIX "lib64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    # 32 bits
    set(CMAKE_SHARED_LIBRARY_PREFIX "lib32")
endif()

if(NOT CREAM_ANDROID_PROXY)
# --- tests -----------------------------------------------------------------
enable_testing()
ADD_EXECUTABLE(creamlinux_tests
    ${CMAKE_SOURCE_DIR}/tests/test_config.cpp
    ${CMAKE_SOURCE_DIR}/src/config.cpp
)
add_test(NAME config COMMAND creamlinux_tests)

ADD_EXECUTABLE(creamlinux_vtable_test
    ${CMAKE_SOURCE_DIR}/tests/test_vtable.cpp
    ${CMAKE_SOURCE_DIR}/src/config.cpp
)
add_test(NAME vtable COMMAND creamlinux_vtable_test)

ADD_EXECUTABLE(creamlinux_fork_test
    ${CMAKE_SOURCE_DIR}/tests/test_fork.cpp
    ${CMAKE_SOURCE_DIR}/src/config.cpp
)
add_test(NAME fork COMMAND creamlinux_fork_test)

ADD_EXECUTABLE(creamlinux_flat_test
    ${CMAKE_SOURCE_DIR}/tests/test_flat.cpp
    ${CMAKE_SOURCE_DIR}/src/config.cpp
)
TARGET_LINK_LIBRARIES(creamlinux_flat_test ${CMAKE_DL_LIBS})
add_test(NAME flat COMMAND creamlinux_flat_test)

add_test(NAME cream_sh COMMAND bash
    ${CMAKE_SOURCE_DIR}/tests/test_cream_sh.sh
    ${CMAKE_SOURCE_DIR}/package)

add_custom_command(
        TARGET Creamlinux POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/package/
                ${CMAKE_CURRENT_BINARY_DIR}/lib/)
endif()

# If you want to build the testapp, you'll need to find libsteam_api and copy it to lib/libsteam_api.so
#ADD_EXECUTABLE(Testapp ${CMAKE_SOURCE_DIR}/test.cpp)
#TARGET_LINK_LIBRARIES(Testapp libsteam_api.so)
