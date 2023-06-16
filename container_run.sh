#!/bin/bash

SCRIPT_DIR="$(dirname "$(realpath "${0}")")"

docker run -it \
    -v ${SCRIPT_DIR}:/root/StreetLight \
    ubuntu