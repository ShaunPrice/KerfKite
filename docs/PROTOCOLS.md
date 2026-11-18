# Controller compatibility

The CAM selector now has eight implemented profiles. A G-code postprocessor is not a live machine driver. None of these profiles has been physically commissioned in this project.

| Profile | Studio connection | File operations |
| --- | --- | --- |
| GRBL / GRBL-ESP32 | USB serial, 115200 | Router, dynamic laser, passive drag knife |
| FluidNC | Experimental GRBL-compatible USB serial, 115200 | Router, dynamic laser, passive drag knife |
| grblHAL | Experimental GRBL-compatible USB serial, 115200 | Router, dynamic laser, passive drag knife |
| Marlin CNC | Experimental USB serial, checksum/resend and M400 | Router, passive drag knife; CNC spindle configuration required |
| LinuxCNC | Export only | XYZ router/relief and passive drag knife; separate continuous-A rotary exporter |
| Mach3 Mill | Export only | XYZ router/relief and passive drag knife |
| Mach4 Mill / Router | Export only | XYZ router/relief and passive drag knife |
| UCCNC | Export only | XYZ router/relief and passive drag knife |

## Mach3, Mach4, LinuxCNC and UCCNC

Select the controller in CAM, select router or knife, generate a preview, and export the .nc file. Load it in the controller's own software. Connect, jog, head changes, phone arming and Run are unavailable for export-only selections. Server dispatch rejects these profiles too. Uploading to the Pi inbox only stores a file for review; it does not add a machine driver.

Mach3 and UCCNC require a **configured dwell units** choice matching the actual machine application. The choice is saved in the project and operation settings. Seconds emits G4 P2; milliseconds emits G4 P2000. Mixed dwell settings within a job or tool-change plan are rejected. Mach4 and LinuxCNC use seconds.

Desktop exports establish G21/G90/G17/G94/G54, cancel cutter and tool-length compensation and canned cycles (G40/G49/G80), and use G61 exact-stop motion. Establish G54 Z zero for the current tool tip **with tool-length compensation off**. Clear existing scaling, coordinate rotation and local offsets before loading. G61 protects corners but may leave witness marks or slow dense relief paths; cutting-time estimates do not model this overhead. Every manual tool stage needs fresh tool-tip Z setup; do not rely on an offset that G49 cancels.

All operation cuts precede one final M30. Each separately exported tool stage has its own end. Knife jobs do not enable spindle output. Laser exports to these desktop profiles are rejected because their ordinary M4 command does not mean GRBL dynamic laser power.

## FluidNC and grblHAL

These profiles reuse the conservative XYZ GRBL serial transport; they are not TCP, WebSocket or HTTP drivers. They retain head-mode, maximum power, travel and job completion checks. Firmware identification is recorded when supplied by $I. FluidNC explicitly queries $30, $32 and $I in acknowledgement order, so older firmware that omits $32 from $$ can report it separately. Missing required readback keeps the driver unready. Nonfinite settings and unexpected reboot banners invalidate readiness.

FluidNC YAML must be configured separately for the mounted spindle or laser. Studio refuses automatic FluidNC head-setting writes. Neither profile enables extra axes, firmware plugins, automatic tool changes or unverified status extensions. grblHAL compatibility settings must provide the GRBL 1.1 response format used by this driver. Test connection, homing, work zero, motion, hold, abort and completion on the actual configured machine before cutting.

## Remaining integrations

Duet/RepRapFirmware, Smoothieware, TinyG/g2core, Klipper/Moonraker, ShopBot, Ruida, Trocen, TopWisdom and galvo controller families still need specific adapters/postprocessors and validation. Live Mach plugins and LinuxCNC bridges are also unimplemented. They are not presented as working choices. The project does **not** yet support every protocol or full LightBurn parity.

## Contributor extension points

The catalog is `studio/static/protocols.json`; the offline mirror in `protocols.js` is checked for parity. Add explicit backend/offline postprocessing, supported modes, a driver only when implemented, dispatch guards and regression tests. Never route an unknown ID through a default firmware conversion. `/api/protocols` exposes implemented capabilities to editor clients.

## Primary references

- [LinuxCNC G-code reference](https://www.linuxcnc.org/docs/stable/html/gcode/g-code.html)
- [Mach4 Mill programming manual](https://www.machsupport.com/wp-content/uploads/2014/05/Mach4%20Mill%20GCode%20Manual.pdf)
- [Mach3 Mill usage manual](https://www.machsupport.com/ftp/Docs/Mach3Mill_1.84.pdf)
- [UCCNC user manual](https://www.cncdrive.com/UCCNC/UCCNC_usersmanual.pdf)
- [FluidNC project](https://github.com/bdring/FluidNC) and [older $32 readback issue](https://github.com/bdring/FluidNC/issues/1319)
- [grblHAL compatibility levels](https://github.com/grblHAL/core/wiki/Compatibility-level)

Validation: 108 Python tests, 76 JavaScript tests; local browser exercise of selector, required Mach3 dwell and generated output. These are software checks, not controller or physical cutting acceptance.
