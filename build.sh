#!/usr/bin/env bash
# Bygger flow_raw_backup til en .eap-fil du laster opp på kameraet.
#
# Bruk:
#   ./build.sh            # bygger for armv7hf (M2035-LE / ARTPEC-7)
#   ./build.sh aarch64    # bygger for ARTPEC-8 / CV25-kameraer
#
# Krever kun at Docker er installert.
set -euo pipefail

ARCH="${1:-armv7hf}"
IMG="flow_raw_backup_build:${ARCH}"

echo ">> Bygger ACAP for ARCH=${ARCH} ..."
docker build --build-arg ARCH="${ARCH}" -t "${IMG}" .

echo ">> Henter ut .eap-fila ..."
CID="$(docker create "${IMG}")"
rm -rf ./_out && mkdir -p ./_out dist
docker cp "${CID}:/opt/app" ./_out >/dev/null
docker rm "${CID}" >/dev/null

cp ./_out/*.eap dist/ 2>/dev/null || cp ./_out/app/*.eap dist/ 2>/dev/null || true
rm -rf ./_out

echo ""
echo ">> Ferdig. Resultat:"
ls -1 dist/*.eap
echo ""
echo "Last opp .eap-fila på kameraet under Apps -> Add app (eller med eap-install.sh)."
