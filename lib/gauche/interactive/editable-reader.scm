;;;
;;; gauche.interactive.editable-reader
;;;
;;;   Copyright (c) 2016-2021  Shiro Kawai  <shiro@acm.org>
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

;; This module is autoloaded from gauche.interactive.

(define-module gauche.interactive.editable-reader
  (use srfi-13)
  (use text.console)
  (use util.match)
  (use file.util)
  (export make-editable-reader))
(select-module gauche.interactive.editable-reader)

;; Delay loading line-edit in case editable console isn't available.
(autoload text.line-edit <line-edit-context> read-line/edit
          load-line-edit-history save-line-edit-history)

;; Internal API, to be used by gauche.interactive.
;; Because of toplevel commands, we can't just provide alternative 'read'
;; procedure.  Instead, this returns three procedures and
;; one <line-edit-context> object. three procedures are one for 'read',
;; one for 'read-line', and another for skipping trailing whitespaces.
;  They are suitable to be passed to make-repl-reader.
;; NB: Currently we assume we use default console.  Might be useful
;; to allow other console (e.g. over pty).
(define (make-editable-reader get-prompt-string hist-file)
  (if-let1 console (make-default-console :if-not-available #f)
    (let ([ctx (make <line-edit-context>
                 :console console
                 :prompt (^[] (display (get-prompt-string)))
                 :input-continues (^s (not (%input-complete? s)))
                 :completion-lister list-completions)]
          [buffer (open-input-string "")])
      (define (read-1 reader)
        (rec (try)
          (let1 x (reader buffer)
            (if (eof-object? x)
              (let1 input (read-line/edit ctx)
                (if (eof-object? input)
                  (begin                ; EOF is typed
                    (if hist-file
                      (save-line-edit-history ctx (expand-path hist-file)))
                    input)
                  (begin
                    (set! buffer (open-input-string (string-append input "\n")))
                    (try))))
              x))))
      (when hist-file
        (load-line-edit-history ctx (expand-path hist-file)))
      (values (read-1 read)
              (read-1 read-line)
              (^[] (consume-trailing-whitespaces buffer))
              ctx))
    (values #f #f #f #f)))                  ;no default console

;; We have to handle both toplevel command (begins with comma, ends with
;; newline) and the complete sexp.
(define (%input-complete? s)
  (if-let1 m (#/^\s*,(.*)/ s)
    (not (#/^\s*$/ (m 1)))
    (complete-sexp? s)))

;; Completion (EXPERIMENTAL)
;; Some questions to consider
;;   - Should we build a trie for quick access to prefix-matching symbols?
;;     If we do so, how to keep it updated?
;;   - Do we want to complete w-i-f-f to with-input-from-file?
;;   - We need to access runtime 'current-module' info, which currently isn't
;;     a public API.  Should we have an official API for that?
;;   - The routine is pretty similar to apropos.  Should we refactor?
(define (list-completions word buf start end)
  (let ([mod ((with-module gauche.internal vm-current-module))]
        [visited '()]
        [hits (make-hash-table 'string=?)])
    (define (search m)
      (unless (memq m visited)
        (push! visited m)
        ($ hash-table-for-each (module-table m)
           (^[sym _]
             (let1 s (symbol->string sym)
               (when (string-prefix? word s)
                 (hash-table-put! hits s #t)))))))

    (search mod)
    (dolist [m (module-imports mod)]
      (for-each search (module-precedence-list m)))
    (sort (hash-table-keys hits))))
