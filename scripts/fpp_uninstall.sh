#!/bin/bash

# fpp-ArtNetAdv uninstall script
echo "Running fpp-ArtNetAdv uninstall Script"

BASEDIR=$(dirname $0)
cd $BASEDIR
cd ..
make clean  "SRCDIR=${SRCDIR}"


# No restartFlag: the Plugin Manager unloads the plugin through fppd before it
# removes these files, so the uninstall has already taken effect by the time
# this runs.
