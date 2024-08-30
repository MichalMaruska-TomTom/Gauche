;;
;; Test gauche.version
;;

(use gauche.test)
(test-start "gauche.version")

(use gauche.version)
(test-module 'gauche.version)

(test* "relnum-compare" '(-1 0 1)
       (list (relnum-compare "1" "3")
             (relnum-compare "1" "1")
             (relnum-compare "3" "1")))

(test* "relnum-compare" '(-1 0 1)
       (list (relnum-compare "1b" "2a")
             (relnum-compare "1b" "1b")
             (relnum-compare "3a" "1b")))

(test* "relnum-compare" '(-1 0 1)
       (list (relnum-compare "b" "1")
             (relnum-compare "b" "b")
             (relnum-compare "1" "b")))

(define (vercmp x y r)
  (test (format #f "version-compare ~a ~a" x y)
        (list r (- r))
        (lambda ()
          (list (version-compare x y)
                (version-compare y x)))))

(vercmp "1" "1" 0)
(vercmp "2.3" "2.3" 0)
(vercmp "2.34.5b-patch3" "2.34.5b-patch3" 0)
(vercmp "20020202-1" "20020202-1" 0)

(vercmp "1" "1.0" -1)
(vercmp "1.0" "1.1" -1)
(vercmp "1.1" "1.1.1" -1)
(vercmp "1.1" "1.1.1.1" -1)
(vercmp "1.0.1" "1.1" -1)
(vercmp "1.1.1" "1.1.2" -1)
(vercmp "1.1.2" "1.2" -1)
(vercmp "1.2" "1.11" -1)

(vercmp "1.2.3" "1.2.3-1" -1)
(vercmp "1.2.3-1" "1.2.3-10" -1)
(vercmp "1.2.3-1" "1.2.4" -1)
(vercmp "1.2.3" "1.2.3a" -1)
(vercmp "1.2.3a" "1.2.3b" -1)
(vercmp "1.2.3a" "1.2.12" -1)

(vercmp "1.2_rc0" "1.2_rc1" -1)
(vercmp "1.2_rc1" "1.2" -1)
(vercmp "1.2" "1.2-patch1" -1)
(vercmp "1.2-patch1" "1.2-patch2" -1)
(vercmp "1.2_pre0" "1.2-patch1" -1)
(vercmp "1.1-patch112" "1.2_alpha" -1)

(vercmp "19990312" "20000801" -1)
(vercmp "20010101-4" "20010101-13" -1)
(vercmp "20011125-2.1" "20011213-2.1" -1)
(vercmp "20011213-1.4" "20011213-1.12" -1)
(vercmp "20011213-1.12" "20011213-3.1" -1)
(vercmp "20011213-1.12_alpha0" "20011213-1.12" -1)
(vercmp "20011213-1.12_alpha0" "20011213-1.12.1" -1)
(vercmp "1.2-p1" "1.2-p1+20241012" -1)
(vercmp "1.2-p1+20250101" "1.2-p1+20241012" 1)
(vercmp "1.2-p1+20250101" "1.2-p2+20241012" -1)

(test* "version=?"  #t (version=? "1.1.12" "1.1.12"))
(test* "version=?"  #f (version=? "1.1.12" "1.1.21"))
(test* "version<?"  #t (version<? "1.1.12" "1.2"))
(test* "version<?"  #f (version<? "1.1.12" "1.1.12"))
(test* "version<?"  #f (version<? "1.1.2" "1.1.1"))
(test* "version<=?" #t (version<=? "1.1.12" "1.2"))
(test* "version<=?" #t (version<=? "1.1.12" "1.1.12"))
(test* "version<=?" #f (version<=? "1.1.2" "1.1.1"))
(test* "version>?"  #f (version>? "1.1.12" "1.2"))
(test* "version>?"  #f (version>? "1.1.12" "1.1.12"))
(test* "version>?"  #t (version>? "1.1.2" "1.1.1"))
(test* "version>=?" #f (version>=? "1.1.12" "1.2"))
(test* "version>=?" #t (version>=? "1.1.12" "1.1.12"))
(test* "version>=?" #t (version>=? "1.1.2" "1.1.1"))

(let ()
  (define (valid? expected spec)
    (test* #"valid-version-spec? ,|spec|" expected (valid-version-spec? spec)))
  (valid? #t "1")
  (valid? #t "1a")
  (valid? #t "1.3")
  (valid? #t '(= "1.3"))
  (valid? #t '(< "1.3"))
  (valid? #t '(> "1.3"))
  (valid? #t '(<= "1.3"))
  (valid? #t '(>= "1.3"))
  (valid? #t '(and))
  (valid? #t '(and "1.3" "1.4"))
  (valid? #t '(or))
  (valid? #t '(or "1.3" "1.4"))
  (valid? #t '(not "1.3"))
  (valid? #t '(and (not "1.3.1")
                   (>= "1.1")))
  (valid? #t '(or (>= "2")
                  (and (< "1.8.1")
                       (>= "1.6.7"))))
  (valid? #f 3.14)
  (valid? #f '(= "1.3" "1.4"))
  (valid? #f '(<< "1.3"))
  (valid? #f '(not))
  (valid? #f '(and not "3.3"))
  )

(let ()
  (define (ok? spec tests)
    (dolist [test tests]
      (test* #"version ~(cadr test) satisfies ~|spec|" (car test)
             (version-satisfy? spec (cadr test)))))

  (ok? "1.2" '((#t "1.2") (#f "1.2.1") (#f "2.2")))
  (ok? '(= "1.2") '((#t "1.2") (#f "1.2.1") (#f "2.2")))
  (ok? '(< "1.2") '((#f "1.2") (#f "1.2.1") (#f "1.9") (#t "1.1") (#t "0.9")))
  (ok? '(and (not "1.3.1") (>= "1.1"))
       '((#t "1.1") (#t "1.2.1") (#t "1.3") (#f "1.3.1") (#t "1.4")
         (#f "1.0.9")))
  (ok? '(or (>= "2") (and (< "1.8.1") (>= "1.6.7")))
       '((#f "1.6") (#t "1.7.1") (#f "1.8.1") (#f "1.10") (#t "2.0")))
  )

(test-end)
