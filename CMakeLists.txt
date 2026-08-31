
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
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /WX")
  set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcompiler /W4")
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror")
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
target_include_directories(ServingObservatory PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(ServingObservatory PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME ServingObservatory
  POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
  target_compile_options(ServingObservatory PRIVATE /W4 /WX)
endif()

# ---------------------------------------------------------------- CLI
add_executable(obscli apps/obscli/cli.cpp)
target_link_libraries(obscli PRIVATE ServingObservatory)
if(WIN32)
  target_link_libraries(obscli PRIVATE ws2_32)
endif()

# ---------------------------------------------------------------- CUDA proof
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND AND SOBS_BUILD_CUDA)
  enable_language(CUDA)
  add_library(ServingObservatoryCuda STATIC cuda/cuda_proof.cu)
  target_link_libraries(ServingObservatoryCuda PUBLIC ServingObservatory)
  set_target_properties(ServingObservatoryCuda PROPERTIES CUDA_ARCHITECTURES "120")
  target_include_directories(ServingObservatoryCuda PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_definitions(obscli PRIVATE SOBS_HAVE_CUDA)
  target_link_libraries(obscli PRIVATE ServingObservatoryCuda)
  add_executable(sobs_cuda_proof cuda/cuda_proof_main.cpp)
  target_link_libraries(sobs_cuda_proof PRIVATE ServingObservatory ServingObservatoryCuda)
endif()

# ---------------------------------------------------------------- install/export
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ServingObservatory EXPORT ServingObservatoryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ServingObservatoryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ServingObservatoryConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ServingObservatory)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ServingObservatoryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(EXPORT ServingObservatoryTargets
  FILE ServingObservatoryTargets.cmake
  NAMESPACE ServingObservatory::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ServingObservatory)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/ServingObservatoryConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ServingObservatoryConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ServingObservatory)

# ---------------------------------------------------------------- tests
if(SOBS_BUILD_TESTS)
  enable_testing()
  add_executable(sobs_tests tests/test_main.cpp)
  target_link_libraries(sobs_tests PRIVATE ServingObservatory)
  target_include_directories(sobs_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME sobs_tests COMMAND sobs_tests)
endif()

# ---------------------------------------------------------------- examples
if(SOBS_BUILD_EXAMPLES)
  add_executable(example_basic examples/example_basic.cpp)
  target_link_libraries(example_basic PRIVATE ServingObservatory)
endif()
