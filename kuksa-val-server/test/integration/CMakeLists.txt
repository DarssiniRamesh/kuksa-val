#
# ******************************************************************************
# Copyright (c) 2026
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
# ******************************************************************************

project(kuksa-val-server-integration-test)

enable_testing()
include(CTest)
include(FetchContent)

# Build these P0 tests only when the server executable is built.
if (NOT TARGET kuksa-val-server)
  message(STATUS "Skipping kuksa-val-server integration tests: kuksa-val-server target not present.")
  return()
endif()

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        release-1.11.0
)
FetchContent_GetProperties(googletest)
if(NOT googletest_POPULATED)
  FetchContent_Populate(googletest)
  add_subdirectory(${googletest_SOURCE_DIR} ${googletest_BINARY_DIR})
endif()

set(INTEGRATION_TEST_EXE kuksaval-server-integration-test)

add_executable(${INTEGRATION_TEST_EXE}
  ServerStartupAndGrpcIT.cpp
)

target_compile_features(${INTEGRATION_TEST_EXE} PRIVATE cxx_std_17)

# Provide repo root for locating test assets (VSS JSON + certificates) in a stable way.
# CMAKE_CURRENT_LIST_DIR = .../kuksa-val-server/test/integration
# Repo root = .../code-generation
get_filename_component(_KUKSA_VAL_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
target_compile_definitions(${INTEGRATION_TEST_EXE} PRIVATE KUKSA_VAL_REPO_ROOT="${_KUKSA_VAL_REPO_ROOT}")

# Include generated proto headers and server headers.
target_include_directories(${INTEGRATION_TEST_EXE} PRIVATE
  "${CMAKE_BINARY_DIR}/proto"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include/interface"
)

# Link to gRPC and protobuf.
target_link_libraries(${INTEGRATION_TEST_EXE} PRIVATE
  gtest_main
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF}
)

# The test spawns the server executable via absolute path provided by env.
add_test(
  NAME ${INTEGRATION_TEST_EXE}
  COMMAND $<TARGET_FILE:${INTEGRATION_TEST_EXE}>
)

set_tests_properties(${INTEGRATION_TEST_EXE}
  PROPERTIES
    ENVIRONMENT "KUKSA_VAL_SERVER_EXE=$<TARGET_FILE:kuksa-val-server>"
    TIMEOUT 60
)
