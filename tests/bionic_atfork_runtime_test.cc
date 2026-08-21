#include "compat/bionic_atfork_runtime.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <string>

#include <gtest/gtest.h>

namespace {

std::array<char, 16> g_callback_trace{};
volatile sig_atomic_t g_callback_trace_size = 0;

void AppendCallbackTrace(char marker) {
  const sig_atomic_t index = g_callback_trace_size;
  if (index < static_cast<sig_atomic_t>(g_callback_trace.size())) {
    g_callback_trace[static_cast<size_t>(index)] = marker;
    g_callback_trace_size = index + 1;
  }
}

void PrepareFirst() { AppendCallbackTrace('A'); }
void ParentFirst() { AppendCallbackTrace('B'); }
void ChildFirst() { AppendCallbackTrace('C'); }
void PrepareSecond() { AppendCallbackTrace('D'); }
void ParentSecond() { AppendCallbackTrace('E'); }
void ChildSecond() { AppendCallbackTrace('F'); }

std::string CurrentTrace() {
  return std::string(g_callback_trace.data(),
                     static_cast<size_t>(g_callback_trace_size));
}

TEST(BionicAtForkRuntimeTest, RegistersCallbacksWithPosixOrdering) {
  g_callback_trace_size = 0;
  int first_dso = 0;
  int second_dso = 0;
  ASSERT_EQ(mocktail_bionic_register_atfork(PrepareFirst, ParentFirst,
                                            ChildFirst, &first_dso),
            0);
  ASSERT_EQ(mocktail_bionic_register_atfork(PrepareSecond, ParentSecond,
                                            ChildSecond, &second_dso),
            0);

  int child_trace_pipe[2] = {-1, -1};
  ASSERT_EQ(pipe(child_trace_pipe), 0);

  const pid_t child = fork();
  if (child == 0) {
    close(child_trace_pipe[0]);
    const size_t trace_size = static_cast<size_t>(g_callback_trace_size);
    const ssize_t written =
        write(child_trace_pipe[1], g_callback_trace.data(), trace_size);
    close(child_trace_pipe[1]);
    _exit(written == static_cast<ssize_t>(trace_size) ? 0 : 1);
  }
  ASSERT_NE(child, -1);

  close(child_trace_pipe[1]);
  std::array<char, 16> child_trace{};
  const ssize_t child_trace_size =
      read(child_trace_pipe[0], child_trace.data(), child_trace.size());
  close(child_trace_pipe[0]);

  int child_status = 0;
  ASSERT_EQ(waitpid(child, &child_status, 0), child);
  ASSERT_TRUE(WIFEXITED(child_status));
  ASSERT_EQ(WEXITSTATUS(child_status), 0);
  ASSERT_GE(child_trace_size, 0);

  // POSIX runs prepare callbacks in reverse registration order and both
  // parent and child callbacks in forward order.
  EXPECT_EQ(CurrentTrace(), "DABE");
  EXPECT_EQ(
      std::string(child_trace.data(), static_cast<size_t>(child_trace_size)),
      "DACF");
}

} // namespace
