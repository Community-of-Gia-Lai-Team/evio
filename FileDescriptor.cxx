// evio -- Event Driven I/O support.
//
//! @file
//! @brief Definition of namespace evio; class FileDescriptor.
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
#include "debug.h"
#include "FileDescriptor.h"
#ifdef CW_CONFIG_NONBLOCK_SYSV
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#else
#include <unistd.h>     // Needed for fcntl.
#include <fcntl.h>
#endif

namespace evio {

void set_nonblocking(int fd)
{
  if (fd <= 2)
  {
    // You don't want to do this; it will only result in std::out / std::cerr to go 'bad' and not printing anything anymore.
    // See https://stackoverflow.com/questions/32508801/fprintf-stdcout-doesnt-print-part-of-the-string-to-stdout-stderr
    Dout(dc::warning, "Setting fd " << fd << " to non-blocking will cause all standard streams to become non-blocking "
        "which in turn will cause erratic write failures to the standard output streams causing them to go bad and stop "
        "displaying output.");
  }
#ifdef CW_CONFIG_NONBLOCK_POSIX
  int nonb = O_NONBLOCK;
#elif defined(CW_CONFIG_NONBLOCK_BSD)
  int nonb = O_NDELAY;
#endif
#ifdef CW_CONFIG_NONBLOCK_SYSV
  // This portion of code might also apply to NeXT.
  // According to IBMs manual page, this might only work for sockets :/
  #warning "This is not really supported, as I've never been able to test it."
  int res = 1;
  if (ioctl(fd, FIONBIO, &res) < 0)
    perror("ioctl(fd, FIONBIO)");
#else
  int res;
  if ((res = fcntl(fd, F_GETFL)) == -1)
    perror("fcntl(fd, F_GETFL)");
  else
  {
    if (!(res & nonb) && fcntl(fd, F_SETFL, res | nonb) == -1)
      perror("fcntl(fd, F_SETL, nonb)");
#ifdef O_CLOEXEC
    if ((res = fcntl(fd, F_GETFD)) == -1)
      perror("fcntl(fd, F_GETFD)");
    else if (!(res & FD_CLOEXEC))
      Dout(dc::warning, "FD_CLOEXEC is not set on fd " << fd);
#endif
  }
#endif
  return;
}

bool is_valid(int fd)
{
#ifdef _WIN32
  return EV_FD_TO_WIN32_HANDLE (fd) != -1;
#elif defined(CW_CONFIG_NONBLOCK_SYSV)
#error "Not implemented."
#else
  return fcntl(fd, F_GETFL) != -1;
#endif
}

void FileDescriptor::init(int fd)
{
  DoutEntering(dc::io, "FileDescriptor::init(" << fd << ") [" << (void*)this << ']');
  // Only call init() with a valid, open filedescriptor.
  ASSERT(is_valid(fd));

  // Make file descriptor non-blocking by default.
  set_nonblocking(fd);

  // Reset all flags except FDS_RW.
  flags_t::wat flags_w(m_flags);
  flags_w->reset();
  m_fd = fd;
  init_input_device(flags_w);
  init_output_device(flags_w);
}

} // namespace evio
