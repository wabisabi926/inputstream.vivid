cmake_minimum_required(VERSION 3.16)
project(inputstream.vivid)

set(ADDON_ID inputstream.vivid)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 关闭严格告警（晶晨头文件与 Kodi SDK 混编时噪声较多）
add_compile_options(-fpermissive -w)

# ==============================================
# Kodi addon SDK 头文件路径
# Kodi 21 Omega / 22 Piers 均为 xbmc/addons/kodi-dev-kit/include。
# 这里做多候选探测，兼容不同分支/打包方式的命名差异。
# ==============================================
if(NOT KODI_SOURCE_DIR)
  message(FATAL_ERROR "KODI_SOURCE_DIR 未设置，请用 -DKODI_SOURCE_DIR=<kodi_src> 指定 Kodi 源码目录")
endif()

foreach(_kit
    ${KODI_SOURCE_DIR}/xbmc/addons/kodi-dev-kit/include
    ${KODI_SOURCE_DIR}/xbmc/addons/kodi-addon-dev-kit/include
    ${KODI_SOURCE_DIR}/xbmc/addons/kodi-addon-kit/include)
  if(EXISTS ${_kit})
    include_directories(${_kit})
    message(STATUS "[inputstream.vivid] Kodi SDK: ${_kit}")
  endif()
endforeach()

include_directories(src)

# 自动搜索源码
file(GLOB_RECURSE SOURCES src/*.cpp)

# 编译插件
add_library(${ADDON_ID} SHARED ${SOURCES})
set_target_properties(${ADDON_ID} PROPERTIES PREFIX "")

# 系统依赖
target_link_libraries(${ADDON_ID} pthread dl)
