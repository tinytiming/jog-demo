#!/usr/bin/env python3
"""
Jog-dial FDM light pipe  --  generator  (Spool & Spur)
=======================================================
Builds a 3D-printable light pipe that sits ON TOP of the jog-dial breakout PCB:
it encapsulates the side-emitting RGB LED (LED1), carries the light along a
floating bar over the MCU (U1), then a transverse manifold splits it to two
skirts + a flat cover that form a light box around the jog dial (SW1).

Outputs light_pipe.step + light_pipe.stl.

Dependencies:  build123d, trimesh, shapely, numpy   (cadquery-ocp / OCP kernel)
The board.stl collision check at the end is OPTIONAL -- set BOARD_STL to your
file or leave it; if the file is missing the check is skipped.

UNITS: millimetres.  FRAME: the board.stl frame, Z up.
  - PCB front face (where parts mount AND where this pipe seats) is z = 1.51 (ZB).
  - Everything below is expressed relative to ZB so you can re-floor it easily.

KiCad position file -> this STL frame (verified against all 4 mount holes + J1):
      stl_x = kicad_x - 136.45
      stl_y = kicad_y + 60.75          # NOTE: +, not -. The mount holes are
                                       # mirror-symmetric in Y so they cannot
                                       # disambiguate the sign; J1 (asymmetric) does.

Key board features in this frame (origin = component placement origin):
  LED1  (-7.45, -0.10)  side-emitting, fires +X out its end face (z 1.51..2.60)
  U1    ( 0.30,  1.15)  QFN 3x3, top ~z2.35 (0.75mm pkg + solder); the bar floats over it
  SW1   ( 6.50, -0.10)  jog dial, body to x15.05, top flat at z4.70
  J1    (-2.45, -6.75)  4-pin JST, -Y side, OUTSIDE the footprint
  H1 (-7.70, 5.25)  H3 (-7.70,-5.25)   <- LED end, LEFT FREE for the PCB
  H2 ( 2.80,-7.75)  H4 ( 2.80, 7.75)   <- dial end, USED to mount the pipe
  R2 (1.65,-2.60) R5 (2.05,-4.00) R12 (2.05,-5.00)  0402s under the manifold (-Y)
"""

import numpy as np, trimesh
from shapely.geometry import box as sbox
from shapely.ops import unary_union
from shapely.geometry.polygon import orient as _orient
from build123d import (Polygon, Box, Cylinder, Cone, Pos, Plane, Axis, Align,
                       Rectangle, loft, extrude, chamfer, export_step, export_stl)

BOARD_STL = '/mnt/user-data/uploads/board.stl'   # only used for the collision check

# ----------------------------------------------------------------------------
# Z LEVELS  (the main things you'll tweak)
# ----------------------------------------------------------------------------
ZB     = 1.51            # PCB front face = pipe seating plane. Everything sits on this.
ZF     = ZB + 0.99       # = 2.50  bar underside over the MCU (the "floor").
                         #   TOP-ZF is the solid section over the MCU (currently 3.50mm).
                         #   Lower ZF = thicker section + less air gap, but U1 top is ~2.35,
                         #   so ZF below ~2.40 starts touching the chip. 2.50 leaves ~0.15mm.
CUNDER = ZB + 3.79       # = 5.30  cover underside / dial-cave ceiling. Clears the dial
                         #   top (4.70) by 0.60mm. Raise if the jog LEVER fouls it when tilted.
TOP    = ZB + 4.49       # = 6.00  single flat top plane. This face prints ON THE BED
                         #   (upside-down print) so it comes out smooth. Raising TOP makes
                         #   the whole part taller; the cover/bar thicken with it.

# ----------------------------------------------------------------------------
# small geometry helpers (absolute-coordinate primitives)
# ----------------------------------------------------------------------------
def box(x0,x1,y0,y1,z0,z1):
    return Pos((x0+x1)/2,(y0+y1)/2,(z0+z1)/2)*Box(x1-x0,y1-y0,z1-z0)
def cyl(cx,cy,r,z0,z1):
    return Pos(cx,cy,(z0+z1)/2)*Cylinder(radius=r,height=z1-z0)
def _cc(poly):                                   # shapely polygon -> CCW vertex list
    return [(float(x),float(y)) for x,y in _orient(poly,1.0).exterior.coords[:-1]]
def ext(poly,z0,z1):                             # extrude a shapely footprint between two Z
    # NB: force CCW winding (_orient) or build123d extrudes the solid INSIDE-OUT,
    # which silently no-ops every later boolean cut. This bit me once; leave it.
    return Pos(0,0,z0)*extrude(Polygon(*_cc(poly),align=None),amount=z1-z0)

# ----------------------------------------------------------------------------
# 1) FOOTPRINT  (sbox is sbox(minx, miny, maxx, maxy) -- NOT x,y,w,h)
#    Kept rectilinear/sharp on purpose so the chamfer step can cut clean edges.
# ----------------------------------------------------------------------------
feed   = sbox(-8.40, -2.60,  3.45,  2.60)   # feed bar: LED end -> manifold, y +-2.6
manif  = sbox( 1.80, -8.60,  3.45,  8.60)   # transverse manifold (the splitter wall)
skirtP = sbox( 2.00,  7.40, 12.30,  8.60)   # +Y skirt down the dial side
skirtM = sbox( 2.00, -8.60, 12.30, -7.40)   # -Y skirt down the dial side
cover  = sbox( 3.45, -7.60, 12.30,  7.60)   # flat cover spanning over the dial top
footprint = unary_union([feed, manif, skirtP, skirtM, cover])

body = ext(footprint, ZB, TOP)              # solid block, seat to flat top

# ----------------------------------------------------------------------------
# 2) CUTS  (carve the optical path + clearances out of the block)
# ----------------------------------------------------------------------------
# Float the bar over the MCU (U1) -- the air gap directly above the chip.
body = body - box(-2.00, 2.00, -2.80, 2.80, ZB, ZF)
# Ramp lifting the captured LED beam up to the float height so it clears U1.
body = body - extrude(Plane.XZ*Polygon((-6,ZB),(-2,ZF),(-2,ZB),align=None), amount=4.0, both=True)
# LED1 pocket: closed top, open +X toward the dial (captures the side emission).
body = body - box(-8.10, -6.40, -2.05, 2.25, ZB, ZB+1.25)
# Dial cave: hollow under the cover so the dial sits in a light box (cover stays above CUNDER).
body = body - box(3.50, 12.40, -7.30, 7.30, ZB, CUNDER)
# 0402 clearance pocket for R2/R5/R12 under the manifold on the -Y side (0.85mm recess).
#   ceiling 2.35 clears a ~0.5mm 0402; deepen if your parts/paste run taller.
body = body - box(0.60, 3.10, -5.80, -2.00, ZB, 2.35)

# Cave-ceiling 45-deg bevel, done GEOMETRICALLY: the OCC kernel here won't reliably
# select that internal edge to chamfer it, so we subtract a small flared frustum instead.
_cx,_cy,_c = 7.95, 0.0, 0.45
_s1 = Plane(origin=(_cx,_cy,CUNDER-_c))     * Rectangle(8.90,        14.60)
_s2 = Plane(origin=(_cx,_cy,CUNDER+0.05))   * Rectangle(8.90+2*_c,   14.60+2*_c)
body = body - loft([_s1, _s2])

# ----------------------------------------------------------------------------
# 3) EDGE TREATMENT
#    This OCC build is fragile chamfering many edges at once, so:
#      - top outer perimeter: one group
#      - vertical edges: one at a time, re-selecting each time (robust)
#      - bottom edges: LEFT SHARP on purpose -> flat seating face on the PCB
#    Each attempt is reverted if it breaks watertightness (the try/size-fallback).
# ----------------------------------------------------------------------------
def _safe(s, edges, L, label):
    edges=list(edges)
    if not edges: print('  ',label,'(no edges)'); return s
    for l in (L, L*0.6, L*0.35):
        try:
            o=chamfer(edges,length=l); export_stl(o,'_t.stl')
            if trimesh.load('_t.stl').is_watertight:
                print('  ',label,'chamfered %.2f (n=%d)'%(l,len(edges))); return o
        except Exception: pass
    print('  ',label,'SKIPPED (n=%d)'%len(edges)); return s

def _chamfer_each(s, getter, sizes, label):
    # chamfer edges individually; re-select on the live solid each pass so refs stay valid
    targets=sorted({(round(e.center().X,2),round(e.center().Y,2),round(e.center().Z,2)) for e in getter(s)})
    done=0
    for (tx,ty,tz) in targets:
        cand=[e for e in getter(s) if abs(e.center().X-tx)<0.25 and abs(e.center().Y-ty)<0.25 and abs(e.center().Z-tz)<0.25]
        if not cand: continue
        for L in sizes:
            try:
                o=chamfer([cand[0]],length=L); export_stl(o,'_t.stl')
                if trimesh.load('_t.stl').is_watertight: s=o; done+=1; break
            except Exception: pass
    print('  ',label,'%d/%d edges'%(done,len(targets)))
    return s

body = _safe(body, body.faces().sort_by(Axis.Z)[-1].edges(), 0.4, 'top perimeter')
body = _chamfer_each(body, lambda s: s.edges().filter_by(Axis.Z), (0.4,0.3,0.18), 'verticals')
# (bottom edges intentionally not chamfered -> flat contact with the PCB)

# ----------------------------------------------------------------------------
# 4) MOUNTING  (boss + clearance hole at H2/H4, coaxial with the PCB holes)
#    A single screw per side clamps pipe + PCB into the enclosure. Bosses give a
#    real wall around the hole where the manifold is otherwise too thin.
# ----------------------------------------------------------------------------
BOSS_R = 2.0            # boss OD 4.0
HOLE_R = 1.2            # M2 clearance, OD 2.4  (drop to ~0.85 for a self-tap pilot)
for (hx,hy) in [(2.80,-7.75),(2.80,7.75)]:
    body = body + cyl(hx,hy,BOSS_R,ZB,TOP)
for (hx,hy) in [(2.80,-7.75),(2.80,7.75)]:
    body = body - cyl(hx,hy,HOLE_R,ZB-0.2,TOP+0.2)

# ----------------------------------------------------------------------------
# 5) EXPORT + sanity check
# ----------------------------------------------------------------------------
export_stl(body,'light_pipe.stl'); export_step(body,'light_pipe.step')
p=trimesh.load('light_pipe.stl')
print('watertight:', p.is_watertight, '| solids:', len(body.solids()), '| volume:', round(body.volume,1))
try:
    b=trimesh.load(BOARD_STL); BV=b.vertices
    comp=BV[BV[:,2]>ZB+0.04]; hit=comp[p.contains(comp)]; hit=hit[hit[:,2]-ZB>0.15]
    print('board collisions (>0.15mm into a component):', len(hit))
except Exception as e:
    print('collision check skipped (no board.stl):', e)
