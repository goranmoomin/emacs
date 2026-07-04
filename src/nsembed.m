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
#include "blockinput.h"
#include "nsterm.h"
#include "nsembed.h"
#include "keyboard.h"
#include "frame.h"

void *embemacs_embed_parent_view = NULL;
static id embfiber_event_monitor;

bool
embemacs_frame_embedded_p (struct frame *f)
{
  EmacsView *view;

  if (f == NULL || f->output_data.ns == NULL)
    return false;

  view = FRAME_NS_VIEW (f);
  return view != nil && [view embemacsIsEmbeddedInHostWindow];
}

@implementation EmacsView (EmbemacsEmbed)

- (BOOL)embemacsIsEmbeddedInHostWindow
{
  return embemacsEmbeddedInHostWindow;
}

- (void)embemacsUpdateLayerForHostWindow: (NSWindow *)window
{
#if defined (NS_IMPL_COCOA) && MAC_OS_X_VERSION_MIN_REQUIRED >= 101400
  if (window == nil || ![[self layer] isKindOfClass:[EmacsLayer class]])
    return;

  [(EmacsLayer *)[self layer] setColorSpace:[[window colorSpace] CGColorSpace]];
  [(EmacsLayer *)[self layer] setContentsScale:[window backingScaleFactor]];
#endif
}

- (void)embemacsResizeFrameToSuperviewBounds
{
  NSRect frame;
  int width, height;

  EMBFIBER_TRAMPOLINE ([self embemacsResizeFrameToSuperviewBounds]);

  if (! FRAME_LIVE_P (emacsframe) || [self superview] == nil)
    return;

  frame = [[self superview] bounds];
  width = (int)NSWidth (frame);
  height = (int)NSHeight (frame);

  NSTRACE_SIZE ("New size", NSMakeSize (width, height));

  embemacsHandlingFrameChange = YES;
  [self setFrame:frame];
  change_frame_size (emacsframe, width, height, false, YES, false);
  embemacsHandlingFrameChange = NO;

  SET_FRAME_GARBAGED (emacsframe);
  cancel_mouse_face (emacsframe);
  ns_send_appdefined (-1);
}

- (void)embemacsViewFrameDidChange: (NSNotification *)notification
{
  (void)notification;
  if (!embemacsEmbeddedInHostWindow || embemacsHandlingFrameChange)
    return;
  [self embemacsResizeFrameToSuperviewBounds];
}

- (void)embemacsSuperviewFrameDidChange: (NSNotification *)notification
{
  (void)notification;
  if (!embemacsEmbeddedInHostWindow || embemacsHandlingFrameChange)
    return;
  [self embemacsResizeFrameToSuperviewBounds];
}

- (void)embemacsHostWindowDidBecomeKey: (NSNotification *)notification
{
  [self windowDidBecomeKey:notification];
}

- (void)embemacsHostWindowDidResignKey: (NSNotification *)notification
{
  [self windowDidResignKey:notification];
}

- (void)embemacsHostWindowDidMove: (NSNotification *)notification
{
  [self windowDidMove:notification];
}

- (void)embemacsHostWindowDidMiniaturize: (NSNotification *)notification
{
  [self windowDidMiniaturize:notification];
}

- (void)embemacsHostWindowDidDeminiaturize: (NSNotification *)notification
{
  [self windowDidDeminiaturize:notification];
}

- (void)embemacsHostWindowDidChangeOcclusionState: (NSNotification *)notification
{
  NSWindow *window = [notification object];
  BOOL visible;

  if (!emacsframe->output_data.ns || window == nil)
    return;

  visible = ![window isMiniaturized] && [window isVisible]
    && (([window occlusionState] & NSWindowOcclusionStateVisible) != 0);
  SET_FRAME_VISIBLE (emacsframe, visible);
  if (visible)
    {
      SET_FRAME_GARBAGED (emacsframe);
      ns_send_appdefined (-1);
    }
}

- (void)embemacsRemoveHostWindowObservers
{
  NSNotificationCenter *center = [NSNotificationCenter defaultCenter];

  if (embemacsObservedHostWindow != nil)
    {
      [center removeObserver:self name:NSWindowDidBecomeKeyNotification
                      object:embemacsObservedHostWindow];
      [center removeObserver:self name:NSWindowDidResignKeyNotification
                      object:embemacsObservedHostWindow];
      [center removeObserver:self name:NSWindowDidMoveNotification
                      object:embemacsObservedHostWindow];
      [center removeObserver:self name:NSWindowDidMiniaturizeNotification
                      object:embemacsObservedHostWindow];
      [center removeObserver:self name:NSWindowDidDeminiaturizeNotification
                      object:embemacsObservedHostWindow];
      [center removeObserver:self
                        name:NSWindowDidChangeOcclusionStateNotification
                      object:embemacsObservedHostWindow];
    }

  [center removeObserver:self name:NSViewFrameDidChangeNotification
                  object:self];
  if (embemacsObservedSuperview != nil)
    [center removeObserver:self name:NSViewFrameDidChangeNotification
                    object:embemacsObservedSuperview];

  embemacsObservedHostWindow = nil;
  embemacsObservedSuperview = nil;
}

- (void)embemacsInstallHostWindowObservers: (NSWindow *)window
{
  NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
  NSView *superview = [self superview];

  [self embemacsRemoveHostWindowObservers];
  embemacsEmbeddedInHostWindow = YES;
  [self embemacsUpdateLayerForHostWindow:window];

  if (window == nil || superview == nil)
    return;

  embemacsObservedHostWindow = window;
  embemacsObservedSuperview = superview;

  [self setPostsFrameChangedNotifications:YES];
  [superview setPostsFrameChangedNotifications:YES];
  [center addObserver:self selector:@selector (embemacsHostWindowDidBecomeKey:)
                 name:NSWindowDidBecomeKeyNotification object:window];
  [center addObserver:self selector:@selector (embemacsHostWindowDidResignKey:)
                 name:NSWindowDidResignKeyNotification object:window];
  [center addObserver:self selector:@selector (embemacsHostWindowDidMove:)
                 name:NSWindowDidMoveNotification object:window];
  [center addObserver:self selector:@selector (embemacsHostWindowDidMiniaturize:)
                 name:NSWindowDidMiniaturizeNotification object:window];
  [center addObserver:self selector:@selector (embemacsHostWindowDidDeminiaturize:)
                 name:NSWindowDidDeminiaturizeNotification object:window];
  [center addObserver:self
             selector:@selector (embemacsHostWindowDidChangeOcclusionState:)
                 name:NSWindowDidChangeOcclusionStateNotification object:window];
  [center addObserver:self selector:@selector (embemacsViewFrameDidChange:)
                 name:NSViewFrameDidChangeNotification object:self];
  [center addObserver:self selector:@selector (embemacsSuperviewFrameDidChange:)
                 name:NSViewFrameDidChangeNotification object:superview];
  [self embemacsResizeFrameToSuperviewBounds];
  [self embemacsHostWindowDidChangeOcclusionState:
          [NSNotification notificationWithName:
              NSWindowDidChangeOcclusionStateNotification object:window]];
}

- (void)embemacsViewDidMoveToWindow
{
  if (embemacsEmbeddedInHostWindow)
    [self embemacsInstallHostWindowObservers:[self window]];
}

- (void)embemacsViewDidMoveToSuperview
{
  if (embemacsEmbeddedInHostWindow)
    [self embemacsInstallHostWindowObservers:[self window]];
}

- (BOOL)embemacsEmbedIntoParentView: (NSView *)parent frame: (struct frame *)f
{
  NSWindow *host;

  if (parent == nil)
    return NO;

  /* Consumed on attempt: a missing host window falls back to degraded
     embedding rather than letting a later frame reuse the initial parent.  */
  embemacs_embed_parent_view = NULL;
  [self setFrame:[parent bounds]];
  [parent addSubview:self];
  host = [parent window];
  if (host == nil)
    fputs ("embemacs: embed parent view has no window\n", stderr);
  [self embemacsInstallHostWindowObservers:host];

  (void) f;
  return YES;
}

@end

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
