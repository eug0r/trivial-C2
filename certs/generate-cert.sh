#!/bin/bash
set -e

CERT_DIR="$(dirname "$0")"
KEY_FILE="$CERT_DIR/server-priv.pem"
CRT_FILE="$CERT_DIR/server-cert.pem"

rm -f "$KEY_FILE" "$CRT_FILE"


openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$KEY_FILE" \
    -out "$CRT_FILE" \
    -days 365 \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

echo "Generated:"
echo "  private key: $KEY_FILE"
echo "  certificate: $CRT_FILE"
