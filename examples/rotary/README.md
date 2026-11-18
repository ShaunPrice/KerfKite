# Two full-turn engraving projects

Open either file in **Projects → Open project** in Studio 0.6.0 or later. Open **Rotary → Preview wrapped job**, then play the cylinder simulation. Both use a 20 mm diameter, a full 360° turn and the independent-A xPRO V5 postprocessor. You can inspect and export them with the machine disconnected.

| Project | What to look for |
| --- | --- |
| [Three router grooves](router-360-simulation.cncstudio.json) | Three sinusoidal paths return to their axial start positions. The programmed radial depth ranges from 0.25 to 0.95 mm. The version 0.6.0 post emits 931 lines. |
| [Four-slice laser relief](laser-360-simulation.cncstudio.json) | A radial grayscale map spans one circumference. Dark regions receive up to four exposures. Follow the changes between S0 and S200 in the rotary program. The post emits 295 lines. |

These are simulation fixtures. The feed, power and depth values demonstrate the file format and toolpaths. A physical machine requires its own independent rotary configuration and material measurements. [Rotary setup](../../docs/ROTARY.md)
