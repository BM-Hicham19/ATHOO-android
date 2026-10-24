cmake_minimum_required(VERSION 3.18)
project(avm C)

# the interpreter / game runtime lives in ../../../../../src (shared with the Vita build)
get_filename_component(SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src" ABSOLUTE)
set(GLUE "${ANDROID_NDK}/sources/android/native_app_glue")

add_library(avm SHARED
    ${SRC}/vm.c ${SRC}/jlib.c ${SRC}/droid.c ${SRC}/res.c ${SRC}/audio.c ${SRC}/input.c ${SRC}/buttons.c
    main_android.c
    ${GLUE}/android_native_app_glue.c)

target_include_directories(avm PRIVATE ${SRC} ${GLUE})
target_compile_options(avm PRIVATE -O2 -std=gnu99 -Wno-misleading-indentation -Wno-unused-function)
set_target_properties(avm PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
target_link_libraries(avm android log aaudio m)
