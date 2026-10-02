# Local HTTP client TLS fixtures

These certificates and this private key are public, test-only fixtures. Never use
them for a deployed service or install either CA in an operating-system trust store.
Tests pass ca.pem to their own child process through SSL_CERT_FILE.

The server uses a self-signed certificate, repeated in ca.pem as the explicit
trust anchor. This avoids a separate leaf/intermediate chain needing an online
revocation service in these entirely local tests; production Schannel revocation
policy is unchanged. Its only IP SAN is 127.0.0.1, so requests to localhost
deliberately exercise hostname rejection. wrong-ca.pem is an independent CA
used to test trust rejection and environment precedence. Its key was discarded.

Certificates were generated with OpenSSL on 2026-10-02 for 3650 days. The test
runner uses only Python's standard ssl module; no OpenSSL command is required
on Windows or at test runtime. Replace these fixtures together before expiry.
