#!/bin/sh

set -e

PKG_NAME=rekkord
PKG_AUTHOR="Niels Martignène <niels.martignene@protonmail.com>"
PKG_DESCRIPTION="Backup tool with deduplication and asymmetric encryption"
PKG_DEPENDENCIES=""
PKG_LICENSE=AGPL-3.0-or-later
PKG_ARCHITECTURES="amd64 i386 arm64"

SCRIPT_PATH=src/rekkord/dist/linux/debian.sh
VERSION_TARGET=rekkord
DOCKER_IMAGE=debian11

build() {
    ./bootstrap.sh
    ./felix -pFast --host=$1 rekkord

    install -D -m0755 bin/Fast/rekkord ${ROOT_DIR}/usr/bin/rekkord
}

cd "$(dirname $0)/../../../.."
. deploy/package/debian/package.sh