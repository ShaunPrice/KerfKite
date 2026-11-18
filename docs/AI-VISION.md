# Optional AI camera module — proposal, not implemented

Assuming the official Raspberry Pi AI HAT+ 13 TOPS: this is a Hailo-8L INT8 inference accelerator. Raspberry Pi provides supported camera inference examples. It is not a general-purpose GPU or extra system RAM, and ordinary geometry/CAM code does not automatically run on it.

Recommended initial use: a fixed overhead camera feeding an optional local vision worker, reporting observations to the workstation and phone.

1. Work-area observations: stock/clamp presence and possible stock movement. Fiducial tracking and precise alignment should also evaluate conventional calibrated vision, which may not need an NPU.
2. Head identification: distinguish the laser, router and drag-knife holder. A dedicated dataset and trained/validated model are required; generic object detectors will not reliably recognise these tools without adaptation.
3. Process anomaly alerts: visible part movement or unexpected appearance changes. Smoke, flame and small broken-bit detection are research candidates, not dependable safeguards. Camera resolution, occlusion, dust and lighting determine usefulness.
4. After-job visual inspection: compare visible results with references, subject to calibration and measurement limits.

AI observations must remain distinct from motion control and physical interlocks. Do not use visual inference as the sole fire protection, E-stop, tool-clearance or tool-identity gate. An alert should show its timestamp, confidence and camera availability. Automatic motion actions would need a separately validated policy.

The 13 TOPS HAT runs supported compiled models; custom model training and compilation are separate workflows. No inference FPS, accuracy or actual HAT connectivity has been measured for this project. Do not install or change the adjacent AI Trainer project as part of this proposal.

For other hosts, keep inference behind a provider interface (CPU, optional Metal-capable backend, optional other accelerators). No Metal backend is implemented today. The CNC application must function without AI hardware.

## Implemented conventional tracing

The editor now captures camera images or imports image files for outline and centreline tracing. This is conventional image processing, not Hailo inference. Physical dimensions are entered by the user; lens/bed calibration, registered overlays, fiducials and reliable stock/tool identification remain outstanding. Camera tracing does not authorize motion or replace setup checks.

## Pi 5 8 GB assessment

A bounded CAM stress test on the real Pi 5 with 8 GB completed a complex 256×256 relief fixture generating 135,886 G-code lines. Across 62 complete matching jobs, peak sampled temperature was 72.7°C, available system memory stayed above 6.2 GiB, swap use was zero and no throttling flags were reported. That test measured the calculation workload, not the full browser/UI, live control and camera pipeline together. See [the stress report](../validation/PI-CAM-STRESS-2026-09-11.md).

A subsequent installed optional C++ extension reduced the same fixture's complete calculation median from 2.864 s to 1.818 s, with matching G-code. The earlier standalone prototype results are historical; [NATIVE.md](NATIVE.md) records the integrated implementation and evidence. No Rust or HAT CAM acceleration comparison has been performed.

These results support the Pi 5 / 8 GB for the tested workload, not unlimited models or full-parity capacity. Large meshes, finer stock previews and simultaneous vision can increase CPU/memory demand. The application imposes work caps; future background/chunked CAM and full combined-workload performance still need engineering and measurement. Use active cooling for sustained processing and validate intended workloads on the deployment host. The HAT cannot substitute for CPU CAM capacity or physical commissioning.

Primary references checked 11 September 2026:
- https://www.raspberrypi.com/documentation/accessories/ai-hat-plus.html
- https://www.raspberrypi.com/documentation/computers/ai.html
- https://www.raspberrypi.com/news/heating-and-cooling-raspberry-pi-5/
