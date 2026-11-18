# Draw and model by typing

KerfKite has a command line above the canvas and a multiline **CAD script** editor. Use the command line for a measured shape or camera move. Use a script for a part whose holes, dimensions or repeated features should change together. Lengths are in millimetres and angles are in degrees.

To keep a command sequence or script as a named recipe, open the macro library. You can give a recipe numeric parameters and share it with another workshop. [Create and run CAD macros](CAD-MACROS.md)

## First drawing from the keyboard

Open an installed Studio workshop in simulation, or sign in to a hosted workshop. Click the drawing canvas and press **/**. Type `rect 60 24 10 15` and press **Enter**. A 60 × 24 mm rectangle appears with its lower-left corner at X 10, Y 15. Enter `circle 3 18 27` to create a separate circle with a 3 mm radius. Enter `fit drawing` to frame both objects. Each command creates ordinary, selectable artwork on the active layer.

For a single object with a hole, use a script. The local-file Browser Editor can open and save script text and run the quick drawing and 2D view commands. Script geometry compilation requires the installed Studio service or a hosted workshop.

## A flat tag with a hole

1. Choose **Script… → New** and paste the following source.
2. Press **F5** or **Preview**. Check the dimensions and the hole in the shaded 2D preview.
3. Press **Ctrl+Enter** on Windows or Linux, **Command+Enter** on Mac, or choose **Apply**. The tag becomes a drawing object with an outer contour and an inner contour.
4. Move the tag on the bed. With the object selected in the drawing workspace, type `edit`. Change `tag_length` to `80`, preview, then apply. The revised tag keeps its placement.

```scad
tag_length = 68;
tag_height = 24;
hole_diameter = 5;
$fn = 64;

difference() {
    hull() {
        translate([tag_height/2, tag_height/2]) circle(d=tag_height);
        translate([tag_length-tag_height/2, tag_height/2]) circle(d=tag_height);
    }
    translate([8, tag_height/2]) circle(d=hole_diameter);
}
```

The initial tag spans X 0–68 and Y 0–24. A new script uses its own XY coordinates on the drawing bed. If its bounds extend beyond the bed, Studio translates it just far enough to fit and reports the offset. A shape larger than the bed requires resizing in the source or a suitable work area. Rebuilding an existing scripted object preserves whole-object movement, rotation and scaling. After editing individual nodes or assigning per-vertex machining values, use **New** for a separate scripted copy.

## A mounting plate with four holes

Start a new project to inspect this part by itself. Choose **Script… → New**, paste the program and preview it. The first solid in `difference()` supplies the plate. Each instance of `drill_at` removes a cylinder extending 1 mm past both faces.

```scad
plate_width = 70;
plate_depth = 36;
plate_height = 5;
margin = 6;
$fn = 48;

module drill_at(x, y, diameter=4) {
    translate([x, y, -1])
        cylinder(h=plate_height+2, d=diameter);
}

difference() {
    cube([plate_width, plate_depth, plate_height]);
    for (x = [margin, plate_width-margin], y = [margin, plate_depth-margin])
        drill_at(x, y);
}
```

Apply the 70 × 36 × 5 mm solid. Enter `view top` and check the four holes. Their centres are at X 6 or 64 and Y 6 or 30. Try `front`, `iso`, `orbit 15 10` and `zoom 1.25`. A view change moves the camera without changing the part.

Select the scripted feature in **3D CAD** and choose **Edit CAD script…**. Increase `plate_width` to `85` and apply. The right-hand holes move to X 79. A named part parameter with the same name overrides its script default. For a drawing, Studio stores a copy of the part parameter values used at creation. For a solid feature, rebuilding uses the part's current parameters.

## A tapered dial with a circular pattern

Use a new project for a separate dial. The two end diameters define the taper. The loop rotates a radial hole around the Z axis in equal increments. Change `holes` from `8` to `6` and press **F5** to compare the pattern before applying.

```scad
holes = 8;
dial_height = 12;
pitch_radius = 9;
$fn = 64;

module through_bore(diameter) {
    translate([0, 0, -1]) cylinder(h=dial_height+2, d=diameter);
}

difference() {
    cylinder(h=dial_height, d1=32, d2=26);
    through_bore(6);
    for (i = [0:holes-1])
        rotate([0, 0, i*360/holes])
            translate([pitch_radius, 0, 0]) through_bore(2.4);
}
```

The preview spans 32 × 32 × 12 mm. Drag the preview to inspect the taper. Use the top view of the applied model to inspect the hole spacing.

## Save and exchange

**Open .scad…** loads source for editing. **Save .scad** downloads the contents of the source field, including unapplied edits. Save the Studio project after applying to preserve geometry, editable source, layers and feature history together. To add a separate scripted feature while another one is selected, choose **New** in the script editor. A new solid uses the **Join** operation in the current part.

**Export STL** writes the tessellated solid for mesh exchange. Exact STEP export works with supported exact CAD features. If a part contains an active scripted feature, Studio reports that its surfaces are tessellated. Use STL for the complete result, or suppress the scripted features to export the remaining exact part. A geometry preview describes the design. To produce a machining file, transfer or select the desired geometry, set the CAM operation and generate a toolpath preview.

## Commands and shortcuts

In the **Command** field, **Enter** runs one command. **↑** and **↓** recall recent entries. **Escape** leaves the field. Type `help` or choose **?** for the command reference. The **/** shortcut is available outside input fields and dialogs when the pendant is inactive.

| Command | Effect |
| --- | --- |
| `rect width height [x y]` or `rectangle width height [x y]` | Add a rectangle. The default lower-left corner is (0, 0). |
| `circle radius [cx cy]` | Add a circle. Its default centre is (radius, radius), which keeps it on the positive bed. |
| `line x1 y1 x2 y2` | Add an open path between two different points. |
| `script`, `edit` | Open the script editor for the selected scripted drawing or solid feature. |
| `cube([40,25,5]);` | Compile the entered script statement and add its result. An expression such as `circle(r=4);` produces a scripted 2D region. |
| `top`, `bottom`, `front`, `back`, `left`, `right`, `iso` | Open the 3D viewport at the named orientation. The optional prefix `view` works with these commands. |
| `fit`, `fit drawing`, `fit bed` | Frame the current geometry, visible 2D artwork, or the full drawing bed. |
| `zoom factor`, `pan dx dy` | Zoom by a positive factor (1.25 enlarges, 0.8 reduces) or shift the picture by screen pixels. |
| `orbit yaw pitch` | Rotate the solid view by two angle increments in degrees. |

Click the appropriate viewport before using its keys. The **Drawing view** and **View orientation** menus provide alternatives on keyboards without a numeric keypad.

| Context | Keys or gesture | Effect |
| --- | --- | --- |
| Drawing or solid viewport | **F**, **+** / **−** | Fit visible geometry, zoom in or zoom out. |
| Drawing viewport | Arrow keys, middle-drag, scroll | Pan with arrows or drag. Scroll to zoom around the pointer. |
| Solid viewport | Arrow keys or drag | Orbit. Use **Shift+arrow**, **Shift+drag** or middle-drag to pan. |
| Viewport | **Num 0 / 1 / 9 / 3 / 4 / 7 / 2** | Isometric / front / back / right / left / top / bottom. |
| Script editor | **F5** | Build the preview. Drag a solid or scroll to inspect it. |
| Script editor | **Ctrl+Enter**, **Command+Enter** or **F6** | Build and apply the current source to the project. |

## Supported script language

Studio implements a geometry-focused subset of OpenSCAD syntax. Each script produces one 2D or one 3D result. Multiple top-level shapes of the same dimension form a union. Use extrusion or projection to cross between 2D and 3D. Source errors identify the affected line and column where available.

| Construct | Supported forms |
| --- | --- |
| 2D primitives | `square(size, center)`, `circle(r=…)` or `circle(d=…)`, `polygon(points, paths)`. Polygon paths index into points and can describe holes. |
| 3D primitives | `cube(size, center)`, `sphere(r=…)` or `sphere(d=…)`, `cylinder(h=…,r=…)`, tapered cylinders with `r1/r2` or `d1/d2`, convex `polyhedron(points, faces)` with planar faces wound clockwise from outside. |
| Shape combinations | `union()`, `difference()`, `intersection()`, `hull()`. Put child shapes inside braces. |
| Transforms | `translate(v)`, `rotate(a)` or `rotate(a,v)`, `scale(v)`, `mirror(v)`, `multmatrix(m)` with an affine 3×4 or 4×4 matrix. A 2D drawing stays in the XY plane. |
| Shape conversions | `linear_extrude(height=…,center=…,twist=…,slices=…,scale=…)`, `rotate_extrude(angle=…)`, `offset(r=…)` or `offset(delta=…,chamfer=…)`, `projection(cut=false)` for a silhouette or `projection(cut=true)` for a Z=0 section. |
| Parameters and expressions | Numeric assignments, `true` / `false`, `PI`, vectors, indexing, parentheses, `+ - * / % ^`, comparison operators, `! && ||`, and `condition ? a : b`. Vector addition/subtraction, scalar scaling and flat vector dot products are supported. |
| Math functions | `abs`, `sign`, `sqrt`, `pow`, `exp`, `ln`, `log` (base 10), `round`, `ceil`, `floor`, `min`, `max`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `len` and `norm`. |
| Repetition and reuse | `module name(args) { … }`, `function name(args) = expression;`, positional and named arguments, parameter defaults, `children()`, `children(index)`, `$children`, `if (…) … else …`, `for (i = …) …`, vector comprehensions and inclusive ranges `[start:end]` or `[start:step:end]`. A descending range uses a negative step. |
| Curves and comments | `$fn` sets facet count on supported curved geometry. Use `//` for a line comment and `/* … */` for a block comment. |

OpenSCAD library directives (`include`, `use`), file-backed `import`, string and text geometry, animation and preview modifiers are outside this subset. Open existing scripts in **Preview** to check compatibility. The compiler accepts up to 64 KiB of UTF-8 source, checks numeric and recursion limits, and caps range and geometry complexity. Reducing `$fn`, the number of loop instances, or extrusion slices can keep a detailed model within those limits.
