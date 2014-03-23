#!/usr/bin/env bash
# 
# Simple build script crafted for the APrinter project to support multiple 
# architecture targets and build actions using an elegant commandline.
# 
# Copyright (c) 2014 Bernard `Guyzmo` Pratz
# All rights reserved.
# 
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
# DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
# 
#####################################################################################
# MAIN BUILD SCRIPT

#####################################################################################
# configure paths

ROOT=$(dirname $0)
SPATH=$ROOT/aprinter/printer
BUILD=$ROOT/build
DEPS=$ROOT/depends

#####################################################################################
# Guard to avoid running this script with another shell than bash

[ -z "$BASH_VERSION" ] && (
    echo "This script shall be ran only using bash"
    exit 1
)

set -e

#####################################################################################
# source any user configuration

[ -f config.sh ] && source config.sh

#####################################################################################
# load build functions

TARGETS=()

for f in scripts/*; do
    source $f
done

#####################################################################################
# main

TARGETS_STR=$(IFS=\| ; echo "${TARGETS[*]}")
USAGE="Usage: $0 (${TARGETS_STR}|all) [-v] (install|build|upload [-p <port>|-b <baudrate>|-B <bitrate>]|clean|flush)"

if [ $# -lt 2 ]; then
    echo $USAGE
    exit 1
fi

# PARSE BUILD TARGET

if [ "$1" != "all" ]; then
    in_array $1 ${TARGETS[*]} || (
        echo $USAGE
        echo "Error: unknown target $1"
        exit 1
    )
fi

DEST=$1

# PARSE BUILD ACTION

ACT=()

shift

while true; do
    case "$1" in
        -v|--verbose)
            shift;
            V="set -x"
            ;;
        install)
            shift;
            ACT+=("install")
            ;;
        build)
            shift;
            ACT+=("build")
            ;;
        clean)
            shift;
            ACT+=("clean")
            ;;
        flush)
            shift;
            ACT+=("flush")
            ;;
        upload)
            ACT+=("upload")
            shift;
            case "$1" in
                -p|--port)
                    shift;
                    PORT=$1
                    shift;
                    ;;
                -b|--baudrate)
                    shift;
                    BAUDRATE=$1
                    shift;
                    ;;
                -B|--bitrate)
                    shift;
                    BITRATE=$1
                    shift;
                    ;;
            esac
            ACT+=("UPLOAD")
            ;;
    esac
    if [ $# -eq 0 ]; then
        break
    fi
done

# LAUNCH ACTION ON TARGET

if [ "$DEST" = "all" ]; then
    for DEST in ${TARGETS[@]}; do
         in_array "install" ${ACT[*]} && (
            configure_$DEST
            ${INSTALL}
         )
         in_array "build" ${ACT[*]} && (
            configure_$DEST
            ${RUNBUILD}
         )
    done
    for DEST in ${TARGETS[@]}; do
         in_array "upload" ${ACT[*]} && (
            configure_$DEST
            ${UPLOAD} "${PORT}" "${BAUDRATE}" "${BITRATE}"
         )
    done
    for DEST in ${TARGETS[@]}; do
         in_array "clean" ${ACT[*]} && (
            configure_$DEST
            ${CLEAN}
         )
         in_array "flush" ${ACT[*]} && (
            configure_$DEST
            ${FLUSH}
         )

    done
else
    configure_$DEST
    in_array "install" ${ACT[*]} && (
        ${INSTALL}
    )
    in_array "build" ${ACT[*]} && (
        ${RUNBUILD}
    )
    in_array "upload" ${ACT[*]} && (
        ${UPLOAD} "${PORT}" "${BAUDRATE}" "${BITRATE}"
    )
    in_array "clean" ${ACT[*]} && (
        ${CLEAN}
    )
    in_array "flush" ${ACT[*]} && (
        ${FLUSH}
    )
fi

exit 0

#####################################################################################

