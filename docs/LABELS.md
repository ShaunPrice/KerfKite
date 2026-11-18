# Engraved lettering and production labels

## Single-line text

Open **Text** and choose **Workshop Mono · single line**. Enter lettering, size and spacing. Set the depth for router engraving. Straight, vertical and Bézier layouts use the same editable centreline glyphs. Rotation and Y tilt work with variable-depth toolpaths. This face covers printable ASCII. Use an outline font for other scripts supported by the text editor.

**Browse installed fonts** asks the browser for access to local fonts. Select a face to load it into the editor. Browsers without the Local Font Access API can import a TTF or OTF file. Imported and installed fonts are session-local. Other machines can reproduce the saved paths immediately. To edit the lettering there, load the original font or select a replacement. [Local Font Access API](https://wicg.github.io/local-font-access/)

## Number a batch

1. Choose the X and Y starting position under **Design**. In **Layers**, select the destination layer.
2. In **Text → Production labels**, choose **Engraved text**, enter `PART-{n}`, set the number of labels to 6, the first number to 17 and minimum digits to 3.
3. Set **Labels per row** to 3. The gap is measured between the artwork bounds. Choose **Insert labels**. The six separate objects read PART-017 through PART-022. New rows grow upward.
4. Use **Undo** to remove the whole batch. To revise one label, select it, open its text settings and choose **Replace selected text**. Its layer, operation and position in the object list are preserved.

## Machine-readable codes

Choose **QR code** for a short URL or other text (up to 160 UTF-8 bytes). **Code 39 barcode** supports uppercase A–Z, digits, spaces and the standard punctuation. Start and stop characters are generated automatically. You can insert a single code or use the same serial pattern as the text batch.

Set the physical module width and engraving line spacing. Dark regions become parallel centreline engraving paths. Each code reserves a quiet zone when computing the batch layout: four modules around QR and ten narrow bars at each horizontal end of Code 39. Keep other artwork outside this area. Select a line operation and preview the job. Size the modules for the beam spot or cutter, cut a sample and scan the finished result at the intended distance. The code geometry and batch insertion work in the offline browser editor.
