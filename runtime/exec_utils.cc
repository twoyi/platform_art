/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "exec_utils.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <vector>

#if defined(__ANDROID__) && defined(__aarch64__)
#include <twoyi_exec.h>
#endif

#include "android-base/stringprintf.h"
#include "android-base/strings.h"

#include "runtime.h"

namespace art {

using android::base::StringPrintf;

int ExecAndReturnCode(std::vector<std::string>& arg_vector, std::string* error_msg) {
  const std::string command_line(android::base::Join(arg_vector, ' '));
  CHECK_GE(arg_vector.size(), 1U) << command_line;

  // Convert the args to char pointers.
  const char* program = arg_vector[0].c_str();
  std::vector<char*> args;
  for (size_t i = 0; i < arg_vector.size(); ++i) {
    const std::string& arg = arg_vector[i];
    char* arg_str = const_cast<char*>(arg.c_str());
    CHECK(arg_str != nullptr) << i;
    args.push_back(arg_str);
  }
  args.push_back(nullptr);

#if defined(__ANDROID__) && defined(__aarch64__)
  struct twoyi_data_vfs_exec_clients exec_clients = {-1, -1};
  if (__twoyi_data_vfs_prepare_exec_clients(
          geteuid(), getegid(), &exec_clients) != 0) {
    const int saved_errno = errno;
    *error_msg = StringPrintf(
        "Failed to prepare B1 clients for execv(%s): %s",
        command_line.c_str(), strerror(saved_errno));
    return -1;
  }
#endif

  // fork and exec
  pid_t pid = fork();
#if defined(__ANDROID__) && defined(__aarch64__)
  if (pid != 0) {
    __twoyi_data_vfs_discard_exec_clients(&exec_clients);
  }
#endif
  if (pid == 0) {
    // no allocation allowed between fork and exec

    // change process groups, so we don't get reaped by ProcessManager
    setpgid(0, 0);

    // (b/30160149): protect subprocesses from modifications to LD_LIBRARY_PATH, etc.
    // Use the snapshot of the environment from the time the runtime was created.
    char** envp = (Runtime::Current() == nullptr) ? nullptr : Runtime::Current()->GetEnvSnapshot();
#if defined(__ANDROID__) && defined(__aarch64__)
    // Last operation before exec: replacing 192/195 makes both request
    // streams pristine without touching malloc or inherited B1 mutex state.
    if (__twoyi_data_vfs_install_exec_clients_after_fork(&exec_clients) != 0) {
      PLOG(ERROR) << "Failed to install B1 clients for execve(" << command_line << ")";
      _exit(1);
    }
#endif
    if (envp == nullptr) {
      execv(program, &args[0]);
    } else {
      execve(program, &args[0], envp);
    }
    PLOG(ERROR) << "Failed to execve(" << command_line << ")";
    // _exit to avoid atexit handlers in child.
    _exit(1);
  } else {
    if (pid == -1) {
      *error_msg = StringPrintf("Failed to execv(%s) because fork failed: %s",
                                command_line.c_str(), strerror(errno));
      return -1;
    }

    // wait for subprocess to finish
    int status = -1;
    pid_t got_pid = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));
    if (got_pid != pid) {
      *error_msg = StringPrintf("Failed after fork for execv(%s) because waitpid failed: "
                                "wanted %d, got %d: %s",
                                command_line.c_str(), pid, got_pid, strerror(errno));
      return -1;
    }
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    }
    return -1;
  }
}

bool Exec(std::vector<std::string>& arg_vector, std::string* error_msg) {
  int status = ExecAndReturnCode(arg_vector, error_msg);
  if (status != 0) {
    const std::string command_line(android::base::Join(arg_vector, ' '));
    *error_msg = StringPrintf("Failed execv(%s) because non-0 exit status",
                              command_line.c_str());
    return false;
  }
  return true;
}

}  // namespace art
