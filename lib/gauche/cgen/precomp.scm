;;;
;;; gauche.cgen.precomp - Precompile Scheme into C data
;;;  
;;;   Copyright (c) 2004-2010  Shiro Kawai  <shiro@acm.org>
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

;;; Precompiler takes compiled VM instruction array and dumps it
;;; as C code.

(define-module gauche.cgen.precomp
  (use srfi-1)
  (use srfi-13)
  (use gauche.cgen)
  (use gauche.cgen.stub)
  (use gauche.vm.insn)
  (use gauche.parameter)
  (use gauche.sequence)
  (use gauche.experimental.app)
  (use gauche.experimental.ref)
  (use gauche.experimental.lamb)
  (use file.util)
  (use util.match)
  (use util.list)
  (use util.toposort)
  (use text.tr)
  (export cgen-precompile cgen-precompile-multi))
(select-module gauche.cgen.precomp)

;;================================================================
;; Main Entry point
;;
;;  The cgen-precompile function reads a scheme file "<foo>.scm",
;;  compiles it and dumps the result as a C source "<foo>.c".
;;  It may also generates "<foo>.sci", an interface definition file
;;  which contains forms like define-module, use, export, etc.
;;
;;  A generated C file contains an initialization function, named
;;  Scm_Init_<foo> by default.   Typically it is called by "extention
;;  initializer", which is invoked when the DSO file is loaded by
;;  dynamic-load.
;;
;;    Example1:
;;
;;     *  An extention extention.so is built from extension.c,
;;        foo.scm and bar.scm.
;;
;;     *  extension.c must contain a funciton Scm_Init_extension(),
;;        which is the extention initializer.  It is called from
;;        (dynamic-load "extension")
;;
;;     *  By processing foo.scm and bar.scm, you'll get foo.c and
;;        bar.c, each contain Scm_Init_foo() and Scm_Init_bar(),
;;        respectively.
;;
;;     *  Scm_Init_extension() is responsible to call Scm_Init_foo()
;;        and Scm_Init_bar().
;;
;;  Sometimes sources consist of Scheme files only.  In which case,
;;  giving true value to ext-initializer keyword argument makes
;;  the inialization function work as an extention initializer.
;;
;;    Example2:
;;
;;     * An extension extension.so is built from extention.scm.
;;
;;     * Processing extension.scm with :ext-initializer #t
;;       makes generated Scm_Init_extension() work as an extention
;;       initializer.
;;
;;  If there are more than one Scheme files and you want to make
;;  one of its initializer funtion as an extention initializer,
;;  give :sub-initializers argument to the 'main' source whose
;;  initialization function becomes an extention initializer.
;;
;;    Example3:
;;
;;     * An extension extension.so is built from extension.scm,
;;       foo.scm and bar.scm
;;
;;     * foo.c and bar.c are to be created normally.  Each has
;;       Scm_Init_foo() and Scm_Init_bar(), respectively.
;;
;;     * extension.c are to be created with :ext-initializer #t
;;       and :sub-initializers '(Scm_Init_foo Scm_Init_bar).
;;       The generated Scm_Init_extension() works as an extension
;;       initializer, _and_ it calls Scm_Init_foo() and Scm_Init_bar().
;;

;; Keyword arguments:
;;
;; ext-initializer : See above.
;; sub-initializers : See above.
;;
;; out.c :   Alternative name for C output  #f to use the default
;;           (path-swap-extension (sys-basename src) "c").
;; out.sci : Alternative name for SCI output.  If #f, take the default
;;           behavior which is:
;;           - If the source's first form is define-module, use
;;             (strip-prefix prefix (path-swap-extension src "sci"))
;;           - Otherwise, do not produce SCI output.
;;           If the source has define-module form and you don't want
;;           to create SCI output, pass "/dev/null" to this argument.
;;
;; strip-prefix : Used to derive sci file path from the source file name.
;;           This argument is ignored when out.sci is specified.
;;           - If #f, the sci file is the same as src except its suffix is
;;             substituted for ".sci".
;;           - If #t, the sci file is the basename of src, with its suffx
;;             substituted for ".sci".
;;           - Otherwise, this argument must be a string.  First src's
;;             prefix is checked to match this argument; if they match,
;;             the matching prefix is removed from SRC, then its extension
;;             is substituted for ".sci".  If SRC's suffix does not match,
;;             it works just like #f is given to this argument.
;;           This feature is useful to generate *.sci files mirroring
;;           the source directory hierarchy.
;;
;; predef-syms : A list of strings, to insert #defines at the top of
;;      generated C source.
;;
;; macros-to-keep : List of names of private macros that should be included
;;      in the output.  Usually private macros (macros bound to a variable
;;      which isn't exported) are not included in the output.  But sometimes
;;      hygienic public macros expands to a call of private macros, and
;;      gauche.cgen.precomp cannot detect such dependencies yet.

;; TO BE FIXED AFTER 0.9 RELEASE:
;;  The keyword arguments marked 'TBF' in cgen-precomp and cgen-precomp-multi
;;  should be fixed to the newer syntax ((:key var) init).  We use older
;;  syntax here just to make the source buildable by 0.8.14.  After releasing
;;  0.9 they should be changed to use the newer syntax.  See also the
;;  pass1/extended-lambda for another temporary fix.
(define (cgen-precompile src
                         :key (out.c #f)
                              (out.sci #f)
                              (prefix :strip-prefix #f) ; TBF
                              (ext-initializer #f)
                              (dso :dso-name #f)        ; TBF
                              (initializer-name #f)
                              (sub-initializers '())
                              (predef-syms '())
                              (macros-to-keep '()))
  (let ([out.c   (or out.c (path-swap-extension (sys-basename src) "c"))]
        [out.sci (or out.sci
                     (and (check-first-form-is-define-module src)
                          (strip-prefix (path-swap-extension src "sci")
                                        prefix)))])
    ;; see PARAMETERS section below
    (parameterize ([cgen-current-unit (get-unit src out.c predef-syms
                                                ext-initializer)]
                   [compile-module    (make-module #f)]
                   [dso-name (cons
                              (or dso
                                  (and ext-initializer
                                       (basename-sans-extension out.c)))
                              initializer-name)]
                   [vm-eval-situation SCM_VM_COMPILING]
                   [private-macros-to-keep macros-to-keep])
      (cond [out.sci
             (make-directory* (sys-dirname out.sci))
             (call-with-output-file out.sci
               (lambda (p)
                 (display ";; generated automatically.  DO NOT EDIT\n" p)
                 (display "#!no-fold-case\n" p)
                 (parameterize ([ext-module-file p])
                   (do-it src ext-initializer sub-initializers))))]
            [else
             (parameterize ([ext-module-file #f])
               (do-it src ext-initializer sub-initializers))]))))

(define (do-it src ext-initializer sub-initializers)
  (setup ext-initializer sub-initializers)
  (with-input-from-file src
    (cut emit-toplevel-executor
         (reverse (port-fold compile-toplevel-form '() read))))
  (finalize sub-initializers)
  (cgen-emit-c (cgen-current-unit)))

;; Precompile multiple Scheme sources that are to be linked into
;; single DSO.  Need to check dependency.  The name of the first
;; source is used to derive DSO name.
(define (cgen-precompile-multi srcs
                               :key (ext-initializer #f)
                                    (prefix :strip-prefix #f) ; TBF
                                    (dso :dso-name #f)        ; TBF
                                    (predef-syms '())
                                    (macros-to-keep '()))
  (match srcs
    [() #f]
    [(main . subs)
     (clean-output-files srcs prefix)
     (dolist [src (order-files-by-dependency srcs)]
       (let* ([out.c ($ xlate-cfilename
                        $ strip-prefix (path-swap-extension src "c") prefix)]
              [initname (string-tr (path-sans-extension out.c) "-+." "___")])
         (cgen-precompile src
                          :out.c out.c
                          :dso-name (or dso (basename-sans-extension main))
                          :predef-syms predef-syms
                          :strip-prefix prefix
                          :macros-to-keep macros-to-keep
                          :ext-initializer (and (equal? src main)
                                                ext-initializer)
                          :initializer-name #`"Scm_Init_,initname")))]
    ))

;;================================================================
;; Parameters
;;

;; we compile things within an anonymous module to avoid interference.
(define compile-module (make-parameter #f))

;; keep the (supposed) name of the current module.  (current-module) syntax
;; is compiled into (find-module ...) expression to lookup this name at
;; runtime.
(define compile-module-name (make-parameter #f))

;; A pair of the name of the generated DSO (w/o extension) and the name
;; of the initializer function.  
(define dso-name (make-parameter #f))

;; keep the list of exported bindings (or #t if export-all)
(define compile-module-exports (make-parameter '()))

;; when we're compiling extension module (--ext-module=file), this parameter
;; keeps a port to the specified file.  the file becomes a module definition
;; file, containing define-module and dynamic-load forms, as well as the
;; exported macros.
;; NB: we insert (dynamic-load ...) just after select-module in the ext-module
;; file, assuming the source file has standard layout.
(define ext-module-file (make-parameter #f))

;; list of private macros that should be included in the output.
;; (--keep-private-macro=name,name,...)
;; usually private macros (macros bound to a variable which isn't exported)
;; are discarded, but sometimes hygienic public macros expands to a call
;; of private macros.  gencomp cannot detect such dependency yet, and
;; so they need to be explicitly listed for the time being.
(define private-macros-to-keep (make-parameter '()))

;;================================================================
;; Bridge to the internal stuff
;;
  
;; compatibility kludge
(define compile       (with-module gauche.internal compile))
(define compile-toplevel-lambda
  (with-module gauche.internal compile-toplevel-lambda))
(define %procedure-inliner
  (with-module gauche.internal %procedure-inliner))
(define vm-code->list (with-module gauche.internal vm-code->list))
(define vm-eval-situation
  (with-module gauche.internal vm-eval-situation))
(define global-eq?? (with-module gauche.internal global-eq??))

(define-constant SCM_VM_COMPILING 2) ;; must match with vm.h

;;================================================================
;; Utilities
;;

(define (get-unit src out.c predef-syms ext-init?)
  (let* ([base (basename-sans-extension out.c)]
         [safe-name (string-tr base "-+" "__")])
    (make <cgen-stub-unit>
      :name base :c-name-prefix safe-name
      :preamble `(,#`"/* Generated automatically from ,|src|.  DO NOT EDIT */")
      :pre-decl (map (lambda (s) #`"#define ,s") predef-syms)
      :init-prologue (format "~avoid Scm_Init_~a() { ScmModule *mod;"
                             (if ext-init? "SCM_EXTENSION_ENTRY " "")
                             safe-name)
      )))

(define (strip-prefix path prefix)
  (cond
   [(not prefix) path]
   [(eq? prefix #t) (sys-basename path)]
   [else
    (let1 pre (if (#/[\/\\]$/ prefix) prefix (string-append prefix "/"))
      (if (string-prefix? pre path)
        (string-drop path (string-length pre))
        path))]))

(define (xlate-cfilename path)
  (regexp-replace-all #/[\/\\]/ (sys-normalize-pathname path :canonicalize #t)
                      "--"))

(define (basename-sans-extension path)
  (path-sans-extension (sys-basename path)))

;; Read the first form.
;; We don't read the entire content of the file, since it may contain
;; srfi-10 read-time constructor that we don't know about yet.
(define (first-form src) (with-input-from-file src read))

;; Check if the first form is define-module.
(define (check-first-form-is-define-module src)
  (match (first-form src)
    [('define-module . _) #t]
    [else #f]))

;; Returns (<module> <srcname> (<depended> ...))
(define (get-module-dependency src)
  (match (first-form src)
    [('define-module modname . forms)
     (list modname src
           (filter-map (^(x)(match x [('use mod . _) mod] [_ #f])) forms))]
    [_ #f]))

;; Sort the given list of source files so that each file only depends on
;; the files that appear later.
(define (order-files-by-dependency srcs)
  (let* ([deps (filter-map get-module-dependency srcs)]
         [sorted (topological-sort (map (^.[(n _ ns) (cons n ns)]) deps))]
         [sorted-srcs (filter-map (^(s) (cond [(assq s deps) => cadr]
                                               [else #f]))
                                   sorted)]
         [unsorted-srcs (lset-difference string=? srcs sorted-srcs)])
    (append sorted-srcs unsorted-srcs)))

;; Removes *.sci files before start compiling so that the old file
;; won't interfere with compilation.
(define (clean-output-files scms prefix)
  (dolist [s scms]
    (when (equal? (path-extension s) "sci")
      (error "source file list contains *.sci file:" s))
    (sys-unlink (strip-prefix (path-swap-extension s "sci") prefix))))

(define (write-ext-module form)
  (cond [(ext-module-file) => (^_ (write form _) (newline _))]))

(define (setup ext-init? subinits)
  (cgen-decl "#include <gauche/code.h>")
  (cgen-decl "#include <gauche/macro.h>") ; for MakeMacroTransformerOld. temporary.
  (cond [(and ext-init? (ext-module-file))
         => (lambda (extm)
              (cgen-decl "#include <gauche/extend.h>")
              (let* ([extname ($ path-sans-extension
                                 $ sys-basename $ port-name extm)]
                     [safe-extname (regexp-replace-all #/\W/ extname "_")])
                (cgen-init #`"SCM_INIT_EXTENSION(,safe-extname);")))])
  (dolist [init subinits]
    (cgen-decl #`"extern void Scm_Init_,init(void);"))

  (setup-compiler-environment (compile-module))
  )

(define (finalize subinits)
  (dolist [init subinits]
    (cgen-init #`"  Scm_Init_,init();")))

;;================================================================
;; Compiler stuff
;;

;; NOTE:
;;   The code is compiled in the version of the compiler currently
;;   running gencomp (host compiler).  It may differ from the version
;;   of the compiler we're compiling (target compiler), and it becomes
;;   a problem if the two versions of compilers are using different
;;   mappings between mnemonics and codes.
;;
;;   When gencomp generates the C literals for the compiled code, it
;;   uses the following mapping scheme.
;;
;;    1. use vm-code->list to extract mnemonics from the code
;;       compiled by the host compiler.
;;    2. use vm-find-insn-info (in gauche.vm.insn module) to map
;;       mnemonics to the target compiler's code.
;;   
;;   For this scheme to work, the following conditions should be satisfied.
;;
;;    a. gauche.vm.insn should be the one generated from the same
;;       vminsn.scm of the target compiler.
;;    b. all the mnemonics that consists of the code generated by
;;       the host compiler must exists in the target compiler's ISA.
;;
;;   The condition b. implies that if you want to rename an instruction,
;;   you have to take three steps:
;;    (1) add a new instruction of the desired name, compile the
;;        target compiler #1.  (This version of the compiled target
;;        compiler still uses old instruction).
;;    (2) compile the target compiler again, using the target compiler #1,
;;        to generate the target compiler #2.  (This version of
;;        the target compiler uses the new instruction).
;;    (3) remove the old instruction.
;;

;; predicates to match the global identifiers that are not redefined
(define-syntax define-global-pred
  (syntax-rules ()
    [(_ name sym)
     (define name (global-eq?? 'sym 'gauche compile-module))]))
(define-global-pred =define-module?   define-module)
(define-global-pred =select-module?   select-module)
(define-global-pred =use?             use)
(define-global-pred =export?          export)
(define-global-pred =export-all?      export-all)
(define-global-pred =export-if-defined? export-if-defined)
(define-global-pred =provide?         provide)
(define-global-pred =lambda?          lambda)
(define-global-pred =declare?         declare)

;; compile FORM, and conses the toplevel code (something to be
;; executed at toplevel).
(define (compile-toplevel-form form seed)
  (guard (e
          [(<error> e)
           (format (current-error-port) "Error in compiling ~s\n" form)
           (raise e)])
    (match form
      ;; Module related stuff
      [((? =define-module?) mod . body)
       (write-ext-module form)
       (parameterize ([compile-module-name mod])
         (fold compile-toplevel-form seed body))]
      [((? =select-module?) mod)
       (write-ext-module form)
       (match (dso-name)
         [(name . #f)
          (write-ext-module `(dynamic-load ,name))]
         [(name . initfn)
          (write-ext-module `(dynamic-load ,name :init-function ,initfn))]
         [_ #f])
       (let1 sym (cgen-literal mod)
         (cgen-init
          (format "  mod = Scm_FindModule(SCM_SYMBOL(~a),\
                                          SCM_FIND_MODULE_CREATE);"
                  (cgen-cexpr sym))
          ;; force the current module to be mod
          "  Scm_SelectModule(mod);"))
       (compile-module-name mod)
       seed]
      [((? =use?) mod)
       (eval `(use ,mod) (compile-module)) seed]
      [((? =export?) . syms)
       (when (list? (compile-module-exports))
         (compile-module-exports
          (lset-union eq? syms (compile-module-exports))))
       (eval `(export ,@syms) (compile-module)) seed]
      [((? =export-all?)) (compile-module-exports #t)]
      [((? =export-if-defined?) . _) (write-ext-module form) seed]
      [((? =provide?) arg) (write-ext-module form) seed]
      ;; TODO - we need more general framework supporting various declarations.
      ;; for the time being, this ad-hoc solution suffice our needs.
      [((? =declare?) decls ...)
       (for-each (^.[('keep-private-macro . macros)
                     (private-macros-to-keep (append (private-macros-to-keep)
                                                     macros))]
                    [other (error "Unknown declaration:" other)])
                 decls)
       seed]
      ;; Finally, ordinary expressions.
      [else
       (let1 compiled-code (compile form (compile-module))
         ;; We exclude a compiled code with only CONSTU-RET, which appears
         ;; as the result of macro expansion sometimes.
         (if (toplevel-constu-ret-code? compiled-code)
           seed
           (cons (cgen-literal compiled-code) seed)))]
      )))

;; check to see the compiled code only contains CONSTU-RET insn.
(define (toplevel-constu-ret-code? compiled-code)
  (and (eq? (~ compiled-code'name) '%toplevel)
       (= (~ compiled-code'size) 1)
       (let1 code (vm-code->list compiled-code)
         (null? (cdr code))
         (eq? (caar code) 'CONSTU-RET))))

;; given list of toplevel compiled codes, generate code in init
;; that calls them.  This is assumed to be the last procedure before
;; calling cgen-emit.
(define (emit-toplevel-executor topcodes)
  (cgen-body "static ScmCompiledCode *toplevels[] = {")
  (dolist (t topcodes)
    (cgen-body (format "  SCM_COMPILED_CODE(~a)," (cgen-cexpr t))))
  (cgen-body " NULL /*termination*/" "};")

  (cgen-init (format "  Scm_VMExecuteToplevels(toplevels);"))
  )

;;================================================================
;; Special form handlers
;;

;; Some special forms must be handled differently from the ordinary
;; compilation.  We implement it by replacing those special forms
;; for tailored handlers within the compiler environment.
;;
;; NB: We used to recognize those forms literally within
;; compile-toplevel-form.  It failed to work, however, when these
;; forms are generated as the result of macro expansion.
;; The current approach still has an issue when the compiled source
;; overrides these special forms.  Such sources should be very unusual,
;; so we don't support them for the time being.

(define *special-handlers*
  '((define-macro (current-module)
      `(find-module ',(with-module gauche.cgen.precomp
                        (compile-module-name))))
    (define-macro (inline-stub . forms)
      (for-each (lambda (s)
                  ((with-module gauche.cgen.stub cgen-stub-parse-form)
                   (unwrap-syntax s)))
                forms)
      (undefined))
    (define-macro (define . f)
      ((with-module gauche.cgen.precomp handle-define) f))
    (define-macro (define-constant . f)
      ((with-module gauche.cgen.precomp handle-define-constant) f))
    (define-macro (define-syntax . f)
      ((with-module gauche.cgen.precomp handle-define-syntax) f))
    (define-macro (define-macro . f)
      ((with-module gauche.cgen.precomp handle-define-macro) f))
    ))

(define (setup-compiler-environment mod)
  (dolist [form *special-handlers*]
    (eval form mod)))

;; For the time being, we only compile the legacy macros into C file.
;; R5RS macros are put in ext-module file as is.
(define (handle-define-macro form)
  (match form
    [((name . formals) . body)
     (when (or (symbol-exported? name)
               (memq name (private-macros-to-keep)))
       (let* ([body-closure (compile-toplevel-lambda form name formals
                                                     body (compile-module))]
              [code (cgen-literal (closure-code body-closure))]
              [var  (cgen-literal name)])
         (cgen-init
          (format "  Scm_Define(mod, SCM_SYMBOL(~a), \
                             Scm_MakeMacroTransformerOld(~a,\
                                 SCM_PROCEDURE(Scm_MakeClosure(~a, NULL)))); /* ~s */"
                  (cgen-cexpr var) (cgen-cexpr var)
                  (cgen-cexpr code) name))))]
    [(name . expr)
     (when (symbol-exported? name)
       (write-ext-module `(define-macro . ,form)))]
    [_ #f])
  (cons '(with-module gauche define-macro) form))

(define (handle-define-syntax form)
  (match form
    [(name . _)
     (when (or (symbol-exported? name)
               (memq name (private-macros-to-keep)))
       (write-ext-module `(define-syntax . ,form)))]
    [_ #f])
  (cons '(with-module gauche define-syntax) form))  

(define (handle-define form)
  (match form
    [((name . args) . body)
     (handle-define `(,name (lambda ,args ,@body)))]
    [((? symbol? name) ((? =lambda?) args . body))
     (let* ([closure
             (compile-toplevel-lambda form name args body (compile-module))]
            [code (cgen-literal (closure-code closure))]
            [var  (cgen-literal name)])
       (cgen-init
        (format "  Scm_Define(mod, SCM_SYMBOL(~a), Scm_MakeClosure(~a, NULL)); /* ~s */"
                (cgen-cexpr var) (cgen-cexpr code) name)))
     (undefined)]
    [_
     (cons '(with-module gauche define) form)]))

(define (handle-define-constant form)
  (match form
    [((? symbol? name) expr)
     (eval `((with-module gauche define-constant) ,@form) (compile-module))]
    [_ #f])
  (cons '(with-module gauche define-constant) form))

;; check to see if the symbol is exported
(define (symbol-exported? sym)
  (or (eq? (compile-module-exports) #t)
      (memq sym (compile-module-exports))))

;;================================================================
;; Compiler-specific literal handling definitions
;;       
(define-cgen-literal <cgen-scheme-code> <compiled-code>
  ((code-name   :init-keyword :code-name)
   (code-vector-c-name :init-keyword :code-vector-c-name)
   (literals    :init-keyword :literals)
   )
  (make (value)
    (let* ((cv  (vm-code->list value))
           (lv  (extract-literals cv))
           (cvn (allocate-code-vector cv lv (~ value'full-name)))
           (code-name (cgen-literal (~ value'name)))
           (arg-info (cgen-literal (~ value'arg-info)))
           (inliner (check-packed-inliner value))
           )
      (define (init-thunk)
        (format #t "    SCM_COMPILED_CODE_CONST_INITIALIZER(  /* ~a */\n"
                (cgen-safe-comment (~ value'name)))
        (format #t "            (ScmWord*)(~a), ~a,\n"
                cvn (length cv))
        (format #t "            ~a, ~a, ~a, ~a, SCM_NIL, ~a,\n"
                (~ value'max-stack)
                (~ value'required-args)
                (~ value'optional-args)
                (if (cgen-literal-static? code-name)
                  (cgen-cexpr code-name)
                  "SCM_FALSE")
                (cgen-cexpr arg-info))
        (format #t "            ~a, ~a)"
                (cgen-cexpr (cgen-literal (~ value'parent)))
                (if inliner
                  (cgen-cexpr inliner)
                  "SCM_FALSE")))
      (make <cgen-scheme-code> :value value
            :c-name (cgen-allocate-static-datum 'runtime 'ScmCompiledCode
                                                init-thunk)
            :code-vector-c-name cvn
            :code-name code-name
            :literals lv)))
  (init (self)
    (unless (cgen-literal-static? [~ self'code-name])
      (print "  SCM_COMPILED_CODE("[~ self'c-name]")->name = "
             (cgen-cexpr [~ self'code-name])";"
             "/* "(cgen-safe-comment [~ self'value'full-name])" */"))
    (fill-code self))
  (static (self) #t)
  )

;; Returns a list of the same length of CODE, which includes the
;; <cgen-literal>s corresponding to the literal values in CODE.
;; #f is filled in the places that don't have corresponding litaral value.
(define (extract-literals code)
  (let loop ((code code)
             (lits '()))
    (if (null? code)
      (reverse lits)
      (let* ([insn (car code)]
             [info (vm-find-insn-info (car insn))])
        (case (~ info'operand-type)
          [(none) (loop (cdr code)  (cons #f lits))]
          [(addr) (loop (cddr code) (list* #f #f lits))]
          [(code codes) (loop (cddr code)
                              (list* (cgen-literal (cadr code)) #f lits))]
          [(obj) (loop (cddr code)
                        (list* (cgen-literal (cadr code)) #f lits))]
          [(obj+addr)
           (loop (cdddr code)
                 (list* #f (cgen-literal (cadr code)) #f lits))]
          )))))

(define (allocate-code-vector cv lv full-name)

  (define (alloc-word initval)
    (cgen-allocate-static-datum 'runtime 'ScmWord initval))

  (define (loop cv lv count first-cexpr)
    (if (null? cv)
      first-cexpr
      (let* ([insn (car cv)]
             [info (vm-find-insn-info (car insn))]
             [insnval (vm-build-insn insn)]
             [name-info (if first-cexpr
                          ""
                          (format "/* ~a */\n    " (cgen-safe-comment full-name)))]
             [insn-cexpr
              (alloc-word
               ;; We emit it as signed integer so that 64bit machine
               ;; correctly handles negative parameter value.
               (if (> insnval #x80000000)
                 (format "~a-0x~8,'0x   /* ~3d ~a */"
                         name-info (- #x100000000 insnval) count
                         (cgen-safe-comment insn))
                 (format "~a0x~8,'0x    /* ~3d ~a */"
                         name-info insnval count
                         (cgen-safe-comment insn))))]
             [first-cexpr (or first-cexpr insn-cexpr)])
        (case (~ info'operand-type)
          [(none)
           (loop (cdr cv) (cdr lv) (+ count 1) first-cexpr)]
          [(addr)
           (alloc-word
            (format "SCM_WORD((ScmWord*)~a + ~d)"
                    first-cexpr (cadr cv)))
           (loop (cddr cv) (cddr lv) (+ count 2) first-cexpr)]
          [(obj code codes)
           (alloc-word
            (if (cgen-literal-static? (cadr lv))
              (format "SCM_WORD(~a) /* ~a */"
                      (cgen-cexpr (cadr lv))
                      (cgen-safe-comment (write-to-string (cadr cv))))
              (format "SCM_WORD(SCM_UNDEFINED) /* ~a */"
                      (cgen-safe-comment (write-to-string (cadr cv))))))
           (loop (cddr cv) (cddr lv) (+ count 2) first-cexpr)]
          [(obj+addr)
           (alloc-word
            (if (cgen-literal-static? (cadr lv))
              (format "SCM_WORD(~a) /* ~a */"
                      (cgen-cexpr (cadr lv))
                      (cgen-safe-comment (write-to-string (cadr cv))))
              (format "SCM_WORD(SCM_UNDEFINED) /* ~a */"
                      (cgen-safe-comment (write-to-string (cadr cv))))))
           (alloc-word
            (format "SCM_WORD((ScmWord*)~a + ~d)  /*    ~3d */"
                    first-cexpr (caddr cv) (caddr cv)))
           (loop (cdddr cv) (cdddr lv) (+ count 3) first-cexpr)]
          ))))

  (loop cv lv 0 #f))

(define (fill-code code)
  (let ([cvn  (~ code'code-vector-c-name)]
        [lv   (~ code'literals)])
    (for-each-with-index
     (lambda (index lit)
       (when (and lit (not (cgen-literal-static? lit)))
         (format #t "  ((ScmWord*)~a)[~a] = SCM_WORD(~a);\n"
                 cvn index (cgen-cexpr lit))))
     lv)
    ))

;; If the compiled-code has packed IForm for inliner, translate it for
;; the target VM insns and returns the packed IForm.
(define (check-packed-inliner compiled-code)
  (let1 il (~ compiled-code'intermediate-form)
    (and (vector? il)
         (let* ([insns (class-slot-ref <vm-insn-info> 'all-insns)]
                [packed ((with-module gauche.internal translate-packed-iform)
                         il insns)])
           (cgen-literal packed)))))

;; NB: this doesn't yet handle identifiers that are inserted by hygienic
;; macro (so that they have different module than the current one).
(define-cgen-literal <cgen-scheme-identifier> <identifier>
  ((id-name   :init-keyword :id-name)
   (mod-name  :init-keyword :mod-name))
  (make (value)
    (unless (null? (~ value'env))
      (error "identifier with compiler environment can't be compiled" value))
    (make <cgen-scheme-identifier> :value value
          :c-name (cgen-allocate-static-datum)
          :id-name (cgen-literal (~ value'name))
          :mod-name (and-let* ([modnam (module-name-fix (~ value'module))])
                      (cgen-literal modnam))))
  (init (self)
    (let ([name (cgen-cexpr (~ self'id-name))]
          [cname (~ self'c-name)])
      (or (and-let* ([modnam (~ self'mod-name)])
            (print "  "cname" = Scm_MakeIdentifier(SCM_SYMBOL("name"), "
                   "Scm_FindModule(SCM_SYMBOL("(cgen-cexpr modnam)"), SCM_FIND_MODULE_CREATE),"
                   "SCM_NIL);"))
          (print "  "cname" = Scm_MakeIdentifier(SCM_SYMBOL("name"), mod, SCM_NIL);"))))
  (static (self) #f)
  )

;; NB: for compatibility, we check modnam vs '# to find out anonymous
;; modules.  (By 0.8.14 anonymous modules are named as |#|.)
(define (module-name-fix module)
  (and-let* ([nam (module-name module)]
             [ (not (eq? nam '|#|)) ]) ;|# <- to fool emacs
    nam))

;; NB: for now, we ignore macros (we assume they are only used within
;; the source file).
(define-cgen-literal <cgen-scheme-macro> <macro>
  ()
  (make (value)
    (make <cgen-scheme-macro> :value value :c-name #f))
  )

;; For generic functions, we initialize it at runtime.
(define-cgen-literal <cgen-scheme-generic> <generic>
  ((gf-name :init-keyword :gf-name))
  (make (value)
    (make <cgen-scheme-generic>
      :value value
      :c-name  (cgen-allocate-static-datum)
      :gf-name (cgen-literal (ref value 'name))))
  (init (self)
    (format #t "  ~a = Scm_SymbolValue(mod, SCM_SYMBOL(~a));\n"
            (~ self'c-name) (~ self'gf-name'c-name)))
  (static (self) #f)
  )

