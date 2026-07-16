#ifndef EMBFIBER_TEST
#include <config.h>
#endif

#include "embfiber.h"

#include <stdio.h>
#include <stdlib.h>

volatile bool embfiber_active;
volatile sig_atomic_t embfiber_resumable;
EMBFIBER_TLS volatile bool embfiber_on_fiber;
EMBFIBER_TLS bool embfiber_forbid_lisp;

void
embfiber_abort_on_host_stack (const char *operation, const char *entry)
{
  if (entry != NULL)
    fprintf (stderr, "embemacs: %s on host stack: %s\n", operation, entry);
  else
    fprintf (stderr, "embemacs: %s on host stack\n", operation);
  abort ();
}

#if (defined HAVE_NS || defined HAVE_PGTK || defined EMBFIBER_TEST) \
  && (defined __aarch64__ || defined __x86_64__)

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined __linux__ && defined __x86_64__
# include <asm/prctl.h>
# include <sys/syscall.h>
# ifndef ARCH_SHSTK_STATUS
#  define ARCH_SHSTK_STATUS 0x5005
# endif
# ifndef ARCH_SHSTK_SHSTK
#  define ARCH_SHSTK_SHSTK (1ULL << 0)
# endif
#endif

bool
embfiber_platform_supported_p (void)
{
#if defined __linux__ && defined __x86_64__
  unsigned long status = 0;
  if (syscall (SYS_arch_prctl, ARCH_SHSTK_STATUS, &status) == 0
      && (status & ARCH_SHSTK_SHSTK) != 0)
    return false;
#endif
  /* AArch64 guarded-control-stack enablement will need an equivalent runtime
     check before Linux distributions begin enabling it for host processes. */
  return true;
}

/* The switch assembly has no CFI directives, so debugger backtraces stop
   at the fiber boundary.  */
/* Mach-O prefixes C symbols with an underscore; ELF does not.  */
#if defined (__APPLE__)
# define EMBFIBER_SWITCH_ASM "_embfiber_switch"
#else
# define EMBFIBER_SWITCH_ASM "embfiber_switch"
#endif

#if defined (__aarch64__)
__asm__ (
".text\n"
".align 2\n"
".globl " EMBFIBER_SWITCH_ASM "\n"
EMBFIBER_SWITCH_ASM ":\n"
"  stp d14, d15, [sp, #-16]!\n"
"  stp d12, d13, [sp, #-16]!\n"
"  stp d10, d11, [sp, #-16]!\n"
"  stp d8, d9, [sp, #-16]!\n"
"  stp x29, x30, [sp, #-16]!\n"
"  stp x27, x28, [sp, #-16]!\n"
"  stp x25, x26, [sp, #-16]!\n"
"  stp x23, x24, [sp, #-16]!\n"
"  stp x21, x22, [sp, #-16]!\n"
"  stp x19, x20, [sp, #-16]!\n"
"  mov x2, sp\n"
"  str x2, [x0]\n"
"  mov sp, x1\n"
"  ldp x19, x20, [sp], #16\n"
"  ldp x21, x22, [sp], #16\n"
"  ldp x23, x24, [sp], #16\n"
"  ldp x25, x26, [sp], #16\n"
"  ldp x27, x28, [sp], #16\n"
"  ldp x29, x30, [sp], #16\n"
"  ldp d8, d9, [sp], #16\n"
"  ldp d10, d11, [sp], #16\n"
"  ldp d12, d13, [sp], #16\n"
"  ldp d14, d15, [sp], #16\n"
"  ret\n");
#elif defined (__x86_64__)
__asm__ (
".text\n"
".globl " EMBFIBER_SWITCH_ASM "\n"
EMBFIBER_SWITCH_ASM ":\n"
"  pushq %rbp\n"
"  pushq %rbx\n"
"  pushq %r12\n"
"  pushq %r13\n"
"  pushq %r14\n"
"  pushq %r15\n"
"  movq %rsp, (%rdi)\n"
"  movq %rsi, %rsp\n"
"  popq %r15\n"
"  popq %r14\n"
"  popq %r13\n"
"  popq %r12\n"
"  popq %rbx\n"
"  popq %rbp\n"
"  retq\n");
#else
#error "embfiber requires arm64 or x86_64"
#endif

extern void embfiber_switch (void **save_sp, void *load_sp);

static bool embfiber_launched;
static bool embfiber_done;
static void *embfiber_host_sp;
static void *embfiber_fiber_sp;
static void (*embfiber_entry) (void *);
static void *embfiber_entry_arg;
static void *(*embfiber_service_fn) (void *);
static void *embfiber_service_arg;
static void *embfiber_service_result;
static bool embfiber_service_pending;
static bool embfiber_service_blocked_logged;
static bool embfiber_service_after_exit_logged;

static void embfiber_trampoline (void) __attribute__ ((noreturn));

static void
embfiber_die (const char *message)
{
  fprintf (stderr, "embfiber: %s\n", message);
  abort ();
}

static size_t
embfiber_round_up (size_t value, size_t alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

static void *
embfiber_make_initial_sp (void *stack_top)
{
  uintptr_t sp = (uintptr_t) stack_top & ~(uintptr_t) 0xf;

#if defined (__aarch64__)
  uint64_t *frame = (uint64_t *) (sp - 20 * sizeof *frame);

  memset (frame, 0, 20 * sizeof *frame);
  frame[11] = (uint64_t) (uintptr_t) embfiber_trampoline;
  return frame;
#elif defined (__x86_64__)
  uint64_t *frame = (uint64_t *) (sp - 8 * sizeof *frame);

  memset (frame, 0, 8 * sizeof *frame);
  frame[6] = (uint64_t) (uintptr_t) embfiber_trampoline;
  return frame;
#endif
}

static bool
embfiber_create_stack (size_t stack_size)
{
  long page_size_long = sysconf (_SC_PAGESIZE);
  size_t page_size;
  size_t usable_size;
  size_t mapping_size;
  char *mapping;
  char *stack_low;
  char *stack_high;

  if (page_size_long <= 0)
    return false;

  page_size = (size_t) page_size_long;
  if (stack_size == 0)
    stack_size = EMBFIBER_DEFAULT_STACK_SIZE;
  usable_size = embfiber_round_up (stack_size, page_size);
  mapping_size = usable_size + page_size;

  mapping = mmap (NULL, mapping_size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mapping == MAP_FAILED)
    {
      fprintf (stderr, "embfiber: mmap failed: %s\n", strerror (errno));
      return false;
    }

  if (mprotect (mapping, page_size, PROT_NONE) != 0)
    {
      fprintf (stderr, "embfiber: mprotect failed: %s\n", strerror (errno));
      munmap (mapping, mapping_size);
      return false;
    }

  stack_low = mapping + page_size;
  stack_high = stack_low + usable_size;

  /* This stack is deliberately process-lifetime storage.  Unmapping it
     safely would require a separate reclamation stack after the fiber exits.  */
  embfiber_fiber_sp = embfiber_make_initial_sp (stack_high);
  return true;
}

static void
embfiber_set_on_fiber (bool on_fiber)
{
  embfiber_on_fiber = on_fiber;
  embfiber_forbid_lisp = embfiber_active && !on_fiber;
}

static void
embfiber_trampoline (void)
{
  embfiber_entry (embfiber_entry_arg);
  embfiber_done = true;
  embfiber_resumable = 0;
  embfiber_set_on_fiber (false);

  for (;;)
    embfiber_switch (&embfiber_fiber_sp, embfiber_host_sp);
}

bool
embfiber_launch (void (*entry) (void *), void *arg, size_t stack_size)
{
  if (embfiber_launched)
    return false;
  if (entry == NULL)
    embfiber_die ("launch called with null entry");

  if (!embfiber_create_stack (stack_size))
    return false;
  embfiber_entry = entry;
  embfiber_entry_arg = arg;
  embfiber_launched = true;
  /* This is the single place that marks fiber mode active.  */
  embfiber_active = true;
  embfiber_resumable = 1;
  embfiber_forbid_lisp = true;

  if (!embfiber_resume ())
    embfiber_die ("initial resume failed");
  return true;
}

void
embfiber_yield (void)
{
  if (!embfiber_on_fiber)
    embfiber_die ("yield called while not on fiber");

  embfiber_set_on_fiber (false);
  embfiber_switch (&embfiber_fiber_sp, embfiber_host_sp);
}

void
embfiber_park (void)
{
  if (!embfiber_on_fiber)
    embfiber_die ("park called while not on fiber");

  for (;;)
    {
      embfiber_yield ();
      if (!embfiber_service_pending)
        return;

      embfiber_service_pending = false;
      embfiber_service_result = embfiber_service_fn (embfiber_service_arg);
      embfiber_service_fn = NULL;
      embfiber_service_arg = NULL;
    }
}

void
embfiber_exit (void)
{
  if (!embfiber_on_fiber)
    embfiber_die ("exit called while not on fiber");

  embfiber_done = true;
  embfiber_resumable = 0;
  embfiber_set_on_fiber (false);

  for (;;)
    embfiber_switch (&embfiber_fiber_sp, embfiber_host_sp);
}

bool
embfiber_resume (void)
{
  if (!embfiber_launched || embfiber_on_fiber || embfiber_done)
    return false;

  embfiber_set_on_fiber (true);
  embfiber_switch (&embfiber_host_sp, embfiber_fiber_sp);
  embfiber_set_on_fiber (false);
  return true;
}

bool
embfiber_launched_p (void)
{
  return embfiber_launched;
}

bool
embfiber_finished_p (void)
{
  return embfiber_done;
}

/* If a service body parks, it continues later on the fiber and the host
   gets NULL; no second host service may start while that body is blocked.  */
void *
embfiber_call_on (void *(*fn) (void *), void *arg)
{
  if (fn == NULL)
    embfiber_die ("service call with null function");

  if (!embfiber_active || embfiber_on_fiber)
    return fn (arg);

  /* AppKit keeps calling back (redraws, notifications) after Emacs has
     exited; the defunct fiber cannot serve them.  */
  if (embfiber_done)
    {
      if (!embfiber_service_after_exit_logged)
        {
          fputs ("embemacs: service call after fiber exit; result unavailable\n",
                 stderr);
          embfiber_service_after_exit_logged = true;
        }
      return NULL;
    }

  if (embfiber_service_fn != NULL)
    embfiber_die ("nested service call while a previous service is blocked on the fiber");

  embfiber_service_fn = fn;
  embfiber_service_arg = arg;
  embfiber_service_result = NULL;
  embfiber_service_pending = true;

  if (!embfiber_resume ())
    embfiber_die ("service call could not resume the fiber");

  if (embfiber_service_fn != NULL)
    {
      if (!embfiber_service_blocked_logged)
        {
          fputs ("embemacs: service call blocked on the fiber; result unavailable\n",
                 stderr);
          embfiber_service_blocked_logged = true;
        }
      return NULL;
    }

  return embfiber_service_result;
}

#ifdef EMBFIBER_TEST
static char embfiber_test_events[32];
static int embfiber_test_event_count;
static int embfiber_test_counter;
static bool embfiber_test_direct_on_fiber;
static bool embfiber_test_roundtrip_on_fiber;
static bool embfiber_test_blocked_complete;
static int embfiber_test_token;

static void
embfiber_test_record (char event)
{
  embfiber_test_events[embfiber_test_event_count++] = event;
}

static void *
embfiber_test_direct_service (void *arg)
{
  embfiber_test_direct_on_fiber = embfiber_on_fiber;
  embfiber_test_record ('D');
  return arg;
}

static void *
embfiber_test_roundtrip_service (void *arg)
{
  embfiber_test_roundtrip_on_fiber = embfiber_on_fiber;
  embfiber_test_record ('R');
  return arg;
}

static void *
embfiber_test_blocking_service (void *arg)
{
  (void) arg;
  embfiber_test_record ('S');
  embfiber_park ();
  embfiber_test_record ('T');
  embfiber_test_blocked_complete = true;
  return &embfiber_test_token;
}

static void
embfiber_test_entry (void *arg)
{
  int *counter = arg;

  ++*counter;
  embfiber_test_record ('A');
  if (embfiber_call_on (embfiber_test_direct_service, counter) != counter
      || !embfiber_test_direct_on_fiber)
    abort ();

  embfiber_park ();
  ++*counter;
  embfiber_test_record ('B');

  embfiber_park ();
  ++*counter;
  embfiber_test_record ('C');
}

int
main (void)
{
  void *result;

  embfiber_test_record ('0');
  if (!embfiber_launch (embfiber_test_entry, &embfiber_test_counter,
                        EMBFIBER_DEFAULT_STACK_SIZE))
    abort ();
  if (embfiber_test_counter != 1 || embfiber_finished_p ())
    abort ();

  result = embfiber_call_on (embfiber_test_roundtrip_service,
                             &embfiber_test_token);
  if (result != &embfiber_test_token || !embfiber_test_roundtrip_on_fiber)
    abort ();

  embfiber_test_record ('1');
  if (!embfiber_resume () || embfiber_test_counter != 2
      || embfiber_finished_p ())
    abort ();

  embfiber_test_record ('2');
  result = embfiber_call_on (embfiber_test_blocking_service, NULL);
  if (result != NULL || embfiber_test_blocked_complete)
    abort ();

  embfiber_test_record ('3');
  if (!embfiber_resume () || !embfiber_test_blocked_complete
      || embfiber_finished_p ())
    abort ();

  embfiber_test_record ('4');
  if (!embfiber_resume () || embfiber_test_counter != 3
      || !embfiber_finished_p ())
    abort ();

  embfiber_test_record ('5');
  embfiber_test_events[embfiber_test_event_count] = '\0';
  if (strcmp (embfiber_test_events, "0ADR1B2S3T4C5") != 0)
    abort ();
  if (embfiber_resume ())
    abort ();

  /* Service calls after the fiber has finished return NULL.  */
  if (embfiber_call_on (embfiber_test_roundtrip_service,
                        &embfiber_test_token)
      != NULL)
    abort ();

  puts ("PASS");
  return 0;
}
#endif /* EMBFIBER_TEST */

#endif /* (HAVE_NS || HAVE_PGTK || EMBFIBER_TEST)
          && (__aarch64__ || __x86_64__) */
