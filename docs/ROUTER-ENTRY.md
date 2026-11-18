# Enter a router cut with a ramp or helix

Open **CAM → Router entry**. Choose a flat end mill rated by its maker for the intended axial and ramping work. **Maximum entry angle** is measured from the stock plane. Studio uses enough complete turns to stay at or below that slope. Each pass observes the configured step-down, starting from the prior machined level. Entry feeds are limited by both the cutting feed and plunge feed.

| Strategy | Where it runs | Geometry |
| --- | --- | --- |
| Vertical plunge | Existing router workflows | Feed straight down at the starting XY. Drilling uses its own plunge and peck-depth controls. |
| Ramp along closed contour | Desktop and hosted Studio; local-file editor for centreline only | Follow the prepared centreline, inside profile, outside profile or concentric pocket contour while descending, then finish one complete lap at the pass depth. |
| Helix inside pocket | Desktop and hosted Studio | Descend through a circular helix then feed from its endpoint to the prepared pocket contour. Studio checks clearance for each entry against stock boundaries and islands. |

## Try both entries on a pocket

1. Draw a 24 × 24 mm rectangle away from the bed edges. In CAM choose **Router**, use a 4 mm flat end mill with 2 mm stepover, a total depth of 2 mm and a step-down of 1 mm. Use a stock thickness of at least 2 mm.
2. Choose **Line / depth contour**, **Ramp along closed contour** and a 3° maximum angle. Generate the preview. In **Rotatable 3D toolpath preview**, inspect the descent and the level finishing circuit. In the G-code, each interpolated feed specifies X, Y and Z. A clearance retract separates contours and levels.
3. Choose **Pocket clearing** and **Helix inside pocket**, then set **Helix centreline radius** to 1.5 mm. Generate a fresh preview and choose **Simulate sampled stock removal**. The tip follows the helix about its centre, with the cutter radius extending farther out. Each XY traverse between contours takes place at clearance height.
4. Reduce the pocket size or add an island and preview again. If an entry and its connecting feed do not fit, Studio reports the problem. Widen the pocket, choose a smaller appropriate cutter or select a closed-contour ramp. G-code for an older preview is invalidated when you change the setup.
5. Save the project to retain the selected entry, angle and radius. Use **Layers → Use current CAM for layer** or the selected object's operation settings for different strategies in the same project.

**Holding tabs.** A profile ramp follows the compensated cutter path, including its raised tab intervals. At every pass, the entry and finishing circuit respect the material left above each tab. Helical entry applies to pockets. Centreline inputs for ramps must be closed and free of crossings.

**Helix clearance.** The centreline radius must be positive and at most the cutter radius to clear the core. The available pocket must contain the entire swept bit around the helix, and the feed connecting the circle to the prepared ring must remain inside the cutter-centre region. The material boundary uses the original contours including islands. The preview checks nominal geometry at the configured diameter; stock holding and fixture clearances still depend on the machine setup.

**Offline files.** A browser opened from the filesystem supports closed-line ramps. Pocket and compensated entry calculations require server CAM. The local-file editor reports that requirement when you request a pocket helix.
