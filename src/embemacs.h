/* embemacs.h --- public embedding API for GNU Emacs (ns port)      -*- h -*- */

/* This header is the stable entry point for embedding the Emacs ns
   (AppKit) port inside a host macOS application as an NSView.

   FIBER MODE is the recommended embedding path.  The host uses its
   normal NSApplication, owns the top-level [NSApp run] loop, sets an
   NSView parent with embemacs_set_embed_parent_view (), and calls
   embemacs_start () on the main thread after launch.  Emacs starts on
   its own stack and returns to the host at the first idle yield; later
   AppKit events resume Emacs through the ns port's appdefined-event
   monitor.

   emacs_main () is the legacy/blocking entry for standalone drivers,
   batch work, and dumping.  It performs the ordinary Emacs startup
   sequence (argument parsing, pdump image loading, terminal
   initialization, Lisp loadup) and enters the recursive editing command
   loop.  It returns only when Emacs exits.

   Resource discovery: Emacs locates its Lisp, etc/ and pdump files via
   the usual mechanisms (argv[0], EMACSDATA, EMACSLOADPATH, the pdump
   path recorded at dump time).  For an embedded build the host must
   arrange for those to resolve to a bundled Emacs resource directory;
   see ns_init_paths () in nsterm.m and the PDMP loading code in
   emacs.c (load_pdump).  The higher-level embedding guide lives in the
   embemacs superproject.  */

#ifndef EMBEMACS_H
#define EMBEMACS_H

#include <stdbool.h>

#ifdef __OBJC__
@class NSView;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Run Emacs.  argc/argv are as for the standalone `emacs' program
   (argv[0] is used for executable/pdump discovery).  Performs startup
   and then blocks in the editing command loop.  Returns the Emacs exit
   code when Emacs terminates.  Must be called on the main thread.  */
extern int emacs_main (int argc, char **argv);

/* True when Emacs is embedded in a host-owned NSApplication.  Policy code
   uses this to avoid taking over the host app delegate, menu, activation,
   and launch handshake.  */
extern bool embemacs_host_owns_app;

/* Set the NSView* that hosts the initial frame before calling
   emacs_main() or embemacs_start().  The ns port creates the initial
   EmacsView directly as a subview of this parent view and does not create
   an EmacsWindow for that frame.  The parent view is consumed on first
   use: additional frames appear as regular Emacs-owned windows.  */
#ifdef __OBJC__
extern void embemacs_set_embed_parent_view (NSView *parent_view);
#else
extern void embemacs_set_embed_parent_view (void *parent_view);
#endif

enum embemacs_start_result
{
  EMBEMACS_OK = 0,
  EMBEMACS_ERR_ALREADY_STARTED = -1,
  EMBEMACS_ERR_WRONG_THREAD = -2,
  EMBEMACS_ERR_NO_PARENT_VIEW = -3,
  EMBEMACS_ERR_STACK_ALLOC = -4,
  EMBEMACS_ERR_UNSUPPORTED = -5,
  EMBEMACS_ERR_INVALID_ARGUMENT = -6,
  EMBEMACS_ERR_NO_MEMORY = -7
};

/* Fiber-mode entry: start Emacs on its own stack (a fiber) on the main
   thread, so the host keeps ownership of the [NSApp run] loop.

   Preconditions: main thread; the host app has finished launching;
   embemacs_set_embed_parent_view has been called.

   Performs the whole Emacs startup (argument parsing, pdump load, frame
   creation inside the embed parent view) on the fiber and returns to the
   caller at Emacs's first idle yield (ns_select).  Thereafter the host's
   own [NSApp run] drives Emacs: appdefined events posted by the ns port
   resume the fiber from a local NSEvent monitor.

   argv is copied; the caller need not keep it alive.  Returns 0 once the
   fiber is launched, or a negative embemacs_start_result code for a logged
   precondition/platform/resource failure.  Emacs terminates via process
   exit.  */
extern int embemacs_start (int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* EMBEMACS_H */
