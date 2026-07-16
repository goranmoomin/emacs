/* embemacs.h --- public embedding API for GNU Emacs.             -*- h -*- */

/* This header is the stable entry point for embedding Emacs.

   On macOS, the ns/AppKit port supports the recommended returning fiber
   API: a host-owned NSApplication supplies an NSView and calls
   embemacs_start().

   On Linux, the PGTK port supports the same returning main-thread fiber
   API.  A host creates its GTK UI, supplies a GtkContainer with
   embemacs_set_embed_parent_widget(), calls embemacs_start(), and then owns
   the top-level GMainLoop.  An epoll-backed high-priority GLib source resumes
   Emacs before ordinary GTK sources dispatch, keeping PGTK callbacks on the
   Emacs fiber stack.

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

/* True when Emacs is embedded in a host-owned GUI application.  Backend
   policy code uses this to avoid taking over host application/window state. */
extern bool embemacs_host_owns_app;

/* Set the NSView* that hosts the initial frame before calling
   emacs_main() or embemacs_start().  The pointer is borrowed: the host keeps
   the view and its containing hierarchy alive through embedded Emacs exit.
   The ns port creates the initial EmacsView directly as a subview of this
   parent view and does not create an EmacsWindow for that frame.  The parent
   placement slot is consumed on first use; ownership is not transferred.
   Additional frames appear as regular Emacs-owned windows.  */
#ifdef __OBJC__
extern void embemacs_set_embed_parent_view (NSView *parent_view);
#else
extern void embemacs_set_embed_parent_view (void *parent_view);
#endif

/* Set the same-process GtkContainer that hosts the first PGTK frame.  The
   pointer is borrowed: the host keeps the container and its GTK hierarchy
   alive through embedded Emacs exit.  Call this after constructing that
   hierarchy and before embemacs_start().  PGTK temporarily creates its
   normal frame, then reparents the complete frame content into this
   container.  This works on Wayland because it embeds GTK widgets, not
   foreign native windows.  The placement slot is consumed by the first
   non-tooltip frame; ownership is not transferred.  Later frames are
   ordinary Emacs-owned PGTK windows.  After startup, host code must not
   synchronously manipulate the embedded Emacs subtree from outside the
   default-context dispatch path; schedule such UI work on that context.  */
#ifdef __GTK_H__
extern void embemacs_set_embed_parent_widget (GtkWidget *parent_widget);
#else
extern void embemacs_set_embed_parent_widget (void *parent_widget);
#endif

enum embemacs_start_result
{
  EMBEMACS_OK = 0,
  EMBEMACS_ERR_ALREADY_STARTED = -1,
  EMBEMACS_ERR_WRONG_THREAD = -2,
  EMBEMACS_ERR_NO_EMBED_PARENT = -3,
  /* Compatibility name from the original AppKit-only API.  */
  EMBEMACS_ERR_NO_PARENT_VIEW = EMBEMACS_ERR_NO_EMBED_PARENT,
  EMBEMACS_ERR_STACK_ALLOC = -4,
  EMBEMACS_ERR_UNSUPPORTED = -5,
  EMBEMACS_ERR_INVALID_ARGUMENT = -6,
  EMBEMACS_ERR_NO_MEMORY = -7,
  EMBEMACS_ERR_NOT_RUNNING = -8,
  EMBEMACS_ERR_BACKEND = -9
};

/* Fiber-mode entry: start Emacs on its own stack on the GUI main thread, so
   the host keeps ownership of its top-level AppKit or GLib loop.

   Preconditions: main thread; the host GUI is initialized;
   embemacs_set_embed_parent_view or embemacs_set_embed_parent_widget has
   been called for the selected backend.

   Performs Emacs startup (argument parsing, pdump load, embedded frame
   creation) on the fiber and returns at Emacs's first idle wait.  Thereafter
   the host loop drives Emacs through the backend wake gate: AppKit
   application-defined events on NS, or the epoll-backed high-priority
   GSource on PGTK.  On x86_64 Linux, startup returns
   EMBEMACS_ERR_UNSUPPORTED if Intel CET shadow stacks are already active.

   argv is copied; the caller need not keep it alive.  EMBEMACS_OK means
   startup reached its first idle wait.  The one-shot ready callback fires
   once startup Lisp has completed and can therefore run before
   embemacs_start returns.  A negative embemacs_start_result code means Emacs
   is not running.  Without an exit callback, kill-emacs exits the process.
   With one installed, it notifies the host and permanently terminates only
   the fiber.  Emacs can also exit during startup, in which case this function
   returns EMBEMACS_ERR_NOT_RUNNING if the process remains alive.  */
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

/* Host notification callbacks.  All of them run synchronously on the main
   thread, on the Emacs fiber stack; they must return promptly, must not
   block, and must not call Lisp.  They may queue embemacs_eval_async*
   requests, which run at a later timer check rather than recursively.  The
   setters are main-thread-only and a NULL callback clears the notification.
   Callback/context values are borrowed and must remain valid until cleared
   or Emacs exits.  Register the ready callback before embemacs_start; ready
   notification is one-shot and registration after it fires is not
   retroactive.  The setters return EMBEMACS_OK or a negative
   embemacs_start_result code.  */

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
   defunct.  A typical host records EXIT_CODE and schedules app termination
   or view teardown after the callback returns (for example with
   dispatch_async or g_idle_add).  Without this callback, kill-emacs calls
   exit() as before.  Restarting Emacs in the same process is not supported. */
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
