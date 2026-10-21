#!/usr/bin/env bash
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
# Makes the TLS listener's test certificates (WebSocket spec 16.3). Run once; the output is committed:
#   test-ca.pem      the throwaway test CA. Its private key is made in a temporary directory and deleted when this
#                    script exits, so nothing else can ever be signed by it.
#   test-server.pem  the listener's certificate (localhost, 127.0.0.1, ::1), then test-ca.pem: the chain it sends
#   test-server.key  the listener's private key (RSA 2048, unencrypted). A test key: it protects and signs nothing.
# Python's standard library cannot make certificates, so this needs the openssl command line (1.1.1 or later); the
# tests themselves do not. 36500 days: the suite must not expire under itself; regenerate before 2126.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
DAYS=36500

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$DAYS" \
	-subj "/CN=VaRest Test CA (not trusted anywhere)" \
	-addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
	-addext "keyUsage=critical,keyCertSign,cRLSign" \
	-keyout "$WORK/ca.key" -out test-ca.pem

openssl req -newkey rsa:2048 -nodes -sha256 -subj "/CN=localhost" -keyout test-server.key -out "$WORK/server.csr"
cat > "$WORK/server.ext" <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1
EOF
openssl x509 -req -sha256 -days "$DAYS" -in "$WORK/server.csr" -CA test-ca.pem -CAkey "$WORK/ca.key" \
	-CAcreateserial -CAserial "$WORK/ca.srl" -extfile "$WORK/server.ext" -out "$WORK/server.crt"

cat "$WORK/server.crt" test-ca.pem > test-server.pem
chmod 0644 test-ca.pem test-server.pem test-server.key
openssl verify -CAfile test-ca.pem "$WORK/server.crt"
