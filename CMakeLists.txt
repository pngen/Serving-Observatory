cmake_minimum_required(VERSION 3.20)

project(ServingObservatory
        VERSION 1.0.0
        DESCRIPTION "Serving Observatory — serving-observability runtime"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SOBS_BUILD_TESTS "Build tests" ON)
option(SOBS_BUILD_BENCHMARKS "Build benchmarks" ON)
option(SOBS_BUILD_EXAMPLES "Build examples" ON)
option(SOBS_BUILD_CUDA "Build CUDA proof (requires CUDA + sm_120 device)" ON)

if(MSVC)
  add_compile_options(/W4 /WX)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

add_compile_definitions(SOBS_VERSION_MAJOR=1 SOBS_VERSION_MINOR=0 SOBS_VERSION_PATCH=0)
add_compile_definitions(SOBS_VERSION_STRING="1.0.0")

# ---------------------------------------------------------------- library
add_library(ServingObservatory STATIC
  src/core.cpp
  src/observation.cpp
  src/authority.cpp
  src/trace.cpp
  src/latency.cpp
  src/attribution.cpp
  src/analysis.cpp
  src/protocol.cpp
  src/network.cpp
  src/coordinator.cpp
  src/source.cpp
  src/replay.cpp
  src/persistence.cpp
  src/counterfactual.cpp
  src/compare.cpp
)
if(WIN32)
  target_link_libraries(ServingObservatory PUBLIC ws2_32)
endif()
target_include_directories(ServingObservatory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(ServingObservatory PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME ServingObservatory
  POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
  target_compile_options(ServingObservatory PRIVATE /W4 /WX)
endif()

# ---------------------------------------------------------------- tests
if(SOBS_BUILD_TESTS)
  enable_testing()
  add_executable(sobs_tests tests/test_main.cpp)
  target_link_libraries(sobs_tests PRIVATE ServingObservatory)
  target_include_directories(sobs_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME sobs_tests COMMAND sobs_tests)
endif()
