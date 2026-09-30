#!/bin/bash
# start-spraygcs-sim.sh
# Starts the SprayGCS simulator on a Windows PC with WSL (Ubuntu 22.04):
#   1. Virtual Skynode   (Auterion's simulated Skynode; creates the 10.41.200.x network)
#   2. Gazebo            (the 3D world, Baylands park; runs without a window)
#   3. A link for SprayGCS on Windows: localhost:5790 -> the Virtual Skynode's MAVLink port
#   4. The Virtual FMU page in Firefox (to pick the Gazebo X500 drone the first time)
# Then open SprayGCS on Windows; it connects to 127.0.0.1:5790 (TCP).
#
# Run from an Ubuntu (WSL) terminal:   ~/start-spraygcs-sim.sh

GUEST_IP="10.41.200.2"
STORAGE_DIR="$HOME/.virtual-skynode"
MAVLINK_PORT=5790
WSL_DISTRO="${WSL_DISTRO_NAME:-Ubuntu-22.04}"
QCOW2_FILE=$(ls -1 "$HOME"/virtual-skynode-*.qcow2 2>/dev/null | sort -V | tail -1)

missing=0
for tool in virtual-skynode simulation-gazebo socat curl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "!! $tool is not installed (see the setup instructions)."
        missing=1
    fi
done
if [ -z "$QCOW2_FILE" ]; then
    echo "!! No AuterionOS image (virtual-skynode-<version>.qcow2) in your home folder."
    missing=1
fi
[ "$missing" = 1 ] && exit 1

open_tab() {
    # $1 = tab title, $2 = command to run in the new tab
    if command -v wt.exe >/dev/null 2>&1; then
        # Windows Terminal treats a bare ';' as its own separator, so escape it.
        wt.exe -w 0 new-tab --title "$1" wsl.exe -d "$WSL_DISTRO" bash -ic "$2 \; exec bash" >/dev/null 2>&1
    else
        echo "!! Windows Terminal (wt.exe) not found. Open a new Ubuntu tab and run:"
        echo "   $2"
        read -r -p "   Press Enter once it's running... "
    fi
}

echo "==> [1/4] Starting Virtual Skynode ($(basename "$QCOW2_FILE")) in a new tab"
echo "    Type your Ubuntu password in that tab when asked."
open_tab "Virtual Skynode" "cd ~ && sudo virtual-skynode run --rootfs $QCOW2_FILE --storage $STORAGE_DIR --guest-ip $GUEST_IP"

echo "==> Waiting for Virtual Skynode at http://$GUEST_IP (a minute or two; longer the first time)..."
until curl -s --max-time 2 "http://$GUEST_IP" >/dev/null; do
    sleep 3
done
echo "    Virtual Skynode is up."

echo "==> [2/4] Starting Gazebo (Baylands) in a new tab"
# The X500 model Virtual FMU spawns carries a camera. Drawing its image crashes
# Gazebo inside WSL's graphics driver, and a spray drone doesn't need it, so
# take the camera off the model (once; a backup is kept as model.sdf.with-camera).
strip_camera() {
    local sdf
    sdf=$(find "$HOME" -maxdepth 6 -path '*x500_mono_cam/model.sdf' 2>/dev/null | head -1)
    if [ -z "$sdf" ]; then
        echo "    (The drone model isn't downloaded yet. After this first run, close everything"
        echo "     and run this script again so the camera can be taken off.)"
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
# No Gazebo window (the drone shows in SprayGCS); with no camera the CPU draws everything.
open_tab "Gazebo" "simulation-gazebo --world baylands --gz_headless --gpu_render software"
echo "    Giving Gazebo 20 seconds to load the world..."
sleep 20

echo "==> [3/4] Opening the link for SprayGCS (localhost:$MAVLINK_PORT) in a new tab"
open_tab "SprayGCS link" "socat TCP-LISTEN:$MAVLINK_PORT,reuseaddr,fork TCP:$GUEST_IP:$MAVLINK_PORT"

echo "==> [4/4] Opening the Virtual FMU page"
if command -v firefox >/dev/null 2>&1; then
    firefox "http://$GUEST_IP/apps/com.auterion.virtual-fmu/" >/dev/null 2>&1 &
else
    echo "    Firefox isn't installed in Ubuntu; skip this unless the drone doesn't appear."
fi

cat <<MSG

------------------------------------------------------------------
 Now open SprayGCS on Windows. It connects to 127.0.0.1:$MAVLINK_PORT
 (set that up once: see the instructions) and shows the drone.

 IF THE DRONE DOESN'T SHOW UP within about a minute:
   On the Virtual FMU page, pick the External Gazebo X500 mode,
   World "Baylands", and click  Reload Simulation.

 Leave the Virtual Skynode, Gazebo and SprayGCS link tabs open
 while you work, and don't type in them.
------------------------------------------------------------------
MSG
