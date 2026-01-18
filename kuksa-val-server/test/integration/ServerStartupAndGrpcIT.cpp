/**********************************************************************
 * Copyright (c) 2026
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 **********************************************************************/

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "kuksa.grpc.pb.h"

#ifndef KUKSA_VAL_REPO_ROOT
#error "KUKSA_VAL_REPO_ROOT must be defined by CMake for this integration test."
#endif

namespace {

using namespace std::chrono_literals;

static std::filesystem::path repoRoot() {
  return std::filesystem::path(KUKSA_VAL_REPO_ROOT);
}

static std::filesystem::path vssFile() {
  return repoRoot() / "kuksa-val" / "data" / "vss-core" / "vss_release_4.0.json";
}

static std::filesystem::path certDir() {
  // Note: jwt.key.pub lives in kuksa_certificates/jwt, but main.cpp expects it under cert-path.
  // The repository also contains kuksa_certificates/jwt/jwt.key.pub and the server build copies it.
  // For this black-box test, we set cert-path to kuksa_certificates and additionally place
  // a symlink/copy of jwt.key.pub into that dir via config-file test (see below).
  return repoRoot() / "kuksa-val" / "kuksa_certificates";
}

static std::filesystem::path jwtPubKeyFile() {
  return repoRoot() / "kuksa-val" / "kuksa_certificates" / "jwt" / "jwt.key.pub";
}

static std::filesystem::path serverExePath() {
  // Provided by CMake via argv[0] assumption is brittle; we instead rely on executing the built target
  // using absolute path passed by CTest through $<TARGET_FILE:kuksa-val-server>.
  // In this test we read it from env set in CTest.
  const char* env = std::getenv("KUKSA_VAL_SERVER_EXE");
  if (env == nullptr || std::strlen(env) == 0) {
    return {};
  }
  return std::filesystem::path(env);
}

static pid_t spawnProcess(const std::filesystem::path& exe,
                          const std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 2);
  argv.push_back(const_cast<char*>(exe.c_str()));
  for (const auto& a : args) {
    argv.push_back(const_cast<char*>(a.c_str()));
  }
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid == -1) {
    return -1;
  }
  if (pid == 0) {
    // Child process: exec
    execv(exe.c_str(), argv.data());
    // If exec fails:
    std::perror("execv failed");
    std::_Exit(127);
  }
  return pid;
}

static void terminateProcess(pid_t pid) {
  if (pid <= 0) {
    return;
  }
  // Try graceful termination first.
  kill(pid, SIGTERM);

  for (int i = 0; i < 50; i++) {
    int status = 0;
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      return;
    }
    std::this_thread::sleep_for(100ms);
  }

  // Force kill if still running.
  kill(pid, SIGKILL);
  int status = 0;
  (void)waitpid(pid, &status, 0);
}

static bool waitForGrpcReady(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  // Server listens on fixed port 50051.
  const std::string target = "localhost:50051";

  while (std::chrono::steady_clock::now() < deadline) {
    auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    if (channel->WaitForConnected(std::chrono::system_clock::now() + 200ms)) {
      return true;
    }
    std::this_thread::sleep_for(100ms);
  }
  return false;
}

static kuksa::GetResponse grpcGetMetadataInsecure(const std::string& path) {
  auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
  auto stub = kuksa::kuksa_grpc_if::NewStub(channel);

  kuksa::GetRequest req;
  req.set_type(kuksa::RequestType::METADATA);
  req.add_path(path);

  kuksa::GetResponse resp;
  grpc::ClientContext ctx;
  const grpc::Status status = stub->get(&ctx, req, &resp);
  EXPECT_TRUE(status.ok());
  return resp;
}

static std::filesystem::path makeTempDirUnder(const std::filesystem::path& baseDir) {
  std::filesystem::create_directories(baseDir);
  std::string tmpl = (baseDir / "kuksa_it_tmp_XXXXXX").string();
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  char* result = mkdtemp(buf.data());
  if (result == nullptr) {
    return {};
  }
  return std::filesystem::path(result);
}

}  // namespace

TEST(KuksaValServerIT, StartsInsecureAndServesGrpcMetadata) {
  const auto exe = serverExePath();
  ASSERT_FALSE(exe.empty()) << "KUKSA_VAL_SERVER_EXE env var must be set by CTest.";
  ASSERT_TRUE(std::filesystem::exists(exe)) << "Server exe not found at: " << exe;

  ASSERT_TRUE(std::filesystem::exists(vssFile())) << "Missing VSS file: " << vssFile;
  ASSERT_TRUE(std::filesystem::exists(certDir())) << "Missing cert directory: " << certDir;

  // NOTE: In insecure mode, TLS files are not required for gRPC listener, but main.cpp still requires cert-path
  // to exist and must be able to load jwt.key.pub. The repo keeps jwt.key.pub under kuksa_certificates/jwt.
  // For this test, we create a temp dir and place jwt.key.pub there as expected by main.cpp (cert-path/jwt.key.pub).
  const auto tmpDir = makeTempDirUnder(repoRoot() / "kuksa-val" / "kuksa-val-server" / "test" / "integration");
  ASSERT_FALSE(tmpDir.empty());

  const auto tmpCertDir = tmpDir / "certs";
  std::filesystem::create_directories(tmpCertDir);

  // Copy jwt.key.pub to cert-path root (main.cpp expects cert-path/jwt.key.pub).
  std::filesystem::copy_file(jwtPubKeyFile(), tmpCertDir / "jwt.key.pub",
                             std::filesystem::copy_options::overwrite_existing);

  // Start server in insecure mode.
  const pid_t pid = spawnProcess(exe, {
                                          "--insecure",
                                          "--vss",
                                          vssFile().string(),
                                          "--cert-path",
                                          tmpCertDir.string(),
                                  });
  ASSERT_GT(pid, 0) << "Failed to spawn server process";

  // Ensure we always terminate the process.
  struct Guard {
    pid_t pid;
    ~Guard() { terminateProcess(pid); }
  } guard{pid};

  ASSERT_TRUE(waitForGrpcReady(6s)) << "gRPC port 50051 did not become ready in time";

  // P0 core flow: call GetMetaData (does not require authorization).
  const auto resp = grpcGetMetadataInsecure("Vehicle.Speed");
  EXPECT_EQ(resp.status().statuscode(), 200);
  ASSERT_GE(resp.values_size(), 1);
  EXPECT_FALSE(resp.values(0).valuestring().empty());
}

TEST(KuksaValServerIT, StartsViaConfigFileAndResolvesRelativePaths) {
  const auto exe = serverExePath();
  ASSERT_FALSE(exe.empty()) << "KUKSA_VAL_SERVER_EXE env var must be set by CTest.";
  ASSERT_TRUE(std::filesystem::exists(exe)) << "Server exe not found at: " << exe;

  ASSERT_TRUE(std::filesystem::exists(vssFile())) << "Missing VSS file: " << vssFile;

  // Create a temp directory under repo so relative paths make sense and test main.cpp's
  // "rewrite vss/cert-path relative to config file directory" behavior.
  const auto tmpDir = makeTempDirUnder(repoRoot() / "kuksa-val" / "kuksa-val-server" / "test" / "integration");
  ASSERT_FALSE(tmpDir.empty());

  const auto tmpCertDir = tmpDir / "certs";
  std::filesystem::create_directories(tmpCertDir);
  std::filesystem::copy_file(jwtPubKeyFile(), tmpCertDir / "jwt.key.pub",
                             std::filesystem::copy_options::overwrite_existing);

  // Create config.ini with RELATIVE paths.
  // Note: main.cpp will make them absolute based on config file parent dir.
  const auto configPath = tmpDir / "config.ini";
  {
    std::ofstream ofs(configPath);
    ASSERT_TRUE(ofs.good());
    ofs << "vss = " << std::filesystem::relative(vssFile(), tmpDir).string() << "\n";
    ofs << "cert-path = " << std::filesystem::relative(tmpCertDir, tmpDir).string() << "\n";
    ofs << "log-level = NONE\n";
  }

  const pid_t pid = spawnProcess(exe, {
                                          "--insecure",
                                          "--config-file",
                                          configPath.string(),
                                  });
  ASSERT_GT(pid, 0) << "Failed to spawn server process";

  struct Guard {
    pid_t pid;
    ~Guard() { terminateProcess(pid); }
  } guard{pid};

  ASSERT_TRUE(waitForGrpcReady(6s)) << "gRPC port 50051 did not become ready in time";

  const auto resp = grpcGetMetadataInsecure("Vehicle.Speed");
  EXPECT_EQ(resp.status().statuscode(), 200);
}
