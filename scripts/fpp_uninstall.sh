#!/bin/bash

# fpp-ArtNetAdv uninstall script
echo "Running fpp-ArtNetAdv uninstall Script"

BASEDIR=$(dirname $0)
cd $BASEDIR
cd ..
make clean  "SRCDIR=${SRCDIR}"

