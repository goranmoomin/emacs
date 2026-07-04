/* nsembed.m --- embedded host-view support for the Emacs NS port.

Copyright (C) 2026 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

#include <config.h>

#ifdef HAVE_NS

#include "embemacs.h"
#include "embfiber.h"

#include "lisp.h"
#include "nsterm.h"
#include "nsembed.h"

void *embemacs_embed_parent_view = NULL;
static id embfiber_event_monitor;

bool
embemacs_frame_embedded_p (struct frame *f)
{
  (void) f;
  return false;
}

void
embfiber_install_event_monitor (void)
{
#ifdef NS_IMPL_COCOA
  if (embfiber_event_monitor != nil)
    return;

  embfiber_event_monitor = [NSEvent
    addLocalMonitorForEventsMatchingMask:NSEventMaskApplicationDefined
                                 handler:^NSEvent *(NSEvent *event) {
      if (!embfiber_active
          || [event type] != NSEventTypeApplicationDefined
          || [event subtype] != NSAPP_SUBTYPE_EMACS)
        return event;

      if (embfiber_on_fiber)
        {
          switch ([event data2])
            {
            case NSAPP_DATA2_RUNASSCRIPT:
              ns_run_ascript ();
              [NSApp stop:nil];
              return nil;
            case NSAPP_DATA2_RUNFILEDIALOG:
              ns_run_file_dialog ();
              [NSApp stop:nil];
              return nil;
            }
          ns_embfiber_finish_appdefined_event (event, true);
        }
      else
        {
          switch ([event data2])
            {
            case NSAPP_DATA2_RUNASSCRIPT:
              fputs ("embemacs: RUNASSCRIPT on host stack\n", stderr);
              emacs_abort ();
            case NSAPP_DATA2_RUNFILEDIALOG:
              fputs ("embemacs: RUNFILEDIALOG on host stack\n", stderr);
              emacs_abort ();
            }
          ns_embfiber_finish_appdefined_event (event, false);
        }
      return nil;
    }];
#endif
}

#endif /* HAVE_NS */
