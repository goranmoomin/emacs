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

bool embemacs_host_owns_app;

#include <stdio.h>

#if defined HAVE_NS && defined NS_IMPL_COCOA

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static int
embemacs_start_fail (enum embemacs_start_result code, const char *message)
{
  fprintf (stderr, "embemacs: %s\n", message);
  return code;
}

void
embemacs_set_embed_parent_view (void *parent_view)
{
  embemacs_embed_parent_view = parent_view;
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

  if (!pthread_main_np ())
    return embemacs_start_fail (EMBEMACS_ERR_WRONG_THREAD,
                                "embemacs_start must be called on the main thread");
  if (embfiber_launched_p ())
    return embemacs_start_fail (EMBEMACS_ERR_ALREADY_STARTED,
                                "embemacs_start called after Emacs already started");
  if (embemacs_embed_parent_view == NULL)
    return embemacs_start_fail (EMBEMACS_ERR_NO_PARENT_VIEW,
                                "embemacs_start requires an embed parent view");
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
  embfiber_install_event_monitor ();
  if (!embfiber_launch (embemacs_start_entry, args, EMBFIBER_DEFAULT_STACK_SIZE))
    {
      embemacs_host_owns_app = false;
      embemacs_free_start_args (args);
      return embemacs_start_fail (EMBEMACS_ERR_STACK_ALLOC,
                                  "embemacs_start could not allocate the fiber stack");
    }
  return EMBEMACS_OK;
}

#elif defined HAVE_NS

static int
embemacs_start_fail (enum embemacs_start_result code, const char *message)
{
  fprintf (stderr, "embemacs: %s\n", message);
  return code;
}

void
embemacs_set_embed_parent_view (void *parent_view)
{
  embemacs_embed_parent_view = parent_view;
}

int
embemacs_start (int argc, char **argv)
{
  (void) argc;
  (void) argv;
  return embemacs_start_fail (EMBEMACS_ERR_UNSUPPORTED,
                              "embemacs_start requires the Cocoa NS port");
}

#else

void
embemacs_set_embed_parent_view (void *parent_view)
{
  (void) parent_view;
}

int
embemacs_start (int argc, char **argv)
{
  (void) argc;
  (void) argv;
  fprintf (stderr, "embemacs: embemacs_start requires the NS port\n");
  return EMBEMACS_ERR_UNSUPPORTED;
}

#endif
