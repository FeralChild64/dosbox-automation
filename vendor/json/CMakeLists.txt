# System nlohmann_json first, vendored v3.12.0 otherwise; json/json.h collides with jsoncpp on FreeBSD.
if (NOT VCPKG_TOOLCHAIN)
  # GLOBAL: the target is linked from src/ and tests/, outside this directory.
  find_package(nlohmann_json 3.11 CONFIG QUIET GLOBAL)
endif()

if (nlohmann_json_FOUND)
  message(STATUS "json: using system nlohmann_json ${nlohmann_json_VERSION}")
else()
  add_library(nlohmann_json INTERFACE)
  add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
  target_include_directories(nlohmann_json SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  message(STATUS "json: using vendored nlohmann_json 3.12.0")
endif()
