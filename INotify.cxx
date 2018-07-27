// evio -- Event Driven I/O support.
//
//! @file
//! @brief Definition of class INotifyDevice
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
#include "INotify.h"
#include "utils/macros.h"
#include "libcwd/buf2str.h"
#include <algorithm>
#include <sys/inotify.h>

namespace evio {

namespace {
SingletonInstance<INotifyDevice> dummy __attribute__ ((__unused__));
} // namespace

int INotifyDevice::add_watch(char const* pathname, uint32_t mask, INotify* obj)
{
  if (AI_UNLIKELY(!is_open_r()))
  {
    // Set up the inotify device.
    int fd = inotify_init1(IN_NONBLOCK);
    ASSERT(fd != -1);
    init(fd);
    start_input_device();
  }
  int fd = get_input_fd();
  int wd = inotify_add_watch(fd, pathname, mask);
  if (AI_UNLIKELY(wd == -1))
  {
    // FIXME, throw an error.
    ASSERT(wd >= 0);
    return -1;
  }
  wd_to_inotify_map_ts::wat(m_wd_to_inotify_map)->push_back(std::make_pair(wd, obj));
  return wd;
}

//static
INotifyDevice::wd_to_inotify_map_type::const_iterator INotifyDevice::get_inotify_obj(wd_to_inotify_map_ts::crat const& wd_to_inotify_map_r, int wd)
{
  // This kinda sucks - we have to search the vector.
  // However, the vector is very likely to be very small;
  // so this shouldn't be a problem (which is why I opted
  // for a vector in the first place as opposed to -say-
  // a std::map).
  struct FindWatchDescriptor
  {
    int m_wd;
    FindWatchDescriptor(int wd) : m_wd(wd) { }
    bool operator()(std::pair<int, INotify*> const& d) const { return d.first == m_wd; }
  };

  FindWatchDescriptor find_watch_descriptor(wd);
  auto result = std::find_if(wd_to_inotify_map_r->begin(), wd_to_inotify_map_r->end(), find_watch_descriptor);

  if (AI_UNLIKELY(result == wd_to_inotify_map_r->end()))
  {
    // FIXME, throw an error.
    ASSERT(result != wd_to_inotify_map_r->end());
  }

  return result;
}

void INotifyDevice::rm_watch(int wd)
{
  int fd = get_input_fd();
  int result = inotify_rm_watch(fd, wd);
  {
    wd_to_inotify_map_ts::rat wd_to_inotify_map_r(m_wd_to_inotify_map);
    auto iter = get_inotify_obj(wd_to_inotify_map_r, wd);
    wd_to_inotify_map_ts::wat(wd_to_inotify_map_r)->erase(iter);
  }
  // FIXME, throw an error.
  ASSERT(result == 0);
}

size_t INotifyDevice::end_of_msg_finder(char const* new_data, size_t rlen)
{
  m_len_so_far += rlen;
  // Fast track first.
  if (AI_LIKELY(m_len_so_far >= sizeof(int) + 12) && m_name_len == -1)
    m_name_len = *reinterpret_cast<uint32_t const*>(new_data + sizeof(int) + 8);
  else
  {
    // Now the slower cases.
    size_t old_len = m_len_so_far - rlen;
    if (old_len < sizeof(int) + 12)                       // Did not have name_len complete before already?
    {
      if (m_len_so_far <= sizeof(int) + 8)                // Still not any name_len bytes now?
        return 0;
      // Calculate the number of bytes of name_len that we already had.
      int n_old = old_len - (sizeof(int) + 8);
      if (n_old < 0)
        n_old = 0;
      // Calculate the number of bytes of name_len that we have now.
      int n_new = m_len_so_far - (sizeof(int) + 8);
      if (n_new > 4)
        n_new = 4;
      // Read the additional number of bytes.
      for (int i = 0; i < n_new - n_old; ++i)
        m_buf[i + n_old] = new_data[i];
      if (n_new < 4)                                      // Still don't have name_len completely?
        return 0;
    }
  }
  size_t msg_len = sizeof(int) + 12 + m_name_len;
  if (AI_UNLIKELY(m_len_so_far < msg_len))
    return 0;
  m_len_so_far = 0;
  m_name_len = -1;
  return msg_len;
}

void INotifyDevice::decode(MsgBlock msg)
{
  inotify_event const* event = reinterpret_cast<inotify_event const*>(msg.get_start());
  ASSERT(sizeof(int) + 12 + event->len == msg.get_size());
  Dout(dc::notice, "Received inotify event for wd " << event->wd << ": " << event->mask << " with cookie " << event->cookie << " and name \"" << buf2str(event->name, event->len) << "\".");
}

} // namespace evio
