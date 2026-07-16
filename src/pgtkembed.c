/* pgtkembed.c --- embed a PGTK frame in a host GTK container.

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

#include "pgtkembed.h"

#if defined HAVE_PGTK && defined __linux__ \
  && (defined __aarch64__ || defined __x86_64__)

#include "lisp.h"
#include "frame.h"
#include "pgtkterm.h"
#include "embfiber.h"
#include "xgselect.h"

#include <gtk/gtk.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>

static GtkWidget *pgtkembed_parent_widget;
static int pgtk_embfiber_wake_pipe[2] = {-1, -1};
static struct pgtk_embfiber_wait_source *pgtk_embfiber_wait;

/* One high-priority source represents one suspended xg_select call.  It
   mirrors the union of Emacs and GLib poll descriptors assembled by
   xg_select.  The host-owned GMainLoop therefore selects this source before
   ordinary GTK sources; its dispatch resumes Emacs, and Emacs itself drains
   the pending GTK callbacks on the fiber stack.  */
struct pgtk_embfiber_wait_source
{
  GSource source;
  int fds_lim;
  fd_set *rfds;
  fd_set *wfds;
  fd_set *efds;
  fd_set want_rfds;
  fd_set want_wfds;
  fd_set want_efds;
  bool have_rfds;
  bool have_wfds;
  bool have_efds;
  GPollFD epoll_poll;
  int epoll_fd;
  int timer_fd;
  int registered_fds[FD_SETSIZE];
  int n_registered;
  bool have_nonpollable_fd;
  bool active;
  int result;
  int result_errno;
};

static void
pgtk_embfiber_drain_wake_pipe (void)
{
  char buffer[128];

  while (read (pgtk_embfiber_wake_pipe[0], buffer, sizeof buffer) > 0)
    continue;
}

static void
pgtk_embfiber_drain_timerfd (int timer_fd)
{
  uint64_t expirations;

  while (read (timer_fd, &expirations, sizeof expirations)
         == sizeof expirations)
    continue;
}

static void
pgtk_embfiber_finish_select (struct pgtk_embfiber_wait_source *wait)
{
  struct epoll_event events[FD_SETSIZE + 2];
  int n_events = epoll_wait (wait->epoll_fd, events, ARRAYELTS (events), 0);
  int ready = 0;
  bool forced = false;
  bool timer_ready = false;

  if (wait->rfds != NULL)
    FD_ZERO (wait->rfds);
  if (wait->wfds != NULL)
    FD_ZERO (wait->wfds);
  if (wait->efds != NULL)
    FD_ZERO (wait->efds);

  for (int i = 0; i < n_events; ++i)
    {
      if (events[i].data.fd == pgtk_embfiber_wake_pipe[0])
        forced = true;
      else if (events[i].data.fd == wait->timer_fd)
        timer_ready = true;
    }

  if (timer_ready)
    pgtk_embfiber_drain_timerfd (wait->timer_fd);

  if (forced)
    {
      pgtk_embfiber_drain_wake_pipe ();
      wait->result = -1;
      wait->result_errno = EINTR;
      return;
    }

  /* epoll rejects regular files with EPERM.  If one was requested, use a
     zero-time pselect to preserve select semantics for the complete set.  */
  if (wait->have_nonpollable_fd)
    {
      fd_set ready_rfds = wait->want_rfds;
      fd_set ready_wfds = wait->want_wfds;
      fd_set ready_efds = wait->want_efds;
      struct timespec zero = {0, 0};
      int result = pselect (wait->fds_lim,
                            wait->have_rfds ? &ready_rfds : NULL,
                            wait->have_wfds ? &ready_wfds : NULL,
                            wait->have_efds ? &ready_efds : NULL,
                            &zero, NULL);
      if (result >= 0)
        {
          if (wait->rfds != NULL)
            *wait->rfds = ready_rfds;
          if (wait->wfds != NULL)
            *wait->wfds = ready_wfds;
          if (wait->efds != NULL)
            *wait->efds = ready_efds;
        }
      wait->result = result;
      wait->result_errno = result < 0 ? errno : 0;
      return;
    }

  for (int i = 0; i < n_events; ++i)
    {
      int fd = events[i].data.fd;
      uint32_t revents = events[i].events;
      bool fd_ready = false;

      if (fd == pgtk_embfiber_wake_pipe[0] || fd == wait->timer_fd)
        continue;

      if (wait->have_rfds && FD_ISSET (fd, &wait->want_rfds)
          && (revents & (EPOLLIN | EPOLLHUP | EPOLLERR)))
        {
          FD_SET (fd, wait->rfds);
          fd_ready = true;
        }
      if (wait->have_wfds && FD_ISSET (fd, &wait->want_wfds)
          && (revents & (EPOLLOUT | EPOLLHUP | EPOLLERR)))
        {
          FD_SET (fd, wait->wfds);
          fd_ready = true;
        }
      if (wait->have_efds && FD_ISSET (fd, &wait->want_efds)
          && (revents & (EPOLLPRI | EPOLLERR)))
        {
          FD_SET (fd, wait->efds);
          fd_ready = true;
        }
      if (fd_ready)
        ++ready;
    }

  wait->result = n_events < 0 ? -1 : ready;
  wait->result_errno = n_events < 0 ? errno : 0;
}

static gboolean
pgtk_embfiber_source_check (GSource *source)
{
  struct pgtk_embfiber_wait_source *wait
    = (struct pgtk_embfiber_wait_source *) source;
  return wait->active && wait->epoll_poll.revents != 0;
}

static gboolean
pgtk_embfiber_source_dispatch (GSource *source, GSourceFunc callback,
                               gpointer user_data)
{
  struct pgtk_embfiber_wait_source *wait
    = (struct pgtk_embfiber_wait_source *) source;

  (void) callback;
  (void) user_data;

  /* Keep this source recursively visible so its one poll descriptor never
     leaves GLib's poll set while the fiber runs.  active suppresses recursive
     dispatch until Emacs has drained the underlying readiness and reaches
     the exact point where it parks again.  */
  pgtk_embfiber_finish_select (wait);
  wait->active = false;
  if (!embfiber_resume () || embfiber_finished_p ())
    return G_SOURCE_REMOVE;
  return G_SOURCE_CONTINUE;
}

static GSourceFuncs pgtk_embfiber_source_funcs =
{
  .check = pgtk_embfiber_source_check,
  .dispatch = pgtk_embfiber_source_dispatch,
};

bool
pgtkembed_ui_thread_p (void)
{
  return (pid_t) syscall (SYS_gettid) == getpid ();
}

bool
pgtkembed_parent_widget_set_p (void)
{
  return GTK_IS_CONTAINER (pgtkembed_parent_widget);
}

bool
pgtk_embfiber_install_driver (void)
{
  GSource *source = NULL;
  struct pgtk_embfiber_wait_source *wait = NULL;
  int epoll_fd = -1;
  int timer_fd = -1;

  if (pgtk_embfiber_wait != NULL)
    return true;

  if (pgtk_embfiber_wake_pipe[0] < 0)
    {
#ifdef HAVE_PIPE2
      if (pipe2 (pgtk_embfiber_wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
        goto fail;
#else
      if (pipe (pgtk_embfiber_wake_pipe) != 0)
        goto fail;
      for (int i = 0; i < 2; ++i)
        {
          fcntl (pgtk_embfiber_wake_pipe[i], F_SETFD, FD_CLOEXEC);
          int flags = fcntl (pgtk_embfiber_wake_pipe[i], F_GETFL);
          if (flags >= 0)
            fcntl (pgtk_embfiber_wake_pipe[i], F_SETFL,
                   flags | O_NONBLOCK);
        }
#endif
    }

  epoll_fd = epoll_create1 (EPOLL_CLOEXEC);
  if (epoll_fd < 0)
    goto fail;
  timer_fd = timerfd_create (CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (timer_fd < 0)
    goto fail;

  struct epoll_event wake_event =
    {
      .events = EPOLLIN,
      .data.fd = pgtk_embfiber_wake_pipe[0],
    };
  if (epoll_ctl (epoll_fd, EPOLL_CTL_ADD, pgtk_embfiber_wake_pipe[0],
                 &wake_event)
      != 0)
    goto fail;

  struct epoll_event timer_event =
    {
      .events = EPOLLIN,
      .data.fd = timer_fd,
    };
  if (epoll_ctl (epoll_fd, EPOLL_CTL_ADD, timer_fd, &timer_event) != 0)
    goto fail;

  source = g_source_new (&pgtk_embfiber_source_funcs, sizeof *wait);
  if (source == NULL)
    goto fail;
  wait = (struct pgtk_embfiber_wait_source *) source;
  wait->epoll_fd = epoll_fd;
  wait->timer_fd = timer_fd;
  wait->epoll_poll.fd = epoll_fd;
  wait->epoll_poll.events = G_IO_IN;
  g_source_add_poll (source, &wait->epoll_poll);
  g_source_set_priority (source, G_PRIORITY_HIGH - 20);
  g_source_set_can_recurse (source, TRUE);
  g_source_set_name (source, "embemacs PGTK fiber select gate");
  g_source_attach (source, g_main_context_default ());
  g_source_unref (source);
  pgtk_embfiber_wait = wait;
  return true;

fail:
  {
    int saved_errno = errno != 0 ? errno : ENOMEM;
    if (source != NULL)
      g_source_unref (source);
    if (timer_fd >= 0)
      close (timer_fd);
    if (epoll_fd >= 0)
      close (epoll_fd);
    for (int i = 0; i < 2; ++i)
      if (pgtk_embfiber_wake_pipe[i] >= 0)
        close (pgtk_embfiber_wake_pipe[i]);
    pgtk_embfiber_wake_pipe[0] = pgtk_embfiber_wake_pipe[1] = -1;
    fprintf (stderr, "embemacs: PGTK fiber gate: %s\n",
             strerror (saved_errno));
    errno = saved_errno;
  }
  return false;
}

void
pgtk_embfiber_wake_from_signal (void)
{
  char byte = 1;

  if (pgtk_embfiber_wake_pipe[1] >= 0)
    {
      ssize_t written = write (pgtk_embfiber_wake_pipe[1], &byte, 1);
      (void) written;
    }
}

void
pgtk_embfiber_wake (void)
{
  pgtk_embfiber_wake_from_signal ();
}

int
pgtk_embfiber_select (int fds_lim, fd_set *rfds, fd_set *wfds, fd_set *efds,
                      const struct timespec *timeout, const sigset_t *sigmask)
{
  struct pgtk_embfiber_wait_source *wait;
  struct itimerspec timer = {0};

  if (!embfiber_active || !embfiber_on_fiber)
    return pselect (fds_lim, rfds, wfds, efds, timeout, sigmask);

  /* The host GMainLoop, rather than this select function, performs the
     blocking poll.  There is no atomic way to apply a pselect mask across
     the intervening fiber switch.  Current PGTK callers pass NULL; reject a
     future non-NULL mask instead of providing subtly racy semantics.  */
  if (sigmask != NULL)
    {
      errno = ENOTSUP;
      return -1;
    }

  if (pgtk_embfiber_wait == NULL && !pgtk_embfiber_install_driver ())
    {
      errno = ENOMEM;
      return -1;
    }

  wait = pgtk_embfiber_wait;
  if (wait->active)
    {
      fputs ("embemacs: nested PGTK fiber select wait\n", stderr);
      abort ();
    }

  wait->fds_lim = fds_lim;
  wait->rfds = rfds;
  wait->wfds = wfds;
  wait->efds = efds;
  wait->have_rfds = rfds != NULL;
  wait->have_wfds = wfds != NULL;
  wait->have_efds = efds != NULL;
  wait->have_nonpollable_fd = false;
  wait->result = 0;
  wait->result_errno = 0;
  wait->epoll_poll.revents = 0;
  for (int i = 0; i < wait->n_registered; ++i)
    (void) epoll_ctl (wait->epoll_fd, EPOLL_CTL_DEL,
                      wait->registered_fds[i], NULL);
  wait->n_registered = 0;
  if (rfds != NULL)
    wait->want_rfds = *rfds;
  if (wfds != NULL)
    wait->want_wfds = *wfds;
  if (efds != NULL)
    wait->want_efds = *efds;

  for (int fd = 0; fd < fds_lim; ++fd)
    {
      uint32_t events = 0;
      if (rfds != NULL && FD_ISSET (fd, rfds))
        events |= EPOLLIN;
      if (wfds != NULL && FD_ISSET (fd, wfds))
        events |= EPOLLOUT;
      if (efds != NULL && FD_ISSET (fd, efds))
        events |= EPOLLPRI;
      if (events != 0
          && fd != pgtk_embfiber_wake_pipe[0]
          && fd != wait->timer_fd
          && fd != wait->epoll_fd)
        {
          struct epoll_event event = {.events = events, .data.fd = fd};
          if (epoll_ctl (wait->epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0)
            wait->registered_fds[wait->n_registered++] = fd;
          else if (errno == EPERM)
            wait->have_nonpollable_fd = true;
          else
            {
              wait->result_errno = errno;
              return -1;
            }
        }
    }

  pgtk_embfiber_drain_timerfd (wait->timer_fd);
  if (wait->have_nonpollable_fd)
    timer.it_value.tv_nsec = 1;
  else if (timeout != NULL)
    {
      timer.it_value = *timeout;
      /* A zero it_value disarms timerfd; use the smallest nonzero deadline
         to represent a zero-time select.  */
      if (timer.it_value.tv_sec == 0 && timer.it_value.tv_nsec == 0)
        timer.it_value.tv_nsec = 1;
    }
  if (timerfd_settime (wait->timer_fd, 0, &timer, NULL) != 0)
    return -1;

  wait->active = true;

  /* xg_select may own CONTEXT during initial startup.  Drop that ownership
     before returning to the host stack.  During later resumes the host's
     dispatch frame already owns CONTEXT, so release_select_lock is a no-op.  */
  release_select_lock ();
  embfiber_park ();

  int result = wait->result;
  int result_errno = wait->result_errno;
  if (result < 0)
    errno = result_errno;
  return result;
}

void
pgtkembed_set_parent_widget (void *parent_widget)
{
  pgtkembed_parent_widget = parent_widget;
}

bool
pgtkembed_parent_pending_p (void)
{
  return pgtkembed_parent_widget != NULL;
}

bool
pgtkembed_frame_p (struct frame *f)
{
  return (f != NULL && FRAME_PGTK_P (f) && FRAME_X_OUTPUT (f) != NULL
          && FRAME_X_OUTPUT (f)->embedded_in_host);
}

bool
pgtkembed_attach_frame (struct frame *f)
{
  GtkWidget *parent = pgtkembed_parent_widget;
  GtkWidget *outer;
  GtkWidget *content;

  if (parent == NULL || f == NULL || f->tooltip)
    return false;

  if (!GTK_IS_CONTAINER (parent))
    {
      fputs ("embemacs: PGTK embed parent is not a GtkContainer\n", stderr);
      return false;
    }

  outer = FRAME_GTK_OUTER_WIDGET (f);
  content = FRAME_X_OUTPUT (f)->vbox_widget;
  if (outer == NULL || content == NULL)
    {
      fputs ("embemacs: PGTK frame has no attachable GTK content\n", stderr);
      return false;
    }

  if (gtk_widget_get_display (parent) != gtk_widget_get_display (outer))
    {
      fputs ("embemacs: PGTK embed parent is on a different display\n",
             stderr);
      return false;
    }

  /* Keep the complete Emacs frame hierarchy (menu/tool bars, scroll bars,
     and the EmacsFixed drawing widget), but replace its temporary GtkWindow
     with the caller's same-process GTK container.  This is the Wayland-safe
     equivalent of embedding an NSView: no foreign native-window handle or
     XEmbed protocol is involved.  Keep a reference while the widget has no
     parent, and restore the original hierarchy if the host container rejects
     gtk_container_add.  */
  g_object_ref (content);
  gtk_container_remove (GTK_CONTAINER (outer), content);
  gtk_container_add (GTK_CONTAINER (parent), content);
  if (gtk_widget_get_parent (content) != parent)
    {
      gtk_container_add (GTK_CONTAINER (outer), content);
      g_object_unref (content);
      fputs ("embemacs: PGTK embed parent rejected frame content\n", stderr);
      return false;
    }

  {
    gpointer tbinfo = g_object_steal_data (G_OBJECT (outer),
                                           "xg_frame_tb_info");
    if (tbinfo != NULL)
      g_object_set_data (G_OBJECT (content), "xg_frame_tb_info", tbinfo);
  }

  gtk_widget_destroy (outer);
  g_object_unref (content);

  FRAME_GTK_OUTER_WIDGET (f) = NULL;
  FRAME_X_OUTPUT (f)->embedded_in_host = true;
  FRAME_X_OUTPUT (f)->embed_container = content;
  FRAME_X_OUTPUT (f)->explicit_parent = true;
  pgtkembed_parent_widget = NULL;

  gtk_widget_show_all (content);
  gtk_widget_grab_focus (FRAME_GTK_WIDGET (f));
  fputs ("embemacs: PGTK frame embedded in host GtkContainer\n", stderr);
  return true;
}

#else /* unsupported PGTK fiber platform */

void
pgtkembed_set_parent_widget (void *parent_widget)
{
  (void) parent_widget;
}

bool
pgtkembed_parent_pending_p (void)
{
  return false;
}

bool
pgtkembed_attach_frame (struct frame *f)
{
  (void) f;
  return false;
}

bool
pgtkembed_frame_p (struct frame *f)
{
  (void) f;
  return false;
}

bool
pgtkembed_ui_thread_p (void)
{
  return false;
}

bool
pgtkembed_parent_widget_set_p (void)
{
  return false;
}

bool
pgtk_embfiber_install_driver (void)
{
  return false;
}

int
pgtk_embfiber_select (int fds_lim, fd_set *rfds, fd_set *wfds, fd_set *efds,
                      const struct timespec *timeout, const sigset_t *sigmask)
{
  return pselect (fds_lim, rfds, wfds, efds, timeout, sigmask);
}

void
pgtk_embfiber_wake (void)
{
}

void
pgtk_embfiber_wake_from_signal (void)
{
}

#endif /* supported Linux PGTK fiber platform */
