# Drill holes and peck cycles

Use **CAM → Router drilling** for a single bore or a rectangular hole pattern. Each hole is a circular design outline at the bit diameter. Studio preserves the centres through moves, rotation, save and reopen. The desktop, hosted and local-file editors can generate the same drilling program.

## Make a four-hole plate

1. Choose **Router** in CAM. As a simulator example, set the cutter diameter to 4 mm, cut depth to 2.5 mm, clearance Z to 3 mm, and plunge feed to 80 mm/min. Enter the spindle RPM for your setup.
2. Under **Router drilling**, set the first hole centre to (25, 30) mm. Enter 2 columns at a 12 mm pitch and 2 rows at a 10 mm pitch. Use one row and one column for a single hole.
3. Enter a **Peck depth** of 1 mm and choose **Add drilling pattern**. The new object's operation stores its own drill settings. Generate a preview. The four circles indicate the bit footprint at the four centres. For an existing circular drawing, choose **Drill circular holes** under **Operation**, set the same cutter diameter as its circles and assign those settings to the object.
4. In the G-code preview, each XY traverse starts at clearance height. Studio feeds to Z −1, −2 and −2.5 mm at each location, returning to clearance Z after **every** peck. A peck depth of zero produces one feed plunge per hole. Programs use explicit absolute moves and work with router dialects that lack canned drill cycles.
5. Open the 3D stock preview. **Drill point angle** is the included angle of the conical tip. The specified depth is measured at the tip, so the full-diameter portion stops above that depth. To use a centre-cutting flat end mill, choose **Flat end mill** when assigning its manual tool and current drilling settings. Check the manufacturer's plunge capability and your fixture clearance.

For different bits in one project, assign the tool identities in **Manual tool changes** and export separate stages. Re-enter the measured work Z after changing tools. Peck timing includes every downward feed from clearance. Rapid retracts and spindle acceleration add machine time.

A larger circular outline requires a matching larger drill or a milled profile or pocket. An oval, open polyline or incomplete arc cannot determine a drilled hole of the specified diameter. For imported geometry, circular outlines should follow a single full turn with at least eight chords. Resizing a hole pattern changes the outlines and can require a different bit.
