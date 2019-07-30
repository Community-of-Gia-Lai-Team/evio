// evio -- Event Driven I/O support.
//
//! @file
//! @brief Definition of class EventLoopThread.
//
// Copyright (C) 2018 Carlo Wood.
//
// RSA-1024 0x624ACAD5 1997-01-26                    Sign & Encrypt
// Fingerprint16 = 32 EC A7 B6 AC DB 65 A6  F6 F6 55 DD 1C DC FF 61
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "sys.h"
#include "EventLoopThread.h"
#include "InputDevice.h"
#include "OutputDevice.h"
#include "threadpool/AIThreadPool.h"
#include "utils/cpu_relax.h"
#include "debug.h"
#include <chrono>

#ifndef DEBUG_EPOLL_PWAIT_DELAY_MICROSECONDS
#define DEBUG_EPOLL_PWAIT_DELAY_MICROSECONDS 0
#endif

std::string epoll_events_str(uint32_t events);

namespace evio {

// Singleton initialization.
void EventLoopThread::init(AIQueueHandle handler)
{
  DoutEntering(dc::evio, "EventLoopThread::init(" << handler << ')');
  m_handler = handler;

  // Create the thread running the loop around epoll_pwait.
  m_event_thread = std::thread(&EventLoopThread::main, &EventLoopThread::instance());

  // Wait till we're actually running.
  while (!m_running)
    cpu_relax();
}

EventLoopThread::~EventLoopThread()
{
  DoutEntering(dc::evio, "EventLoopThread::~EventLoopThread()");
  // Call EventLoopThread::instance().terminate() before leaving main().
  ASSERT(!m_event_thread.joinable());

  if (m_epoll_fd != -1)        // Was init() called?
  {
//    if (m_terminate)
//      ev_ref();
    if (::close(m_epoll_fd) == -1)
      Dout(dc::warning|error_cf, "close(" << m_epoll_fd << ") = -1");
  }
}

//static
struct epoll_event EventLoopThread::s_events[maxevents];

//static
void EventLoopThread::s_wakeup_handler(int)
{
  EventLoopThread& self(instance());
  if (self.m_terminate == forced || (self.m_terminate == cleanly && self.m_active == 0))
  {
    Dout(dc::evio, "EventLoopThread::s_wakeup_handler: Stopping event loop thread because m_active == 0.");
    self.m_stop_running.store(true, std::memory_order_relaxed);
  }
}

// EventLoopThread main function.
void EventLoopThread::main()
{
  Debug(NAMESPACE_DEBUG::init_thread("EventLoopThr"));
  DoutEntering(dc::evio, "EventLoopThread::main()");

  Dout(dc::system|continued_cf, "epoll_create1(EPOLL_CLOEXEC) = ");
  m_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  Dout(dc::finish|cond_error_cf(m_epoll_fd == -1), m_epoll_fd);
  if (m_epoll_fd == -1)
    DoutFatal(dc::fatal, "Failed to obtain an epoll file descriptor.");

  // Prepare a sigset for the signal(s) that we use to wake up epoll_pwait in epoll_sigmask.
  // Unblock those signals.
  sigset_t epoll_sigmask;
  utils::Signals::unblock(&epoll_sigmask, m_epoll_signum, &s_wakeup_handler);

  // The current signal mask is the mask that we want to use while inside epoll_pwait too.
  // It will be copied to pwait_sigmask before entering epoll_pwait.
  sigset_t pwait_sigmask;
  sigemptyset(&pwait_sigmask);

  m_stop_running = false;
  m_running = true;

  AIThreadPool& thread_pool(AIThreadPool::instance());
  auto queues_access = thread_pool.queues_read_access();
  auto& queue = thread_pool.get_queue(queues_access, m_handler);

  while (!m_stop_running.load(std::memory_order_relaxed))
  {
    // Before entering epoll_pwait, block the signal(s) that can change our wake-up flags.
    sigprocmask(SIG_BLOCK, &epoll_sigmask, &pwait_sigmask);
    int nfds;
    do
    {
      // While m_epoll_signum is blocked, deal with a wake-up and see if we must terminate.
      if (AI_UNLIKELY(m_stop_running.load(std::memory_order_relaxed)))
      {
        nfds = -1;
        garbage_collection();
        break;
      }
      Dout(dc::system|continued_cf|flush_cf, "epoll_pwait() = ");
#ifdef CWDEBUG
      utils::InstanceTracker<FileDescriptorBase>::for_each([](FileDescriptorBase const* p){ Dout(dc::system, p << ": " << p->get_fd() << ", " << p->get_flags()); });
#endif
      nfds = epoll_pwait(m_epoll_fd, s_events, maxevents, -1, &pwait_sigmask);
      Dout(dc::finish|cond_error_cf(nfds == -1), nfds);
    }
    while (nfds == -1 && errno == EINTR);
    // Unblock the signal(s) that can change our wake-up flags again by restoring the old set.
    sigprocmask(SIG_SETMASK, &pwait_sigmask, NULL);

#if DEBUG_EPOLL_PWAIT_DELAY_MICROSECONDS
    std::this_thread::sleep_for(std::chrono::microseconds(DEBUG_EPOLL_PWAIT_DELAY_MICROSECONDS));
#endif

    // Handle the returned event(s) for each fd.
    while (nfds > 0)
    {
      epoll_event& event(s_events[--nfds]);

      FileDescriptor* device = static_cast<FileDescriptor*>(event.data.ptr);
      uint32_t already_being_processed_by_thread_pool = device->test_and_set_being_processed_by_thread_pool(event.events);
      Dout(dc::evio((already_being_processed_by_thread_pool & event.events) != 0),
          "epoll_pwait event(s) " << epoll_events_str(already_being_processed_by_thread_pool & event.events) << " of fd " << device <<
          " ignored because the event(s) are being handled by the thread pool.");
      event.events &= ~already_being_processed_by_thread_pool;
      if (event.events == 0)
        continue;       // The FileDescriptor is being_processed_by_thread_pool for all returned events.
      Dout(dc::evio, "epoll_pwait new event: " << event);

      CWDEBUG_ONLY(bool queue_was_full = false;)
      {
        bool queue_full;
        auto queue_access = queue.producer_access();
        do
        {
          if ((queue_full = queue_access.length() == queue.capacity()))
          {
            // Queue is full! Wait for a broadcast.
            Dout(dc::warning(!queue_was_full), "Thread pool queue " << m_handler << " is full! Now no longer handling any socket etc. I/O until this is resolved.");
            Debug(queue_was_full = true);
            queue_access.wait();                    // Wait until queue_access.notify_one() is called.
          }
          else
          {
            Dout(dc::warning(queue_was_full), "Queue is no longer full; resuming I/O.");
            Dout(dc::evio, "Queuing I/O event " << event << " for " << device << " in thread pool queue " << m_handler);
            device->inhibit_deletion();
            queue_access.move_in([device, events = event.events, epoll_fd = m_epoll_fd](){
              int need_allow_deletion = 1;      // Balance with the call to inhibit_deletion() above.
              if (AI_UNLIKELY(events & ~(EPOLLIN|EPOLLOUT)))
              {
                if ((events & EPOLLHUP))
                {
                  device->hup_event(need_allow_deletion);
                  device->close();      // Leaving this alive would cause a flood of events.
                  device->clear_being_processed_by_thread_pool(epoll_fd, EPOLLHUP);
                }
                else if ((events & EPOLLERR))
                {
                  device->exceptional_event(need_allow_deletion);
                  device->clear_being_processed_by_thread_pool(epoll_fd, EPOLLERR);
                }
                else
                  DoutFatal(dc::core, "event.events = " << std::hex << events);
              }
              else
              {
                if ((events & EPOLLIN))
                {
                  device->read_event(need_allow_deletion);
                  device->clear_being_processed_by_thread_pool(epoll_fd, EPOLLIN);
                }
                if ((events & EPOLLOUT))
                {
                  device->write_event(need_allow_deletion);
                  device->clear_being_processed_by_thread_pool(epoll_fd, EPOLLOUT);
                }
              }
              device->allow_deletion(need_allow_deletion);
              return false;
            });
          }
        }
        while (queue_full);
      }
      queue.notify_one();
    }
    garbage_collection();
  }

  m_running = false;

  // Deinit.
  ASSERT(m_terminate == forced || m_active == 0);
  utils::Signals::block_and_unregister(m_epoll_signum);
  Dout(dc::system|continued_cf, "close(" << m_epoll_fd << ") = ");
  CWDEBUG_ONLY(int res =) ::close(m_epoll_fd);
  Dout(dc::finish|cond_error_cf(res == -1), res);
  m_epoll_fd = -1;
  m_terminate = not_yet;
  // Keep the value of m_epoll_signum!

  Dout(dc::evio, "Leaving EventLoopThread::main()");
}

void EventLoopThread::terminate(bool normal_exit)
{
  DoutEntering(dc::evio, "EventLoopThread::terminate(" << normal_exit << ")");
  // This function should only be called from the destructor of EventLoop.
  // Do not call terminate() directly.
  ASSERT(aithreadid::in_main_thread());
  {
    m_terminate = normal_exit ? cleanly : forced;
    bump_terminate();
  }
  if (m_event_thread.joinable())
  {
    Dout(dc::evio|continued_cf|flush_cf, "Joining m_event_thread... ");
    m_event_thread.join();
    Dout(dc::finish, "joined");
  }
}

#if 0
void EventLoopThread::invoke_pending()
{
  DoutEntering(dc::evio|flush_cf, "EventLoopThread::invoke_pending()");
  std::unique_lock<std::mutex> lock(m_loop_mutex);
  ASSERT(ev_pending_count());
  ev_invoke_pending();

  // Instead of returning control to the ev_run thread immediately, do a
  // quick poll here if there is already more activity in the meantime.
  if (!m_inside_invoke_pending && !ev_requested_break())
  {
    // No recursive calls.
    m_inside_invoke_pending = true;
    // Poll for additional events that might have occurred.
    ev_set_invoke_pending_cb(ev_invoke_pending);                  // Call ev_invoke_pending directly.
    Dout(dc::evio, "Entering ev_run(EVRUN_NOWAIT).");
    ev_run(EVRUN_NOWAIT);
    Dout(dc::evio, "Leaving ev_run(EVRUN_NOWAIT).");
    ev_set_invoke_pending_cb(EventLoopThread::invoke_pending_cb); // Restore normal operation.
    m_inside_invoke_pending = false;
  }

  // Notify ev_run thread.
  Dout(dc::evio, "Setting m_invoke_handled to true and notifying ev_run thread.");
  m_invoke_handled = true;
  m_invoke_handled_cv.notify_one();
}

void EventLoopThread::handle_invoke_pending()
{
  // Seriously, why does this even happen? The reason is apparently that libev
  // calls EV_INVOKE_PENDING at the beginning of ev_run, at which point it
  // obviously has nothing pending.
  if (!ev_pending_count())
    return;

  DoutEntering(dc::evio|flush_cf, "EventLoopThread::handle_invoke_pending()");
  // The lock is already locked when we get here.
  std::unique_lock<std::mutex> lock(m_loop_mutex, std::adopt_lock);
  while (ev_pending_count())
  {
    if (AI_UNLIKELY(m_terminate == forced))
    {
      Dout(dc::evio, "Forced exit from event loop.");
      break;
    }
    else
    {
      AIThreadPool& thread_pool(AIThreadPool::instance());
      //auto const max_duration = std::chrono::milliseconds(64);
      //auto duration = std::chrono::milliseconds(1);
      auto queues_access = thread_pool.queues_read_access();
      auto& queue = thread_pool.get_queue(queues_access, m_handler);
      CWDEBUG_ONLY(bool queue_was_full = false;)
      {
        bool queue_full;
        auto queue_access = queue.producer_access();
        do
        {
          if ((queue_full = queue_access.length() == queue.capacity()))
          {
            // Queue is full! Wait for a broadcast.
            Dout(dc::warning(!queue_was_full), "Thread pool queue " << m_handler << " is full! Now no longer handling any filedescriptor I/O until this is resolved.");
            Debug(queue_was_full = true);
            queue_access.wait();        // Wait until queue_access.notify_one() is called.
          }
          else
          {
            Dout(dc::warning(queue_was_full), "Queue is no longer full; resuming I/O.");
            Dout(dc::evio, "Queuing call to invoke_pending() in thread pool queue " << m_handler);
            queue_access.move_in([this](){ invoke_pending(); return false; });
          }
        }
        while (queue_full);
      }
      queue.notify_one();
    }

    // Wait until invoke_pending() was called.
    m_invoke_handled = false;
    Dout(dc::evio, "Waiting for m_invoke_handled to become true.");
    m_invoke_handled_cv.wait(lock, [this](){ return m_invoke_handled; });
    Dout(dc::evio, "m_invoke_handled is now true.");
  }
  // Leave the mutex locked.
  lock.release();
  if (AI_UNLIKELY(m_terminate == forced))
    stop_running();
  Dout(dc::evio|flush_cf, "Leaving EventLoopThread::handle_invoke_pending()");
}
#endif

void EventLoopThread::wake_up()
{
  // Test if EventLoopThread::init was called by testing if m_event_thread is joinable.
  if (m_event_thread.joinable())
  {
    Dout(dc::evio, "Sending wake-up signal " << m_epoll_signum);
    pthread_kill(m_event_thread.native_handle(), m_epoll_signum);
  }
  else
    Dout(dc::warning, "Calling EventLoopThread::wake_up(), but event thread is not running. Did you create an EventLoop object at the start of main()?");
}

void EventLoopThread::bump_terminate()
{
  if (m_terminate)
    wake_up();
}

void EventLoopThread::handle_regular_file(FileDescriptorFlags::mask_t active_flag, FileDescriptor* device)
{
  DoutEntering(dc::evio, "EventLoopThread::handle_regular_file(" << active_flag << ", " << device << ")");
  AIThreadPool& thread_pool(AIThreadPool::instance());
  auto queues_access = thread_pool.queues_read_access();
  auto& queue = thread_pool.get_queue(queues_access, m_handler);
  CWDEBUG_ONLY(bool queue_was_full = false;)
  {
    bool queue_full;
    auto queue_access = queue.producer_access();
    do
    {
      if ((queue_full = queue_access.length() == queue.capacity()))
      {
        // Queue is full! Wait for a broadcast.
        Dout(dc::warning(!queue_was_full), "Thread pool queue " << m_handler << " is full! Now no longer handling any filedescriptor I/O until this is resolved.");
        Debug(queue_was_full = true);
        queue_access.wait();        // Wait until queue_access.notify_one() is called.
      }
      else
      {
        Dout(dc::warning(queue_was_full), "Queue is no longer full; resuming I/O.");
        Dout(dc::evio, "Queuing call to " << ((active_flag == FileDescriptorFlags::FDS_R_ACTIVE) ? "read_event" : "write_event") << "() in thread pool queue " << m_handler);
        device->inhibit_deletion();
        if (active_flag == FileDescriptorFlags::FDS_R_ACTIVE)
          queue_access.move_in([device](){
              int need_allow_deletion = 1;      // The balance the call to inhibit_deletion above.
              NAD_CALL(device->read_event);
              device->allow_deletion(need_allow_deletion);
              return false;
          });
        else // active_flag == FileDescriptorFlags::FDS_W_ACTIVE
          queue_access.move_in([device](){
              int need_allow_deletion = 1;      // The balance the call to inhibit_deletion above.
              NAD_CALL(device->write_event);
              device->allow_deletion(need_allow_deletion);
              return false;
          });
      }
    }
    while (queue_full);
  }
  queue.notify_one();
}

void EventLoopThread::start(FileDescriptor::state_t::wat const& state_w, FileDescriptorFlags::mask_t active_flag, FileDescriptor* device)
{
  // Don't start a device that is disabled.
  if (AI_UNLIKELY(state_w->m_flags.test_disabled(active_flag)))
  {
    Dout(dc::warning, "Calling EventLoopThread::start(" << *state_w << ", " << active_flag << ", " << device << ") for a device that is disabled [" << this << "]");
    return;
  }

  DoutEntering(dc::evio, "EventLoopThread::start(" << *state_w << ", " << active_flag << ", " << device << ")");

  // Don't start a device that is already active.
  if (!state_w->m_flags.test_and_set_active(active_flag))
    return;

  bool needs_adding = state_w->m_flags.test_and_set_added(active_flag);

  if (!state_w->m_flags.test_inferior(active_flag))
  {
    CWDEBUG_ONLY(int active =) ++m_active;
    Dout(dc::evio, "Incremented m_active to " << active);
  }
  else
    Dout(dc::evio, "Not incrementing m_active because inferior device!");

  if (AI_LIKELY(!state_w->m_flags.is_regular_file()))
  {
    if (needs_adding)
    {
      // Increment ref count to stop device from being deleted while being active.
      // Object is kept alive until a call to allow_deletion(), which will be indirectly caused automatically
      // as a result of calling InputDevice::remove_input_device() (or InputDevice::close_input_device,
      // which also calls InputDevice::remove_input_device).
      CWDEBUG_ONLY(int count =) device->inhibit_deletion();
      Dout(dc::evio, "Incremented ref count (now " << (count + 1) << ") [" << device << ']');
    }
    device->start_watching(state_w, m_epoll_fd, FileDescriptorFlags::active_to_events(active_flag), needs_adding);
  }
  else
    handle_regular_file(active_flag, device);
}

bool EventLoopThread::start_if(FileDescriptor::state_t::wat const& state_w, utils::FuzzyCondition const& condition, FileDescriptorFlags::mask_t active_flag, FileDescriptor* device)
{
  // Unlikely because you shouldn't call this function if this is the case, see below.
  if (AI_UNLIKELY(condition.is_false()))
  {
    Dout(dc::warning, "Calling EventLoopThread::start_if(" << condition << ", " << active_flag << ", " << device << ") -- don't call start_if when it is sure that it will fail?!");
    return false;
  }

  // Don't start a device that is disabled.
  if (AI_UNLIKELY(state_w->m_flags.test_disabled(active_flag)))
  {
    Dout(dc::warning, "Calling EventLoopThread::start_if(" << condition << ", " << active_flag << ", " << device << ") for a device that is disabled.");
    return true;
  }

  DoutEntering(dc::evio, "EventLoopThread::start_if(" << condition << ", " << active_flag << ", " << device << ")");

  // This should never happen. First of all, for speed up reasons you should only call
  // this function when condition.is_momentary_true(), secondly if the condition would
  // be transitory_false it is nonsense to check the condition again here (we really
  // only want to call start_watching when the condition is true while m_loop_mutex is
  // locked) because if the condition changed from false to true due to another thread
  // then another thread either called this function or wrote to an empty buffer, both
  // of the cases means that it is a put thread; that should never happen since WE are
  // the put thread (only the put thread is expected to call this function).
  //
  // Another case where this assert might fail is when you call (for example)
  // start_output_device() from  a random thread without guaranteeing (by other means)
  // that the device is really stopped. For example, you should not call enable_output_device()
  // without first calling disable_output_device().
  ASSERT(!condition.is_transitory_false());

  // Don't start a device that is already active.
  if (!state_w->m_flags.test_and_set_active(active_flag))
    return true;

  // This is likely because otherwise we shouldn't be using a construction with
  // FuzzyBool / FuzzyCondition in the first place.
  if (AI_LIKELY(condition.is_transitory_true()))
  {
    // Re-test condition in critical area. Note that the critical area is actually the area where m_state is locked
    // which started much sooner. Nevertheless, it is only really required to be locked for testing the condition
    // (again) here. The reason the mutex isn't constantly released before this point is because the code between
    // where it is locked and this point is pretty minimal (just some non-atomic bit fiddling). See README.devices
    // for more information.
    if (condition().is_momentary_false())
    {
      state_w->m_flags.clear_active(active_flag);
      return false;
    }
  }
#ifdef CWDEBUG
  else // is_true()
    Dout(dc::warning, "Calling EventLoopThread::start_if(" << condition << ", " << active_flag <<", " << device << ") -- just call start() without condition?!");
#endif

  bool needs_adding = state_w->m_flags.test_and_set_added(active_flag);

  if (!state_w->m_flags.test_inferior(active_flag))
  {
    CWDEBUG_ONLY(int active =) ++m_active;
    Dout(dc::evio, "Incremented m_active to " << active);
  }
  else
    Dout(dc::evio, "Not incrementing m_active because inferior device!");

  if (AI_LIKELY(!state_w->m_flags.is_regular_file()))
  {
    if (needs_adding)
    {
      // Increment ref count to stop device from being deleted while being active.
      // It is kept alive until a call to allow_deletion().
      CWDEBUG_ONLY(int count =) device->inhibit_deletion();
      Dout(dc::io, "Incremented ref count (now " << (count + 1) << ") [" << device << ']');
    }
    device->start_watching(state_w, m_epoll_fd, FileDescriptorFlags::active_to_events(active_flag), needs_adding);
  }
  else
    handle_regular_file(active_flag, device);

  return true;
}

NAD_DECL(EventLoopThread::remove, FileDescriptor::state_t::wat const& state_w, FileDescriptorFlags::mask_t active_flag, FileDescriptor* device)
{
  DoutEntering(dc::evio, "EventLoopThread::remove({" NAD_DoutEntering_ARG0 << *state_w << "}, " << active_flag << ", " << device << ")");
  bool needs_removal = state_w->m_flags.test_and_clear_added(active_flag) && !state_w->m_flags.is_added();
  bool cleared_active = state_w->m_flags.test_and_clear_active(active_flag);
  if (cleared_active || needs_removal)
  {
    if (AI_LIKELY(!state_w->m_flags.is_regular_file()))
    {
      device->stop_watching(state_w, m_epoll_fd, FileDescriptorFlags::active_to_events(active_flag), needs_removal);
      if (needs_removal)
        ++need_allow_deletion;
    }
  }
  if (cleared_active && !state_w->m_flags.test_inferior(active_flag))
  {
    int active = --m_active;
    Dout(dc::evio, "Decremented m_active to " << active);
    if (active == 0)
      bump_terminate();
  }
}

void EventLoopThread::stop(FileDescriptor::state_t::wat const& state_w, FileDescriptorFlags::mask_t active_flag, FileDescriptor* device)
{
  // Don't stop a device that is already non-active.
  if (!state_w->m_flags.test_and_clear_active(active_flag))
    return;

  if (AI_LIKELY(!state_w->m_flags.is_regular_file()))
    device->stop_watching(state_w, m_epoll_fd, FileDescriptorFlags::active_to_events(active_flag), false);

  if (!state_w->m_flags.test_inferior(active_flag))
  {
    int active = --m_active;
    Dout(dc::evio, "Decremented m_active to " << active);
    if (active == 0)
      bump_terminate();
  }
}

bool EventLoopThread::stop_if(FileDescriptor::state_t::wat const& state_w, utils::FuzzyCondition const& condition, FileDescriptorFlags::mask_t active_flag, FileDescriptor* device)
{
  // Unlikely because you shouldn't call this function if this is the case, see below.
  if (AI_UNLIKELY(condition.is_false()))
  {
    Dout(dc::warning, "Calling EventLoopThread::stop_if(" << condition << ", " << active_flag << ", " << device << ") -- don't call stop_if when it is sure that it will fail?!");
    return false;
  }

  // See start_if.
  ASSERT(!condition.is_transitory_false());

  // Don't stop a device that is already non-active.
  if (!state_w->m_flags.test_and_clear_active(active_flag))
    return true;

  // This is likely because otherwise we shouldn't be using a construction with
  // FuzzyBool / FuzzyCondition in the first place.
  if (AI_LIKELY(condition.is_transitory_true()))
  {
    // Re-test condition in critical area. Note that the critical area is actually where m_state is locked (so that began way sooner).
    // In reality, it is only really needed to have that locked right here, before testing the condition again. We're not releasing
    // that lock constantly however, because the code from where it was locked till here is very minimal (just some non-atomic bit fiddling).
    if (condition().is_momentary_false())
    {
      // Revert the clear of the active flag.
      state_w->m_flags.set_active(active_flag);
      return false;
    }
  }
#ifdef CWDEBUG
  else // is_true()
    Dout(dc::warning, "Calling EventLoopThread::stop_if(" << condition << ", " << active_flag << ", " << device << ") -- just call stop() without condition?!");
#endif

  if (AI_LIKELY(!state_w->m_flags.is_regular_file()))
    device->stop_watching(state_w, m_epoll_fd, FileDescriptorFlags::active_to_events(active_flag), false);

  if (!state_w->m_flags.test_inferior(active_flag))
  {
    int active = --m_active;
    Dout(dc::evio, "Decremented m_active to " << active);
    if (active == 0)
      bump_terminate();
  }

  return true;
}

namespace {
SingletonInstance<EventLoopThread> dummy __attribute__ ((__unused__));
} // namespace

void EventLoopThread::stop_running()
{
  DoutEntering(dc::evio, "EventLoopThread::stop_running()");
  m_stop_running = true;
}

void EventLoopThread::add_needs_deletion(FileDescriptorBase const* node)
{
  // Even though node is const -- this is like an initialization of m_next and therefore we're allowed to change m_next.
  node->m_next = m_needs_deletion_list.load(std::memory_order_relaxed);
  while (!m_needs_deletion_list.compare_exchange_weak(node->m_next, node, std::memory_order_release, std::memory_order_relaxed))
    ;
}

void EventLoopThread::flush_need_deletion()
{
  FileDescriptorBase const* head = m_needs_deletion_list.exchange(nullptr, std::memory_order_acquire);
  while (head)
  {
    FileDescriptorBase const* orphan = head;
    head = orphan->m_next;
    DEBUG_ONLY(orphan->mark_deleted());
    delete orphan;
  }
}

} // namespace evio
