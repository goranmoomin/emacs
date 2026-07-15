/* embfiber.h --- fiber (stackful coroutine) support for embedded Emacs.

Internal-only header for libemacs plumbing.  It is not installed, and
this interface may change without API compatibility guarantees.

Emacs runs on its own stack ("the fiber") on the main thread.  When the
ns port would otherwise park inside a nested [NSApp run] (ns_select /
ns_read_socket), it instead switches back to the host stack; the host's
own [NSApp run] later resumes the fiber when an appdefined event
arrives.  See API-DESIGN.md in the embemacs superproject.  */

#ifndef EMBFIBER_H
#define EMBFIBER_H

#include <stdbool.h>
#include <stddef.h>

/* emacs.c sizes emacs_re_safe_alloca from RLIMIT_STACK and may grow that
   limit before Lisp runs.  In fiber mode that logic still describes the
   stack Emacs expects to have, but execution is on this stack instead of
   the process main stack, so keep the fiber at least as large as the
   ordinary macOS main-thread stack limit it assumes.  */
#define EMBFIBER_DEFAULT_STACK_SIZE (64u * 1024u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

/* True when embedded fiber mode is enabled (set by embfiber_launch
   before the fiber first runs; never cleared).  Volatile because the
   fatal-signal path in sysdep.c reads these from a signal handler.  */
extern volatile bool embfiber_active;

/* True while executing on the Emacs fiber stack.  Volatile for the same
   signal-handler read path as embfiber_active.  */
extern volatile bool embfiber_on_fiber;

/* Derived hot-path flag: true when Lisp/allocation must not run on the
   current stack.  */
extern bool embfiber_forbid_lisp;

/* Keep enabled for embedded runs: this is two bool loads and a branch
   when fiber mode is active, and turns host-stack Lisp work into a
   precise abort.  */
#ifndef EMBFIBER_CHECK_STACK
#define EMBFIBER_CHECK_STACK 1
#endif

extern void embfiber_abort_on_host_stack (const char *operation,
                                          const char *entry);

static inline void
embfiber_check_stack (const char *operation, const char *entry)
{
#if EMBFIBER_CHECK_STACK
  if (embfiber_forbid_lisp)
    embfiber_abort_on_host_stack (operation, entry);
#else
  (void) operation;
  (void) entry;
#endif
}

/* Create a fiber with a stack of STACK_SIZE bytes and immediately switch
   to it, calling ENTRY (ARG) there.  Returns (on the host stack) when the
   fiber first yields, or when ENTRY returns.  Main thread only; must be
   called at most once.  */
extern bool embfiber_launch (void (*entry) (void *), void *arg,
                             size_t stack_size);

/* Switch from the fiber back to the host stack.  Fiber only.  */
extern void embfiber_yield (void);

/* Park the fiber at an AppKit wait point.  Service calls requested from
   the host are run on the fiber stack and then the fiber parks again;
   only a non-service resume returns to the caller.  Fiber only.  */
extern void embfiber_park (void);

/* Resume the fiber; returns when it next yields or its entry returns.
   Host stack (main thread) only.  Returns false (and does nothing) if
   the fiber is not launched, already running, or has finished.  */
extern bool embfiber_resume (void);

/* Run FN (ARG) on the Emacs fiber stack and return its C result.  If the
   caller is already on the fiber, FN is called directly.  In fiber mode,
   host-stack callbacks must use this before doing Lisp evaluation or Lisp
   allocation; no Lisp_Object result may be returned through this API unless
   it has first been converted to non-Lisp C data on the fiber.

   Service calls are only coherent while the fiber is parked and the service
   body does not itself block.  If a service body parks the fiber, this
   function returns NULL to the host and the service continues later on the
   fiber; callers that need a value must treat NULL/zero as unavailable.  */
extern void *embfiber_call_on (void *(*fn) (void *), void *arg);

/* Terminate the fiber: mark it finished and switch back to the host
   stack permanently.  Later resumes do nothing and return false.  The
   fiber stack is not unwound or reclaimed.  Fiber only; never returns.  */
extern void embfiber_exit (void) __attribute__ ((noreturn));

/* True once a fiber has been launched.  */
extern bool embfiber_launched_p (void);

/* True once the fiber's entry function has returned.  */
extern bool embfiber_finished_p (void);

#ifdef __cplusplus
}
#endif

#endif /* EMBFIBER_H */
