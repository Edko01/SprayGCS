#!/bin/bash
# start-skynode-sim.sh  (v6: the simulated drone has no camera; Gazebo runs without its window)
# Starts the spray-drone simulation stack in the order that works:
#   1. Virtual Skynode   (creates the 10.41.200.x network Gazebo needs)
#   2. Gazebo            (only after Virtual Skynode is reachable)
#   3. Firefox dashboard (to click "Reload Simulation" in Virtual FMU)
#   4. SprayGCS          (your custom QGroundControl build; connects over UDP)
#
# Run from an Ubuntu (WSL) terminal:   ~/start-skynode-sim.sh

QCOW2_FILE=$(ls -1 "$HOME"/virtual-skynode-*.qcow2 2>/dev/null | sort -V | tail -1)   # newest AuterionOS image in your home folder
GUEST_IP="10.41.200.2"
STORAGE_DIR="$HOME/.virtual-skynode"
QGC_BIN="$HOME/qgroundcontrol/build/Debug/SprayGCS"
WSL_DISTRO="${WSL_DISTRO_NAME:-Ubuntu-22.04}"   # WSL sets WSL_DISTRO_NAME automatically

open_tab() {
    # $1 = tab title, $2 = command to run in the new tab
    if command -v wt.exe >/dev/null 2>&1; then
        # Windows Terminal treats a bare ';' as its own command separator,
        # so the ';' inside the command must be escaped as '\;'.
        wt.exe -w 0 new-tab --title "$1" wsl.exe -d "$WSL_DISTRO" bash -ic "$2 \; exec bash" >/dev/null 2>&1
    else
        echo "!! Windows Terminal (wt.exe) not found. Open a new Ubuntu tab and run:"
        echo "   $2"
        read -r -p "   Press Enter once it's running... "
    fi
}

echo "==> [1/4] Starting Virtual Skynode in a new tab"
echo "    Type your sudo password in that tab when asked."
open_tab "Virtual Skynode" "cd ~ && sudo virtual-skynode run --rootfs $QCOW2_FILE --storage $STORAGE_DIR --guest-ip $GUEST_IP"

echo "==> Waiting for Virtual Skynode at http://$GUEST_IP (this can take a minute)..."
until curl -s --max-time 2 "http://$GUEST_IP" >/dev/null; do
    sleep 3
done
echo "    Virtual Skynode is up."

echo "==> [2/4] Starting Gazebo (Baylands) in a new tab"

# The X500 model Virtual FMU spawns (x500_mono_cam) carries a camera. Drawing
# its image crashes Gazebo inside WSL's graphics driver ("Segmentation fault"
# in libd3d12core.so), on the NVIDIA card too, and drawing it on the CPU runs
# the sim at under 1% of real time. A spray drone doesn't need it, so take the
# camera off the model (once; a backup is kept as model.sdf.with-camera).
strip_camera() {
    local sdf
    sdf=$(find "$HOME" -maxdepth 6 -path '*x500_mono_cam/model.sdf' 2>/dev/null | head -1)
    if [ -z "$sdf" ]; then
        echo "    (x500_mono_cam model not downloaded yet; start this script again after Gazebo has run once)"
        return
    fi
    [ -f "$sdf.with-camera" ] || cp "$sdf" "$sdf.with-camera"
    python3 - "$sdf" <<'PYEOF'
import sys, xml.etree.ElementTree as ET
path = sys.argv[1]
tree = ET.parse(path)
model = tree.getroot().find('model')
removed = 0
for inc in list(model.findall('include')):
    if 'cam' in (inc.findtext('uri') or '').lower():
        model.remove(inc); removed += 1
for joint in list(model.findall('joint')):
    if 'cam' in (joint.findtext('child') or '').lower() or 'cam' in joint.get('name', '').lower():
        model.remove(joint); removed += 1
for link in list(model.findall('link')):
    if 'cam' in link.get('name', '').lower():
        model.remove(link); removed += 1
for link in model.findall('link'):
    for sensor in list(link.findall('sensor')):
        if sensor.get('type') == 'camera':
            link.remove(sensor); removed += 1
if removed:
    tree.write(path, xml_declaration=True, encoding='UTF-8')
    print("    Camera removed from the simulated drone (%d parts)." % removed)
else:
    print("    Simulated drone has no camera.")
PYEOF
}
strip_camera

# --gz_headless: no Gazebo window; the drone shows in SprayGCS.
# --gpu_render software: with no camera there is nothing to draw, so the CPU
# is plenty and WSL's graphics driver is never used.
# To see the Gazebo window, delete --gz_headless below.
# Check the sim runs at full speed (real_time_factor close to 1.0):
#   eval $(tr '\0' '\n' < /proc/$(pgrep -f "gz sim" | head -1)/environ | grep '^GZ_' | sed 's/^/export /')
#   gz topic -e -t /stats -n 1 | grep real_time_factor
# Note: simulation-gazebo --overwrite downloads the models again, camera included.
GAZEBO_ARGS="--gz_headless --gpu_render software"
open_tab "Gazebo" "simulation-gazebo --world baylands $GAZEBO_ARGS"
echo "    Giving Gazebo 20 seconds to load the world..."
sleep 20

echo "==> [3/4] Opening the Virtual FMU page in Firefox"
firefox "http://$GUEST_IP/apps/com.auterion.virtual-fmu/" >/dev/null 2>&1 &

echo "==> [4/4] Starting SprayGCS"
( cd "$HOME/qgroundcontrol" && QT_QPA_PLATFORM=xcb "$QGC_BIN" >/dev/null 2>&1 & )

cat <<'EOF'

------------------------------------------------------------------
 IF THE DRONE DOESN'T SHOW UP:
   In Firefox (Virtual FMU page), keep the mode on
   "External Gazebo 7 (Garden) - X500", World "Baylands",
   and click  Reload Simulation  ONLY if the drone does not appear
   in SprayGCS within about a minute.

 Then:
   - the drone appears in SprayGCS (Gazebo has no window in this mode)
   - SprayGCS connects by itself and shows "Ready"

 Don't type in the Virtual Skynode tab, and leave the
 Virtual Skynode and Gazebo tabs open while you work.
------------------------------------------------------------------
EOF
