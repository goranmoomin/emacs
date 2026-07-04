/* embemacs.h --- public embedding API for GNU Emacs (ns port)      -*- h -*- */

/* This header is the stable entry point for embedding the Emacs ns
   (AppKit) port inside a host macOS application as an NSView.

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

#ifdef __cplusplus
extern "C" {
#endif

/* Run Emacs.  argc/argv are as for the standalone `emacs' program
   (argv[0] is used for executable/pdump discovery).  Performs startup
   and then blocks in the editing command loop.  Returns the Emacs exit
   code when Emacs terminates.  Must be called on the main thread.  */
extern int emacs_main (int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* EMBEMACS_H */
