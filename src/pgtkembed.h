/* pgtkembed.h --- internal declarations for embedded PGTK widgets.  -*- c -*-

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

#ifndef PGTKEMBED_H
#define PGTKEMBED_H

#include <stdbool.h>
#include <signal.h>
#include <sys/select.h>
#include <time.h>

struct frame;

/* Store a borrowed same-process GtkContainer for the first PGTK frame.  The
   returning Linux fiber path consumes the placement slot after normal frame
   setup and reparents the frame's GTK content into it.  */
extern void pgtkembed_set_parent_widget (void *parent_widget);
extern bool pgtkembed_parent_pending_p (void);
extern bool pgtkembed_attach_frame (struct frame *f);
extern bool pgtkembed_frame_p (struct frame *f);

/* PGTK fiber backend used by embemacs.c and xgselect.c.  */
extern bool pgtkembed_ui_thread_p (void);
extern bool pgtkembed_parent_widget_set_p (void);
extern bool pgtk_embfiber_install_driver (void);
extern int pgtk_embfiber_select (int, fd_set *, fd_set *, fd_set *,
                                 const struct timespec *, const sigset_t *);
extern void pgtk_embfiber_wake (void);
extern void pgtk_embfiber_wake_from_signal (void);

#endif /* PGTKEMBED_H */
