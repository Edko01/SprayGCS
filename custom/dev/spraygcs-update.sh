#!/bin/bash
# Gets the latest SprayGCS changes from GitHub (the branch this copy is on,
# normally dev), builds them, and opens SprayGCS. Run it in Ubuntu as:
#   ~/qgroundcontrol/custom/dev/spraygcs-update.sh
#
# SprayGCS is drawn on the processor (LIBGL_ALWAYS_SOFTWARE=1): with the
# graphics chip, WSL drew its text in the wrong colours on some PCs (white as
# black or yellow).

# The braces make bash read the whole script before running it, so the pull
# can update this file safely.
{
    cd ~/qgroundcontrol || exit 1

    git pull --ff-only || { echo "UPDATE FAILED - send me this output"; exit 1; }
    cmake --build build --config Debug -j 8 || { echo "BUILD FAILED - send me this output"; exit 1; }

    if pgrep -x SprayGCS > /dev/null; then
        echo "SprayGCS is still open: close it and run this again to start the new build."
    else
        echo "Starting SprayGCS..."
        LIBGL_ALWAYS_SOFTWARE=1 QT_QPA_PLATFORM=xcb ./build/Debug/SprayGCS > /dev/null 2>&1 &
    fi
    exit 0
}
