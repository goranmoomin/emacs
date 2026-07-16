/* embemacs.c --- public embedding API implementation for GNU Emacs.

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

#include "embemacs.h"
#include "embfiber.h"
#include "nsembed.h"
#include "pgtkembed.h"

#include "lisp.h"
#include "coding.h"

bool embemacs_host_owns_app;

void
embemacs_set_embed_parent_view (void *parent_view)
{
#ifdef HAVE_NS
  embemacs_embed_parent_view = parent_view;
#else
  (void) parent_view;
#endif
}

void
embemacs_set_embed_parent_widget (void *parent_widget)
{
  pgtkembed_set_parent_widget (parent_widget);
#ifdef HAVE_PGTK
  embemacs_host_owns_app = parent_widget != NULL;
#endif
}

#include <stdio.h>

#if (defined HAVE_NS && defined NS_IMPL_COCOA) \
  || (defined HAVE_PGTK && defined __linux__ \
      && (defined __aarch64__ || defined __x86_64__))

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static int
embemacs_start_fail (enum embemacs_start_result code, const char *message)
{
  fprintf (stderr, "embemacs: %s\n", message);
  return code;
}

static bool
embemacs_ui_thread_p (void)
{
#if defined HAVE_NS && defined NS_IMPL_COCOA
  return pthread_main_np ();
#else
  return pgtkembed_ui_thread_p ();
#endif
}

static bool
embemacs_parent_set_p (void)
{
#if defined HAVE_NS && defined NS_IMPL_COCOA
  return embemacs_embed_parent_view != NULL;
#else
  return pgtkembed_parent_widget_set_p ();
#endif
}

static bool
embemacs_install_backend_driver (void)
{
#if defined HAVE_NS && defined NS_IMPL_COCOA
  embfiber_install_event_monitor ();
  return true;
#else
  return pgtk_embfiber_install_driver ();
#endif
}

static void
embemacs_wake_fiber (void)
{
#if defined HAVE_NS && defined NS_IMPL_COCOA
  /* Declared in nsterm.h, which is not includable from C here.  */
  extern void ns_send_appdefined (int value);
  ns_send_appdefined (-1);
#else
  pgtk_embfiber_wake ();
#endif
}

struct embemacs_start_args
{
  int argc;
  char **argv;
};

static void
embemacs_free_start_args (struct embemacs_start_args *args)
{
  if (args != NULL)
    {
      for (int i = 0; i < args->argc; ++i)
        free (args->argv[i]);
      free (args->argv);
      free (args);
    }
}

/* --- Host notification callbacks -----------------------------------------

   Registered on the main thread, invoked on the fiber stack (still the
   main thread) at Lisp-safe points, so no locking is needed.  */

static embemacs_ready_callback embemacs_ready_cb;
static void *embemacs_ready_ctx;
static bool embemacs_ready_fired;
static embemacs_title_callback embemacs_title_cb;
static void *embemacs_title_ctx;
static embemacs_exit_callback embemacs_exit_cb;
static void *embemacs_exit_ctx;

int
embemacs_set_ready_callback (embemacs_ready_callback callback, void *context)
{
  if (!embemacs_ui_thread_p ())
    return embemacs_start_fail (EMBEMACS_ERR_WRONG_THREAD,
                                "embemacs_set_ready_callback must be called on the main thread");
  embemacs_ready_cb = callback;
  embemacs_ready_ctx = context;
  return EMBEMACS_OK;
}

int
embemacs_set_title_callback (embemacs_title_callback callback, void *context)
{
  if (!embemacs_ui_thread_p ())
    return embemacs_start_fail (EMBEMACS_ERR_WRONG_THREAD,
                                "embemacs_set_title_callback must be called on the main thread");
  embemacs_title_cb = callback;
  embemacs_title_ctx = context;
  return EMBEMACS_OK;
}

int
embemacs_set_exit_callback (embemacs_exit_callback callback, void *context)
{
  if (!embemacs_ui_thread_p ())
    return embemacs_start_fail (EMBEMACS_ERR_WRONG_THREAD,
                                "embemacs_set_exit_callback must be called on the main thread");
  embemacs_exit_cb = callback;
  embemacs_exit_ctx = context;
  return EMBEMACS_OK;
}

/* Fire the ready notification once startup is complete.  Fiber only
   (called from the command loop's timer check); `after-init-time' is
   the same signal startup.el exposes to Lisp.  */
static void
embemacs_maybe_notify_ready (void)
{
  Lisp_Object sym;

  if (embemacs_ready_cb == NULL || embemacs_ready_fired || !embfiber_active)
    return;

  sym = intern ("after-init-time");
  if (NILP (Fboundp (sym)) || NILP (Fsymbol_value (sym)))
    return;

  embemacs_ready_fired = true;
  embemacs_ready_cb (embemacs_ready_ctx);
}

void
embemacs_notify_title (const char *title)
{
  if (embemacs_title_cb != NULL && title != NULL)
    embemacs_title_cb (title, embemacs_title_ctx);
}

void
embemacs_handle_exit (int exit_code)
{
  embemacs_exit_callback callback = embemacs_exit_cb;

  if (!embfiber_active || !embfiber_on_fiber || callback == NULL)
    return;

  callback (exit_code, embemacs_exit_ctx);
  embfiber_exit ();
}

/* --- Host-queued async evaluation ---------------------------------------

   The host enqueues UTF-8 expression strings on the main thread and
   wakes a parked fiber through the backend gate; the command loop's
   timer check drains the queue on the fiber (embemacs_run_pending_evals,
   called from timer_check_2 in keyboard.c next to pending_funcalls).
   Host and fiber strictly alternate on the main thread, so the queue
   needs no locking.  */

struct embemacs_eval_request
{
  char *lisp;
  embemacs_eval_callback callback;
  void *context;
  struct embemacs_eval_request *next;
};

static struct embemacs_eval_request *embemacs_eval_head;
static struct embemacs_eval_request *embemacs_eval_tail;

static int
embemacs_eval_enqueue (const char *lisp, embemacs_eval_callback callback,
                       void *context)
{
  struct embemacs_eval_request *req;

  if (!embemacs_ui_thread_p ())
    return embemacs_start_fail (EMBEMACS_ERR_WRONG_THREAD,
                                "embemacs_eval_async must be called on the main thread");
  if (!embfiber_launched_p () || embfiber_finished_p ())
    return embemacs_start_fail (EMBEMACS_ERR_NOT_RUNNING,
                                "embemacs_eval_async requires a running embedded Emacs");
  if (lisp == NULL)
    return embemacs_start_fail (EMBEMACS_ERR_INVALID_ARGUMENT,
                                "embemacs_eval_async called with a null expression");

  req = malloc (sizeof *req);
  if (req == NULL)
    return embemacs_start_fail (EMBEMACS_ERR_NO_MEMORY,
                                "embemacs_eval_async could not allocate a request");
  req->lisp = strdup (lisp);
  if (req->lisp == NULL)
    {
      free (req);
      return embemacs_start_fail (EMBEMACS_ERR_NO_MEMORY,
                                  "embemacs_eval_async could not copy the expression");
    }
  req->callback = callback;
  req->context = context;
  req->next = NULL;

  if (embemacs_eval_tail != NULL)
    embemacs_eval_tail->next = req;
  else
    embemacs_eval_head = req;
  embemacs_eval_tail = req;

  /* Wake a parked fiber so the command loop reaches its next timer
     check promptly.  */
  embemacs_wake_fiber ();
  return EMBEMACS_OK;
}

int
embemacs_eval_async (const char *lisp)
{
  return embemacs_eval_enqueue (lisp, NULL, NULL);
}

int
embemacs_eval_async_with_result (const char *lisp,
                                 embemacs_eval_callback callback,
                                 void *context)
{
  if (callback == NULL)
    return embemacs_start_fail (EMBEMACS_ERR_INVALID_ARGUMENT,
                                "embemacs_eval_async_with_result called with a null callback");
  return embemacs_eval_enqueue (lisp, callback, context);
}

/* Set by embemacs_eval_on_error while one request runs; single-threaded
   by the strict host/fiber alternation.  */
static bool embemacs_eval_failed;

/* Read and eval REQ->lisp; return its prin1 form when a callback wants
   the result.  Runs under internal_catch_all.  */
static Lisp_Object
embemacs_eval_body (void *ptr)
{
  struct embemacs_eval_request *req = ptr;
  Lisp_Object form
    = Fcar (Fread_from_string (build_string (req->lisp), Qnil, Qnil));
  Lisp_Object value = Feval (form, Qt);

  if (req->callback == NULL)
    return Qnil;
  return Fprin1_to_string (value, Qnil, Qnil);
}

static Lisp_Object
embemacs_eval_on_error (enum nonlocal_exit exit, Lisp_Object error)
{
  embemacs_eval_failed = true;
  if (exit == NONLOCAL_EXIT_SIGNAL)
    return Ferror_message_string (error);
  return Fprin1_to_string (Fcons (build_string ("throw"), error), Qnil, Qnil);
}

static void
embemacs_run_one_eval (struct embemacs_eval_request *req)
{
  Lisp_Object printed;

  embemacs_eval_failed = false;
  printed = internal_catch_all (embemacs_eval_body, req,
                                embemacs_eval_on_error);

  if (req->callback != NULL)
    {
      Lisp_Object encoded = (STRINGP (printed) ? ENCODE_UTF_8 (printed)
                             : build_string ("nil"));
      req->callback (!embemacs_eval_failed, SSDATA (encoded), req->context);
    }
  else if (embemacs_eval_failed)
    {
      Lisp_Object encoded = (STRINGP (printed) ? ENCODE_UTF_8 (printed)
                             : build_string ("unknown error"));
      fprintf (stderr, "embemacs: eval error: %s\n", SSDATA (encoded));
    }
}

void
embemacs_run_pending_evals (void)
{
  struct embemacs_eval_request *req;

  embemacs_maybe_notify_ready ();

  if (embemacs_eval_head == NULL)
    return;

  /* Snapshot the queue: requests enqueued by result callbacks run at
     the next timer check, after a fresh wakeup.  */
  req = embemacs_eval_head;
  embemacs_eval_head = NULL;
  embemacs_eval_tail = NULL;

  while (req != NULL)
    {
      struct embemacs_eval_request *next = req->next;
      embemacs_run_one_eval (req);
      free (req->lisp);
      free (req);
      req = next;
    }
}

static void
embemacs_start_entry (void *arg)
{
  struct embemacs_start_args *args = arg;

  (void) emacs_main (args->argc, args->argv);
}

int
embemacs_start (int argc, char **argv)
{
  struct embemacs_start_args *args;
  char **copy;
  int i;

  if (!embemacs_ui_thread_p ())
    return embemacs_start_fail (EMBEMACS_ERR_WRONG_THREAD,
                                "embemacs_start must be called on the main thread");
  if (!embfiber_platform_supported_p ())
    return embemacs_start_fail (EMBEMACS_ERR_UNSUPPORTED,
                                "embemacs_start cannot switch stacks while CET shadow stack is active");
  if (embfiber_launched_p ())
    return embemacs_start_fail (EMBEMACS_ERR_ALREADY_STARTED,
                                "embemacs_start called after Emacs already started");
  if (!embemacs_parent_set_p ())
    return embemacs_start_fail (EMBEMACS_ERR_NO_EMBED_PARENT,
                                "embemacs_start requires an embed parent widget/view");
  if (argc < 0)
    return embemacs_start_fail (EMBEMACS_ERR_INVALID_ARGUMENT,
                                "embemacs_start called with negative argc");
  if (argc > 0 && argv == NULL)
    return embemacs_start_fail (EMBEMACS_ERR_INVALID_ARGUMENT,
                                "embemacs_start called with null argv");

  args = malloc (sizeof *args);
  if (args == NULL)
    return embemacs_start_fail (EMBEMACS_ERR_NO_MEMORY,
                                "embemacs_start could not allocate arguments");

  copy = calloc ((size_t) argc + 1, sizeof *copy);
  if (copy == NULL)
    {
      free (args);
      return embemacs_start_fail (EMBEMACS_ERR_NO_MEMORY,
                                  "embemacs_start could not allocate argv copy");
    }

  for (i = 0; i < argc; ++i)
    {
      if (argv[i] == NULL)
        {
          while (i-- > 0)
            free (copy[i]);
          free (copy);
          free (args);
          return embemacs_start_fail (EMBEMACS_ERR_INVALID_ARGUMENT,
                                      "embemacs_start called with null argv element");
        }
      copy[i] = strdup (argv[i]);
      if (copy[i] == NULL)
        {
          while (i-- > 0)
            free (copy[i]);
          free (copy);
          free (args);
          return embemacs_start_fail (EMBEMACS_ERR_NO_MEMORY,
                                      "embemacs_start could not copy argv");
        }
    }

  args->argc = argc;
  args->argv = copy;

  /* Process-lifetime copy: it becomes a real leak only if emacs_main
     ever returns in fiber mode.  */
  embemacs_host_owns_app = true;
  if (!embemacs_install_backend_driver ())
    {
      embemacs_host_owns_app = false;
      embemacs_free_start_args (args);
      return embemacs_start_fail (EMBEMACS_ERR_NO_MEMORY,
                                  "embemacs_start could not install the backend fiber driver");
    }
  if (!embfiber_launch (embemacs_start_entry, args, EMBFIBER_DEFAULT_STACK_SIZE))
    {
      embemacs_host_owns_app = false;
      embemacs_free_start_args (args);
      return embemacs_start_fail (EMBEMACS_ERR_STACK_ALLOC,
                                  "embemacs_start could not allocate the fiber stack");
    }
  if (embfiber_finished_p ())
    return embemacs_start_fail (EMBEMACS_ERR_NOT_RUNNING,
                                "Emacs exited during startup");
  return EMBEMACS_OK;
}

#else

static int
embemacs_start_fail (enum embemacs_start_result code, const char *message)
{
  fprintf (stderr, "embemacs: %s\n", message);
  return code;
}

int
embemacs_start (int argc, char **argv)
{
  (void) argc;
  (void) argv;
  return embemacs_start_fail (EMBEMACS_ERR_UNSUPPORTED,
                              "embemacs_start requires the Cocoa NS or PGTK port");
}

int
embemacs_eval_async (const char *lisp)
{
  (void) lisp;
  return embemacs_start_fail (EMBEMACS_ERR_UNSUPPORTED,
                              "embemacs_eval_async requires the Cocoa NS or PGTK port");
}

int
embemacs_eval_async_with_result (const char *lisp,
                                 embemacs_eval_callback callback,
                                 void *context)
{
  (void) lisp;
  (void) callback;
  (void) context;
  return embemacs_start_fail (EMBEMACS_ERR_UNSUPPORTED,
                              "embemacs_eval_async requires the Cocoa NS or PGTK port");
}

void
embemacs_run_pending_evals (void)
{
}

int
embemacs_set_ready_callback (embemacs_ready_callback callback, void *context)
{
  (void) callback;
  (void) context;
  return embemacs_start_fail (EMBEMACS_ERR_UNSUPPORTED,
                              "embemacs callbacks require the Cocoa NS or PGTK port");
}

int
embemacs_set_title_callback (embemacs_title_callback callback, void *context)
{
  (void) callback;
  (void) context;
  return embemacs_start_fail (EMBEMACS_ERR_UNSUPPORTED,
                              "embemacs callbacks require the Cocoa NS or PGTK port");
}

int
embemacs_set_exit_callback (embemacs_exit_callback callback, void *context)
{
  (void) callback;
  (void) context;
  return embemacs_start_fail (EMBEMACS_ERR_UNSUPPORTED,
                              "embemacs callbacks require the Cocoa NS or PGTK port");
}

void
embemacs_notify_title (const char *title)
{
  (void) title;
}

void
embemacs_handle_exit (int exit_code)
{
  (void) exit_code;
}

#endif
