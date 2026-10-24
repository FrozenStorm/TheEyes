"""Steampunk goggles enclosure for TheEyes (variant D "Bullaugen + Manometer",
magnetic hat mount from variant C).

Parametric CadQuery model. Run:  python brille.py   -> writes STL + STEP into out/

Coordinate system (mm), seen from the front (the viewer looks at the eyes):
  x to the right, y up, z towards the viewer.
  Origin = centre of the Waveshare ESP32-S3-DualEye-Touch-LCD-1.28 PCB.
  z = 0 is the front face of the PCB (display side), the PCB occupies z -1.6..0,
  the components and the M2 standoffs point backwards (negative z).

Parts (print each separately):
  front    porthole bezels, display windows, pressure gauge with camera pocket
  carrier  2 mm plate screwed to the board's standoffs, holds the XIAO
  back     rear cover with battery bay and power switch slot
  saddle   wedge that is screwed to the back and is shaped to the hat crown,
           with 4 magnet pockets
  counter  curved strip with 4 magnets that goes inside the hat
"""
import math
import os

import cadquery as cq

# ---------------------------------------------------------------- parameters
# Board (from the Waveshare STEP/DXF, centred on the PCB)
EYE_DX = 28.5            # eye centre x = +-EYE_DX  (57 mm eye distance)
PCB_R = 18.25            # round parts of the PCB
PCB_BRIDGE_H = 12.8      # straight middle bridge, 14.5 wide
PCB_T = 1.6
DISP_R = 17.8            # display glass radius
DISP_FRONT = 1.2         # display glass front, measured from PCB front face
ACTIVE_R = 16.2          # visible area (32.4 mm)
STANDOFF_L = 4.0         # M2 standoffs on the back of the board
STANDOFFS = [(39.0, -11.3), (-38.94, -11.3), (18.2, 11.35), (-18.07, 11.3)]
# Parts on the back of the board that are taller than the standoffs
FPC_CONN = [(-36.9, -20.1, -15.9, -12.4), (20.1, 36.9, -15.9, -12.4)]  # x0,x1,y0,y1
BAT_CONN = (-44.6, -39.8, -8.8, 8.8)     # BAT (upper) + speaker (lower) plugs, open to -x
USB_W, USB_Y0 = 11.8, -6.65              # USB-C on the bottom edge of the bridge

# Housing
R_BODY = 26.0            # outer radius of the two portholes / body lobes
WALL = 2.0
GAP = 0.2                # display glass to window lip
LIP_T = 1.5              # window lip in front of the display
WINDOW_R = 16.7          # window opening (active area + margin)
BEZEL_H = 4.5            # porthole ring height in front of the face plate
BOLT_N = 8
SCREWS = []              # 4 housing screws (M2x12), outside the PCB outline
for sx in (-1, 1):
    for sy in (-1, 1):
        a = math.radians(45)
        SCREWS.append((sx * (EYE_DX + 21.6 * math.cos(a)), sy * 21.6 * math.sin(a)))

# Pressure gauge with the camera
GAUGE_R = 10.0
GAUGE_Y = 4.0
GAUGE_H = 6.0            # how far the gauge stands in front of the bezels' base
CAM_Y = -1.5             # lens position (in front of the PCB bridge)
CAM_W, CAM_H, CAM_D = 8.6, 8.6, 6.0   # OV3660 module pocket (w, h, depth along the lens)
LENS_R = 3.6

# Inside the back
CARRIER_T = 2.0
XIAO_W, XIAO_H, XIAO_T = 21.0, 17.8, 8.8   # XIAO ESP32S3 + Sense board, without camera
BATT_W, BATT_H, BATT_T = 50.5, 34.5, 6.5   # LiPo 603450 incl. tolerance
SWITCH = (8.8, 4.0)      # SS12D00 slide switch body seen from below

# Hat mount
HAT_R = 87.5             # radius of the hat crown at the front (measured on Daniel's hat)
TILT = 5.0               # goggles tilted down by this many degrees
SADDLE_W, SADDLE_H, SADDLE_T = 76.0, 36.0, 3.0
MAGNET_D, MAGNET_T = 10.2, 3.2      # 10x3 mm neodymium discs + clearance
MAGNETS = [(-22, -9), (22, -9), (-22, 9), (22, 9)]
HAT_WALL = 2.5           # felt + stiffener of the crown
SADDLE_SCREWS = [(-30, 0), (30, 0)]  # M3 heat inserts in the back

# Derived z levels
Z_LIP0 = DISP_FRONT + GAP                 # back of window lip
Z_FACE = Z_LIP0 + LIP_T                   # front face plate
Z_CARRIER0 = -PCB_T - STANDOFF_L          # carrier front face (on the standoffs)
Z_CARRIER1 = Z_CARRIER0 - CARRIER_T
Z_XIAO1 = Z_CARRIER1 - XIAO_T
Z_BATT1 = Z_XIAO1 - 0.4 - BATT_T
Z_BACK = Z_BATT1 - 0.4 - WALL             # outer back face


# ---------------------------------------------------------------- helpers
def lobes(r, z0, z1):
    """Body outline: hull of the two porthole circles, extruded from z0 to z1."""
    h = z1 - z0
    w = cq.Workplane("XY").workplane(offset=z0)
    s = (w.pushPoints([(-EYE_DX, 0), (EYE_DX, 0)]).circle(r).extrude(h)
         .union(w.rect(2 * EYE_DX, 2 * r).extrude(h)))
    return s


def pcb_outline(grow, z0, z1):
    w = cq.Workplane("XY").workplane(offset=z0)
    h = z1 - z0
    s = w.pushPoints([(-EYE_DX, 0), (EYE_DX, 0)]).circle(PCB_R + grow).extrude(h)
    s = s.union(w.rect(2 * EYE_DX, PCB_BRIDGE_H + 2 * grow).extrude(h))
    return s


def box(x0, x1, y0, y1, z0, z1):
    return (cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0)
            .translate(((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2)))


def cyl(x, y, r, z0, z1):
    return cq.Workplane("XY").workplane(offset=z0).center(x, y).circle(r).extrude(z1 - z0)


def hexagon(x, y, af, z0, h):
    return (cq.Workplane("XY").workplane(offset=z0).center(x, y)
            .polygon(6, af / math.cos(math.radians(30))).extrude(h))


def usb_arch(z0, z1):
    """Nose-bridge shaped notch under the PCB bridge so the USB-C plug fits."""
    w = cq.Workplane("XY").workplane(offset=z0)
    top = USB_Y0 - 0.3
    arch = (w.moveTo(-USB_W / 2 - 1.5, -R_BODY - 1)
            .lineTo(-USB_W / 2, top - 3)
            .threePointArc((0, top), (USB_W / 2, top - 3))
            .lineTo(USB_W / 2 + 1.5, -R_BODY - 1).close().extrude(z1 - z0))
    return arch


# ---------------------------------------------------------------- front
def make_front():
    z_bezel = Z_FACE + BEZEL_H
    body = lobes(R_BODY, Z_CARRIER0, Z_FACE)
    # hollow: room for PCB + display, plus the lip that holds the displays
    body = body.cut(lobes(R_BODY - WALL, Z_CARRIER0 - 1, Z_LIP0))
    # porthole rings with rounded top
    for sx in (-1, 1):
        cx = sx * EYE_DX
        ring = cyl(cx, 0, R_BODY, Z_FACE - 0.01, z_bezel).cut(cyl(cx, 0, WINDOW_R + 3, Z_FACE - 1, z_bezel + 1))
        ring = ring.faces(">Z").edges().fillet(1.2)
        body = body.union(ring)
        # inner sleeve of the window, slightly lower
        body = body.union(cyl(cx, 0, WINDOW_R + 3, Z_FACE - 0.01, z_bezel - 1.2))
        for i in range(BOLT_N):
            a = 2 * math.pi * (i + 0.5) / BOLT_N
            bx, by = cx + 22.6 * math.cos(a), 22.6 * math.sin(a)
            body = body.union(hexagon(bx, by, 3.2, z_bezel - 0.2, 1.4))
        # window, chamfered
        win = (cq.Workplane("XY").workplane(offset=Z_LIP0 - 1).center(cx, 0).circle(WINDOW_R)
               .workplane(offset=z_bezel - Z_LIP0 + 2).circle(WINDOW_R + 2.2).loft())
        body = body.cut(win)
    # pressure gauge
    gz0, gz1 = Z_FACE - 0.01, z_bezel + GAUGE_H - BEZEL_H + 2.5
    gauge = cyl(0, GAUGE_Y, GAUGE_R + 1.6, gz0, gz1)
    gauge = gauge.faces(">Z").edges().fillet(0.8)
    gauge = gauge.cut(cyl(0, GAUGE_Y, GAUGE_R - 0.4, gz1 - 1.2, gz1 + 1))   # recessed dial
    body = body.union(gauge)
    dial_z = gz1 - 1.2
    for i in range(9):                                     # tick marks
        a = math.radians(225 - i * 33.75)
        tx, ty = (GAUGE_R - 2.2) * math.cos(a), GAUGE_Y + (GAUGE_R - 2.2) * math.sin(a)
        tick = (cq.Workplane("XY").box(0.7, 2.2, 0.5).rotate((0, 0, 0), (0, 0, 1), math.degrees(a) - 90)
                .translate((tx, ty, dial_z + 0.25)))
        body = body.union(tick)
    needle = (cq.Workplane("XY").box(0.9, 6.5, 0.6).translate((0, 3.25, 0))
              .rotate((0, 0, 0), (0, 0, 1), -40).translate((0, GAUGE_Y, dial_z + 0.3)))
    body = body.union(needle).union(cyl(0, GAUGE_Y, 1.2, dial_z, dial_z + 0.9))
    # valve stem on top of the gauge
    stem_y = GAUGE_Y + GAUGE_R + 1.6
    body = body.union(box(-2.2, 2.2, stem_y - 2, stem_y + 2.5, Z_FACE, gz1 - 2.5))
    body = body.union(cq.Workplane("XZ").workplane(offset=-(stem_y + 2.5)).center(0, (Z_FACE + gz1 - 2.5) / 2)
                      .circle(2.6).extrude(-2.0))
    # camera pocket (behind the dial, in front of the PCB bridge) + lens hole with FOV cone
    cam_z1 = dial_z - 1.0                 # lens front sits right behind the 1 mm dial
    body = body.cut(box(-CAM_W / 2, CAM_W / 2, CAM_Y - CAM_H / 2, CAM_Y + CAM_H / 2, 0.2, cam_z1))
    fov = math.tan(math.radians(36))      # OV3660 ~68 deg diagonal
    cone = (cq.Workplane("XY").workplane(offset=cam_z1 - 0.05).center(0, CAM_Y).circle(LENS_R)
            .workplane(offset=gz1 - cam_z1 + 0.1).circle(LENS_R + fov * (gz1 - cam_z1 + 0.1))
            .loft())
    body = body.cut(cone)
    # flex channel from the camera pocket over the top edge of the bridge to the back
    body = body.cut(box(-5, 5, CAM_Y, PCB_BRIDGE_H / 2 + 5.5, Z_CARRIER0 - 1, 3.0))
    # screw bosses (pilot for M2 self-tapping)
    for (sx, sy) in SCREWS:
        body = body.union(cyl(sx, sy, 2.3, Z_CARRIER0, Z_LIP0 + 0.1))
        body = body.cut(cyl(sx, sy, 0.85, Z_CARRIER0 - 1, Z_LIP0 - 0.5))
    # keep the PCB and display free
    body = body.cut(pcb_outline(0.35, -PCB_T - 0.1, Z_LIP0 - 0.001))
    body = body.cut(usb_arch(Z_CARRIER0 - 1, z_bezel + 10))
    return body


# ---------------------------------------------------------------- carrier
def make_carrier():
    c = lobes(R_BODY, Z_CARRIER1, Z_CARRIER0)
    for (x0, x1, y0, y1) in FPC_CONN:
        c = c.cut(box(x0, x1, y0, y1, Z_CARRIER1 - 1, Z_CARRIER0 + 1))
    x0, x1, y0, y1 = BAT_CONN
    c = c.cut(box(-R_BODY - EYE_DX, x1 + 1, y0 - 1.5, y1 + 1.5, Z_CARRIER1 - 1, Z_CARRIER0 + 1))
    c = c.union(lobes(R_BODY, Z_CARRIER1, Z_CARRIER0).cut(lobes(R_BODY - WALL, Z_CARRIER1 - 1, Z_CARRIER0 + 1)))
    c = c.cut(box(-6, 6, PCB_BRIDGE_H / 2 + 0.5, PCB_BRIDGE_H / 2 + 6, Z_CARRIER1 - 1, Z_CARRIER0 + 1))  # flex
    c = c.cut(usb_arch(Z_CARRIER1 - 1, Z_CARRIER0 + 1))
    for (sx, sy) in STANDOFFS:          # M2x5 into the board's standoffs
        c = c.cut(cyl(sx, sy, 1.15, Z_CARRIER1 - 1, Z_CARRIER0 + 1))
    for (sx, sy) in SCREWS:
        c = c.cut(cyl(sx, sy, 1.15, Z_CARRIER1 - 1, Z_CARRIER0 + 1))
    # XIAO cradle: four corner brackets on the back
    hx, hy = XIAO_W / 2 + 0.3, XIAO_H / 2 + 0.3
    for sx in (-1, 1):
        for sy in (-1, 1):
            cx, cy = sx * hx, sy * hy
            l = box(min(cx, cx + sx * 1.4), max(cx, cx + sx * 1.4), min(cy, cy - sy * 5), max(cy, cy - sy * 5) + 0,
                    Z_XIAO1 + 1.5, Z_CARRIER1)
            l2 = box(min(cx, cx - sx * 5), max(cx, cx - sx * 5), min(cy, cy + sy * 1.4), max(cy, cy + sy * 1.4),
                     Z_XIAO1 + 1.5, Z_CARRIER1)
            c = c.union(l).union(l2)
    return c


# ---------------------------------------------------------------- back
def make_back():
    b = lobes(R_BODY, Z_BACK, Z_CARRIER1)
    b = b.cut(lobes(R_BODY - WALL, Z_BACK + WALL, Z_CARRIER1 + 1))
    b = b.edges("<Z").fillet(2.0)
    # battery fence on the inside of the back wall
    fx, fy = BATT_W / 2 + 0.2, BATT_H / 2 + 0.2
    fence = box(-fx - 1.2, fx + 1.2, -fy - 1.2, fy + 1.2, Z_BACK + WALL, Z_BACK + WALL + 2.5)
    fence = fence.cut(box(-fx, fx, -fy, fy, Z_BACK, Z_BACK + 10))
    fence = fence.cut(box(-fx + 6, fx - 6, -fy - 3, fy + 3, Z_BACK, Z_BACK + 10))   # open sides for cables
    b = b.union(fence)
    # screw pillars with counterbore from the back (M2x12)
    head_z = Z_CARRIER0 + 6.0 - 12.0
    for (sx, sy) in SCREWS:
        b = b.union(cyl(sx, sy, 3.2, Z_BACK + 0.5, Z_CARRIER1))
        b = b.cut(cyl(sx, sy, 1.15, Z_BACK - 1, Z_CARRIER1 + 1))
        b = b.cut(cyl(sx, sy, 2.1, Z_BACK - 1, head_z))
    # heat insert bosses for the saddle (M3, OD 4.0)
    for (sx, sy) in SADDLE_SCREWS:
        b = b.union(cyl(sx, sy, 3.6, Z_BACK + 0.5, Z_BACK + WALL + 5))
        b = b.cut(cyl(sx, sy, 2.0, Z_BACK - 1, Z_BACK + WALL + 4.5))
    # slide switch in the bottom wall, right side, behind the battery bay
    sw_x, sw_z = 32.0, (Z_BACK + Z_CARRIER1) / 2
    b = b.cut(box(sw_x - 2.4, sw_x + 2.4, -R_BODY - 1, -R_BODY + WALL + 0.5, sw_z - 1.3, sw_z + 1.3))
    pocket = box(sw_x - SWITCH[0] / 2 - 1.6, sw_x + SWITCH[0] / 2 + 1.6, -R_BODY + WALL - 0.01,
                 -R_BODY + WALL + 4.5, sw_z - SWITCH[1] / 2 - 1.6, sw_z + SWITCH[1] / 2 + 1.6)
    pocket = pocket.cut(box(sw_x - SWITCH[0] / 2, sw_x + SWITCH[0] / 2, -R_BODY, -R_BODY + WALL + 5,
                            sw_z - SWITCH[1] / 2, sw_z + SWITCH[1] / 2))
    b = b.union(pocket)
    return b


# ---------------------------------------------------------------- hat mount
def hat_cylinder(r, z_touch):
    """Vertical cylinder (the hat crown), tilted by TILT, touching z_touch at x=0."""
    c = (cq.Workplane("XZ").circle(r).extrude(200, both=True)
         .rotate((0, 0, 0), (0, 0, 1), 0)
         .translate((0, 0, 0)))
    c = cq.Workplane("XY").circle(r).extrude(300).translate((0, 0, -150))   # axis along z
    c = c.rotate((0, 0, 0), (1, 0, 0), 90)                                   # axis along y (vertical)
    c = c.rotate((0, 0, 0), (1, 0, 0), -TILT)
    return c.translate((0, 0, z_touch - r))


def make_saddle():
    z_front = Z_BACK
    z_touch = z_front - SADDLE_T
    hw = SADDLE_W / 2
    depth = HAT_R - math.sqrt(HAT_R ** 2 - hw ** 2) + 8
    s = box(-hw, hw, -SADDLE_H / 2, SADDLE_H / 2, z_touch - depth, z_front)
    s = s.edges("|Z").fillet(4)
    s = s.cut(hat_cylinder(HAT_R, z_touch))
    for (sx, sy) in SADDLE_SCREWS:
        s = s.cut(cyl(sx, sy, 1.7, z_touch - depth - 1, z_front + 1))
        s = s.cut(cyl(sx, sy, 3.1, z_touch - depth - 1, z_front - 4.0))   # head access
    for (mx, my) in MAGNETS:
        # pocket from the curved surface, along z
        zs = z_touch - (HAT_R - math.sqrt(HAT_R ** 2 - mx ** 2)) + my * math.tan(math.radians(TILT))
        s = s.cut(cyl(mx, my, MAGNET_D / 2, zs - 5, zs + MAGNET_T))
    return s


def make_counter():
    """Curved strip for the inside of the crown, magnets face outwards."""
    hw = SADDLE_W / 2
    r_out = HAT_R - HAT_WALL
    z_touch = Z_BACK - SADDLE_T - HAT_WALL
    outer = hat_cylinder(r_out, z_touch)
    inner = hat_cylinder(r_out - 3.6, z_touch - 3.6)
    s = box(-hw, hw, -SADDLE_H / 2, SADDLE_H / 2, z_touch - 40, z_touch + 1).intersect(outer).cut(inner)
    for (mx, my) in MAGNETS:
        zs = z_touch - (r_out - math.sqrt(r_out ** 2 - mx ** 2)) + my * math.tan(math.radians(TILT))
        s = s.cut(cyl(mx, my, MAGNET_D / 2, zs - MAGNET_T, zs + 3))
    return s


PARTS = {
    "front": make_front,
    "carrier": make_carrier,
    "back": make_back,
    "saddle": make_saddle,
    "counter": make_counter,
}

if __name__ == "__main__":
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
    os.makedirs(out, exist_ok=True)
    asm = cq.Assembly()
    for name, fn in PARTS.items():
        part = fn()
        cq.exporters.export(part, os.path.join(out, f"{name}.stl"), tolerance=0.05, angularTolerance=0.15)
        cq.exporters.export(part, os.path.join(out, f"{name}.step"))
        asm.add(part, name=name)
        bb = part.val().BoundingBox()
        print(f"{name:8s} {bb.xlen:6.1f} x {bb.ylen:6.1f} x {bb.zlen:6.1f} mm")
    asm.save(os.path.join(out, "brille_assembly.step"))
