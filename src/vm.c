/*
 * vm.c - virtual machine
 *
 *   Copyright (c) 2000-2010  Shiro Kawai  <shiro@acm.org>
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of the authors nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LIBGAUCHE_BODY
#include "gauche.h"
#include "gauche/class.h"
#include "gauche/exception.h"
#include "gauche/builtin-syms.h"
#include "gauche/code.h"
#include "gauche/vminsn.h"
#include "gauche/prof.h"


/* Experimental code to use custom mark procedure for stack gc.
   Currently it doens't show any improvement, so we disable it
   by default. */
#ifdef USE_CUSTOM_STACK_MARKER
#include "gc_mark.h"

static void **vm_stack_free_list;
static int vm_stack_kind;
static int vm_stack_mark_proc;
#endif /*USE_CUSTOM_STACK_MARKER*/

#ifdef HAVE_SCHED_H
#include <sched.h>
#endif

#ifndef EX_SOFTWARE
/* SRFI-22 requires this. */
#define EX_SOFTWARE 70
#endif

/* An object to mark the boundary frame. */
static ScmWord boundaryFrameMark = SCM_VM_INSN(SCM_VM_NOP);

/* return true if cont is a boundary continuation frame */
#define BOUNDARY_FRAME_P(cont) ((cont)->pc == &boundaryFrameMark)

/* A stub VM code to make VM return immediately */
static ScmWord return_code[] = { SCM_VM_INSN(SCM_VM_RET) };
#define PC_TO_RETURN  return_code

/* A dummy compiled code structure used as 'fill-in', when Scm_Apply
   is called without any VM code running.  See Scm_Apply below. */
static ScmCompiledCode internal_apply_compiled_code = 
    SCM_COMPILED_CODE_CONST_INITIALIZER(NULL, 0, 0, 0, 0,
                                        SCM_SYM_INTERNAL_APPLY,
                                        SCM_NIL, SCM_FALSE,
                                        SCM_FALSE, SCM_FALSE);

/*
 * The VM. 
 *
 *   VM encapsulates the dynamic status of the current exection.
 *   In Gauche, there's always one active virtual machine per thread,
 *   referred by Scm_VM().   From Scheme, VM is seen as a <thread> object.
 *
 *   From Scheme, VM is viewed as <thread> object.  The class definition
 *   is in thrlib.stub.
 */

static ScmVM *rootVM = NULL;         /* VM for primodial thread */

#ifdef GAUCHE_USE_PTHREADS
static pthread_key_t vm_key;
#define theVM   ((ScmVM*)pthread_getspecific(vm_key))
#else
static ScmVM *theVM;
#endif  /* !GAUCHE_USE_PTHREADS */

static void save_stack(ScmVM *vm);

static ScmSubr default_exception_handler_rec;
#define DEFAULT_EXCEPTION_HANDLER  SCM_OBJ(&default_exception_handler_rec)
static ScmObj throw_cont_calculate_handlers(ScmEscapePoint *, ScmVM *);
static ScmObj throw_cont_body(ScmObj, ScmEscapePoint*, ScmObj);
static void   process_queued_requests(ScmVM *vm);
static void   vm_finalize(ScmObj vm, void *data);

static ScmEnvFrame *get_env(ScmVM *vm);

/*#define COUNT_INSN_FREQUENCY*/
#ifdef COUNT_INSN_FREQUENCY
#include "vmstat.c"
#endif /*COUNT_INSN_FREQUENCY*/

/*
 * Constructor
 *
 *   PROTO argument is treated as a prototype for the new VM, i.e.
 *   some of default values are 'inherited' from PROTO.
 *
 *   VM should be 'attached' to the running OS thread before being
 *   used.  The root thread is always attached to the primordial thread
 *   at the initialization stage (see Scm__InitVM()).   For other threads,
 *   it depends on whether the thread is created from Gauche side or not.
 *
 *   If the thread is created from Gauche side (i.e. by Scm_MakeThread() 
 *   C API or make-thread Scheme API), attaching is handled automatically
 *   by Gauche.
 *
 *   If the thread is created by other means, the VM should be attached
 *   to the thread by Scm_AttachVM() API.   The VMs attached by this are
 *   somewhat different than the ones attached by Gauche; such VM can't
 *   be passed to thread-join, for example.   This type of VM is for
 *   the applications that want to evaluate Gauche program in their own
 *   thread.
 *   NOTE: the thread should still be created by Boehm-GC's pthread_create,
 *   for it is the only way for GC to see the thread's stack.
 */

ScmVM *Scm_NewVM(ScmVM *proto, ScmObj name)
{
    ScmVM *v = SCM_NEW(ScmVM);
    int i;
    
    SCM_SET_CLASS(v, SCM_CLASS_VM);
    v->state = SCM_VM_NEW;
    (void)SCM_INTERNAL_MUTEX_INIT(v->vmlock);
    (void)SCM_INTERNAL_COND_INIT(v->cond);
    v->canceller = NULL;
    v->inspector = NULL;
    v->name = name;
    v->specific = SCM_FALSE;
    v->thunk = NULL;
    v->result = SCM_UNDEFINED;
    v->resultException = SCM_UNDEFINED;
    v->module = proto ? proto->module : Scm_SchemeModule();
    v->cstack = proto ? proto->cstack : NULL;
    
    v->curin  = proto? proto->curin  : SCM_PORT(Scm_Stdin());
    v->curout = proto? proto->curout : SCM_PORT(Scm_Stdout());
    v->curerr = proto? proto->curerr : SCM_PORT(Scm_Stderr());

    Scm__VMParameterTableInit(&(v->parameters), proto);

    v->compilerFlags = proto? proto->compilerFlags : 0;
    v->runtimeFlags = proto? proto->runtimeFlags : 0;
    v->attentionRequest = 0;
    v->signalPending = 0;
    v->finalizerPending = 0;
    v->stopRequest = 0;

#ifdef USE_CUSTOM_STACK_MARKER
    v->stack = (ScmObj*)GC_generic_malloc((SCM_VM_STACK_SIZE+1)*sizeof(ScmObj),
                                          vm_stack_kind);
    *v->stack++ = SCM_OBJ(v);
#else  /*!USE_CUSTOM_STACK_MARKER*/
    v->stack = SCM_NEW_ARRAY(ScmObj, SCM_VM_STACK_SIZE);
#endif /*!USE_CUSTOM_STACK_MARKER*/
    v->sp = v->stack;
    v->stackBase = v->stack;
    v->stackEnd = v->stack + SCM_VM_STACK_SIZE;
#if GAUCHE_FFX
    v->fpstack = SCM_NEW_ATOMIC_ARRAY(ScmFlonum, SCM_VM_STACK_SIZE);
    v->fpstackEnd = v->fpstack + SCM_VM_STACK_SIZE;
    v->fpsp = v->fpstack;
#endif /* GAUCHE_FFX */

    v->env = NULL;
    v->argp = v->stack;
    v->cont = NULL;
    v->pc = PC_TO_RETURN;
    v->base = NULL;
    v->val0 = SCM_UNDEFINED;
    for (i=0; i<SCM_VM_MAX_VALUES; i++) v->vals[i] = SCM_UNDEFINED;
    v->numVals = 1;
    
    v->handlers = SCM_NIL;

    v->exceptionHandler = DEFAULT_EXCEPTION_HANDLER;
    v->escapePoint = v->escapePointFloating = NULL;
    v->escapeReason = SCM_VM_ESCAPE_NONE;
    v->escapeData[0] = NULL;
    v->escapeData[1] = NULL;
    v->defaultEscapeHandler = SCM_FALSE;

    v->load_history = SCM_NIL;
    v->load_next = SCM_NIL;
    v->load_port = SCM_FALSE;
    v->evalSituation = SCM_VM_EXECUTING;

    sigemptyset(&v->sigMask);
    Scm_SignalQueueInit(&v->sigq);

    /* stats */
    v->stat.sovCount = 0;
    v->stat.sovTime = 0;
    v->stat.loadStat = SCM_NIL;
    v->profilerRunning = FALSE;
    v->prof = NULL;

    (void)SCM_INTERNAL_THREAD_INIT(v->thread);

    Scm_RegisterFinalizer(SCM_OBJ(v), vm_finalize, NULL);
    return v;
}

/* Attach the thread to the current thread.
   See the notes of Scm_NewVM above.
   Returns TRUE on success, FALSE on failure. */
int Scm_AttachVM(ScmVM *vm)
{
#ifdef GAUCHE_USE_PTHREADS
    if (SCM_INTERNAL_THREAD_INITIALIZED_P(vm->thread)) return FALSE;
    if (theVM != NULL) return FALSE;

    if (pthread_setspecific(Scm_VMKey(), vm) != 0) return FALSE;

    vm->thread = pthread_self();
    vm->state = SCM_VM_RUNNABLE;
    return TRUE;
#else  /*!GAUCHE_USE_PTHREADS*/
    return FALSE;
#endif /*!GAUCHE_USE_PTHREADS*/
}

int Scm_VMGetNumResults(ScmVM *vm)
{
    return vm->numVals;
}

ScmObj Scm_VMGetResult(ScmVM *vm)
{
    ScmObj head = SCM_NIL, tail = SCM_NIL;
    int i;
    if (vm->numVals == 0) return SCM_NIL;
    SCM_APPEND1(head, tail, vm->val0);
    for (i=1; i<vm->numVals; i++) {
        SCM_APPEND1(head, tail, vm->vals[i-1]);
    }
    return head;
}

void Scm_VMSetResult(ScmObj obj)
{
    ScmVM *vm = theVM;
    vm->val0 = obj;
    vm->numVals = 1;
}

/*
 * Current VM.
 */
ScmVM *Scm_VM(void)
{
    return theVM;
}

/* Some macros inserts Scm_VM() in its output.  If such macros are expanded
   below, we can safely replace Scm_VM() to theVM. */
#define Scm_VM() theVM

/*
 * Get VM key
 */
#ifdef GAUCHE_USE_PTHREADS
pthread_key_t Scm_VMKey(void)
{
    return vm_key;
}
#endif /*GAUCHE_USE_PTHREADS*/

/* Warn if VM is terminated by uncaught exception, and GC-ed without
   joining.  It is cleary an unexpected case and worth reporting. */
static void vm_finalize(ScmObj obj, void *data)
{
    ScmVM *vm = SCM_VM(obj);
    ScmObj re = vm->resultException;

    if (SCM_UNCAUGHT_EXCEPTION_P(re)) {
        Scm_Warn("A thread %S died a lonely death with uncaught exception %S.",
                 vm->name, SCM_THREAD_EXCEPTION(re)->data);
    }
}

/*====================================================================
 * VM interpreter
 *
 *  Interprets intermediate code CODE on VM.
 */

/*
 * Micro-operations
 */

/* fetching */
#define INCR_PC                 (PC++)
#define FETCH_LOCATION(var)     ((var) = (ScmWord*)*PC)
#define FETCH_OPERAND(var)      ((var) = SCM_OBJ(*PC))
#define FETCH_OPERAND_PUSH      (*SP++ = SCM_OBJ(*PC))

#ifndef COUNT_INSN_FREQUENCY
#define FETCH_INSN(var)         ((var) = *PC++)
#else
#define FETCH_INSN(var)         ((var) = fetch_insn_counting(vm, var))
#endif

/* For sanity check in debugging mode */
#ifdef PARANOIA
#define CHECK_STACK_PARANOIA(n)  CHECK_STACK(n)
#else
#define CHECK_STACK_PARANOIA(n)  /*empty*/
#endif

/* Hint for gcc -- at this moment, using __builtin_expect doesn't 
   do any good.  I'll try this later on. */
#if 0
#define MOSTLY_FALSE(expr)  __builtin_expect(expr, 0)
#else
#define MOSTLY_FALSE(expr)  expr
#endif

/* Find the stack bottom next to the continuation frame.
   This macro should be applied only if CONT is in stack. */
#define CONT_FRAME_END(cont)                                            \
    ((cont)->argp?                                                      \
     ((ScmObj*)(cont) + CONT_FRAME_SIZE) :          /*Scheme continuation*/ \
     ((ScmObj*)(cont) + CONT_FRAME_SIZE + (cont)->size)) /*C continuation*/

/* check if *pc is an return instruction.  if so, some
   shortcuts are taken. */
#define TAIL_POS()         (*PC == SCM_VM_INSN(SCM_VM_RET))

/* push OBJ to the top of the stack */
#define PUSH_ARG(obj)      (*SP++ = (obj))

/* pop the top object of the stack and store it to VAR */
#define POP_ARG(var)       ((var) = *--SP)

#define SHIFT_FRAME(from, to, size)                     \
    do {                                                \
        ScmObj *f = (from), *t = (to);                  \
        int c;                                          \
        for (c=0; c<(size); c++, f++, t++) *t = *f;     \
    } while (0)

/* VM registers.  We've benchmarked if keeping some of those registers
   local variables makes VM loop run faster; however, it turned out
   that more local variables tended to make them spill from machine
   registers and didn't improve performance.  Having only vm, a pointer
   to the current VM, on register is enough. */
#define PC    (vm->pc)
#define SP    (vm->sp)
#define VAL0  (vm->val0)
#define ENV   (vm->env)
#define CONT  (vm->cont)
#define ARGP  (vm->argp)
#define BASE  (vm->base)

/* return true if ptr points into the stack area */
#define IN_STACK_P(ptr)                         \
      ((unsigned long)((ptr) - vm->stackBase) < SCM_VM_STACK_SIZE)

/* Check if stack has room at least size bytes. */
#define CHECK_STACK(size)                                       \
    do {                                                        \
        if (MOSTLY_FALSE(SP >= vm->stackEnd - (size))) {        \
            save_stack(vm);                                     \
        }                                                       \
    } while (0)

/* Push a continuation frame.  next_pc is the PC from where execution
   will be resumed.  */
#define PUSH_CONT(next_pc)                              \
    do {                                                \
        ScmContFrame *newcont = (ScmContFrame*)SP;      \
        newcont->prev = CONT;                           \
        newcont->env = ENV;                             \
        newcont->argp = ARGP;                           \
        newcont->size = (int)(SP - ARGP);               \
        newcont->pc = next_pc;                          \
        newcont->base = BASE;                           \
        CONT = newcont;                                 \
        SP += CONT_FRAME_SIZE;                          \
        ARGP = SP;                                      \
    } while (0)

#define CALL_CCONT(p, v, d) p(v, d)

/* pop a continuation frame, i.e. return from a procedure. */
#define POP_CONT()                                                      \
    do {                                                                \
        if (CONT->argp == NULL) {                                       \
            void *data__[SCM_CCONT_DATA_SIZE];                          \
            ScmObj v__ = VAL0;                                          \
            ScmCContinuationProc *after__;                              \
            void **d__ = data__;                                        \
            void **s__ = (void**)((ScmObj*)CONT + CONT_FRAME_SIZE);     \
            int i__ = CONT->size;                                       \
            while (i__-- > 0) {                                         \
                *d__++ = *s__++;                                        \
            }                                                           \
            after__ = (ScmCContinuationProc*)CONT->pc;                  \
            if (IN_STACK_P((ScmObj*)CONT)) SP = (ScmObj*)CONT;          \
            ENV = CONT->env;                                            \
            ARGP = SP;                                                  \
            PC = PC_TO_RETURN;                                          \
            BASE = CONT->base;                                          \
            CONT = CONT->prev;                                          \
            SCM_FLONUM_ENSURE_MEM(v__);                                 \
            VAL0 = CALL_CCONT(after__, v__, data__);                    \
        } else if (IN_STACK_P((ScmObj*)CONT)) {                         \
            SP   = CONT->argp + CONT->size;                             \
            ENV  = CONT->env;                                           \
            ARGP = CONT->argp;                                          \
            PC   = CONT->pc;                                            \
            BASE = CONT->base;                                          \
            CONT = CONT->prev;                                          \
        } else {                                                        \
            int size__ = CONT->size;                                    \
            ARGP = SP = vm->stackBase;                                  \
            ENV = CONT->env;                                            \
            PC = CONT->pc;                                              \
            BASE = CONT->base;                                          \
            if (CONT->argp && size__) {                                 \
                ScmObj *s__ = CONT->argp, *d__ = SP;                    \
                SP += size__;                                           \
                while (size__-- > 0) {                                  \
                    *d__++ = *s__++;                                    \
                }                                                       \
            }                                                           \
            CONT = CONT->prev;                                          \
        }                                                               \
    } while (0)

/* return operation. */
#define RETURN_OP()                                     \
    do {                                                \
        if (CONT == NULL || BOUNDARY_FRAME_P(CONT)) {   \
            return; /* no more continuations */         \
        }                                               \
        POP_CONT();                                     \
    } while (0)

/* push environment header to finish the environment frame.
   env, sp, argp is updated. */
#define FINISH_ENV(info_, up_)                  \
    do {                                        \
        ScmEnvFrame *e__ = (ScmEnvFrame*)SP;    \
        e__->up = up_;                          \
        e__->info = info_;                      \
        e__->size = SP - ARGP;                  \
        SP += ENV_HDR_SIZE;                     \
        ARGP = SP;                              \
        ENV = e__;                              \
    } while (0)

/* extend the current environment by SIZE words.   used for LET. */
#define PUSH_LOCAL_ENV(size_, info_)            \
    do {                                        \
        int i__;                                \
        for (i__=0; i__<size_; i__++) {         \
            *SP++ = SCM_UNDEFINED;              \
        }                                       \
        FINISH_ENV(info_, ENV);                 \
    } while (0)

/* used for the inlined instruction which is supposed to be called at
   tail position (e.g. SLOT-REF).  This checks whether we're at the tail
   position or not, and if not, push a cont frame to make the operation
   a tail call. */
#define TAIL_CALL_INSTRUCTION()                 \
    do {                                        \
        if (!TAIL_POS()) {                      \
            CHECK_STACK(CONT_FRAME_SIZE);       \
            PUSH_CONT(PC);                      \
            PC = PC_TO_RETURN;                  \
        }                                       \
    } while (0)

/* global reference.  this piece of code is used for a few GREF-something
   combined instruction. */
#define GLOBAL_REF(v)                                                   \
    do {                                                                \
        ScmGloc *gloc;                                                  \
        FETCH_OPERAND(v);                                               \
        if (!SCM_GLOCP(v)) {                                            \
            VM_ASSERT(SCM_IDENTIFIERP(v));                              \
            gloc = Scm_FindBinding(SCM_IDENTIFIER(v)->module,           \
                                   SCM_IDENTIFIER(v)->name,             \
                                   0);                                  \
            if (gloc == NULL) {                                         \
                VM_ERR(("unbound variable: %S",                         \
                        SCM_IDENTIFIER(v)->name));                      \
            }                                                           \
            /* memorize gloc */                                         \
            *PC = SCM_WORD(gloc);                                       \
        } else {                                                        \
            gloc = SCM_GLOC(v);                                         \
        }                                                               \
        v = SCM_GLOC_GET(gloc);                                         \
        if (SCM_AUTOLOADP(v)) {                                         \
            v = Scm_ResolveAutoload(SCM_AUTOLOAD(v), 0);                \
        }                                                               \
        if (SCM_UNBOUNDP(v)) {                                          \
            VM_ERR(("unbound variable: %S", SCM_OBJ(gloc->name)));      \
        }                                                               \
        INCR_PC;                                                        \
    } while (0)

/* for debug */
#define VM_DUMP(delimiter)                      \
    fprintf(stderr, delimiter);                 \
    Scm_VMDump(vm)

#define VM_ASSERT(expr)                                                 \
    do {                                                                \
        if (!(expr)) {                                                  \
            fprintf(stderr, "\"%s\", line %d: Assertion failed: %s\n",  \
                    __FILE__, __LINE__, #expr);                         \
            Scm_VMDump(theVM);                                          \
            Scm_Panic("exitting...\n");                                 \
        }                                                               \
    } while (0)

#define VM_ERR(errargs)                         \
   do {                                         \
      Scm_Error errargs;                        \
   } while (0)

/* Discard the current procedure's local frame before performing a tail call.
   Just before the tail call, the typical stack position is like this:
   
   SP  >|      |
        | argN |
        |   :  |
   ARGP>| arg0 |
        | env  |
   ENV >| env  |
        |localM|
        |   :  |
        |local0|
   CONT>| cont |

  Arg0...argN is the arguments for the call, and local0...localM is the
  environment of the current procedure, which is no longer needed.
  We shift arg0...argN to just above the continuation frame.

  If the continuation frame has been saved (i.e. CONT is not pointing
  the stack area), then we know for sure that there's no valid data
  from the stack bottom to ARGP.  So we shift arg0...argN to the
  beginning of the stack.  We set ENV = NULL afterwards to prevent
  the debugging process from being confused---at the end of the procedure
  calling sequence, ENV is set to point to the newly formed environment
  frame out of arg0...argN.

  MEMO: this shifting used to be done after folding &rest arguments.
  Benchmark showed shifting first is slightly faster.
*/
#define DISCARD_ENV()                                                   \
    do {                                                                \
        ScmObj *to;                                                     \
        int argc = (int)(SP - ARGP);                                    \
        if (IN_STACK_P((ScmObj*)CONT)) {                                \
            to = CONT_FRAME_END(CONT);                                  \
        } else {                                                        \
            to = vm->stackBase;                                         \
        }                                                               \
        if (argc) {                                                     \
            ScmObj *t = to, *a = ARGP;                                  \
            int c;                                                      \
            for (c=0; c<argc; c++) *t++ = *a++;                         \
        }                                                               \
        ARGP = to;                                                      \
        SP = to + argc;                                                 \
        ENV = NULL;                                                     \
    } while (0)


/* inline expansion of number comparison. */
#define NUM_CMP(op, r)                                          \
    do {                                                        \
        ScmObj x_, y_ = VAL0;                                   \
        POP_ARG(x_);                                            \
        if (SCM_INTP(y_) && SCM_INTP(x_)) {                     \
            r = ((signed long)(intptr_t)x_ op (signed long)(intptr_t)y_); \
        } else if (SCM_FLONUMP(y_) && SCM_FLONUMP(x_)) {        \
            r = (SCM_FLONUM_VALUE(x_) op SCM_FLONUM_VALUE(y_)); \
        } else {                                                \
            r = (Scm_NumCmp(x_, y_) op 0);                      \
        }                                                       \
    } while (0)

#define NUM_CCMP(op, r)                                         \
    do {                                                        \
        ScmObj x_, y_ = VAL0;                                   \
        FETCH_OPERAND(x_);                                      \
        r = (SCM_FLONUM_VALUE(x_) op Scm_GetDouble(y_));        \
    } while (0)

/* We take advantage of GCC's `computed goto' feature
   (see gcc.info, "Labels as Values"). */
#ifdef __GNUC__
#define SWITCH(val) goto *dispatch_table[val];
#define CASE(insn)  SCM_CPP_CAT(LABEL_, insn) :
#define DEFAULT     LABEL_DEFAULT :
#define DISPATCH    /*empty*/
#define NEXT                                            \
    do {                                                \
        if (vm->attentionRequest) goto process_queue;   \
        FETCH_INSN(code);                               \
        goto *dispatch_table[SCM_VM_INSN_CODE(code)];   \
    } while (0)
#else /* !__GNUC__ */
#define SWITCH(val) switch (val)
#define CASE(insn)  case insn :
#define DISPATCH    dispatch:
#define NEXT        goto dispatch
#endif

/* NEXT1 is a shorthand form to set the number of values to 1.
   The numVals should be set to 1 when (1) the instruction yields
   a single value, and (2) it is at the tail position.  We don't
   have information for each insn that it is at tail position or
   not (yet), but we know that _PUSH insn won't come at the tail pos.
*/
#define NEXT1                                   \
    do {                                        \
        vm->numVals = 1;                        \
        NEXT;                                   \
    } while (0)

/* WNA - "Wrong Number of Arguments" handler.  The actual call is in vmcall.c.
   We handle the autocurrying magic here.

   PROC is the procedure object (guaranteed).
   NGIVEN is # of actual args on the VM stack.  The last several args may
   be folded in a list in APPLY_CALL context.  FOLDLEN holds the number of
   folded args.  In normal call context, FOLDLEN is -1.

   If the proc is curried, the VM stack state is ready to execute next op.
   Otherwise thie procedure won't return.
*/

static void wna(ScmVM *vm, ScmObj proc, int ngiven, int foldlen)
{
    int reqargs = SCM_PROCEDURE_REQUIRED(proc);
#if 0
    /* Disabled for now.  See proc.c (Scm_CurryProcedure) for the details. */
    if (SCM_PROCEDURE_CURRYING(proc) && ngiven < reqargs && ngiven > 0) {
        VAL0 = Scm_CurryProcedure(proc, ARGP, ngiven, foldlen);
        /*TODO: how should we count this path for profiling? */
    } else {
        Scm_Error("wrong number of arguments for %S (required %d, got %d)",
                  proc, reqargs, ngiven);
        /*NOTREACHED*/
    }
#else
    Scm_Error("wrong number of arguments for %S (required %d, got %d)",
              proc, reqargs, ngiven);
#endif
}

/*===================================================================
 * Main loop of VM
 */
static void run_loop()
{
    ScmVM *vm = theVM;
    ScmWord code = 0;
    
#ifdef __GNUC__
    static void *dispatch_table[256] = {
#define DEFINSN(insn, name, nargs, type)   && SCM_CPP_CAT(LABEL_, insn),
#include "vminsn.c"
#undef DEFINSN
    };
#endif /* __GNUC__ */

    /* The following code dumps the address of labels of each instruction
       handler.  Useful for tuning if used with machine instruction-level
       profiler. */
#if 0
    static int init = 0;
    if (!init) {
        int i;
        for (i=0; i<SCM_VM_NUM_INSNS; i++) {
            fprintf(stderr, "%3d %-15s %p (+%04x, %5d)\n",
                    i, Scm_VMInsnName(i),
                    dispatch_table[i],
                    (char*)dispatch_table[i] - (char*)run_loop,
                    (char*)dispatch_table[i] - (char*)run_loop);
        }
        init = TRUE;
    }
#endif

    for (;;) {
        DISPATCH;
        /*VM_DUMP("");*/
        if (vm->attentionRequest) goto process_queue;
        FETCH_INSN(code);
        SWITCH(SCM_VM_INSN_CODE(code)) {
#define VMLOOP
#include "vminsn.c"
#undef  VMLOOP
#ifndef __GNUC__
        default:
            Scm_Panic("Illegal vm instruction: %08x",
                      SCM_VM_INSN_CODE(code));
#endif
        }
      process_queue:
        CHECK_STACK(CONT_FRAME_SIZE);
        PUSH_CONT(PC);
        process_queued_requests(vm);
        POP_CONT();
        NEXT;
    }
}
/* End of run_loop */

/*==================================================================
 * Stack management
 */

/* We have 'fowarding pointer' for env and cont frames being moved.
   Forwarding pointers are resolved within these internal routines
   and should never leak out.

   Forwarded pointer is marked by the 'size' field be set -1.
   Env->up or Cont->prev field holds the relocated frame.

   Invariance: forwarded pointer only appear in stack.  We skip some
   IN_STACK_P check because of it. */

#define FORWARDED_ENV_P(e)  ((e)&&((e)->size == -1))
#define FORWARDED_ENV(e)    ((e)->up)

#define FORWARDED_CONT_P(c) ((c)&&((c)->size == -1))
#define FORWARDED_CONT(c)   ((c)->prev)

/* Performance note: As of 0.8.4_pre1, each get_env call spends about
   1us to 4us on P4 2GHz machine with several benchmark suites.  The
   average env frames to be saved is less than 3.  The ratio of the pass1
   (env frame save) and the pass 2 (cont pointer adjustment) is somewhere
   around 2:1 to 1:2.  Inlining SCM_NEW call didn't help.

   This is a considerable amount of time, since save_env may be called
   the order of 10^6 times.   I'm not sure I can optimize this routine
   further without a radical change in stack management code.

   Better strategy is to put an effort in the compiler to avoid closure 
   creation as much as possible.  */

/* Move the chain of env frames from the stack to the heap,
   replacing the in-stack frames for forwarding env frames.
   
   This routine just moves the env frames, but leaves pointers that
   point to moved frames intact (such pointers are found only in
   the in-stack contniuation frames, chained from vm->cont).
   It's the caller's responsibility to update those pointers. */
static inline ScmEnvFrame *save_env(ScmVM *vm, ScmEnvFrame *env_begin)
{
    ScmEnvFrame *e = env_begin, *prev = NULL, *next, *head = NULL, *saved;

    if (!IN_STACK_P((ScmObj*)e)) return e;

    do {
        long esize = (long)e->size, i;
        ScmObj *d, *s;

        if (esize < 0) {
            /* forwaded frame */
            if (prev) prev->up = FORWARDED_ENV(e);
            return head;
        }

        d = SCM_NEW2(ScmObj*, ENV_SIZE(esize) * sizeof(ScmObj));
        for (i=esize, s = (ScmObj*)e - esize; i>0; i--) {
            SCM_FLONUM_ENSURE_MEM(*s);
            *d++ = *s++;
        }
        *(ScmEnvFrame*)d = *e; /* copy env header */
        saved = (ScmEnvFrame*)d;
        if (prev) prev->up = saved;
        if (head == NULL) head = saved;
        next = e->up;
        e->up = prev = saved; /* forwarding pointer */
        e->size = -1;         /* indicates forwarded */
        e->info = SCM_FALSE;
        e = next;
    } while (IN_STACK_P((ScmObj*)e));
    return head;
}

/* Copy the continuation frames to the heap.
   We run two passes, first replacing cont frames with the forwarding
   cont frames, then updates the pointers to them.
   After save_cont, the only thing possibly left in the stack is the argument
   frame pointed by vm->argp.
 */
static void save_cont(ScmVM *vm)
{
    ScmContFrame *c = vm->cont, *prev = NULL, *tmp;
    ScmCStack *cstk;
    ScmEscapePoint *ep;
    ScmObj *s, *d;
    int i;

    /* Save the environment chain first. */
    vm->env = save_env(vm, vm->env);

    if (!IN_STACK_P((ScmObj*)c)) return;

    /* First pass */
    do {
        int size = (CONT_FRAME_SIZE + c->size) * sizeof(ScmObj);
        ScmContFrame *csave = SCM_NEW2(ScmContFrame*, size);

        /* update env ptr if necessary */
        if (FORWARDED_ENV_P(c->env)) {
            c->env = FORWARDED_ENV(c->env);
        } else if (IN_STACK_P((ScmObj*)c->env)) {
            c->env = save_env(vm, c->env);
        }

        /* copy cont frame */
        if (c->argp) {
            *csave = *c; /* copy the frame */
            if (c->size) {
                /* copy the args */
                s = c->argp;
                d = (ScmObj*)csave + CONT_FRAME_SIZE;
                for (i=c->size; i>0; i--) {
                    SCM_FLONUM_ENSURE_MEM(*s);
                    *d++ = *s++;
                }
            }
            csave->argp = ((ScmObj*)csave + CONT_FRAME_SIZE);
        } else {
            /* C continuation */
            s = (ScmObj*)c;
            d = (ScmObj*)csave;
            for (i=CONT_FRAME_SIZE + c->size; i>0; i--) {
                /* NB: C continuation frame contains opaque pointer,
                   so we shouldn't ENSURE_MEM. */
                *d++ = *s++;
            }
        }

        /* make the orig frame forwarded */
        if (prev) prev->prev = csave;
        prev = csave;
        
        tmp = c->prev;
        c->prev = csave;
        c->size = -1;
        c = tmp;
    } while (IN_STACK_P((ScmObj*)c));
    
    /* Second pass */
    if (FORWARDED_CONT_P(vm->cont)) {
        vm->cont = FORWARDED_CONT(vm->cont);
    }
    for (cstk = vm->cstack; cstk; cstk = cstk->prev) {
        if (FORWARDED_CONT_P(cstk->cont)) {
            cstk->cont = FORWARDED_CONT(cstk->cont);
        }
    }
    for (ep = vm->escapePoint; ep; ep = ep->prev) {
        if (FORWARDED_CONT_P(ep->cont)) {
            ep->cont = FORWARDED_CONT(ep->cont);
        }
    }
    for (ep = SCM_VM_FLOATING_EP(vm); ep; ep = ep->floating) {
        if (FORWARDED_CONT_P(ep->cont)) {
            ep->cont = FORWARDED_CONT(ep->cont);
        }
    }
}

static void save_stack(ScmVM *vm)
{
    ScmObj *p;
#if HAVE_GETTIMEOFDAY
    int stats = SCM_VM_RUNTIME_FLAG_IS_SET(vm, SCM_COLLECT_VM_STATS);
    struct timeval t0, t1;

    if (stats) {
        gettimeofday(&t0, NULL);
    }
#endif

    save_cont(vm);
    memmove(vm->stackBase, vm->argp,
            (vm->sp - (ScmObj*)vm->argp) * sizeof(ScmObj*));
    vm->sp -= (ScmObj*)vm->argp - vm->stackBase;
    vm->argp = vm->stackBase;
    /* Clear the stack.  This removes bogus pointers and accelerates GC */
    for (p = vm->sp; p < vm->stackEnd; p++) *p = NULL;

#if HAVE_GETTIMEOFDAY
    if (stats) {
        gettimeofday(&t1, NULL);
        vm->stat.sovCount++;
        vm->stat.sovTime +=
            (t1.tv_sec - t0.tv_sec)*1000000+(t1.tv_usec - t0.tv_usec);
    }
#endif
}

static ScmEnvFrame *get_env(ScmVM *vm)
{
    ScmEnvFrame *e;
    ScmContFrame *c;
    
    e = save_env(vm, vm->env);
    if (e != vm->env) {
        vm->env = e;
        for (c = vm->cont; IN_STACK_P((ScmObj*)c); c = c->prev) {
            if (FORWARDED_ENV_P(c->env)) {
                c->env = FORWARDED_ENV(c->env);
            }
        }
    }
    return e;
}

#if GAUCHE_FFX
/* Move all the FLONUM_REGs to heap and clear the fpstack.
   We cache small number of visited env frames to avoid duplicate scanning
   (if there are more env frames, linear search in the cache gets even
   more costly than duplicate scanning).
 */

#define ENV_CACHE_SIZE 32

#undef COUNT_FLUSH_FPSTACK

#ifdef COUNT_FLUSH_FPSTACK
static int flush_fpstack_count = 0;
static u_long flush_fpstack_time = 0;
static void print_flush_fpstack_count(void*z)
{
    fprintf(stderr, "fpstack count = %d  time = %ldus (avg %fus)\n",
            flush_fpstack_count, flush_fpstack_time,
            flush_fpstack_time/(double)flush_fpstack_count);
}
#endif

void Scm_VMFlushFPStack(ScmVM *vm)
{
    ScmEnvFrame *visited[ENV_CACHE_SIZE], *e;
    ScmContFrame *c;
    int visited_index = 0, i;
    ScmObj *p;
#ifdef COUNT_FLUSH_FPSTACK
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
#endif

    /* first, scan value registers and incomplete frames */
    SCM_FLONUM_ENSURE_MEM(VAL0);
    for (i=0; i<SCM_VM_MAX_VALUES; i++) {
        SCM_FLONUM_ENSURE_MEM(vm->vals[i]);
    }
    if (IN_STACK_P(ARGP)) {
        for (p = ARGP; p < SP; p++) SCM_FLONUM_ENSURE_MEM(*p);
    }

    /* scan the main environment chain */
    e = ENV;
    while (IN_STACK_P((ScmObj*)e)) {
        for (i = 0; i < visited_index; i++) {
            if (visited[i] == e) goto next;
        }
        if (visited_index < ENV_CACHE_SIZE) {
            visited[visited_index++] = e;
        }
        
        for (i = 0; i < e->size; i++) {
            p = &ENV_DATA(e, i);
            SCM_FLONUM_ENSURE_MEM(*p);
        }
      next:
        e = e->up;
    }

    /* scan the env chains grabbed by cont chain */
    c = CONT;
    while (IN_STACK_P((ScmObj*)c)) {
        e = c->env;
        while (IN_STACK_P((ScmObj*)e)) {
            for (i = 0; i < visited_index; i++) {
                if (visited[i] == e) goto next2;
            }
            if (visited_index < ENV_CACHE_SIZE) {
                visited[visited_index++] = e;
            }
            for (i = 0; i < e->size; i++) {
                p = &ENV_DATA(e, i);
                SCM_FLONUM_ENSURE_MEM(*p);
            }
          next2:
            e = e->up;
        }
        if (IN_STACK_P(c->argp) && c->size > 0) {
            p = c->argp;
            for (i=0; i<c->size; i++, p++) SCM_FLONUM_ENSURE_MEM(*p);
        }
        c = c->prev;
    }

    vm->fpsp = vm->fpstack;

#ifdef COUNT_FLUSH_FPSTACK
    flush_fpstack_count++;
    gettimeofday(&t1, NULL);
    flush_fpstack_time +=
        (t1.tv_sec - t0.tv_sec)*1000000+(t1.tv_usec - t0.tv_usec);
#endif
}
#undef ENV_CACHE_SIZE

#endif /*GAUCHE_FFX*/


/*==================================================================
 * Function application from C
 */

/* The Scm_VMApply family is supposed to be called in SUBR.  It doesn't really
   applies the function in it.  Instead, it modifies the VM state so that
   the specified function will be called immediately after this SUBR
   returns to the VM.   The return value of Scm_VMApply is just a PROC,
   but it should be returned as the return value of SUBR, which will be
   used by the VM.
   NB: we don't check proc is a procedure or not.  It can be a non-procedure
   object, because of the object-apply hook. */

/* Static VM instruction arrays.
   Scm_VMApplyN modifies VM's pc to point it. */

static ScmWord apply_calls[][2] = {
    { SCM_VM_INSN1(SCM_VM_TAIL_CALL, 0),
      SCM_VM_INSN(SCM_VM_RET) },
    { SCM_VM_INSN1(SCM_VM_TAIL_CALL, 1),
      SCM_VM_INSN(SCM_VM_RET) },
    { SCM_VM_INSN1(SCM_VM_TAIL_CALL, 2),
      SCM_VM_INSN(SCM_VM_RET) },
    { SCM_VM_INSN1(SCM_VM_TAIL_CALL, 3),
      SCM_VM_INSN(SCM_VM_RET) },
    { SCM_VM_INSN1(SCM_VM_TAIL_CALL, 4),
      SCM_VM_INSN(SCM_VM_RET) },
};

static ScmWord apply_callN[2] = {
    SCM_VM_INSN1(SCM_VM_TAIL_APPLY, 2),
    SCM_VM_INSN(SCM_VM_RET)
};

ScmObj Scm_VMApply(ScmObj proc, ScmObj args)
{
    int numargs = Scm_Length(args);
    int reqstack;
    ScmObj cp;
    ScmVM *vm = theVM;

    if (numargs < 0) Scm_Error("improper list not allowed: %S", args);
    SCM_ASSERT(TAIL_POS());
    SCM_ASSERT(ARGP == SP);
#if 0
    reqstack = ENV_SIZE(numargs) + 1;
    if (reqstack >= SCM_VM_STACK_SIZE) {
        /* there's no way we can accept that many arguments */
        Scm_Error("too many arguments (%d) to apply", numargs);
    }
    CHECK_STACK(reqstack);

    SCM_FOR_EACH(cp, args) {
        PUSH_ARG(SCM_CAR(cp));
    }
    if (numargs <= 4) {
        PC = apply_calls[numargs];
    } else {
        PC = SCM_NEW_ARRAY(ScmWord, 2);
        PC[0] = SCM_VM_INSN1(SCM_VM_TAIL_CALL, numargs);
        PC[1] = SCM_VM_INSN(SCM_VM_RET);
    }
    return proc;
#else
    reqstack = ENV_SIZE(1) + 1;
    CHECK_STACK(reqstack);
    PUSH_ARG(proc);
    PC = apply_callN;
    return Scm_CopyList(args);
#endif
}

/* shortcuts for common cases */
ScmObj Scm_VMApply0(ScmObj proc)
{
    theVM->pc = apply_calls[0];
    return proc;
}

ScmObj Scm_VMApply1(ScmObj proc, ScmObj arg)
{
    ScmVM *vm = theVM;
    CHECK_STACK(1);
    PUSH_ARG(arg);
    PC = apply_calls[1];
    return proc;
}

ScmObj Scm_VMApply2(ScmObj proc, ScmObj arg1, ScmObj arg2)
{
    ScmVM *vm = theVM;
    CHECK_STACK(2);
    PUSH_ARG(arg1);
    PUSH_ARG(arg2);
    PC = apply_calls[2];
    return proc;
}

ScmObj Scm_VMApply3(ScmObj proc, ScmObj arg1, ScmObj arg2, ScmObj arg3)
{
    ScmVM *vm = theVM;
    CHECK_STACK(3);
    PUSH_ARG(arg1);
    PUSH_ARG(arg2);
    PUSH_ARG(arg3);
    PC = apply_calls[3];
    return proc;
}

ScmObj Scm_VMApply4(ScmObj proc, ScmObj arg1, ScmObj arg2, ScmObj arg3, ScmObj arg4)
{
    ScmVM *vm = theVM;
    CHECK_STACK(4);
    PUSH_ARG(arg1);
    PUSH_ARG(arg2);
    PUSH_ARG(arg3);
    PUSH_ARG(arg4);
    PC = apply_calls[4];
    return proc;
}

static ScmObj eval_restore_env(ScmObj *args, int argc, void *data)
{
    theVM->module = SCM_MODULE(data);
    return SCM_UNDEFINED;
}

/* For now, we only supports a module as the evaluation environment */
ScmObj Scm_VMEval(ScmObj expr, ScmObj e)
{
    ScmObj v = SCM_NIL;
    int restore_module = SCM_MODULEP(e);
    ScmVM *vm = theVM;

    v = Scm_Compile(expr, e);
    if (SCM_VM_COMPILER_FLAG_IS_SET(theVM, SCM_COMPILE_SHOWRESULT)) {
        Scm_CompiledCodeDump(SCM_COMPILED_CODE(v));
    }

    vm->numVals = 1;
    if (restore_module) {
        /* if we swap the module, we need to make sure it is recovered
           after eval */
        ScmObj body = Scm_MakeClosure(v, get_env(vm));
        ScmObj before = Scm_MakeSubr(eval_restore_env, SCM_MODULE(e),
                                     0, 0, SCM_SYM_EVAL_BEFORE);
        ScmObj after = Scm_MakeSubr(eval_restore_env, (void*)vm->module,
                                    0, 0, SCM_SYM_EVAL_AFTER);
        return Scm_VMDynamicWind(before, body, after);
    } else {
        /* shortcut */
        SCM_ASSERT(SCM_COMPILED_CODE_P(v));
        vm->base = SCM_COMPILED_CODE(v);
        vm->pc = SCM_COMPILED_CODE(v)->code;
        SCM_PROF_COUNT_CALL(vm, v);
        return SCM_UNDEFINED;
    }
}

/* Arrange C function AFTER to be called after the procedure returns.
 * Usually followed by Scm_VMApply* function.
 */
void Scm_VMPushCC(ScmCContinuationProc *after,
                  void **data, int datasize)
{
    int i;
    ScmContFrame *cc;
    ScmObj *s;
    ScmVM *vm = theVM;

    CHECK_STACK(CONT_FRAME_SIZE+datasize);
    s = SP;
    cc = (ScmContFrame*)s;
    s += CONT_FRAME_SIZE;
    cc->prev = CONT;
    cc->argp = NULL;
    cc->size = datasize;
    cc->pc = (ScmWord*)after;
    cc->base = BASE;
    cc->env = ENV;
    for (i=0; i<datasize; i++) {
        *s++ = SCM_OBJ(data[i]);
    }
    CONT = cc;
    ARGP = SP = s;
}

/*-------------------------------------------------------------
 * User level eval and apply.
 *   When the C routine wants the Scheme code to return to it,
 *   instead of using C-continuation, the continuation
 *   "cross the border" of C-stack and Scheme-stack.  This
 *   border has peculiar characteristics.   Once the Scheme
 *   returns, continuations saved during the execution of the
 *   Scheme code becomes invalid.
 *
 *   At the implementation level, this boundary is kept in a
 *   structure ScmCStack.
 */

/* Border gate.  All the C->Scheme calls should go through here.
 *
 *   The current C stack information is saved in cstack.  The
 *   current VM stack information is saved (as a continuation
 *   frame pointer) in cstack.cont.
 */

static ScmObj user_eval_inner(ScmObj program, ScmWord *codevec)
{
    ScmCStack cstack;
    ScmVM * volatile vm = theVM;
    /* Save prev_pc, for the boundary continuation uses pc slot
       to mark the boundary. */
    ScmWord * volatile prev_pc = PC;

    /* Push extra continuation.  This continuation frame is a 'boundary
       frame' and marked by pc == &boundaryFrameMark.   VM loop knows
       it should return to C frame when it sees a boundary frame.
       A boundary frame also keeps the unfinished argument frame at
       the point when Scm_Eval or Scm_Apply is called. */
    CHECK_STACK(CONT_FRAME_SIZE);
    PUSH_CONT(&boundaryFrameMark);
    SCM_ASSERT(SCM_COMPILED_CODE_P(program));
    vm->base = SCM_COMPILED_CODE(program);
    if (codevec != NULL) {
        PC = codevec;
    } else {
        PC = vm->base->code;
        CHECK_STACK(vm->base->maxstack);
    }
    SCM_PROF_COUNT_CALL(vm, program);

    cstack.prev = vm->cstack;
    cstack.cont = vm->cont;
    vm->cstack = &cstack;
    Scm_GetSigmask(&cstack.mask);
    
  restart:
    vm->escapeReason = SCM_VM_ESCAPE_NONE;
    if (sigsetjmp(cstack.jbuf, FALSE) == 0) {
        run_loop();             /* VM loop */
        VAL0 = vm->val0;
        if (vm->cont == cstack.cont) {
            POP_CONT();
            PC = prev_pc;
        } else if (vm->cont == NULL) {
            /* we're finished with executing partial continuation.*/
            vm->cont = cstack.cont;
            POP_CONT();
            PC = prev_pc;
        } else {
            /* If we come here, we've been executing a ghost continuation.
               The C world the ghost should return no longer exists, so we
               raise an error. */
            Scm_Error("attempt to return from a ghost continuation.");
        }
    } else {
        Scm_SetSigmask(&cstack.mask);
        /* An escape situation happened. */
        if (vm->escapeReason == SCM_VM_ESCAPE_CONT) {
            ScmEscapePoint *ep = (ScmEscapePoint*)vm->escapeData[0];
            if (ep->cstack == vm->cstack) {
                ScmObj handlers = throw_cont_calculate_handlers(ep, vm);
                /* force popping continuation when restarted */
                vm->pc = PC_TO_RETURN;
                vm->val0 = throw_cont_body(handlers, ep, vm->escapeData[1]);
                goto restart;
            } else {
                SCM_ASSERT(vm->cstack && vm->cstack->prev);
                vm->cont = cstack.cont;
                VAL0 = vm->val0;
                POP_CONT();
                vm->cstack = vm->cstack->prev;
                siglongjmp(vm->cstack->jbuf, 1);
            }
        } else if (vm->escapeReason == SCM_VM_ESCAPE_ERROR) {
            ScmEscapePoint *ep = (ScmEscapePoint*)vm->escapeData[0];
            if (ep && ep->cstack == vm->cstack) {
                vm->cont = ep->cont;
                vm->pc = PC_TO_RETURN;
                goto restart;
            } else if (vm->cstack->prev == NULL) {
                /* This loop is the outermost C stack, and nobody will
                   capture the error.  Usually this means we're running
                   scripts.  We can safely exit here, for the dynamic
                   stack is already rewound. */
                exit(EX_SOFTWARE);
            } else {
                /* Jump again until C stack is recovered.  We sould pop
                   the extra continuation frame so that the VM stack
                   is consistent. */
                vm->cont = cstack.cont;
                VAL0 = vm->val0;
                POP_CONT();
                vm->cstack = vm->cstack->prev;
                siglongjmp(vm->cstack->jbuf, 1);
            }
        } else {
            Scm_Panic("invalid longjmp");
        }
        /* NOTREACHED */
    }
    vm->cstack = vm->cstack->prev;
    return vm->val0;
}

/* API for recursive call to VM.  Exceptions are not captured.
   Returns the primary result.  To retrieve the rest of results,
   you have to use Scm_VMGetResult etc. */

ScmObj Scm_EvalRec(ScmObj expr, ScmObj e)
{
    ScmObj v = SCM_NIL;
    v = Scm_Compile(expr, e);
    SCM_COMPILED_CODE(v)->name = SCM_SYM_INTERNAL_EVAL;
    if (SCM_VM_COMPILER_FLAG_IS_SET(theVM, SCM_COMPILE_SHOWRESULT)) {
        Scm_CompiledCodeDump(SCM_COMPILED_CODE(v));
    }
    return user_eval_inner(v, NULL);
}

/* NB: The ApplyRec family can be called in an inner loop (e.g. the display
   callback from GLUT.)  So we don't want to allocate at all.  We put
   a temporary code vector on C stack.  It is OK, since once
   user_eval_inner returns it would never be reused.   However, tools
   that want to keep a pointer to a code vector would need to be aware
   of this case. */
static ScmObj apply_rec(ScmVM *vm, ScmObj proc, int nargs)
{
    ScmObj program;
    ScmWord code[2];
    code[0] = SCM_WORD(SCM_VM_INSN1(SCM_VM_VALUES_APPLY, nargs));
    code[1] = SCM_WORD(SCM_VM_INSN(SCM_VM_RET));

    vm->val0 = proc;
    program = vm->base? SCM_OBJ(vm->base) : SCM_OBJ(&internal_apply_compiled_code);
    return user_eval_inner(program, code);
}

ScmObj Scm_ApplyRec(ScmObj proc, ScmObj args)
{
    int nargs = Scm_Length(args), i;
    ScmVM *vm = theVM;

    if (nargs < 0) {
        Scm_Error("improper list not allowed: %S", args);        
    }

    for (i=0; i<nargs; i++) {
        if (i == SCM_VM_MAX_VALUES-1) {
            vm->vals[i] = args;
            break;
        }
        vm->vals[i] = SCM_CAR(args);
        args = SCM_CDR(args);
    }
    return apply_rec(vm, proc, nargs);
}

ScmObj Scm_ApplyRec0(ScmObj proc)
{
    return apply_rec(theVM, proc, 0);
}

ScmObj Scm_ApplyRec1(ScmObj proc, ScmObj arg0)
{
    ScmVM *vm = theVM;
    vm->vals[0] = arg0;
    return apply_rec(vm, proc, 1);
}

ScmObj Scm_ApplyRec2(ScmObj proc, ScmObj arg0, ScmObj arg1)
{
    ScmVM *vm = theVM;
    vm->vals[0] = arg0;
    vm->vals[1] = arg1;
    return apply_rec(vm, proc, 2);
}

ScmObj Scm_ApplyRec3(ScmObj proc, ScmObj arg0, ScmObj arg1, ScmObj arg2)
{
    ScmVM *vm = theVM;
    vm->vals[0] = arg0;
    vm->vals[1] = arg1;
    vm->vals[2] = arg2;
    return apply_rec(vm, proc, 3);
}

ScmObj Scm_ApplyRec4(ScmObj proc, ScmObj arg0, ScmObj arg1, ScmObj arg2,
                     ScmObj arg3)
{
    ScmVM *vm = theVM;
    vm->vals[0] = arg0;
    vm->vals[1] = arg1;
    vm->vals[2] = arg2;
    vm->vals[3] = arg3;
    return apply_rec(vm, proc, 4);
}

ScmObj Scm_ApplyRec5(ScmObj proc, ScmObj arg0, ScmObj arg1, ScmObj arg2,
                     ScmObj arg3, ScmObj arg4)
{
    ScmVM *vm = theVM;
    vm->vals[0] = arg0;
    vm->vals[1] = arg1;
    vm->vals[2] = arg2;
    vm->vals[3] = arg3;
    vm->vals[4] = arg4;
    return apply_rec(vm, proc, 5);
}


/*
 * Safe version of user-level Eval, Apply and Load.
 * Exceptions are caught and stored in ScmEvalPacket.
 */

enum {
    SAFE_EVAL,
    SAFE_EVAL_CSTRING,
    SAFE_APPLY
};

struct eval_packet_rec {
    ScmObj env;
    int kind;
    ScmObj arg0;      /* form (EVAL), proc (APPLY) */
    ScmObj args;      /* args (APPLY) */
    const char *cstr; /* cstring (EVAL_CSTRING) */
    ScmObj exception;
};

static ScmObj safe_eval_handler(ScmObj *args,
                                int nargs, void *data)
{
    SCM_ASSERT(nargs == 1);
    ((struct eval_packet_rec *)data)->exception = args[0];
    return SCM_UNDEFINED;
}

static ScmObj safe_eval_thunk(ScmObj *args, int nargs, void *data)
{
    struct eval_packet_rec *epak = (struct eval_packet_rec*)data;

    switch (epak->kind) {
    case SAFE_EVAL_CSTRING:
        return Scm_VMEval(Scm_ReadFromCString(epak->cstr), epak->env);
    case SAFE_EVAL:
        return Scm_VMEval(epak->arg0, epak->env);
    case SAFE_APPLY:
        return Scm_VMApply(epak->arg0, epak->args);
    default:
        Scm_Panic("safe_eval_subr: bad kind");
        return SCM_UNBOUND;     /* dummy */
    }
}

static ScmObj safe_eval_int(ScmObj *args, int nargs, void *data)
{
    ScmObj thunk, handler;
    
    thunk   = Scm_MakeSubr(safe_eval_thunk, data, 0, 0, SCM_FALSE);
    handler = Scm_MakeSubr(safe_eval_handler, data, 1, 0, SCM_FALSE);
    return Scm_VMWithErrorHandler(handler, thunk);
}

static int safe_eval_wrap(int kind, ScmObj arg0, ScmObj args,
                          const char *cstr, ScmObj env,
                          ScmEvalPacket *result)
{
    struct eval_packet_rec epak;
    ScmObj proc, r;
    ScmVM *vm = theVM;
    int i;

    epak.env  = env;
    epak.kind = kind;
    epak.arg0 = arg0;
    epak.args = args;
    epak.cstr = cstr;
    epak.exception = SCM_UNBOUND;

    proc = Scm_MakeSubr(safe_eval_int, &epak, 0, 0, SCM_FALSE);
    r = Scm_ApplyRec(proc, SCM_NIL);

    if (SCM_UNBOUNDP(epak.exception)) {
        /* normal termination */
        if (result) {
            result->numResults = vm->numVals;
            result->results[0] = r;
            for (i=1; i<vm->numVals; i++) {
                result->results[i] = vm->vals[i-1];
            }
            result->exception = SCM_FALSE;
        }
        return vm->numVals;
    } else {
        /* abnormal termination */
        if (result) {
            result->numResults = 0;
            result->exception = epak.exception;
        }
        return -1;
    }
}

int Scm_Eval(ScmObj form, ScmObj env, ScmEvalPacket *packet)
{
    return safe_eval_wrap(SAFE_EVAL, form, SCM_FALSE, NULL, env, packet);
}

int Scm_EvalCString(const char *expr, ScmObj env, ScmEvalPacket *packet)
{
    return safe_eval_wrap(SAFE_EVAL_CSTRING, SCM_FALSE, SCM_FALSE,
                          expr, env, packet);
}

int Scm_Apply(ScmObj proc, ScmObj args, ScmEvalPacket *packet)
{
    return safe_eval_wrap(SAFE_APPLY, proc, args, NULL, SCM_FALSE, packet);
}


/*=================================================================
 * Dynamic handlers
 */

static ScmCContinuationProc dynwind_before_cc;
static ScmCContinuationProc dynwind_body_cc;
static ScmCContinuationProc dynwind_after_cc;

ScmObj Scm_VMDynamicWind(ScmObj before, ScmObj body, ScmObj after)
{
    void *data[3];

    /* NB: we don't check types of arguments, since we allow object-apply
       hooks can be used for them. */
    data[0] = (void*)before;
    data[1] = (void*)body;
    data[2] = (void*)after;

    Scm_VMPushCC(dynwind_before_cc, data, 3);
    return Scm_VMApply0(before);
}

static ScmObj dynwind_before_cc(ScmObj result, void **data)
{
    ScmObj before  = SCM_OBJ(data[0]);
    ScmObj body = SCM_OBJ(data[1]);
    ScmObj after = SCM_OBJ(data[2]);
    ScmObj prev;
    void *d[2];
    ScmVM *vm = theVM;

    prev = vm->handlers;
    d[0] = (void*)after;
    d[1] = (void*)prev;
    vm->handlers = Scm_Cons(Scm_Cons(before, after), prev);
    Scm_VMPushCC(dynwind_body_cc, d, 2);
    return Scm_VMApply0(body);
}

static ScmObj dynwind_body_cc(ScmObj result, void **data)
{
    ScmObj after = SCM_OBJ(data[0]);
    ScmObj prev  = SCM_OBJ(data[1]);
    void *d[3];
    ScmVM *vm = theVM;

    vm->handlers = prev;
    d[0] = (void*)result;
    d[1] = (void*)(intptr_t)vm->numVals;
    if (vm->numVals > 1) {
        ScmObj *array = SCM_NEW_ARRAY(ScmObj, (vm->numVals-1));
        memcpy(array, vm->vals, sizeof(ScmObj)*(vm->numVals-1));
        d[2] = (void*)array;
    }
    Scm_VMPushCC(dynwind_after_cc, d, 3);
    return Scm_VMApply0(after);
}

static ScmObj dynwind_after_cc(ScmObj result, void **data)
{
    ScmObj val0 = SCM_OBJ(data[0]);
    int nvals = (int)(intptr_t)data[1];
    ScmVM *vm = theVM;
    
    vm->numVals = nvals;
    if (nvals > 1) {
        SCM_ASSERT(nvals <= SCM_VM_MAX_VALUES);
        memcpy(vm->vals, data[2], sizeof(ScmObj)*(nvals-1));
    }
    return val0;
}

/* C-friendly wrapper */
ScmObj Scm_VMDynamicWindC(ScmSubrProc *before,
                          ScmSubrProc *body,
                          ScmSubrProc *after,
                          void *data)
{
    ScmObj beforeproc, bodyproc, afterproc;
    beforeproc =
        before ? Scm_MakeSubr(before, data, 0, 0, SCM_FALSE) : Scm_NullProc();
    afterproc =
        after ? Scm_MakeSubr(after, data, 0, 0, SCM_FALSE) : Scm_NullProc();
    bodyproc =
        body ? Scm_MakeSubr(body, data, 0, 0, SCM_FALSE) : Scm_NullProc();
    
    return Scm_VMDynamicWind(beforeproc, bodyproc, afterproc);
}


/*=================================================================
 * Exception handling
 */

/* Conceptually, exception handling is nothing more than a particular
 * combination of dynamic-wind and call/cc.   Gauche implements a parts
 * of it in C so that it will be efficient and safer to use.
 *
 * The most basic layer consists of these two functions:
 *
 *  with-exception-handler
 *  raise
 *
 * There is a slight problem, though.  These two functions are defined
 * both in srfi-18 (multithreads) and srfi-34 (exception handling), and
 * two disagrees in the semantics of raise.
 *
 * Srfi-18 requires an exception handler to be called with the same dynamic
 * environment as the one of the primitive that raises the exception.
 * That means when an exception handler is running, the current
 * exception handler is the running handler itself.  Naturally, calling
 * raise unconditionally within the exception handler causes infinite loop.
 *
 * Srfi-34 says that an exception handler is called with the same dynamic
 * envionment where the exception is raised, _except_ that the current
 * exception handler is "popped", i.e. when an exception handler is running,
 * the current exception handler is the "outer" or "old" one.  Calling
 * raise within an exception handler passes the control to the outer
 * exception handler.
 *
 * At this point I haven't decided which model Gauche should support natively.
 * The current implementation predates srfi-34 and roughly follows srfi-18.
 * It appears that srfi-18's mechanism is more "primitive" or "lightweight"
 * than srfi-34's, so it's likely that Gauche will continue to support
 * srfi-18 model natively, and maybe provides srfi-34's interface by an
 * additional module.
 *
 * The following is a model of the current implementation, sans the messy
 * part of handling C stacks.
 * Suppose a system variable %xh keeps the list of exception handlers.
 *
 *  (define (current-exception-handler) (car %xh))
 *
 *  (define (raise exn)
 *    (receive r ((car %xh) exn)
 *      (when (uncontinuable-exception? exn)
 *        (set! %xh (cdr %xh))
 *        (error "returned from uncontinuable exception"))
 *      (apply values r)))
 *
 *  (define (with-exception-handler handler thunk)
 *    (let ((prev %xh))
 *      (dynamic-wind
 *        (lambda () (set! %xh (cons handler)))
 *        thunk
 *        (lambda () (set! %xh prev)))))
 *
 * In C level, the chain of the handlers are represented in the chain
 * of ScmEscapePoints.
 *
 * Note that this model assumes an exception handler returns unless it
 * explictly invokes continuation captured elsewhere.   In reality,
 * "error" exceptions are not supposed to return (hence it is checked
 * in raise).  Gauche provides another useful exception handling
 * constructs that automates such continuation capturing.  It can be
 * explained by the following code.
 *
 * (define (with-error-handler handler thunk)
 *   (call/cc
 *     (lambda (cont)
 *       (let ((prev-handler (current-exception-handler)))
 *         (with-exception-handler
 *           (lambda (exn)
 *             (if (error? exn)
 *                 (call-with-values (handler exn) cont)
 *                 (prev-handler exn)))
 *           thunk)))))
 *
 * In the actual implementation,
 *
 *  - No "real" continuation procedure is created, but a lightweight
 *    mechanism is used.  The lightweight mechanism is similar to
 *    "one-shot" callback (call/1cc in Chez Scheme).
 *  - The error handler chain is kept in vm->escapePoint
 *  - There are messy lonjmp/setjmp stuff involved to keep C stack sane.
 */

/*
 * Default exception handler
 *  This is what we have as the system default, and also
 *  what with-error-handler installs as an exception handler.
 */

void Scm_VMDefaultExceptionHandler(ScmObj e)
{
    ScmVM *vm = theVM;
    ScmEscapePoint *ep = vm->escapePoint;
    ScmObj hp;

    if (ep) {
        /* There's an escape point defined by with-error-handler. */
        ScmObj target, current;
        ScmObj result = SCM_FALSE, rvals[SCM_VM_MAX_VALUES];
        int numVals = 0, i;

        /* To conform SRFI-34, the error handler (clauses in 'guard' form)
           should be executed with the same continuation and dynamic
           environment of the guard form itself.  That means the dynamic
           handlers should be rewound before we invoke the guard clause.

           If an error is raised within the dynamic handlers, it will be
           captured by the same error handler. */
        if (ep->rewindBefore) {
            target = ep->handlers;
            current = vm->handlers;
            for (hp=current; SCM_PAIRP(hp) && (hp!=target); hp=SCM_CDR(hp)) {
                ScmObj proc = SCM_CDAR(hp);
                vm->handlers = SCM_CDR(hp);
                Scm_ApplyRec(proc, SCM_NIL);
            }
        }

        /* Call the error handler and save the results.
           NB: before calling the error handler, we need to pop
           vm->escapePoint, so that the error occurred during
           the error handler should be dealt with the upstream error
           handler.  We keep ep in vm->escapePoint->floating, so that
           ep->cont can be updated when stack overflow occurs during the
           error handler.  See also the description of ScmEscapePoint in
           gauche/vm.h. */
        vm->escapePoint = ep->prev;
        SCM_VM_FLOATING_EP_SET(vm, ep);

        SCM_UNWIND_PROTECT {
            result = Scm_ApplyRec(ep->ehandler, SCM_LIST1(e));
            if ((numVals = vm->numVals) > 1) {
                for (i=0; i<numVals-1; i++) rvals[i] = vm->vals[i];
            }
            if (!ep->rewindBefore) {
                target = ep->handlers;
                current = vm->handlers;
                for (hp=current; SCM_PAIRP(hp) && (hp!=target); hp=SCM_CDR(hp)) {
                    ScmObj proc = SCM_CDAR(hp);
                    vm->handlers = SCM_CDR(hp);
                    Scm_ApplyRec(proc, SCM_NIL);
                }
            }
        }
        SCM_WHEN_ERROR {
            /* make sure the floating pointer is reset when an error is
               signalled during handlers */
            SCM_VM_FLOATING_EP_SET(vm, ep->floating);
            SCM_NEXT_HANDLER;
        }
        SCM_END_PROTECT;
        
        /* Install the continuation */
        for (i=0; i<numVals; i++) vm->vals[i] = rvals[i];
        vm->numVals = numVals;
        vm->val0 = result;
        vm->cont = ep->cont;
        SCM_VM_FLOATING_EP_SET(vm, ep->floating);
        if (ep->errorReporting) {
            SCM_VM_RUNTIME_FLAG_SET(vm, SCM_ERROR_BEING_REPORTED);
        }
    } else {
        /* We don't have an active error handler, so this is the fallback
           behavior.  Reports the error and rewind dynamic handlers and
           C stacks. */
        Scm_ReportError(e);
        /* unwind the dynamic handlers */
        SCM_FOR_EACH(hp, vm->handlers) {
            ScmObj proc = SCM_CDAR(hp);
            vm->handlers = SCM_CDR(hp);
            Scm_ApplyRec(proc, SCM_NIL);
        }
    }

    if (vm->cstack) {
        vm->escapeReason = SCM_VM_ESCAPE_ERROR;
        vm->escapeData[0] = ep;
        vm->escapeData[1] = e;
        siglongjmp(vm->cstack->jbuf, 1);
    } else {
        exit(EX_SOFTWARE);
    }
}

static ScmObj default_exception_handler_body(ScmObj *argv,
                                             int argc, void *data)
{
    SCM_ASSERT(argc == 1);
    Scm_VMDefaultExceptionHandler(argv[0]);
    return SCM_UNDEFINED;       /*NOTREACHED*/
}

static SCM_DEFINE_STRING_CONST(default_exception_handler_name,
                               "default-exception-handler",
                               25, 25); /* strlen("default-exception-handler") */
static SCM_DEFINE_SUBR(default_exception_handler_rec, 1, 0,
                       SCM_OBJ(&default_exception_handler_name),
                       default_exception_handler_body, NULL, NULL);

/*
 * Entry point of throwing exception.
 *
 *  This function can be called from Scheme function raise,
 *  or C-function Scm_Error families and signal handler. 
 *  So there may be a raw C code in the continuation of this C call.
 *  Thus we can't use Scm_VMApply to call the user-defined exception
 *  handler.
 *  Note that this function may return.
 */
ScmObj Scm_VMThrowException(ScmVM *vm, ScmObj exception)
{
    ScmEscapePoint *ep = vm->escapePoint;

    SCM_VM_RUNTIME_FLAG_CLEAR(vm, SCM_ERROR_BEING_HANDLED);

    if (vm->exceptionHandler != DEFAULT_EXCEPTION_HANDLER) {
        vm->val0 = Scm_ApplyRec(vm->exceptionHandler, SCM_LIST1(exception));
        if (SCM_SERIOUS_CONDITION_P(exception)) {
            /* the user-installed exception handler returned while it
               shouldn't.  In order to prevent infinite loop, we should
               pop the erroneous handler.  For now, we just reset
               the current exception handler. */
            vm->exceptionHandler = DEFAULT_EXCEPTION_HANDLER;
            Scm_Error("user-defined exception handler returned on non-continuable exception %S", exception);
        }
        return vm->val0;
    } else if (!SCM_SERIOUS_CONDITION_P(exception)) {
        /* The system's default handler does't care about
           continuable exception.  See if there's a user-defined
           exception handler in the chain.  */
        for (; ep; ep = ep->prev) {
            if (ep->xhandler != DEFAULT_EXCEPTION_HANDLER) {
                return Scm_ApplyRec(ep->xhandler, SCM_LIST1(exception));
            }
        }
    }
    Scm_VMDefaultExceptionHandler(exception);
    /* this never returns */
    return SCM_UNDEFINED;       /* dummy */
}

/*
 * with-error-handler
 */
static ScmObj install_ehandler(ScmObj *args, int nargs, void *data)
{
    ScmEscapePoint *ep = (ScmEscapePoint*)data;
    ScmVM *vm = theVM;
    vm->exceptionHandler = DEFAULT_EXCEPTION_HANDLER;
    vm->escapePoint = ep;
    SCM_VM_RUNTIME_FLAG_CLEAR(vm, SCM_ERROR_BEING_REPORTED);
    return SCM_UNDEFINED;
}

static ScmObj discard_ehandler(ScmObj *args, int nargs, void *data)
{
    ScmEscapePoint *ep = (ScmEscapePoint *)data;
    ScmVM *vm = theVM;
    vm->escapePoint = ep->prev;
    vm->exceptionHandler = ep->xhandler;
    if (ep->errorReporting) {
        SCM_VM_RUNTIME_FLAG_SET(vm, SCM_ERROR_BEING_REPORTED);
    }
    return SCM_UNDEFINED;
}

static ScmObj with_error_handler(ScmVM *vm, ScmObj handler,
                                 ScmObj thunk, int rewindBefore)
{
    ScmEscapePoint *ep = SCM_NEW(ScmEscapePoint);
    ScmObj before, after;

    /* NB: we can save pointer to the stack area (vm->cont) to ep->cont,
     * since such ep is always accessible via vm->escapePoint chain and
     * ep->cont is redirected whenever the continuation is captured while
     * ep is valid.
     */
    ep->prev = vm->escapePoint;
    ep->floating = SCM_VM_FLOATING_EP(vm);
    ep->ehandler = handler;
    ep->handlers = vm->handlers;
    ep->cstack = vm->cstack;
    ep->xhandler = vm->exceptionHandler;
    ep->cont = vm->cont;
    ep->errorReporting =
        SCM_VM_RUNTIME_FLAG_IS_SET(vm, SCM_ERROR_BEING_REPORTED);
    ep->rewindBefore = rewindBefore;
    
    vm->escapePoint = ep; /* This will be done in install_ehandler, but
                             make sure ep is visible from save_cont
                             to redirect ep->cont */
    before = Scm_MakeSubr(install_ehandler, ep, 0, 0, SCM_FALSE);
    after  = Scm_MakeSubr(discard_ehandler, ep, 0, 0, SCM_FALSE);
    return Scm_VMDynamicWind(before, thunk, after);
}

ScmObj Scm_VMWithErrorHandler(ScmObj handler, ScmObj thunk)
{
    return with_error_handler(theVM, handler, thunk, FALSE);
}

ScmObj Scm_VMWithGuardHandler(ScmObj handler, ScmObj thunk)
{
    return with_error_handler(theVM, handler, thunk, TRUE);
}


/* 
 * with-exception-handler
 *
 *   This primitive gives the programmer whole responsibility of
 *   dealing with exceptions.
 */

static ScmObj install_xhandler(ScmObj *args, int nargs, void *data)
{
    theVM->exceptionHandler = SCM_OBJ(data);
    return SCM_UNDEFINED;
}

ScmObj Scm_VMWithExceptionHandler(ScmObj handler, ScmObj thunk)
{
    ScmObj current = theVM->exceptionHandler;
    ScmObj before = Scm_MakeSubr(install_xhandler, handler, 0, 0, SCM_FALSE);
    ScmObj after  = Scm_MakeSubr(install_xhandler, current, 0, 0, SCM_FALSE);
    return Scm_VMDynamicWind(before, thunk, after);
}

/*==============================================================
 * Call With Current Continuation
 */

/* Figure out which before and after thunk should be called.
   Returns a list of (<handler> . <handler-chain>), where the <handler-chain>
   is the state of handlers on which <handler> should be executed. */
static ScmObj throw_cont_calculate_handlers(ScmEscapePoint *ep, /*target*/
                                            ScmVM *vm)
{
    ScmObj target  = Scm_Reverse(ep->handlers);
    ScmObj current = vm->handlers;
    ScmObj h = SCM_NIL, t = SCM_NIL, p;

    SCM_FOR_EACH(p, current) {
        SCM_ASSERT(SCM_PAIRP(SCM_CAR(p)));
        if (!SCM_FALSEP(Scm_Memq(SCM_CAR(p), target))) break;
        /* push 'after' handlers to be called */
        SCM_APPEND1(h, t, Scm_Cons(SCM_CDAR(p), SCM_CDR(p)));
    }
    SCM_FOR_EACH(p, target) {
        ScmObj chain;
        SCM_ASSERT(SCM_PAIRP(SCM_CAR(p)));
        if (!SCM_FALSEP(Scm_Memq(SCM_CAR(p), current))) continue;
        chain = Scm_Memq(SCM_CAR(p), ep->handlers);
        SCM_ASSERT(SCM_PAIRP(chain));
        /* push 'before' handlers to be called */
        SCM_APPEND1(h, t, Scm_Cons(SCM_CAAR(p), SCM_CDR(chain)));
    }
    return h;
}

static ScmObj throw_cont_cc(ScmObj, void **);

static ScmObj throw_cont_body(ScmObj handlers,    /* after/before thunks
                                                     to be called */
                              ScmEscapePoint *ep, /* target continuation */
                              ScmObj args)        /* args to pass to the
                                                     target continuation */
{
    void *data[3];
    int nargs, i;
    ScmObj ap;
    ScmVM *vm = theVM;

    /*
     * first, check to see if we need to evaluate dynamic handlers.
     */
    if (SCM_PAIRP(handlers)) {
        ScmObj handler, chain;
        SCM_ASSERT(SCM_PAIRP(SCM_CAR(handlers)));
        handler = SCM_CAAR(handlers);
        chain   = SCM_CDAR(handlers);
        
        data[0] = (void*)SCM_CDR(handlers);
        data[1] = (void*)ep;
        data[2] = (void*)args;
        Scm_VMPushCC(throw_cont_cc, data, 3);
        vm->handlers = chain;
        return Scm_VMApply0(handler);
    }

    /*
     * If the target continuation is a full continuation, we can abandon
     * the current continuation.  However, if the target continuation is
     * partial, we must return to the current continuation after executing
     * the partial continuation.  The returning part is handled by
     * user_level_inner, but we have to make sure that our current continuation
     * won't be overwritten by execution of the partial continuation.
     */
    if (ep->cstack == NULL) save_cont(vm);
    
    /*
     * now, install the target continuation
     */
    vm->pc = PC_TO_RETURN;
    vm->cont = ep->cont;
    vm->handlers = ep->handlers;

    nargs = Scm_Length(args);
    if (nargs == 1) {
        return SCM_CAR(args);
    } else if (nargs < 1) {
        return SCM_UNDEFINED;
    } else if (nargs >= SCM_VM_MAX_VALUES) {
        Scm_Error("too many values passed to the continuation");
    }

    for (i=0, ap=SCM_CDR(args); SCM_PAIRP(ap); i++, ap=SCM_CDR(ap)) {
        vm->vals[i] = SCM_CAR(ap);
    }
    vm->numVals = nargs;
    return SCM_CAR(args);
}

static ScmObj throw_cont_cc(ScmObj result, void **data)
{
    ScmObj handlers = SCM_OBJ(data[0]);
    ScmEscapePoint *ep = (ScmEscapePoint *)data[1];
    ScmObj args = SCM_OBJ(data[2]);
    return throw_cont_body(handlers, ep, args);
}

/* Body of the continuation SUBR */
static ScmObj throw_continuation(ScmObj *argframe, int nargs, void *data)
{
    ScmEscapePoint *ep = (ScmEscapePoint*)data;
    ScmObj args = argframe[0];
    ScmVM *vm = theVM;
    ScmObj handlers_to_call;

    if (ep->cstack && vm->cstack != ep->cstack) {
        ScmCStack *cs;
        for (cs = vm->cstack; cs; cs = cs->prev) {
            if (ep->cstack == cs) break;
        }

        /* If the continuation captured below the current C stack, we rewind
           to the captured stack first.  If not, the continuation is 'ghost'.
           We execute the scheme portion of the continuation on the current
           C stack (no rewinding), but we'll catch it if it tries to return
           to the C world.   See user_eval_inner().  */
        if (cs != NULL) {
            vm->escapeReason = SCM_VM_ESCAPE_CONT;
            vm->escapeData[0] = ep;
            vm->escapeData[1] = args;
            siglongjmp(vm->cstack->jbuf, 1);
        }
    }

    handlers_to_call = throw_cont_calculate_handlers(ep, vm);
    return throw_cont_body(handlers_to_call, ep, args);
}

ScmObj Scm_VMCallCC(ScmObj proc)
{
    ScmObj contproc;
    ScmEscapePoint *ep;
    ScmVM *vm = theVM;

    save_cont(vm);
    ep = SCM_NEW(ScmEscapePoint);
    ep->prev = NULL;
    ep->ehandler = SCM_FALSE;
    ep->cont = vm->cont;
    ep->handlers = vm->handlers;
    ep->cstack = vm->cstack;

    contproc = Scm_MakeSubr(throw_continuation, ep, 0, 1,
                            SCM_MAKE_STR("continuation"));
    return Scm_VMApply1(proc, contproc);
}

/* call with partial continuation.  this corresponds to the 'shift' operator
   in shift/reset controls (Gasbichler&Sperber, "Final Shift for Call/cc",
   ICFP02.)   Note that we treat the boundary frame as the bottom of
   partial continuation. */
ScmObj Scm_VMCallPC(ScmObj proc)
{
    ScmObj contproc;
    ScmEscapePoint *ep;
    ScmContFrame *c, *cp;
    ScmVM *vm = theVM;
    
    /* save the continuation.  we only need to save the portion above the
       latest boundary frame (+environmentns pointed from them), but for now,
       we save everything to make things easier.  If we want to squeeze
       performance we'll optimize it later. */
    save_cont(vm);
    
    /* find the latest boundary frame */
    for (c = vm->cont, cp = NULL;
         c && !BOUNDARY_FRAME_P(c);
         cp = c, c = c->prev)
        /*empty*/;

    if (cp != NULL) cp->prev = NULL; /* cut the dynamic chain */

    ep = SCM_NEW(ScmEscapePoint);
    ep->prev = NULL;
    ep->ehandler = SCM_FALSE;
    ep->cont = vm->cont;
    ep->handlers = vm->handlers;
    ep->cstack = NULL; /* so that the partial continuation can be run
                          on any cstack state. */
    contproc = Scm_MakeSubr(throw_continuation, ep, 0, 1,
                            SCM_MAKE_STR("partial continuation"));
    /* Remove the saved continuation chain.
       NB: c can be NULL if we've been executing a partial continuation.
       It's ok, for a continuation pointed by cstack will be restored
       in user_eval_inner. */
    vm->cont = c;
    return Scm_VMApply1(proc, contproc);
}

/*==============================================================
 * Unwind protect API
 */

long Scm_VMUnwindProtect(ScmVM *vm, ScmCStack *cstack)
{
    cstack->prev = vm->cstack;
    cstack->cont = NULL;
    vm->cstack = cstack;
    return sigsetjmp(cstack->jbuf, FALSE);
}

void Scm_VMNextHandler(ScmVM *vm)
{
    if (vm->cstack->prev) {
        vm->cstack = vm->cstack->prev;
        siglongjmp(vm->cstack->jbuf, 1);
    } else {
        Scm_Exit(1);
    }
}

void Scm_VMRewindProtect(ScmVM *vm)
{
    SCM_ASSERT(vm->cstack);
    vm->cstack = vm->cstack->prev;
}

/*==============================================================
 * Values
 */

ScmObj Scm_VMValues(ScmVM *vm, ScmObj args)
{
    ScmObj cp;
    int nvals;
    
    if (!SCM_PAIRP(args)) {
        vm->numVals = 0;
        return SCM_UNDEFINED;
    }
    nvals = 1;
    SCM_FOR_EACH(cp, SCM_CDR(args)) {
        vm->vals[nvals-1] = SCM_CAR(cp);
        if (nvals++ >= SCM_VM_MAX_VALUES) {
            Scm_Error("too many values: %S", args);
        }
    }
    vm->numVals = nvals;
    return SCM_CAR(args);
}

ScmObj Scm_Values(ScmObj args)
{
    return Scm_VMValues(theVM, args);
}

ScmObj Scm_VMValues2(ScmVM *vm, ScmObj val0, ScmObj val1)
{
    vm->numVals = 2;
    vm->vals[0] = val1;
    return val0;
}


ScmObj Scm_Values2(ScmObj val0, ScmObj val1)
{
    return Scm_VMValues2(theVM, val0, val1);
}

ScmObj Scm_VMValues3(ScmVM *vm, ScmObj val0, ScmObj val1, ScmObj val2)
{
    vm->numVals = 3;
    vm->vals[0] = val1;
    vm->vals[1] = val2;
    return val0;
}

ScmObj Scm_Values3(ScmObj val0, ScmObj val1, ScmObj val2)
{
    return Scm_VMValues3(theVM, val0, val1, val2);
}


ScmObj Scm_VMValues4(ScmVM *vm, ScmObj val0, ScmObj val1,
                     ScmObj val2, ScmObj val3)
{
    vm->numVals = 4;
    vm->vals[0] = val1;
    vm->vals[1] = val2;
    vm->vals[2] = val3;
    return val0;
}

ScmObj Scm_Values4(ScmObj val0, ScmObj val1, ScmObj val2, ScmObj val3)
{
    return Scm_VMValues4(theVM, val0, val1, val2, val3);
}

ScmObj Scm_VMValues5(ScmVM *vm, ScmObj val0, ScmObj val1,
                     ScmObj val2, ScmObj val3, ScmObj val4)
{
    vm->numVals = 5;
    vm->vals[0] = val1;
    vm->vals[1] = val2;
    vm->vals[2] = val3;
    vm->vals[3] = val4;
    return val0;
}

ScmObj Scm_Values5(ScmObj val0, ScmObj val1,
                   ScmObj val2, ScmObj val3, ScmObj val4)
{
    return Scm_VMValues5(theVM, val0, val1, val2, val3, val4);
}


/*==================================================================
 * Queued handler processing.
 */

/* Signal handlers and finalizers are queued in VM when they
 * are requested, and processed when VM is in consistent state.
 * process_queued_requests() are called near the beginning of
 * VM loop, when the VM checks if there's any queued request.
 *
 * When this procedure is called, VM is in middle of any two
 * VM instructions.  We need to make sure the handlers won't
 * disturb the VM state.
 *
 * Conceptually, this procedure inserts handler invocations before
 * the current continuation.
 */

static ScmObj process_queued_requests_cc(ScmObj result, void **data)
{
    /* restore the saved continuation of normal execution flow of VM */
    int i;
    ScmObj cp;
    ScmVM *vm = theVM;
    
    vm->numVals = (int)(intptr_t)data[0];
    vm->val0 = data[1];
    if (vm->numVals > 1) {
        cp = SCM_OBJ(data[2]);
        for (i=0; i<vm->numVals-1; i++) {
            vm->vals[i] = SCM_CAR(cp);
            cp = SCM_CDR(cp);
        }
    }
    return vm->val0;
}

static void process_queued_requests(ScmVM *vm)
{
    void *data[3];

    /* preserve the current continuation */
    data[0] = (void*)(intptr_t)vm->numVals;
    data[1] = vm->val0;
    if (vm->numVals > 1) {
        int i;
        ScmObj h = SCM_NIL, t = SCM_NIL;

        for (i=0; i<vm->numVals-1; i++) {
            SCM_APPEND1(h, t, vm->vals[i]);
        }
        data[2] = h;
    } else {
        data[2] = NULL;
    }
    Scm_VMPushCC(process_queued_requests_cc, data, 3);

    /* NB: it is safe to turn off attentionRequest here; if attentionRequest
       is turned on again after this and before SigCheck() or FinalizerRun(),
       the new request is processed within these procedures; we'll enter
       process_queued_requests() again without anything to process, but
       that's an acceptable overhead. */
    vm->attentionRequest = FALSE;

    /* Process queued stuff.  Currently they call VM recursively,
       but we'd better to arrange them to be processed in the same
       VM level. */
    if (vm->signalPending)   Scm_SigCheck(vm);
    if (vm->finalizerPending) Scm_VMFinalizerRun(vm);

    /* VM STOP is required from other thread.
       See Scm_ThreadStop() in ext/threads/threads.c */
    if (vm->stopRequest) {
        SCM_INTERNAL_MUTEX_SAFE_LOCK_BEGIN(vm->vmlock);
        /* Double check, since stopRequest can be canceled between the above
           two lines. */
        if (vm->stopRequest) {
            vm->stopRequest = FALSE;
            vm->state = SCM_VM_STOPPED;
            (void)SCM_INTERNAL_COND_BROADCAST(vm->cond);
            while (vm->state == SCM_VM_STOPPED) {
                /* Here the inspector thread examines VM state */
                (void)SCM_INTERNAL_COND_WAIT(vm->cond, vm->vmlock);
            }
        }
        SCM_INTERNAL_MUTEX_SAFE_LOCK_END();
    }
}

/*==============================================================
 * Debug features.
 */

/*
 * Stack trace.
 *
 *   The "lite" version returns a list of source information of
 *   continuation frames.
 *
 *   The full stack trace is consisted by a list of pair of
 *   source information and environment vector.  Environment vector
 *   is a copy of content of env frame, with the first element
 *   be the environment info.   Environment vector may be #f if
 *   the continuation frame doesn't have associated env.
 */

ScmObj Scm_VMGetStackLite(ScmVM *vm)
{
    ScmContFrame *c = vm->cont;
    ScmObj stack = SCM_NIL, tail = SCM_NIL;
    ScmObj info;

    info = Scm_VMGetSourceInfo(vm->base, vm->pc);
    if (!SCM_FALSEP(info)) SCM_APPEND1(stack, tail, info);
    while (c) {
        info = Scm_VMGetSourceInfo(c->base, c->pc);
        if (!SCM_FALSEP(info)) SCM_APPEND1(stack, tail, info);
        c = c->prev;
    }
    return stack;
}

#define DEFAULT_ENV_TABLE_SIZE  64

struct EnvTab {
    struct EnvTabEntry {
        ScmEnvFrame *env;
        ScmObj vec;
    } entries[DEFAULT_ENV_TABLE_SIZE];
    int numEntries;
};

static ScmObj env2vec(ScmEnvFrame *env, struct EnvTab *etab)
{
    int i;
    ScmObj vec;
    
    if (!env) return SCM_FALSE;
    for (i=0; i<etab->numEntries; i++) {
        if (etab->entries[i].env == env) {
            return etab->entries[i].vec;
        }
    }
    vec = Scm_MakeVector((int)env->size+2, SCM_FALSE);
    SCM_VECTOR_ELEMENT(vec, 0) = env2vec(env->up, etab);
    SCM_VECTOR_ELEMENT(vec, 1) = SCM_NIL; /*Scm_VMGetBindInfo(env->info);*/
    for (i=0; i<env->size; i++) {
        SCM_VECTOR_ELEMENT(vec, i+2) = ENV_DATA(env, (env->size-i-1));
    }
    if (etab->numEntries < DEFAULT_ENV_TABLE_SIZE) {
        etab->entries[etab->numEntries].env = env;
        etab->entries[etab->numEntries].vec = vec;
        etab->numEntries++;
    }
    return vec;
}

ScmObj Scm_VMGetStack(ScmVM *vm)
{
#if 0 /* for now */
    ScmContFrame *c = vm->cont;
    ScmObj stack = SCM_NIL, tail = SCM_NIL;
    ScmObj info, evec;
    struct EnvTab envTab;

    envTab.numEntries = 0;
    if (SCM_PAIRP(vm->pc)) {
        info = Scm_VMGetSourceInfo(vm->pc);
        SCM_APPEND1(stack, tail, Scm_Cons(info, env2vec(vm->env, &envTab)));
    }
    
    for (; c; c = c->prev) {
        if (!SCM_PAIRP(c->info)) continue;
        info = Scm_VMGetSourceInfo(c->info);
        evec = env2vec(c->env, &envTab);
        SCM_APPEND1(stack, tail, Scm_Cons(info, evec));
    }
    return stack;
#endif
    return SCM_NIL;
}

/*
 * Dump VM internal state.
 */
static ScmObj get_debug_info(ScmCompiledCode *base, SCM_PCTYPE pc)
{
    int off;
    ScmObj ip;
    if (base == NULL
        || (pc < base->code || pc >= base->code + base->codeSize)) {
        return SCM_FALSE;
    }
    off = (int)(pc - base->code - 1);  /* pc is already incremented, so -1. */
    SCM_FOR_EACH(ip, base->info) {
        ScmObj p = SCM_CAR(ip);
        if (!SCM_PAIRP(p) || !SCM_INTP(SCM_CAR(p))) continue;
        if (SCM_INT_VALUE(SCM_CAR(p)) < off) {
            return SCM_CDR(p);
            break;
        }
    }
    return SCM_FALSE;
}

ScmObj Scm_VMGetSourceInfo(ScmCompiledCode *base, SCM_PCTYPE pc)
{
    ScmObj info = get_debug_info(base, pc);
    if (SCM_PAIRP(info)) {
        ScmObj p = Scm_Assq(SCM_SYM_SOURCE_INFO, info);
        if (SCM_PAIRP(p)) return SCM_CDR(p);
    }
    return SCM_FALSE;
}

ScmObj Scm_VMGetBindInfo(ScmCompiledCode *base, SCM_PCTYPE pc)
{
    ScmObj info = get_debug_info(base, pc);
    if (SCM_PAIRP(info)) {
        ScmObj p = Scm_Assq(SCM_SYM_BIND_INFO, info);
        if (SCM_PAIRP(p)) return SCM_CDR(p);
    }
    return SCM_FALSE;
}

static void dump_env(ScmEnvFrame *env, ScmPort *out)
{
    int i;
    Scm_Printf(out, "   %p %55.1S\n", env, env->info);
    Scm_Printf(out, "       up=%p size=%d\n", env->up, env->size);
    Scm_Printf(out, "       [");
    for (i=0; i<env->size; i++) {
        Scm_Printf(out, " %S", ENV_DATA(env, i));
    }
    Scm_Printf(out, " ]\n");
}

void Scm_VMDump(ScmVM *vm)
{
    ScmPort *out = vm->curerr;
    ScmEnvFrame *env = vm->env;
    ScmContFrame *cont = vm->cont;
    ScmCStack *cstk = vm->cstack;
    ScmEscapePoint *ep = vm->escapePoint;

    Scm_Printf(out, "VM %p -----------------------------------------------------------\n", vm);
    Scm_Printf(out, "   pc: %08x ", vm->pc);
    Scm_Printf(out, "(%08x)\n", *vm->pc);
    Scm_Printf(out, "   sp: %p  base: %p  [%p-%p]\n", vm->sp, vm->stackBase,
               vm->stack, vm->stackEnd);
    Scm_Printf(out, " argp: %p\n", vm->argp);
    Scm_Printf(out, " val0: %#65.1S\n", vm->val0);

    Scm_Printf(out, " envs:\n");
    while (env) {
        dump_env(env, out);
        env = env->up;
    }
    
    Scm_Printf(out, "conts:\n");
    while (cont) {
        Scm_Printf(out, "   %p\n", cont);
        Scm_Printf(out, "              env = %p\n", cont->env);
        Scm_Printf(out, "             argp = %p[%d]\n", cont->argp, cont->size);
        if (cont->argp) {
            Scm_Printf(out, "               pc = %p ", cont->pc);
            Scm_Printf(out, "(%08x)\n", *cont->pc);
        } else {
            Scm_Printf(out, "               pc = {cproc %p}\n", cont->pc);
        }
        Scm_Printf(out, "             base = %p\n", cont->base);
        cont = cont->prev;
    }

    Scm_Printf(out, "C stacks:\n");
    while (cstk) {
        Scm_Printf(out, "  %p: prev=%p, cont=%p\n",
                   cstk, cstk->prev, cstk->cont);
        cstk = cstk->prev;
    }
    Scm_Printf(out, "Escape points:\n");
    while (ep) {
        Scm_Printf(out, "  %p: cont=%p, handler=%#20.1S\n",
                   ep, ep->cont, ep->ehandler);
        ep = ep->prev;
    }
    Scm_Printf(out, "dynenv: %S\n", vm->handlers);
    if (vm->base) {
        Scm_Printf(out, "Code:\n");
        Scm_CompiledCodeDump(vm->base);
    }
}

#ifdef USE_CUSTOM_STACK_MARKER
struct GC_ms_entry *vm_stack_mark(GC_word *addr,
                                  struct GC_ms_entry *mark_sp,
                                  struct GC_ms_entry *mark_sp_limit,
                                  GC_word env)
{
    struct GC_ms_entry *e = mark_sp;
    ScmObj *vmsb = ((ScmObj*)addr)+1;
    ScmVM *vm = (ScmVM*)*addr;
    int i, limit = vm->sp - vm->stackBase + 5;
    void * spb = (void *)vm->stackBase;
    void * sbe = (void *)(vm->stackBase + SCM_VM_STACK_SIZE);
    void * hb = GC_least_plausible_heap_addr;
    void * he = GC_greatest_plausible_heap_addr;

    for (i=0; i<limit; i++, vmsb++) {
        ScmObj z = *vmsb;
        if ((hb < (void *)z && (void *)z < spb)
            || ((void *)z > sbe && (void *)z < he)) {
            e = GC_mark_and_push((void *)z, e, mark_sp_limit, (void *)addr);
        }
    }
    return e;
}
#endif /*USE_CUSTOM_STACK_MARKER*/

/*===============================================================
 * Initialization
 */

void Scm__InitVM(void)
{
#ifdef USE_CUSTOM_STACK_MARKER
    vm_stack_free_list = GC_new_free_list();
    vm_stack_mark_proc = GC_new_proc(vm_stack_mark);
    vm_stack_kind = GC_new_kind(vm_stack_free_list,
                                GC_MAKE_PROC(vm_stack_mark_proc, 0),
                                0, 0);
#endif /*USE_CUSTOM_STACK_MARKER*/

    /* Create root VM */
#ifdef GAUCHE_USE_PTHREADS
    if (pthread_key_create(&vm_key, NULL) != 0) {
        Scm_Panic("pthread_key_create failed.");
    }
    rootVM = Scm_NewVM(NULL, SCM_MAKE_STR_IMMUTABLE("root"));
    if (pthread_setspecific(vm_key, rootVM) != 0) {
        Scm_Panic("pthread_setspecific failed.");
    }
    rootVM->thread = pthread_self();
#else   /* !GAUCHE_USE_PTHREADS */
    rootVM = theVM = Scm_NewVM(NULL, SCM_MAKE_STR_IMMUTABLE("root"));
#endif  /* !GAUCHE_USE_PTHREADS */
    rootVM->state = SCM_VM_RUNNABLE;

#ifdef COUNT_INSN_FREQUENCY
    Scm_AddCleanupHandler(dump_insn_frequency, NULL);
#endif /*COUNT_INSN_FREQUENCY*/

#ifdef COUNT_FLUSH_FPSTACK
    Scm_AddCleanupHandler(print_flush_fpstack_count, NULL);
#endif
}
