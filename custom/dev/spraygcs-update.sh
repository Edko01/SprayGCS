#!/bin/bash
# Applies the SprayGCS patches saved next to this script (your Windows
# Downloads folder) to ~/qgroundcontrol, in order, skipping any that are
# already in, then builds and opens SprayGCS. Safe to run again: applied
# patches are listed in ~/qgroundcontrol/.spraygcs-patches-applied.
# Run it as:  bash /mnt/c/Users/<Windows user>/Downloads/spraygcs-update.sh
DL="$(cd "$(dirname "$0")" && pwd)"
cd ~/qgroundcontrol || exit 1
DONE=.spraygcs-patches-applied
touch "$DONE"
# Patches that were taken back: removed again if they're in.
for p in spraygcs-trips; do
    f="$DL/$p.patch"
    if [ -f "$f" ] && git apply -R --check "$f" 2>/dev/null; then
        git apply -R "$f" && echo "removed  $p"
    fi
    sed -i "/^$p\$/d" "$DONE"
done
for p in spraygcs-editor-width spraygcs-clean-boundary-drawing spraygcs-job-panel spraygcs-installer-upgrade spraygcs-arrows-no-readout spraygcs-transit-arrows spraygcs-boundary-done-visible spraygcs-resume-job spraygcs-resume-build-fix spraygcs-mode-a-return spraygcs-return-via-entry spraygcs-breakpoints spraygcs-principles spraygcs-fly-coverage spraygcs-continue-upload spraygcs-resume-after-landing spraygcs-hide-condition-gate spraygcs-hide-unused-settings spraygcs-readable-dialogs spraygcs-resume-button spraygcs-battery-bar spraygcs-spray-hud spraygcs-release-1.1.0; do
    f="$DL/$p.patch"
    if grep -qx "$p" "$DONE"; then
        echo "had      $p"
    elif [ ! -f "$f" ]; then
        :   # not downloaded: already in a fresh copy of the source, or not sent yet
    elif git apply -R --check -C0 "$f" 2>/dev/null; then   # in already (-C0: later patches may change the lines around it)
        echo "$p" >> "$DONE"
        echo "had      $p"
    elif git apply "$f"; then
        echo "$p" >> "$DONE"
        echo "applied  $p"
    else
        echo "PROBLEM  $p - stopped here, send me this output"
        exit 1
    fi
done
cmake --build build --config Debug -j 8 || { echo "BUILD FAILED - send me this output"; exit 1; }

# Open SprayGCS again (unless it's still running).
if pgrep -x SprayGCS > /dev/null; then
    echo "SprayGCS is still open: close it, then start it with:"
    echo "  cd ~/qgroundcontrol && QT_QPA_PLATFORM=xcb ./build/Debug/SprayGCS &"
else
    echo "Starting SprayGCS..."
    QT_QPA_PLATFORM=xcb ./build/Debug/SprayGCS &
fi
