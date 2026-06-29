# Bygger .eap-fila for flow_raw_backup med Axis ACAP Native SDK.
#
# Standard er armv7hf (ARTPEC-7, som M2035-LE etter all sannsynlighet er).
# Bygg for aarch64 ved:  docker build --build-arg ARCH=aarch64 ...
ARG ARCH=armv7hf
ARG VERSION=12.0.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION} AS builder

WORKDIR /opt/app
COPY ./app .

# Henter inn kryss-kompilator-miljøet og bygger + pakker .eap-fila.
RUN . /opt/axis/acapsdk/environment-setup* && acap-build .
