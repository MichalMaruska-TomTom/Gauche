;;;
;;; control.port - Ports as communication channel
;;;
;;;   Copyright (c) 2023  Shiro Kawai  <shiro@acm.org>
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

;; EXPERIMENTAL

;; This module provides utilities that uses ports for communication
;; between threads and/or processes.

(define-module control.port
  (use data.queue)
  (use gauche.threads)
  (use gauche.uvector)
  (use gauche.vport)

  (export open-tapping-port)
  )
(select-module control.port)

;;=======================================================
;; Tapping port
;;

;; EXPERIMENTAL - This depends on gauche.threads and data.queue, so we
;; might want to split this to a separate module (either util.* or control.*)
;; but not sure which.

;; open-tapping-port iport oport :key close-output
;;   Run a thread that copies data from iport and oport.  Returns an
;;   input port, from which you can read the data read from iport;
;;   that is, you can 'tap' the data stream flowing from iport to oport.
;;
;;   Note that the tapped data is accumulated until it si read from
;;   the returned port.  If you're done with tapping, you can close the
;;   returned port to prevent further accumulation of the data.
;;   The dataflow from iport to oport keeps running until iport reaches EOF.
;;
;;   Once iport reaches EOF, the thread terminates.  Additionally, oport is
;;   closed if :close-output argument is #t.
;;
;;   :unit is similar to the :unit argument of copy-port.

(define (open-tapping-port iport oport :key (close-output #f)
                                            (unit 0))
  (define mtq (make-mtqueue))
  (define tee-closed #f)
  (define straight-closed #f)
  (define-values (reader writer)
    (case unit
      [(:byte) (values read-byte write-byte)]
      [(:char) (values read-char write-char)]
      [else (assume (and (exact-integer? unit) (>= unit 0))
                    "A nonnegative exact integer, :byte, or :char expected \
                     for unit argument, but got: " unit)
            (let1 chunk-size (if (zero? unit) 4096 unit)
              (values (^[port] (read-uvector <u8vector> chunk-size port))
                      (^[obj port] (write-uvector obj port))))]))
  (define (filler buf)
    (let ([len (u8vector-length buf)]
          [data (dequeue/wait! mtq)])     ;this may block
      (cond [(eof-object? data) data]
            [(<= (u8vector-length data) len)
             (u8vector-copy! buf 0 data 0)
             (u8vector-length data)]
            [else
             (u8vector-copy! buf 0 data 0 len)
             (queue-push/wait! mtq (uvector-alias <u8vector> data len))
             len])))
  (define (closer)
    (set! tee-closed #t))
  (define (handler)
    (let loop ()
      ;; We ignore error; it prevents the thread to exit inadvertently.
      ;; However, it has a risk to go into busy loop if error condition
      ;; persists.  We'll revisit this later.
      (unless (guard [e (else #f)]
                (let1 data (reader iport)
                  (unless tee-closed
                    (enqueue/wait! mtq data))
                  (unless straight-closed
                    (if (eof-object? data)
                      (begin
                        (set! straight-closed #t)
                        (when close-output
                          (close-output-port oport)))
                      (writer data oport)))
                  (when (eof-object? data)
                    (close-input-port iport))
                  (and tee-closed straight-closed)))
        (loop))))
  (thread-start! (make-thread handler))
  (make <buffered-input-port> :fill filler :close closer))
