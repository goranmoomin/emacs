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
  EMBEMACS_ERR_NO_MEMORY = -7,
  EMBEMACS_ERR_NOT_RUNNING = -8
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

/* Result callback for embemacs_eval_async_with_result.  Called on the
   main thread, on the Emacs fiber stack, once the queued expression has
   been evaluated.  SUCCESS is false when evaluation signaled an error or
   threw; RESULT is the value printed with prin1 (or the error
   description), UTF-8 encoded, valid only for the duration of the call.
   The callback must return promptly, must not block, and must not call
   Lisp; it may queue further embemacs_eval_async* requests.  */
typedef void (*embemacs_eval_callback) (bool success, const char *result,
                                        void *context);

/* Queue LISP, one Lisp expression in UTF-8 string form (wrap several
   forms in progn), for evaluation on the Emacs fiber.  The expression
   runs at the command loop's next timer check, with timer-function
   semantics: promptly when Emacs is idle, and after the current command
   or blocking call otherwise.  Evaluation errors are caught and logged
   to stderr.  Main thread only; requires a started, still-running
   embedded Emacs.  Returns EMBEMACS_OK when queued, else a negative
   embemacs_start_result code (EMBEMACS_ERR_WRONG_THREAD,
   EMBEMACS_ERR_NOT_RUNNING, EMBEMACS_ERR_INVALID_ARGUMENT,
   EMBEMACS_ERR_NO_MEMORY, EMBEMACS_ERR_UNSUPPORTED).  */
extern int embemacs_eval_async (const char *lisp);

/* As embemacs_eval_async, but deliver the printed result or error
   description to CALLBACK with CONTEXT.  */
extern int embemacs_eval_async_with_result (const char *lisp,
                                            embemacs_eval_callback callback,
                                            void *context);

/* Host notification callbacks.  All of them run on the main thread, on
   the Emacs fiber stack; they must return promptly, must not block, and
   must not call Lisp.  They may queue embemacs_eval_async* requests.
   The setters are main thread only, may be called before or after
   embemacs_start, and a NULL callback clears the notification.  They
   return EMBEMACS_OK or a negative embemacs_start_result code.  */

/* Called once, at the first command-loop timer check after Emacs
   startup has completed (`after-init-time' is set): init files and
   startup argv have been processed and Emacs is ready for
   embemacs_eval_async requests and input.  */
typedef void (*embemacs_ready_callback) (void *context);
extern int embemacs_set_ready_callback (embemacs_ready_callback callback,
                                        void *context);

/* Called when the embedded frame's title or name changes.  Emacs never
   retitles the host window itself (the host owns it); this reports what
   the title would have been.  TITLE is UTF-8 and valid only for the
   duration of the call.  */
typedef void (*embemacs_title_callback) (const char *title, void *context);
extern int embemacs_set_title_callback (embemacs_title_callback callback,
                                        void *context);

/* Called when Emacs exits (kill-emacs), as its final act on the fiber,
   after kill-emacs-hook, auto-save, and subprocess shutdown have run.
   When this callback is set, kill-emacs does NOT exit the process:
   after the callback returns, the fiber is terminated permanently and
   control returns to the host run loop; further embemacs_eval_async
   calls fail with EMBEMACS_ERR_NOT_RUNNING, and the embedded view is
   defunct.  A typical host records EXIT_CODE and defers app termination
   or view teardown with dispatch_async.  Without this callback,
   kill-emacs calls exit() as before.  Restarting Emacs in the same
   process is not supported.  */
typedef void (*embemacs_exit_callback) (int exit_code, void *context);
extern int embemacs_set_exit_callback (embemacs_exit_callback callback,
                                       void *context);

/* Internal: run queued eval requests (and fire the ready notification)
   on the fiber.  Called from the command loop's timer check; not part
   of the host API.  */
extern void embemacs_run_pending_evals (void);

/* Internal: notify the host of an embedded-frame title change.  Fiber
   only; TITLE is UTF-8.  */
extern void embemacs_notify_title (const char *title);

/* Internal: in fiber mode with an exit callback set, notify the host
   and terminate the fiber instead of exiting the process; otherwise
   return so the caller can exit().  Called by kill-emacs.  */
extern void embemacs_handle_exit (int exit_code);

#ifdef __cplusplus
}
#endif

#endif /* EMBEMACS_H */
