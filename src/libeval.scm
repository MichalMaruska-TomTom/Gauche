;;;
;;; libeval.scm - eval, load and related stuff
;;;
;;;   Copyright (c) 2000-2012  Shiro Kawai  <shiro@acm.org>
;;;
;;;   Redistribution and use in source and binary forms, with or without
;;;   modification, are permitted provided that the following conditions
;;;   are met:
;;;
;;;   1. Redistributions of source code must retain the above copyright
;;;      notice, this list of conditions and the following disclaimer.
;;;
;;;   2. Redistributions in binary form must reproduce the above copyright
;;;      notice, this list of conditions and the following disclaimer in the
;;;      documentation and/or other materials provided with the distribution.
;;;
;;;   3. Neither the name of the authors nor the names of its contributors
;;;      may be used to endorse or promote products derived from this
;;;      software without specific prior written permission.
;;;
;;;   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
;;;   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
;;;   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
;;;   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
;;;   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
;;;   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
;;;   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
;;;   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
;;;   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
;;;   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
;;;   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
;;;

(select-module gauche.internal)

(inline-stub
 (declcode (.include <gauche/vminsn.h>
                     <gauche/class.h>
                     <gauche/macro.h>)))

(declare (keep-private-macro autoload add-load-path
                             define-compiler-macro))

;;;
;;; Eval
;;;

(select-module scheme)

(define-cproc eval (expr env) Scm_VMEval)

;; for now, just return a module.
(define-cproc null-environment (version::<fixnum>)
  (when (!= version 5) (Scm_Error "unknown rNrs version: %d" version))
  (result (SCM_OBJ (Scm_NullModule))))
(define-cproc scheme-report-environment (version::<fixnum>)
  (when (!= version 5) (Scm_Error "unknown rNrs version: %d" version))
  (result (SCM_OBJ (Scm_SchemeModule))))
(define-cproc interaction-environment () (result (SCM_OBJ (Scm_UserModule))))

;;;
;;; Load
;;;

(select-module gauche.internal)

(define-cproc %record-load-start (path) ::<void>
  Scm__RecordLoadStart)

;; Main entry of `load'
(define-in-module scheme (load file :key (paths *load-path*)
                                         (suffixes *load-suffixes*)
                                         (error-if-not-found #t)
                                         (environment #f)
                                         (ignore-coding #f)
                                         (main-script #f))
  ;; NB: 'File not found' error is handled in find-load-file.
  (and-let* ([r (find-load-file file paths suffixes error-if-not-found #t)])
    (let* ([path (car r)]
           [remaining-paths (cadr r)]
           [opener (if (pair? (cddr r)) (caddr r) open-input-file)]
           [port (guard (e [else e]) (opener path))])
      (%record-load-start path)
      (if (not (input-port? port))
        (and error-if-not-found (raise e))
        (load-from-port (if ignore-coding
                          port
                          (open-coding-aware-port port))
                        :environment environment
                        :paths remaining-paths
                        :main-script main-script)))))

(select-module gauche)

(define-cproc current-load-history () Scm_CurrentLoadHistory)
(define-cproc current-load-next ()    Scm_CurrentLoadNext)
(define-cproc current-load-port ()    Scm_CurrentLoadPort)

(define-cproc load-from-port (port::<input-port>
                              :key (paths #f) (environment #f)
                              (main-script #f))
  (let* ([flags::int (?: (SCM_FALSEP main-script) 0 SCM_LOAD_MAIN_SCRIPT)])
    (result (Scm_VMLoadFromPort port paths environment flags))))

(define-cproc dynamic-load (file::<string>
                            :key (init-function #f)
                            (export-symbols #f)); for backward compatibility
  (result (Scm_DynLoad file init_function 0)))

(define-cproc provide (feature)   Scm_Provide)
(define-cproc provided? (feature) ::<boolean> Scm_ProvidedP)

(select-module gauche.internal)
;; NB: 'require' is recognized by the compiler, which calls
;; this one directly.
(define-cproc %require (feature) ::<boolean>
  (result (not (Scm_Require feature SCM_LOAD_PROPAGATE_ERROR NULL))))

(define-cproc %add-load-path (path::<const-cstring> :optional afterp)
  (result (Scm_AddLoadPath path (not (SCM_FALSEP afterp)))))

(define-cproc %autoload (mod::<module> file-or-module entries)
  ::<void> Scm_DefineAutoload)

;; Get the file path currently loading from.
;; We may swap the implementation with more reliable way in future.
(define-in-module gauche (current-load-path)
  (and-let* ([p (current-load-port)]
             [info (port-name p)]
             [ (string? info) ]
             [ (not (#/^\(.*\)$/ info)) ])
    info))

(select-module gauche)
(define-macro (autoload file . vars)
  `((with-module gauche.internal %autoload) (current-module) ',file ',vars))

;; Load path needs to be dealt with at the compile time.  this is a
;; hack to do so.   Don't modify *load-path* directly, since it causes
;; weird compiler-evaluator problem.
;; I don't like the current name "add-load-path", though---looks like
;; more a procedure than a compiler syntax---any ideas?
(select-module gauche)
(define-macro (add-load-path path . args)
  (let ([afterp (or (memq #t args) (memq :after args))]
        ;; If :relative is given, we trust the programmer to give a relative
        ;; pathname to PATH.
        [path (or (and-let* ([ (memq :relative args) ]
                             [cur (current-load-path) ])
                    (string-append (sys-dirname cur) "/" path))
                  path)])
    `',((with-module gauche.internal %add-load-path) path afterp)))

;; Load path hooks
(select-module gauche.internal)
(define-cproc %add-load-path-hook! (proc :optional (after?::<boolean> #f))
  ::<void> Scm_AddLoadPathHook)
(define-cproc %delete-load-path-hook! (proc)
  ::<void> Scm_DeleteLoadPathHook)

;; find-load-file
;;
;;   Core function to search specified file from the search path *PATH.
;;   Search rules are:
;;
;;    (1) If given filename begins with "/", "./" or "../", the file is
;;        searched.
;;    (2) If given filename begins with "~", unix-style username
;;        expansion is done, then the resulting file is searched.
;;    (3) Otherwise, the file is searched for each directory in PATHs.
;;
;;   If the named file is found, a list of the actual filename, and the
;;   remaining paths is returned.  The remaining paths can be used again
;;   to find next matching filename.
;;   (The returned list may have the third element, if load-path-hooks is
;;   used.  See below).
;;
;;   If SUFFIXES is given, after the filename is tested, names with
;;   each element in SUFFIXES list appended are tried.
;;   The element in SUFFIXES is directly appended to the FILENAME;
;;   so usually it begins with dot.
;;
;;   PATHs may contain a regular file.  In which case, procedures chained
;;   to *load-path-hooks* are called in turn.  It receives three arguments;
;;   the regular filename in PATHs, the (partial) filename given to the
;;   find-load-file, and the list of suffixes.  The hook is mainly intended
;;   to allow loading from archive files.   If the hook procedure "finds"
;;   the searched file in the archive file, it should return a pair of
;;   the canonical filename (given filename plus suffix if applicable), and
;;   a thunk that opens and returns a port to read the file.  If the hook
;;   procedure doesn't find the searched file, it should return #f.
;;
;;   NB: find-file-in-paths in file.util is similar to this, but this one
;;   captures the exact behavior of `load'.

(select-module gauche.internal)
(define (find-load-file filename paths suffixes
                        :optional (error-if-not-found #f)
                                  (allow-archive #f))
  (define (file-ok? file)
    (and (file-exists? file)
         (not (file-is-directory? file))))
  (define (try-suffixes stem)
    (cond [(file-ok? stem) stem]
          [else (any (^s (let1 file (string-append stem s)
                           (and (file-ok? file) file)))
                     suffixes)]))
  (define (do-absolute stem)
    (if-let1 found (try-suffixes stem)
      (list found '())
      (and error-if-not-found
           (errorf "cannot find ~s to load" stem))))
  (define (do-relative ps)
    (cond
     [(null? ps)
      (and error-if-not-found
           (errorf "cannot find ~s in ~s" filename paths))]
     [(file-is-directory? (car ps))
      (if-let1 found (try-suffixes (string-append (car ps) "/" filename))
        (list found (cdr ps))
        (do-relative (cdr ps)))]
     [(and allow-archive (file-is-regular? (car ps)))
      (if-let1 r (any (^p (p (car ps) filename suffixes)) *load-path-hooks*)
        (list (car r) (cdr ps) (cdr r))
        (do-relative (cdr ps)))]
     [else (do-relative (cdr ps))]))

  (when (equal? filename "")
    (error "bad filename to load" filename))
  (cond [(char=? (string-ref filename 0) #\~)
         (do-absolute (sys-normalize-pathname filename :expand #t))]
        [(rxmatch #/^\.{0,2}\// filename) (do-absolute filename)]
        ;; we can't use cond-expand here, for this file is precompiled
        ;; on a system different from the final target.
        [(and (or (assq 'gauche.os.windows (cond-features))
                  (assq 'gauche.os.cygwin (cond-features)))
              (rxmatch #/^[a-zA-Z]:/ filename)) ; the wicked drive-letter
         (do-absolute filename)]
        [else (do-relative paths)]))

;;;
;;; Repl
;;;

(select-module gauche.internal)

(define (%repl-print . vals) (for-each (^e (write/ss e) (newline)) vals))
(define (%repl-prompt) (display "gosh> ") (flush))

(define-in-module gauche (read-eval-print-loop :optional (reader #f)
                                                         (evaluator #f)
                                                         (printer #f)
                                                         (prompter #f))
  (let ([reader    (or reader read)]
        [evaluator (or evaluator eval)]
        [printer   (or printer %repl-print)]
        [prompter  (or prompter %repl-prompt)])
    (let loop1 ()
      (and
       (with-error-handler
           (^e (report-error e) #t)
         (^[]
           (let loop2 ()
             (prompter)
             (let1 exp (reader)
               (and (not (eof-object? exp))
                    (receive results (evaluator exp (vm-current-module))
                      (apply printer results)
                      (loop2)))))))
       (loop1)))))

;;;
;;; Macros
;;;

(select-module gauche)

(define-cproc macroexpand (form)
  (result (Scm_VMMacroExpand form SCM_NIL FALSE)))
(define-cproc macroexpand-1 (form)
  (result (Scm_VMMacroExpand form SCM_NIL TRUE)))

(define-cproc unwrap-syntax (form) Scm_UnwrapSyntax)

(select-module gauche.internal)
(inline-stub
 (define-type <macro> "ScmMacro*" "macro"
   "SCM_MACROP" "SCM_MACRO" "SCM_OBJ")
 )

;; These are used in the compiler, and hidden inside gauche.internal.
(define-cproc macro? (obj) ::<boolean> SCM_MACROP)
(define-cproc syntax? (obj) ::<boolean> SCM_SYNTAXP)

(define-cproc make-macro-transformer (name::<symbol> proc::<procedure>)
  Scm_MakeMacroTransformerOld)

(define-cproc %make-macro-transformer (name::<symbol> proc)
  Scm_MakeMacroTransformer)

(define-cproc compile-syntax-rules (name literals rules mod env)
  Scm_CompileSyntaxRules)

(define-cproc call-macro-expander (mac::<macro> expr env)
  Scm_CallMacroExpander)

(define-cproc make-syntax (name::<symbol> proc)
  Scm_MakeSyntax)

(define-cproc make-syntactic-closure (env literals expr)
  Scm_MakeSyntacticClosure)

(define-cproc call-syntax-handler (syn program cenv)
  (SCM_ASSERT (SCM_SYNTAXP syn))
  (result (Scm_VMApply2 (-> (SCM_SYNTAX syn) handler) program cenv)))

(define-cproc syntax-handler (syn)
  (SCM_ASSERT (SCM_SYNTAXP syn))
  (result (-> (SCM_SYNTAX syn) handler)))

(define-cproc %internal-macro-expand (form env once::<boolean>)
  Scm_VMMacroExpand)

;;;
;;; System termination
;;;

(select-module gauche)
(define-cproc %exit (:optional (code::<fixnum> 0)) ::<void> Scm_Exit)

;; exit handler.  we don't want to import the fluff with gauche.parameter,
;; so we manually allocate parameter slot.
(select-module gauche.internal)
(define-in-module gauche exit-handler
  (let1 index (%vm-make-parameter-slot)
    ;; set default exit handler
    (%vm-parameter-set! index #f
                        (^[code fmt args]
                          (when fmt
                            (apply format (standard-error-port) fmt args)
                            (newline (standard-error-port)))))
    (^ maybe-arg
      (rlet1 old (%vm-parameter-ref index #f)
        (when (pair? maybe-arg)
          (%vm-parameter-set! index #f (car maybe-arg)))))))

(define-in-module gauche (exit :optional (code 0) (fmt #f) :rest args)
  (cond [(exit-handler)
         => (^h (guard (e [(<error> e) #f]) (h code fmt args)))])
    (%exit code))

;;;
;;; GC control
;;;

(select-module gauche)
(define-cproc gc () (call <void> GC_gcollect))

(define-cproc gc-stat ()
  (result
   (list
    (list ':total-heap-size
          (Scm_MakeIntegerFromUI (cast u_long (GC_get_heap_size))))
    (list ':free-bytes
          (Scm_MakeIntegerFromUI (cast u_long (GC_get_free_bytes))))
    (list ':bytes-since-gc
          (Scm_MakeIntegerFromUI (cast u_long (GC_get_bytes_since_gc))))
    (list ':total-bytes
          (Scm_MakeIntegerFromUI (cast u_long (GC_get_total_bytes)))))))

(select-module gauche.internal)
;; for diagnostics
(define-cproc gc-print-static-roots () ::<void> Scm_PrintStaticRoots)

;;;
;;; Some system introspection
;;;

(select-module gauche)
;; Obtain info about gauche itself
(define-cproc gauche-version () ::<const-cstring> (result GAUCHE_VERSION))
(define-cproc gauche-architecture () ::<const-cstring> (result GAUCHE_ARCH))
(define-cproc gauche-library-directory () Scm_LibraryDirectory)
(define-cproc gauche-architecture-directory () Scm_ArchitectureDirectory)
(define-cproc gauche-site-library-directory () Scm_SiteLibraryDirectory)
(define-cproc gauche-site-architecture-directory () Scm_SiteArchitectureDirectory)
(define-cproc gauche-dso-suffix () ::<const-cstring> (result SHLIB_SO_SUFFIX))

;; Temporary - only usable on Windows(mingw) and Darwin; used to replace
;; '@' in the paths embedded in gauche.config.
(define-cproc %gauche-runtime-directory () Scm__RuntimeDirectory)

;;
;; External view of VM.
;;

(select-module gauche)
(inline-stub
 (define-cclass <thread> "ScmVM" "Scm_VMClass"
   ()
   ((name)
    (specific)
    )
   (printer
    (let* ([vm::ScmVM* (SCM_VM obj)]
           [state::(const char *)])
      (case (-> vm state)
        [(SCM_VM_NEW)        (set! state "new")]
        [(SCM_VM_RUNNABLE)   (set! state "runnable")]
        [(SCM_VM_STOPPED)    (set! state "stopped")]
        [(SCM_VM_TERMINATED) (set! state "terminated")]
        [else                (set! state "(unknown state")])
      (Scm_Printf port "#<thread %S %s %p>" (-> vm name) state vm)))
   )
 )
;; Other thread stuff is in ext/threads/thrlib.stub
(define-cproc current-thread () (result (SCM_OBJ (Scm_VM))))

(define-cproc vm-dump
  (:optional (vm::<thread> (c "SCM_OBJ(Scm_VM())"))) ::<void>
  (Scm_VMDump vm))

(define-cproc vm-get-stack-trace
  (:optional (vm::<thread> (c "SCM_OBJ(Scm_VM())")))
  (result (Scm_VMGetStack vm)))

(define-cproc vm-get-stack-trace-lite
  (:optional (vm::<thread> (c "SCM_OBJ(Scm_VM())")))
  (result (Scm_VMGetStackLite vm)))

(define-cproc %vm-show-stack-trace (trace :key
                                          (port::<port> (current-output-port))
                                          (maxdepth::<int> 0)
                                          (skip::<int> 0)
                                          (offset::<int> 0))
  ::<void>
  (Scm_ShowStackTrace port trace maxdepth skip offset
                      SCM_STACK_TRACE_FORMAT_ORIGINAL))

(define-cproc vm-set-default-exception-handler (vm::<thread> handler) ::<void>
  (unless (or (SCM_FALSEP handler) (SCM_PROCEDUREP handler))
    (SCM_TYPE_ERROR handler "a procedure or #f"))
  (set! (-> vm defaultEscapeHandler) handler))

;; parameter internal API
;; These will be called by the public API in gauche.parameter.  The protocol
;; is a bit weird, for the Scheme-level parameter has its own instance
;; definition distinct from C-level ScmParameterLoc.  Eventually it would
;; be nicer if we could merge two.
(select-module gauche.internal)

(define-cproc %vm-make-parameter-slot () ::<int>
  (let* ([loc::ScmParameterLoc])
    (Scm_InitParameterLoc (Scm_VM) (& loc) SCM_FALSE)
    (result (ref loc index))))

(define-cproc %vm-parameter-ref (index::<int> init-value)
  (let* ([loc::ScmParameterLoc])
    (set! (ref loc index) index
          (ref loc initialValue) init-value)
    (result (Scm_ParameterRef (Scm_VM) (& loc)))))

(define-cproc %vm-parameter-set! (index::<int> init-value new-value)
  (let* ([loc::ScmParameterLoc])
    (set! (ref loc index) index
          (ref loc initialValue) init-value)
    (result (Scm_ParameterSet (Scm_VM) (& loc) new-value))))

;; For the backward compatibility---files precompiled by 0.9.2 or before
;; can contain reference to the old API (as the result of expansion of
;; parameterize).  These definition converts them to the new API.
;; Will be removed on 1.0 release.
(select-module gauche)
(define (%vm-make-parameter-slot)
  (values ((with-module gauche.internal %vm-make-parameter-slot)) 0))
(define (%vm-parameter-ref index id)
  ((with-module gauche.internal %vm-parameter-ref) index #f))
(define (%vm-parameter-set! index id value)
  ((with-module gauche.internal %vm-parameter-set!) index #f value))

;;;
;;;Tentative compiler macro
;;;

;;  (define-compiler-macro <name>
;;    (er-transformer
;;     (lambda (form rename compare) ...)))
;;
;; Er-transformer is a wrapper to indicate the transformer is
;; explicit-renaming.  It leaves room to support other type of macro
;; transformers in future.
;; The transformer itself must return <FORM> itself if it aborts
;; expansion.

(define-macro (define-compiler-macro name xformer-spec)
  ;; TODO: Rewrite this after we get builtin patter matching.
  (unless (and (= (length xformer-spec) 2)
               (eq? (unwrap-syntax (car xformer-spec)) 'er-transformer))
    (error "malformed define-compiler-macro: "
           `(define-compiler-macro ,name ,xformer-spec)))
  `((with-module gauche.internal %bind-inline-er-transformer)
    (current-module) ',name ,(cadr xformer-spec)))
