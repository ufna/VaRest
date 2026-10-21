<!-- Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved. -->
# Test certificates for Tools/testserver (test-only)

These files let the test server's TLS listener (`server.py --tls-port`) serve `https://` and `wss://` to the VaRest
automation tests (WebSocket spec 16.3). They protect nothing and must never be trusted outside these tests.

| File | What it is |
| --- | --- |
| `test-ca.pem` | A throwaway CA, `CN=VaRest Test CA (not trusted anywhere)`. Its private key was made in a temporary directory and destroyed right after it signed the leaf, so this CA can never issue anything else. |
| `test-server.pem` | The listener's certificate (SAN `DNS:localhost`, `IP:127.0.0.1`, `IP:::1`), followed by `test-ca.pem`: the chain the listener sends. |
| `test-server.key` | The listener's private key (RSA 2048, unencrypted). It is committed on purpose: a secret scanner flags it, and it is a test key that serves only `localhost`. |

All three are valid for 36500 days. To regenerate them (needs the `openssl` command line, 1.1.1 or later; the tests do
not): run `Tools/testserver/certs/make_certs.sh`, then commit the three files. Tests that verify the listener trust
`test-ca.pem` (`GET /tls/ca.pem` serves it); tests that only need TLS allow self-signed chains.
