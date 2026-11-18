# Draw and save a project on the desktop

KerfKite Studio opens a native window on macOS, Windows and Linux. The native object list and numeric geometry controls work on the same document as the canvas, layers and CAD tools. Dimensions are in millimetres. X and Y locate the lower-left corner of an object's bounding box.

## Make a test plate in simulation

1. Open KerfKite Studio from your applications menu. In the setup tab, choose **Native desktop** and **Start workshop**. The default workshop is a simulator.
2. In the native geometry controls, enter X **10**, Y **10**, width **40** and height **20**. Add a rectangle. Select it from the native object list or on the canvas.
3. Set the selected object's X to **20** and Y to **15**, then apply the move. Use **Undo** and **Redo** to compare the placement on the canvas.
4. Choose the **3D CAD** workspace to edit solids, or **CAM** to set up the tool and operation. Generate a **Preview**. Use the File menu to export the resulting G-code. Editing geometry or machining settings requires a fresh preview.
5. Use **File → Save As** and choose a folder. Reopen the resulting `.cncstudio.json` project in another desktop installation or in the browser editor's **Projects** tab. Use **Save As** for a separate working copy.
6. Close the document window when finished. The launcher keeps the workshop service running so you can reopen it. Choose **Quit KerfKite** there to shut it down.

When **Open directly next time** is selected, the next launch opens the native simulator window directly. Closing that window shows the launcher. Choose **Show setup next launch** there to change STEP or controller options on the next start. Controller access always requires an explicit choice in setup.

The native file commands read and write ordinary files. Browser-library saves belong to the browser or embedded engine profile that created them. To bring an existing browser-library project into a native window, download its portable project file, then open that file from the native File menu.

The full CAD, simulation and advanced machining panels use the shared editor inside the desktop window. The Android companion has its own interface and connects to the workshop service for pendant use.

To work from the keyboard, focus the canvas and press **/** for drawing and view commands. Choose **Script…** to preview and edit a parameterised drawing or solid. [CAD scripting tutorial and shortcut reference](CAD-SCRIPTING.md)
