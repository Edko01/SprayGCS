@AGENTS.md

## SprayGCS product principles

SprayGCS (this fork, custom build in `custom/`) is a ground station for agricultural spray drones only.
Build every feature with these in mind:

- **Jobs take many trips.** The tank and the battery are small, so the drone usually runs out of liquid
  or battery long before a field is done. A normal job is many trips in and out of the field, with a
  refill or battery swap between them.
- **Fly until empty, then resume.** The whole job is uploaded once. The drone sprays until the tank is
  empty or the battery is low, returns, and SprayGCS saves a breakpoint. After the refill or swap, the
  operator resumes from the breakpoint (only what's left is planned) and the drone continues.
- **Keep the refill-and-resume cycle fast.** It happens many times per job, so it must take as few taps
  as possible and survive a SprayGCS restart or a lost link.
- **The drone is in charge of flying and spraying.** No live pump control from the GCS; the drone must
  finish or return safely without the GCS. Pump, flow rate, terrain following and tank sensing belong
  to onboard Skynode apps.
