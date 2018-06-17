#pragma once
#include "libev-4.24/ev.h"

namespace evio {

enum events_type
{
  UNDEF    = EV_UNDEF,  // Guaranteed to be invalid.
  NONE     = EV_NONE,   // No events.
  READ     = EV_READ,                   // For registering a fd that is readble, or as revents when libev detected that a read will not block.
  WRITE    = EV_WRITE,                  // For registering a fd that is writable, or as revents when libev detected that a write will not block.
  READ_WRITE = EV_READ | EV_WRITE,      // For registering a fd that is both readable and writable.
  CUSTOM   = EV_CUSTOM, // For use by user code.
  ERROR    = EV_ERROR   // Sent when an error occurs.
};

} // namespace evio
