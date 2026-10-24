cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(AdventureTimeVita C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Adventure Time")
set(VITA_TITLEID  "ATVM00001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -O2 -Wall -Wno-misleading-indentation -Wno-unused-function -Wno-unused-parameter")

add_executable(${PROJECT_NAME}
  src/vm.c src/jlib.c src/droid.c src/res.c src/audio.c src/input.c src/buttons.c src/main_vita.c)

target_link_libraries(${PROJECT_NAME}
  m
  SceDisplay_stub SceCtrl_stub SceTouch_stub ScePower_stub SceAudio_stub
)

# bubble icon + LiveArea artwork (turn off with -DWITH_ARTWORK=OFF if your Vita rejects the install)
option(WITH_ARTWORK "Bundle bubble icon and LiveArea artwork" ON)
set(ART_PAIRS "")
if(WITH_ARTWORK)
  set(ART_PAIRS
    sce_sys/icon0.png sce_sys/icon0.png
    sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png)
endif()

# bundle the sound effects (audio/*.wav) inside the VPK; the app reads them from app0:audio/
file(GLOB AUDIO_FILES RELATIVE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/audio/*.wav)
set(AUDIO_PAIRS "")
foreach(f ${AUDIO_FILES})
  list(APPEND AUDIO_PAIRS ${f} ${f})
endforeach()

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${ART_PAIRS} ${AUDIO_PAIRS}
)
