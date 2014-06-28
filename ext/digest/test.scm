(use gauche.test)
(test-start "digest framework")

(use util.digest)
(test-module 'util.digest)

;; digest-hexify with uvector
(test* "digest-hexify"
       "0123456789abcdef"
       (digest-hexify '#u8(1 #x23 #x45 #x67 #x89 #xab #xcd #xef)))

(include "test-md5")
(test-end)


(test-start "digest sha")
(include "test-sha")
(test-end)

(test-start "digest hmac")
(include "test-hmac")
(test-end)
