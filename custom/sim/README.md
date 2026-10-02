# SprayGCS Simulator Setup

Run a simulated spray drone on your Windows PC and fly real SprayGCS jobs with it: plan a field, upload, and watch the drone spray, return and resume, with no hardware.

| Part | What it is |
|---|---|
| **SprayGCS** | The ground station, installed on Windows like any program. |
| **Virtual Skynode** | Auterion's simulated flight computer (PX4), running in Ubuntu on WSL. |
| **Gazebo** | The 3D physics world (Baylands park, California). It runs without a window. |
| **Start script** | Starts the two simulator parts and links them to SprayGCS on port 5790. |

## Before you start

- **Windows 11** with hardware virtualization turned on (most PCs have it on).
- **16 GB of memory** or more, and about **20 GB** of free disk space.
- **Access to the Auterion Skynode Developer program.** The simulated flight computer needs the AuterionOS image, which you download from [Auterion Suite](https://suite.auterion.com) under **Store → Developer**. Each tester needs their own access.
- About an hour for the first setup. Most of it is downloads.

## Setup (once)

### 1. Install SprayGCS

Download and run the installer from <https://github.com/Edko01/SprayGCS/releases/latest> (`SprayGCS-installer-AMD64.exe`).

> **Windows protected your PC?** The installer isn't code-signed yet. Click **More info**, then **Run anyway**.

### 2. Install Ubuntu 22.04 on WSL

In **Windows PowerShell, run as administrator**:

```
wsl --install -d Ubuntu-22.04
```

Restart when asked. Ubuntu then opens and asks you to make a user name and password. Remember the password: the simulator asks for it each time it starts.

From now on, "Ubuntu" means the **Ubuntu** app, or an Ubuntu tab in **Terminal**.

### 3. Check Ubuntu can run a virtual machine

In Ubuntu:

```
sudo apt update && sudo apt install -y cpu-checker && kvm-ok
```

You want to see `KVM acceleration can be used`. If you don't, turn on nested virtualization: in Windows, create a file named `.wslconfig` in your user folder (`C:\Users\<your name>`) containing these two lines, then run `wsl --shutdown` in PowerShell, reopen Ubuntu and check again.

```
[wsl2]
nestedVirtualization=true
```

### 4. Install the simulator

In Ubuntu. This adds the Auterion and Gazebo package sources, then installs Virtual Skynode, Gazebo (Harmonic) and the link tool:

```
curl -1sLf 'https://dl.cloudsmith.io/public/auterion/public/setup.deb.sh' | sudo -E bash
sudo wget https://packages.osrfoundation.org/gazebo.gpg -O /usr/share/keyrings/pkgs-osrf-archive-keyring.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/pkgs-osrf-archive-keyring.gpg] http://packages.osrfoundation.org/gazebo/ubuntu-stable $(lsb_release -cs) main" | sudo tee /etc/apt/sources.list.d/gazebo-stable.list > /dev/null
sudo apt update
sudo apt install -y virtual-skynode simulation-gazebo socat python3 curl
```

Optional, for the simulator's settings page (needed only if the drone ever doesn't appear):

```
sudo apt install -y firefox
```

### 5. Get the AuterionOS image

In Windows, download the Virtual Skynode image from Auterion Suite (**Store → Developer**) into your Downloads folder. It's a file like `virtual-skynode-v4.2.22.tar.gz`, about 2.4 GB.

In Ubuntu (put your Windows user name and the file's version in):

```
tar -xzf /mnt/c/Users/<your Windows name>/Downloads/virtual-skynode-v4.2.22.tar.gz -C ~
```

That leaves `virtual-skynode-v4.2.22.qcow2` (about 4 GB) in your Ubuntu home folder, where the start script looks for it.

### 6. Get the start script

In Ubuntu:

```
curl -fsSL -o ~/start-spraygcs-sim.sh https://raw.githubusercontent.com/Edko01/SprayGCS/main/custom/sim/start-spraygcs-sim.sh
chmod +x ~/start-spraygcs-sim.sh
```

### 7. Start the simulator for the first time

In Ubuntu:

```
~/start-spraygcs-sim.sh
```

It opens three new tabs in Terminal: **Virtual Skynode**, **Gazebo** and **SprayGCS link**.

- Type your Ubuntu password in the **Virtual Skynode** tab when it asks.
- The first start installs AuterionOS and takes several minutes. **Don't close that tab while it installs**, or the simulated Skynode has to be reset.
- If Firefox is installed, it opens the **Virtual FMU** page. Set **Simulation mode** to the External Gazebo X500 option and **World** to **Baylands**, then click **Reload Simulation**.

> **After the first start,** close all the Terminal tabs, then run the script again. The first start downloads the drone model; the second start takes its camera off, which Gazebo needs to run reliably under WSL. The script says when it has done this.

### 8. Connect SprayGCS to the simulator

In SprayGCS on Windows, tap the SprayGCS logo (top left), then **Application Settings → Comm Links → Add**, and fill it in:

| Setting | Value |
|---|---|
| Name | Simulator |
| Type | TCP |
| Server address | 127.0.0.1 |
| Port | 5790 |
| Automatically connect on start | On |

Save it, select it and click **Connect**. If Windows asks whether SprayGCS may use the network, allow it. Within a few seconds the top bar shows **Ready** and the drone appears on the map at Baylands park.

### 9. Fly a test job

1. Go to **Plan**. In **New Spray Plan**, tap **Use the Drone's Position** for the takeoff point.
2. Tap **Draw Field Boundary** and tap the corners of an open grass area near the drone.
3. Tap **Upload**, go back to **Fly**, and slide **Start Mission**.

The top of the screen shows the battery bar and the job's status (SPRAYING, Pass 3 of 20), with a segment for each pass. Speed, height and the pump are at the bottom right. A blue trail marks where the drone has sprayed. Press **Return** mid-field to try the breakpoint: after it lands, tap **Resume Job**, then **Start Mission** again.

## Every time after that

1. Open Ubuntu and run `~/start-spraygcs-sim.sh`. Type your password in the Virtual Skynode tab.
2. Open SprayGCS. It connects by itself.

Leave the Virtual Skynode, Gazebo and SprayGCS link tabs open while you work, and don't type in them. To stop everything, close SprayGCS and those tabs, or run `wsl --shutdown` in PowerShell.

## If something goes wrong

| Problem | What to do |
|---|---|
| **The drone doesn't appear** | Give it a minute. Then, on the Virtual FMU page (Firefox), check the External Gazebo X500 mode and Baylands world are selected and click **Reload Simulation**. |
| **SprayGCS says Disconnected** | Check the **SprayGCS link** tab is still open, and the comm link is TCP, `127.0.0.1`, port `5790`. Start the script before SprayGCS. |
| **The Gazebo tab shows a crash** | Messages such as `ODE INTERNAL ERROR` or `Segmentation fault` mean the 3D world stopped. Close **all** the tabs and run the script again. Restarting only Gazebo leaves the simulated drone out of step with the world, and it can fly off on its own. |
| **"Connection to mission computer lost"** | A normal message while the simulated Skynode starts up. Dismiss it. |
| **The drone won't go faster than 26.8 mph** | That's PX4's top speed setting (`MPC_XY_VEL_MAX`, 12 m/s), not a fault. Planned speeds above it are flown at 26.8 mph. |
| **Virtual Skynode won't start** | Run `kvm-ok` again (step 3). Without KVM the simulated Skynode can't run. |

---

SprayGCS is a ground station for agricultural spray drones. Virtual Skynode, AuterionOS and Auterion Suite are Auterion products; Gazebo is from Open Robotics.
