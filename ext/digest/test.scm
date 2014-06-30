(use gauche.test)
(test-start "digest framework")

(include "test-md5")
(test-end)


(test-start "digest sha")
(include "test-sha")
(test-end)

(test-start "digest hmac")
(include "test-hmac")
(test-end)
