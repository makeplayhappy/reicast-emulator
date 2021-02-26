TA => Tile Accelerator
HOLLY

The graphics/interface core HOLLY is divided into three blocks: the Power VR core (CORE) block,
which is the graphics-related block; the Tile Accelerator (TA) block, which is used during data transfers to the CORE; and the System Bus (SB) block, which is the interface block that handles data transfers among all devices, including the graphics-related block. (Details on each of these blocks are provided in subsequent sections.)

<System register interface>
This is the interface between the SH4 and the HOLLY's internal system registers; the root bus (the bus that links all of the interfaces in the SB block) does not pass through this interface.
This interface uses no waits (5 clock operation) and only 4-byte access; the transfer speed is 80MB/s.
<Root bus interface>
This interface is used to access the root bus that carries data between peripheral devices. The number of waits and the number accesses both depend on the target device of the access, but basically accesses are made in units of 1/2/4/32 bytes.
Burst access utilizes the wraparound function. This interface has a 32-byte write buffer (large enough for two single writes or one burst write). Only when there are consecutive single writes do consecutive writes occur on the root bus, making high-speed access possible (but attention must be made to possession of the bus). The maximum transfer speed is 356MB/s (during a burst write).
<TA FIFO interface>
This interface is primarily used for transferring polygon data and texture data to the TA FIFO.

ISP / TSP

ISP(Image Synthesis Processor)
The ISP performs on-chip depth sorting for triangles without requiring an external Z buffer.

Triangle Setup block consists of the ISP SETUP FPU and the TSP SETUP FPU

TSP(Texture and Shading Processor)


Polygon List

HOLLY utilizes the following five lists:
(1) Opaque: Opaque polygon list
(2) Punch Through: Punch Through polygon list
(3) Opaque Modifier Volume: Opaque polygon and Punch Modifier
(4) Translucent: Translucent polygon list
(5) Translucent Modifier Volume: Translucent Polygon Modifier Volume list



The Opaque list is for a non-textured polygon with no alpha blending, or for a textured polygon with no alpha blending in which all of the texels are opaque (with an alpha value of 1.0 only). 

The Punch Through is for a textured polygon with no alpha blending in which all of the texels are transparent or opaque (with an alpha value of 0.0 or 1.0 only). 

The Translucent List is for textured and non-textured polygons with alpha blending, or for a textured polygon with no alpha blending in which the texels are translucent (with an alpha value ranging from 0.0 to 1.0). 

In addition, Modifier Volume lists are for polygons that are used to distinguish different areas in order to give an object a three-dimensional feel through shadows, etc. 
There are two types of Modifier Volumes, one for Opaque and Punch Through polygons and one for Translucent polygons. (Refer to Section 3.4.3.)

These lists are drawn in order, starting from (1), for each Tile. When drawing Opaque polygons, the ISP processes the number of Opaque polygons that exist in the Tile in question, and then the TSP performs texturing and shading processing on those pixels that are visible. When drawing Punch Through polygons, the ISP sorts the polygons that exist in the Tile in question, starting form the front, and then the TSP performs texturing and shading processing on those pixels that are visible. This processing by the ISP and the TSP continues until all of the pixels in the Tile have been drawn. Furthermore, when drawing translucent polygons, the ISP draws the product of the number of translucent polygons that exist in the Tile in question multiplied by the number of overlapping polygons (when in Auto Sort mode), and then the TSP performs texturing and shading processing on all pixels in the translucent polygons. Therefore, it is important to be aware that drawing translucent polygons can require much more processing time than drawing Opaque polygons.

It is also necessary to note that this also applies to Opaque Modifier Volumes and Translucent Modifier Volumes.



Culling Mode
This specifies the back-face culling mode. The "No Culling" specification means that culling is not performed. The value that is specified in the FPU_CULL_VAL register is used in the remaining three specifications.
 Setting
Culling Mode
Processing
( |det| < fpu_cull_val ) ( |det| < 0 ) or

0 No culling
1 Cull if Small
2 Cull if Negative
3 Cull if Positive

FPUCULL_VAL Address:0x005F8078 30-0
IEEE floating point value for culling compare

Depth Compare Mode
0 ‘Normal’ Polygon
1 Inside Last Polygon
2 Outside Last Polygon
3-7 Reserved


Depth Compare Mode
This bit is used in combination with the Z Write Disable bit, and supports compare processing, which is required for OpenGL and D3D versus Z buffer updates. It is important to note that, because the value of either 1/z or 1/w is referenced for the Z value, the closer that the polygon is, the larger that the Z value will be.

This setting is ignored for Translucent polygons in Auto-sort mode; the comparison must be made on a "Greater or Equal" basis. This setting is also ignored for Punch Through polygons in HOLLY2; the comparison must be made on a "Less or Equal" basis.
0 Never
1 Less
2 Equal
3 Less Or Equal
4 Greater
5 Not Equal
6 Greater Or Equal
7 Always



Auto-sort Mode
In auto-sort mode, the hardware automatically sorts polygons as individual pixels, and draws the pixels starting from the farthest Z value, regardless of the order in which the polygons were input to the TA (registered in the display list). Therefore, α blending is performed properly even in a case where two translucent polygons intersect. However, because the polygons are sorted as individual pixels, sort processing must be performed for [the number of registered polygons] × [the number of overlapping pixels], with the result that a large amount of processing time is required when a large number of translucent polygons overlap.

Furthermore, in auto-sort mode "Depth Compare Mode," specified in the ISP/TSP Instruction Word, is disabled; Z values are always compared on the basis of "greater or equal." When two pixels have the same Z value, the polygon that was input to the TA first is drawn the farthest away.