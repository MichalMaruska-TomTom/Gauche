;;;
;;; gauche.record - record implementation
;;;  
;;;   Copyright (c) 2010  Shiro Kawai  <shiro@acm.org>
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

(define-module gauche.record
  (use gauche.sequence)
  (use srfi-1)
  (use util.match)

  (export <record-meta> <record>
          <pseudo-record-meta> pseudo-rtd
          record? record-rtd rtd-name rtd-parent
          rtd-field-names rtd-all-field-names rtd-field-mutable?
          make-rtd rtd? rtd-constructor rtd-predicate rtd-accessor rtd-mutator
          define-record-type)
  )
(select-module gauche.record)

;; <record> is srfi-9 and srif-99 compatible record types, integrated
;; into Gauche object system, and implemented for efficiency.
;;
;; rtd is a Gauche class with metaclass <record-meta>.  It has more
;; restricted semantics than the ordinary Gauche classes:
;;
;;  - It cannot be redefined - definining the class of the same name
;;    just creates a new class, but the original class and its instances
;;    remain intact.
;;  - The accessors are inlined - so they are fast, but if you redefine
;;    the record, procedures that are using accessors and mutators must
;;    be recompiled.
;;  - The record class must form a single implementation inheritance.

;;  Pseudo records can be used just like records in the code, but at
;;  runtime you cannot distinguish them from ordinary lists or vectors.
;;  They have some advantages:
;;
;;   - Fast.  Operations on pseudo records can be expanded into operations
;;     on primitive data structures (e.g. car, vector-ref, etc), which
;;     will be compiled into efficient VM instructions.
;;   - Less module interdependency.  You can ask callers to pass in
;;     the data by a vector or a list, instead of asking them to import your
;;     record definition.  It is less safe but more flexible, especially
;;     the caller is a foreign entity.

;;;
;;; Infrastructure
;;;

(define-class <record-meta> (<class>)
  ((field-specs :init-keyword :field-specs)))
(define-class <record> () () :metaclass <record-meta>)

(define-class <pseudo-record-meta> (<record-meta>) ())
(define-class <pseudo-record> () () :metaclass <pseudo-record-meta>)

(define-class <vector-pseudo-record-meta> (<pseudo-record-meta>) ())
(define-class <vector-pseudo-record> () () :metaclass <vector-pseudo-record-meta>)
(define-class <list-pseudo-record-meta> (<pseudo-record-meta>) ())
(define-class <list-pseudo-record> () () :metaclass <list-pseudo-record-meta>)

(define-method pseudo-rtd ((class <vector-meta>))
  <vector-pseudo-record>)
(define-method pseudo-rtd ((class <vector-pseudo-record-meta>))
  <vector-pseudo-record>)
(define-method pseudo-rtd ((class <list-meta>))
  <list-pseudo-record>)
(define-method pseudo-rtd ((class <list-pseudo-record-meta>))
  <list-pseudo-record>)
(define-method pseudo-rtd (other)
  (error "pseudo-rtd requires a class object <vector>, <list>, \
          or other pseudo-rtd, but got" other))

;; We just collect ancestor's slots, ignoring duplicate slot names.
;; R6RS records require the same name slot merely shadows ancestors' one,
;; not changing the index of the fields.
;; NB: we make the direct slots come first, so that access via
;; slot-ref/slot-set! prefer direct slots to inherited slots
;; with the same name.
(define-method compute-slots ((class <record-meta>))
  (fold (^(c r) (append (slot-ref c'direct-slots) r))
        '() (reverse (slot-ref class'cpl))))

(define-method compute-get-n-set ((class <record-meta>) slot)
  (let1 s (compute-slot-accessor class slot (next-method))
    (if (slot-definition-option slot :immutable #f)
      `(,(^o (slot-ref-using-accessor o s))
        ,(^(o v) (errorf "slot ~a of ~a is immutable"
                         (slot-definition-name slot) o))
        ,(^o (slot-bound-using-accessor? o s)))
      s)))

(define (fieldspecs->slotspecs specs offset)
  (define (id? x) (or (symbol? x) (identifier? x)))
  (unless (vector? specs)
    (error "make-rtd: fieldspecs must be a vector, but got" specs))
  (map-with-index
   (^(k spec)
     (match spec
       [('mutable   (? id? name))
        `(,(unwrap-syntax name) :index ,(+ k offset))]
       [('immutable (? id? name))
        `(,(unwrap-syntax name) :immutable #t :index ,(+ k offset))]
       [(? id? name) `(,(unwrap-syntax name) :index ,(+ k offset))]
       [other (error "make-rtd: invalid field spec:" other)]))
   specs))

(define (%check-rtd obj)
  (unless (rtd? obj) (error "rtd required, bot got" obj)))

;;;
;;; Inspection layer
;;;

(define-inline (record? obj) (is-a? obj <record>))

(define-inline (record-rtd obj)
  (unless (record? obj) (error "record required, but got" obj))
  (class-of obj))

(define (rtd-name rtd) (%check-rtd rtd) (class-name rtd))

(define (rtd-parent rtd)
  (%check-rtd rtd)
  (find (^c (and (is-a? c <record-meta>) (not (eq? c <record>))))
        (cdr (class-precedence-list rtd))))

(define (rtd-field-names rtd)
  (%check-rtd rtd)
  (map-to <vector> slot-definition-name (class-direct-slots rtd)))

(define (rtd-all-field-names rtd)
  (%check-rtd rtd)
  (let loop ((rtd (rtd-parent rtd))
             (r (map slot-definition-name (class-direct-slots rtd))))
    (if rtd
      (loop (rtd-parent rtd)
            (fold-right (^(s r) (cons (slot-definition-name s) r))
                        r (class-direct-slots rtd)))
      (list->vector r))))

(define (rtd-field-mutable? rtd field)
  (%check-rtd rtd)
  (cond [(assq field (class-slots rtd))
         => (^s (not (slot-definition-option s :immutable #f)))]
        [else (error "rtd-mutable?: ~a does not have a slot ~a" rtd field)]))

;;;
;;; Procedural layer
;;;

(define (make-rtd name fieldspecs :optional (parent #f) :rest opts)
  (make (if parent (class-of parent) <record-meta>)
    :name name :field-specs fieldspecs :metaclass <record-meta>
    :supers (list (or parent <record>))
    :slots (fieldspecs->slotspecs
            fieldspecs (if parent (length (class-slots parent)) 0))))

(define (rtd? obj) (is-a? obj <record-meta>))

;; For conciseness.  We need to use macros (for now) to ensure Gauche compiler
;; optimize the gref away---a kludge not recommended in general.
(define-macro (%make)  '(with-module gauche.object %make-record))
(define-macro (%makev) '(with-module gauche.object %make-recordv))

;; We dispatch by the number of slots to initialize, for fixed-argument
;; lambdas can be optimized more easily.
(define-macro (define-ctor-generator name 01-maker body-maker rest-maker)
  `(define-macro (,name rtd len)
     (define precalc-args 10)
     (define tmps (map (^_(gensym)) (iota (+ precalc-args 1))))
     `(case ,len
        [(0) ,(let1 vars '() `(lambda ,vars ,,01-maker))]
        [(1) ,(let1 vars `(,(car tmps)) `(lambda ,vars ,,01-maker))]
        ,@(map (^n (let1 vars (drop tmps (- precalc-args n -1))
                     `[(,n) (lambda ,vars ,,body-maker)]))
               (iota (- precalc-args 1) 2))
        [else (lambda (,@(cdr tmps) . ,(car tmps))
                ,,rest-maker)])))

(define-macro (define-ctor-generators default-name custom-name
                make1 make* makev)
  `(begin
     (define-ctor-generator ,default-name ,make1 ,make1 ,make*)
     (define-ctor-generator ,custom-name ,make1
       (let1 argv (gensym)
         `(let1 ,argv (make-vector nfields)
            ,@(map-with-index (^(i v)
                                `(vector-set! ,argv (vector-ref mapvec ,i) ,v))
                              vars)
            ,,makev))
       (let ([argv (gensym)] [i (gensym)] [restvar (car tmps)])
         `(let1 ,argv (make-vector nfields)
            ,@(map-with-index (^(i v)
                                `(vector-set! ,argv (vector-ref mapvec ,i) ,v))
                              (cdr tmps))
            (do ([,restvar ,restvar (cdr ,restvar)]
                 [,i ,precalc-args (+ ,i 1)])
                [(null? ,restvar)]
              (vector-set! ,argv (vector-ref mapvec ,i) (car ,restvar)))
            ,,makev)))))

(define-ctor-generators %record-ctor-default %record-ctor-custom
  `((%make) ,rtd ,@vars)
  `(apply (%make) ,rtd ,@(cdr tmps) ,(car tmps))
  `((%makev) ,rtd ,argv))

(define-ctor-generators %vector-ctor-default %vector-ctor-custom
  `(vector ,@vars)
  `(apply vector ,@(cdr tmps) ,(car tmps))
  argv)

;; Returns a vector where V[k] = i means k-th argument of the constructor
;; initializes i-th field.
(define (%calculate-field-mapvec allnames fieldspecs)
  (define (bad f)
    (error "rtd-constructor: field-specs contains unrecognized field name:" f))
  (let1 cat (reverse (map-with-index xcons allnames))
    (map-to <vector> (^f (cond [(assq f cat) => cdr] [else (bad f)]))
            fieldspecs)))

(define-method rtd-constructor ((rtd <record-meta>) . rest)
  (%check-rtd rtd)
  (if (null? rest)
    (%record-ctor-default rtd (length (slot-ref rtd'slots)))
    (let1 all-names (rtd-all-field-names rtd)
      (let ([mapvec  (%calculate-field-mapvec all-names (car rest))]
            [nfields (vector-length all-names)])
        (%record-ctor-custom rtd (vector-length (car rest)))))))

(define-method rtd-constructor ((rtd <vector-pseudo-record-meta>) . rest)
  (%check-rtd rtd)
  (if (null? rest)
    (%vector-ctor-default rtd (length (slot-ref rtd'slots)))
    (let1 all-names (rtd-all-field-names rtd)
      (let ([mapvec  (%calculate-field-mapvec all-names (car rest))]
            [nfields (vector-length all-names)])
        (%vector-ctor-custom rtd (vector-length (car rest)))))))

(define-method rtd-predicate ((rtd <record-meta>)) (^o (is-a? o rtd)))
(define-method rtd-predicate ((rtd <pseudo-record-meta>))
  (errorf "pseudo record type ~s cannot have a predicate" rtd))

;; returns (index immutable?)
(define (%get-slot-index rtd field modify?)
  (%check-rtd rtd)
  (cond [(assq field (class-slots rtd))
         => (^s (values (slot-definition-option s :index)
                        (slot-definition-option s :immutable #f)))]
        [else (errorf "record ~s does not have a slot named ~s" rtd field)]))

(define-method rtd-accessor ((rtd <record-meta>) field)
  (receive (k immutable?) (%get-slot-index rtd field #f)
    (if immutable?
      (^o ((with-module gauche.object %record-ref) rtd o k))
      (getter-with-setter
       (^o ((with-module gauche.object %record-ref) rtd o k))
       (^(o v) ((with-module gauche.object %record-set!) rtd o k v))))))

(define-method rtd-accessor ((rtd <vector-pseudo-record-meta>) field)
  (receive (k immutable?) (%get-slot-index rtd field #f)
    (if immutable?
      (^o (vector-ref o k))
      (getter-with-setter
       (^o (vector-ref o k))
       (^(o v) (vector-set! o k v))))))
       
(define-method rtd-mutator ((rtd <record-meta>) field)
  (receive (k immutable?) (%get-slot-index rtd field #t)
    (when immutable?
      (errorf "slot ~a of record ~s is immutable" field rtd))
    (^(o v) ((with-module gauche.object %record-set!) rtd o k v))))

(define-method rtd-mutator ((rtd <vector-pseudo-record-meta>) field)
  (receive (k immutable?) (%get-slot-index rtd field #t)
    (when immutable?
      (errorf "slot ~a of record ~s is immutable" field rtd))
    (^(o v) (vector-set! o k v))))

;;;
;;; Syntactic layer
;;;

(define-macro (define-record-type type-spec ctor-spec pred-spec . field-specs)
  (define (->id x) ((with-module gauche.internal make-identifier) x
                    (find-module 'gauche.record) '()))
  (define (id? x)  (or (symbol? x) (identifier? x)))
  (define (sym x)  (string->symbol x))
  (define %make (->id 'make-rtd))
  (define %ctor (->id 'rtd-constructor))
  (define %pred (->id 'rtd-predicate))
  (define %asor (->id 'rtd-accessor))
  (define %mtor (->id 'rtd-mutator))
  (define tmp   (gensym))
  (define (build-field-spec)
    (map-to <vector> (match-lambda
                       [((? id? f) a s) f]
                       [((? id? f) a) `(immutable ,f)]
                       [((? id? f)) f]
                       [(? id? f) `(immutable ,f)]
                       [x (error "invalid field spec:" x)])
            field-specs))
  (define (build-def typename parent)
    `(define-inline ,typename
       (,%make ',typename ,(build-field-spec) ,@(if parent `(,parent) '()))))
  (define (build-ctor typename)
    (match ctor-spec
      [#f '()]
      [#t `((define-inline ,(sym #`"make-,typename") (,%ctor ,typename)))]
      [((? id? ctor-name) field ...)
       `((define-inline ,ctor-name (,%ctor ,typename ,(list->vector field))))]
      [(? id? ctor-name)
       `((define-inline ,ctor-name (,%ctor ,typename)))]
      [x (error "invalid constructor spec" ctor-spec)]))
  (define (build-pred typename)
    (match pred-spec
      [#f '()]
      [#t `((define-inline (,(sym #`",|typename|?") ,tmp)
              ((,%pred ,typename) ,tmp)))]
      [(? id? pred-name)
       `((define-inline (,pred-name ,tmp) ((,%pred ,typename) ,tmp)))]
      [x (error "invalid predicate spec" pred-spec)]))
  (define (build-accessors typename)
    (map (match-lambda
           [(f a . _) `(define-inline ,a (,%asor ,typename ',f))]
           [(f)       `(define-inline ,(sym #`",|typename|-,f")
                         (,%asor ,typename ',f))]
           [f         `(define-inline ,(sym #`",|typename|-,f")
                         (,%asor ,typename ',f))])
         field-specs))
  (define (build-mutators typename)
    (append-map (match-lambda
                  [(f a m) `((define-inline ,m (,%mtor ,typename ',f)))]
                  [(f)     `((define-inline ,(sym #`",|typename|-,|f|-set!")
                               (,%mtor ,typename ',f)))]
                  [_ '()])
                field-specs))
    
  (receive (typename parent) (match type-spec
                               [((? id? name) parent) (values name parent)]
                               [(? id? name) (values name #f)]
                               [x (error "invalid type-spec" type-spec)])
    `(begin ,(build-def typename parent)
            ,@(build-ctor typename)
            ,@(build-pred typename)
            ,@(build-accessors typename)
            ,@(build-mutators typename)))
    )
