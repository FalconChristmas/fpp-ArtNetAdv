#!/bin/sh

echo "Running fpp-ArtNetAdv PreStart Script"

BASEDIR=$(dirname $0)
cd $BASEDIR
cd ..
make
