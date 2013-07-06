;;;
;;; vecutil.scm - Auxiliary vector utilities.  Autoloaded.
;;;
;;;   Copyright (c) 2013  Shiro Kawai  <shiro@acm.org>
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

(define-module gauche.vecutil
  (export vector-tabulate vector-map vector-map! vector-for-each
          vector-map-with-index vector-map-with-index!
          vector-for-each-with-index))
(select-module gauche.vecutil)

;; like list-tabulate
(define-inline (vector-tabulate len proc)
  (rlet1 rvec (make-vector len)
    (dotimes [i len] (vector-set! rvec i (proc i)))))

(define-inline (%vector-update! vec len proc)
  (dotimes [i len] (vector-set! vec i (proc i))))

;; R7RS vector-map
(define (vector-map proc vec . more)
  (check-arg vector? vec)
  (if (null? more)
    (vector-tabulate (vector-length vec) (^i (proc (vector-ref vec i))))
    (let1 vecs (cons vec more)
      (vector-tabulate (apply min (map vector-length vecs))
                       (^i (apply proc (map (^v (vector-ref v i)) vecs)))))))

(define (vector-map! proc vec . more)
  (check-arg vector? vec)
  (if (null? more)
    (%vector-update! vec (vector-length vec) (^i (proc (vector-ref vec i))))
    (let1 vecs (cons vec more)
      (%vector-update! vec (apply min (map vector-length vecs))
                       (^i (apply proc (map (^v (vector-ref v i)) vecs)))))))
  
;; srfi-43 vector-map.  passing the index to PROC as the first arg.
(define (vector-map-with-index proc vec . more)
  (check-arg vector? vec)
  (if (null? more)
    (vector-tabulate (vector-length vec) (^i (proc i (vector-ref vec i))))
    (let* ([vecs (cons vec more)]
           [len (apply min (map vector-length vecs))])
      (vector-tabulate (apply min (map vector-length vecs))
                       (^i (apply proc i (map (^v (vector-ref v i)) vecs)))))))

;; srfi-43 vector-map!
(define (vector-map-with-index! proc vec . more)
  (check-arg vector? vec)
  (if (null? more)
    (%vector-update! vec (vector-length vec) (^i (proc i (vector-ref vec i))))
    (let1 vecs (cons vec more)
      (%vector-update! vec (apply min (map vector-length vecs))
                       (^i (apply proc i (map (^v (vector-ref v i)) vecs)))))))

;; R7RS vector-for-each
(define (vector-for-each proc vec . more)
  (check-arg vector? vec)
  (if (null? more)
    (dotimes [i (vector-length vec)] (proc (vector-ref vec i)))
    (let1 vecs (cons vec more)
      (dotimes [i (apply min (map vector-length vecs))]
        (apply proc (map (^v (vector v i)) vecs))))))

;; srfi-43 vector-for-each
(define (vector-for-each-with-index proc vec . more)
  (check-arg vector? vec)
  (if (null? more)
    (dotimes [i (vector-length vec)] (proc i (vector-ref vec i)))
    (let1 vecs (cons vec more)
      (dotimes [i (apply min (map vector-length vecs))]
        (apply proc i (map (^v (vector v i)) vecs))))))