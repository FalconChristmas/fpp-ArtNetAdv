#!/bin/bash

# fpp-ArtNetAdv install script

BASEDIR=$(dirname $0)
cd $BASEDIR
cd ..
make "SRCDIR=${SRCDIR}"
