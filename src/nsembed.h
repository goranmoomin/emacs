/* nsembed.h --- internal declarations for embedded NS views.  -*- objc -*-

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

#ifndef NSEMBED_H
#define NSEMBED_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "embfiber.h"

#ifdef __OBJC__
@class NSView;
@class NSWindow;
@class NSEvent;
@class EmacsView;
#endif

struct frame;

#ifdef __OBJC__

extern void ns_embfiber_finish_appdefined_event (NSEvent *, bool);

@interface EmacsView (EmbemacsEmbed)
- (BOOL)embemacsIsEmbeddedInHostWindow;
- (void)embemacsResizeFrameToSuperviewBounds;
- (void)embemacsViewDidMoveToWindow;
- (void)embemacsViewDidMoveToSuperview;
- (void)embemacsRemoveHostWindowObservers;
- (void)embemacsInstallHostWindowObservers: (NSWindow *)window;
- (void)embemacsUpdateLayerForHostWindow: (NSWindow *)window;
- (BOOL)embemacsEmbedIntoParentView: (NSView *)parent frame: (struct frame *)f;
@end

static inline void *
embfiber_objc_block_runner (void *ptr)
{
  void (^block) (void) = ptr;
  block ();
  return NULL;
}

static inline void
embfiber_run_objc_block_on_fiber (void (^block) (void))
{
  embfiber_call_on (embfiber_objc_block_runner, block);
}

static inline bool
embfiber_copy_call_on (void *(*fn) (void *), void *data, size_t size)
{
  void *copy;

  if (!embfiber_active || embfiber_on_fiber)
    return embfiber_call_on (fn, data) == data;

  copy = malloc (size);
  if (copy == NULL)
    return false;
  memcpy (copy, data, size);
  if (embfiber_call_on (fn, copy) != copy)
    return false; /* The fiber may still own COPY.  */
  memcpy (data, copy, size);
  free (copy);
  return true;
}

/* Hides a return from the surrounding method.  */
#define EMBFIBER_TRAMPOLINE(...) \
  do \
    { \
      if (embfiber_active && !embfiber_on_fiber) \
        { \
          embfiber_run_objc_block_on_fiber (^{ __VA_ARGS__; }); \
          return; \
        } \
    } \
  while (0)

/* Hides a return from the surrounding method.  */
#define EMBFIBER_TRAMPOLINE_RETURN(type, fallback, expr) \
  do \
    { \
      if (embfiber_active && !embfiber_on_fiber) \
        { \
          __block type embfiber_trampoline_result = (fallback); \
          embfiber_run_objc_block_on_fiber (^{ \
            embfiber_trampoline_result = (expr); \
          }); \
          return embfiber_trampoline_result; \
        } \
    } \
  while (0)

#endif

extern void *embemacs_embed_parent_view;
extern bool embemacs_frame_embedded_p (struct frame *f);

/* Install the AppKit local monitor that resumes the fiber from
   application-defined events.  Main thread only.  */
extern void embfiber_install_event_monitor (void);

#endif /* NSEMBED_H */
