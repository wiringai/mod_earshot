#!/usr/bin/env bash
# Generate a throwaway CA + server cert (CN/SAN=localhost) + client cert for the
# mTLS end-to-end test. Also a SECOND, unrelated CA to prove EARSHOT_TLS_CA is
# actually enforced. Generated *.pem/*.key are gitignored — never commit keys.
set -euo pipefail
cd "$(dirname "$0")"
DAYS=3650

# Trust anchor for the test
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days "$DAYS" \
  -subj "/CN=earshot-test-ca" 2>/dev/null

# Server cert — CN + SAN = localhost so hostname verification passes
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr \
  -subj "/CN=localhost" 2>/dev/null
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
  -out server.pem -days "$DAYS" -extfile <(printf "subjectAltName=DNS:localhost") 2>/dev/null

# Client cert — what earshot presents for mTLS (unencrypted key, as required)
openssl req -newkey rsa:2048 -nodes -keyout client.key -out client.csr \
  -subj "/CN=earshot-client" 2>/dev/null
openssl x509 -req -in client.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
  -out client.pem -days "$DAYS" 2>/dev/null

# An unrelated CA — point EARSHOT_TLS_CA at this to prove verification is enforced
openssl req -x509 -newkey rsa:2048 -nodes -keyout bogus-ca.key -out bogus-ca.pem \
  -days "$DAYS" -subj "/CN=bogus-ca" 2>/dev/null

rm -f server.csr client.csr ca.srl
echo "Generated in $(pwd):"
echo "  ca.pem                 trust anchor (EARSHOT_TLS_CA)"
echo "  server.pem server.key  server identity (CN=localhost)"
echo "  client.pem client.key  earshot's client identity (EARSHOT_TLS_CLIENT_CERT/_KEY)"
echo "  bogus-ca.pem           unrelated CA, for the negative test"
