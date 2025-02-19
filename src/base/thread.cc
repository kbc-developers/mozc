// Copyright 2010-2016, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "base/thread.h"

#ifdef OS_WIN
#include <windows.h>
#include <process.h>  // for _beginthreadex
#else
#include <pthread.h>
#endif  // OS_WIN

#include <memory>

#include "base/logging.h"

namespace mozc {

#ifdef OS_WIN
// Win32-based Thread implementation.

namespace {

unsigned __stdcall WrapperForWindows(void *ptr) {
  Thread *p = static_cast<Thread *>(ptr);
  p->Run();
  return 0;
}

}  // namespace

struct ThreadInternalState {
 public:
  ThreadInternalState()
    : handle_(nullptr),
      joinable_(true) {}

  HANDLE handle_;
  bool joinable_;
};

void Thread::Start() {
  if (IsRunning()) {
    return;
  }

  Detach();
  state_->handle_ = reinterpret_cast<HANDLE>(_beginthreadex(
      nullptr, 0, WrapperForWindows, this, 0, nullptr));
}

bool Thread::IsRunning() const {
  DWORD result = 0;
  if (state_->handle_ == nullptr ||
      !::GetExitCodeThread(state_->handle_, &result)) {
    return false;
  }
  return (STILL_ACTIVE == result);
}

void Thread::Detach() {
  if (state_->handle_ != nullptr) {
    ::CloseHandle(state_->handle_);
    state_->handle_ = nullptr;
  }
}

void Thread::Join() {
  if (!state_->joinable_) {
    return;
  }
  if (state_->handle_ == nullptr) {
    return;
  }
  ::WaitForSingleObject(state_->handle_, INFINITE);
  ::CloseHandle(state_->handle_);
  state_->handle_ = nullptr;
}

void Thread::Terminate() {
  if (state_->handle_ != nullptr) {
    ::TerminateThread(state_->handle_, 0);
    state_->handle_ = nullptr;
  }
}

#else  // OS_WIN
// Thread implementation for pthread-based platforms. Currently all the
// platforms except for Windows use pthread.

struct ThreadInternalState {
 public:
  ThreadInternalState() : is_running_(false), joinable_(true) {}

  // As pthread_t is an opaque object, we use (pthread_t *)nullptr to
  // indicate that no thread is attached to this object.
  // When |handle_ != nullptr|, |*handle_| should indicate a
  // valid thread id.
  std::unique_ptr<pthread_t> handle_;
  bool is_running_;
  bool joinable_;
};

void Thread::Start() {
  if (IsRunning()) {
    return;
  }

  Detach();
  state_->is_running_ = true;
  state_->handle_.reset(new pthread_t);
  if (0 != pthread_create(state_->handle_.get(), 0, &Thread::WrapperForPOSIX,
                          static_cast<void *>(this))) {
      state_->is_running_ = false;
      state_->handle_.reset(nullptr);
  }
}

bool Thread::IsRunning() const {
  return state_->is_running_;
}

void Thread::Detach() {
  if (state_->handle_ != nullptr) {
    pthread_detach(*state_->handle_);
    state_->handle_.reset(nullptr);
  }
}

void Thread::Join() {
  if (!state_->joinable_) {
    return;
  }
  if (state_->handle_ == nullptr) {
    return;
  }
  pthread_join(*state_->handle_, nullptr);
  state_->handle_.reset(nullptr);
}

namespace {

#ifdef OS_ANDROID

void ExitThread(int sig) {
  pthread_exit(0);
}

// We don't have pthread_cancel for Android, so we'll use SIGUSR1 as
// work around.
void InitPThreadCancel() {
  struct sigaction actions;
  memset(&actions, 0, sizeof(actions));
  sigemptyset(&actions.sa_mask);
  actions.sa_flags = 0;
  actions.sa_handler = ExitThread;
  sigaction(SIGUSR1, &actions, nullptr);
}

void PThreadCancel(pthread_t thread_id) {
  const int pthread_kill_result = pthread_kill(thread_id, SIGUSR1);
  if (pthread_kill_result != 0) {
    // pthread_kill fails if
    //  EINVAL: in case that the specified handle_ is invalid
    //  ESRCH: in case that the thread is already terminated
    LOG(ERROR) << "Failed to kill a thread. error = " << pthread_kill_result
                << "(" << strerror(pthread_kill_result) << ")";
  }
}

#elif defined(OS_NACL)

void InitPThreadCancel() {
  // Nothing is required.
}

void PThreadCancel(pthread_t thread_id) {
  LOG(ERROR) << "In NaCl we have no way to cancel a thread.";
}

#else

void InitPThreadCancel() {
  // Nothing is required.
}

void PThreadCancel(pthread_t thread_id) {
  pthread_cancel(thread_id);
}

#endif  // OS_ANDROID or OS_NACL or others

#ifndef OS_NACL

void PThreadCleanupRoutine(void *ptr) {
  bool *is_running = static_cast<bool *>(ptr);
  *is_running = false;
}

#endif  // !OS_NACL

}  // namespace

void *Thread::WrapperForPOSIX(void *ptr) {
  Thread *p = static_cast<Thread *>(ptr);
  InitPThreadCancel();
#ifdef OS_NACL
  {
    p->Run();
    // TODO(horo): In NaCl we can't use pthread_cleanup_push() and
    // pthread_cleanup_pop(). So we set "is_running_ = false" here.
    // This hack makes the meaning of IsRunning() different in NaCl.
    p->state_->is_running_ = false;
  }
#else  // OS_NACL
  {
    // Caveat: the pthread_cleanup_push/pthread_cleanup_pop pair should be put
    //     in the same function. Never move them into any other function.
    pthread_cleanup_push(PThreadCleanupRoutine,
                         static_cast<void *>(&p->state_->is_running_));
    p->Run();
    pthread_cleanup_pop(1);
  }
#endif  // OS_NACL
  return nullptr;
}

void Thread::Terminate() {
  if (state_->handle_ != nullptr) {
    PThreadCancel(*state_->handle_);
    // pthread_cancel (or pthread_kill in PThreadCancel on Android) is
    // asynchronous. Join the thread to behave like TerminateThread on Windows.
    Join();
    state_->handle_.reset(nullptr);
  }
}

#endif  // OS_WIN

Thread::Thread() : state_(new ThreadInternalState) {}

Thread::~Thread() {
  Detach();
}

void Thread::SetJoinable(bool joinable) {
  state_->joinable_ = joinable;
}

}  // namespace mozc
