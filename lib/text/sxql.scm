;;;
;;; sxql.scm - S-expression query language
;;;  
;;;   Copyright (c) 2005 Shiro Kawai, All rights reserved.
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
;;;  $Id: sxql.scm,v 1.1 2005-07-10 11:03:48 shirok Exp $
;;;

;; *EXPERIMENTAL*
;; This module will define an S-expr notation of SQL and conversion
;; routines from/to the standard SQL syntax.
;; For the time being, we only use tokenizer which is used by dbi/dbd
;; modules.

(define-module text.sxql
  (use srfi-13)
  (export <sql-parse-error>
          ))
(select-module text.sxql)

;;;=================================================================
;;; SxQL
;;;

;;;-----------------------------------------------------------------
;;; Conditions
;;;

(define-condition-type <sql-parse-error> <error> #f
  (sql-string))         ;; original SQL string

;;;-----------------------------------------------------------------
;;; Full Tokenizer
;;;   Returns a list of tokens.  Each token is either one of the
;;;   following form.
;;;   
;;;    <symbol>              Regular identifier, or special delimiter:
;;;                          + - * / < = > <> <= >= ? ||
;;;    <character>           Special delimiter: 
;;;                          #\, #\. #\: #\( #\) #\;
;;;    (delimited <string>)  Delimited identifier
;;;    (string    <string>)  Character string literal
;;;    (number    <string>)  Numeric literal
;;;    (bitstring <string>)  Binary string.  <string> is like "01101"
;;;    (hexstring <string>)  Binary string.  <string> is like "3AD20"
;;;

(define (sql-tokenize sql-string)
  ;;
  ;; skip whitespaces
  ;;
  (define (skip-ws s)
    (cond ((#/^\s+(--)?/ s)
           => (lambda (m) (if (m 1) (skip-comment s) (m 'after))))
          (else s)))
  (define (skip-comment s)
    (cond ((string-scan s "\n" 'after))
          (else "")))
  ;;
  ;; main dispatcher
  ;;
  (define (entry s r)
    (let1 s (skip-ws s)
      (if (string-null? s)
        (reverse! r)
        (let1 c (string-ref s 0)
          (cond
           ((char=? c #\') (scan-string s r))
           ((char=? c #\") (scan-delimited s r))
           ((#/^[+-]?(?:\d+(?:\.\d*)?|(?:\.\d+))(?:[eE][+-]?\d+)?/ s)
            => (lambda (m)
                 (entry (m 'after) (cons `(number ,(m)) r))))
           ((#/^(<>|<=|>=|\|\|)/ s)
            (entry (string-drop s 2)
                   (cons (string->symbol (string-take s 2)) r)))
           ((#/^[bB]'/ s) (scan-bitstring s r))
           ((#/^[xX]'/ s) (scan-hexstring s r))
           ((char-set-contains? #[-+*/<=>?] c)
            (entry (string-drop s 1) (cons (string->symbol (string c)) r)))
           ((char-set-contains? #[,.:()\;] c)
            (entry (string-drop s 1) (cons c r)))
           ((#/^\w+/ s)
            => (lambda (m)
                 (entry (m 'after) (cons (string->symbol (m)) r))))
           (else (e "invalid SQL token beginning with ~s in: ~s"
                    c sql-string)))))))
  ;;
  ;; subscanners
  ;;
  (define (scan-string s r)
    (cond ((#/^'((?:[^']|'')*)'/ s)
           => (lambda (m)
                (entry (m 'after)
                       (cons `(string ,(regexp-replace-all #/''/ (m 1) "'"))
                             r))))
          (else
           (e "unterminated string literal in SQL: ~s" sql-string))))
  (define (scan-delimited s r)
    (cond ((#/^\"((?:[^\"]|\"\")*)\"/ s)
           => (lambda (m)
                (entry (m 'after)
                       (cons `(string ,(regexp-replace-all #/""/ (m 1) "\""))
                             r))))
          (else
           (e "unterminated delimited identifier in SQL: ~s" sql-string))))
  (define (scan-bitstring s r)
    (cond ((#/^.'([01]+)'/ s)
           => (lambda (m)
                (entry (m 'after)
                       (cons `(bitstring ,(m 1)) r))))
          (else
           (e "unterminated bitstring literal in SQL: ~s" sql-string))))
  (define (scan-hexstring s r)
    (cond ((#/^.'([\da-fA-F]+)'/ s)
           => (lambda (m)
                (entry (m 'after)
                       (cons `(hexstring ,(m 1)) r))))
          (else
           (e "unterminated bitstring literal in SQL: ~s" sql-string))))
  ;;
  ;; raising an error
  ;;
  (define (e fmt . args)
    (raise (condition (<sql-parse-error> (message (apply format fmt args))
                                         (sql-string sql-string)))))
  ;;
  ;; main entry
  ;;
  (entry (skip-ws sql-string) '()))

(provide "text/sxql")

