"""
Wasserraketen-Tracker Gehäuse — STEP Export für Fusion 360
=========================================================
Abhängigkeit:  pip install cadquery
Ausführen:     python generate_step.py
Ausgabe:       rocket_case.step  (importierbar in Fusion 360)

Fusion 360 Import:
  Insert → Insert Mesh  (für STL)
  oder
  File → Open → rocket_case.step  (editierbar als Solid)
"""

import math
import cadquery as cq
from cadquery import exporters

# ── Parameter (identisch mit rocket_case.scad v4) ────────────────────────────
WALL      = 1.4
TOL       = 0.3
BOTTLE_R  = 45.0      # Standard 1,5L Flasche Ø90mm → R=45

# Waveshare ESP32-S3-Touch-AMOLED-1.64 — Datenblatt-Maße
BOARD_W   = 28.6   # PCB Breite
BOARD_L   = 43.5   # PCB Länge (USB-C vorne)
BOARD_H   = 9.6    # Gesamthöhe inkl. Display-Modul (Seitenansicht 9.60mm)
SENSOR_L  = 34.0   # BMP388 (16.5mm) + 3mm Abstand + MPU6050 (14.5mm) — je -1.5mm
GAP       = 2.0

# LiPo — 500mAh (Amazon B0DPZNZ4D6)
BAT_W     = 30.0
BAT_L     = 44.0
BAT_H     = 8.0
RIB_W     = 1.2
POST_D    = 2.6
POST_H    = 2.0

# Display-Fenster: Glasfläche 29.12×44mm, sichtbar ca. 22.34×36.06mm
DISP_W    = 30.0   # Glas 29mm + 0.5mm Toleranz je Seite
DISP_H    = 45.0   # Glas 44mm + 0.5mm Toleranz je Seite
DISP_OFF_Y = 2.3                        # laut Datenblatt 2.28mm

INNER_W   = max(BOARD_W, BAT_W) + TOL * 2 + 2.0                    # +2mm breiter → 32.6 mm
INNER_L   = SENSOR_L + GAP + BAT_L + GAP + BOARD_L + TOL * 2      # 129.1 mm (neu: Sensor→Akku→PCB)
INNER_H   = BOARD_H + 3.0              # 12.6 mm — display bündig mit Oberkante

PCB_X0    = WALL + (INNER_W - BOARD_W) / 2      # PCB zentriert → 3.4mm

OUTER_W   = INNER_W + WALL * 2
OUTER_L   = INNER_L + WALL * 2
OUTER_H   = INNER_H + WALL

FAIRING_NOSE = 14.0   # spitze Nase
FAIRING   = FAIRING_NOSE
TOTAL_L   = OUTER_L + FAIRING_NOSE

# Zonen (Sensor an Spitze, Akku mitte, PCB am Heck)
SEN_Y0    = FAIRING + WALL + TOL                                    #  9.7 mm
BAT_Y0    = SEN_Y0 + SENSOR_L + GAP                                # 48.7 mm
BRD_Y0    = BAT_Y0 + BAT_L + GAP                                   # 94.7 mm

DISP_OX   = PCB_X0 + (BOARD_W - DISP_W) / 2    # 2.7mm — Display über PCB zentriert
# PCB-USB-C zeigt zum Heck → Display-Offset von USB-C-Kante
DISP_OY   = BRD_Y0 + BOARD_L - DISP_OFF_Y - DISP_H                # 99.8 mm

# Deckel-Bossen: 1× Spitze + 2× Heck-Ecken
NOSE_BOSS_R = 3.0;  NOSE_BOSS_D = 2.4
TAIL_BOSS_R = 2.5;  TAIL_BOSS_D = 2.0
NOSE_BOSS_Y  = SEN_Y0 + 4                              # 19.7mm — Sensor-Zone nahe Spitze
TAIL_BOSS_Y  = TOTAL_L - 3                             # 139.9mm — hinter Display-Ende
TAIL_BOSS_LX = -OUTER_W / 2 + WALL + TAIL_BOSS_R + 0.5  # linke Heck-Ecke (CadQuery-Koord.)
TAIL_BOSS_RX =  OUTER_W / 2 - WALL - TAIL_BOSS_R - 0.5  # rechte Heck-Ecke

USB_W     = 9.0;   USB_H = 3.5        # USB-C Stecker
JST_W     = 7.0;   JST_H = 4.0

# ── Aerodynamisches Profil (Draufsicht, mit Cosinusbogen) ────────────────────
def fairing_profile():
    """Spitze Nase (14mm) + flacher runder Abschluss am Heck."""
    hw = OUTER_W / 2
    steps = 12
    pts = []

    pts.append((0.0, 0.0))
    for i in range(1, steps + 1):
        t = i / steps
        x = hw * math.sin(t * math.pi / 2)
        y = FAIRING_NOSE * (1.0 - math.cos(t * math.pi / 2))
        pts.append((x, y))

    pts.append((hw, TOTAL_L))
    pts.append((-hw, TOTAL_L))

    for i in range(steps - 1, 0, -1):
        t = i / steps
        x = -hw * math.sin(t * math.pi / 2)
        y = FAIRING_NOSE * (1.0 - math.cos(t * math.pi / 2))
        pts.append((x, y))

    return pts


# ── Außenhülle ────────────────────────────────────────────────────────────────
profile = fairing_profile()

outer = (
    cq.Workplane("XY")
    .polyline(profile)
    .close()
    .extrude(OUTER_H)
    .edges("|Z").fillet(1.5)    # Kanten in Höhenrichtung abrunden
    .edges(">Z").fillet(0.8)    # Obere Kante abrunden
)

# ── Innenraum ────────────────────────────────────────────────────────────────
# Zentrum des Innenraums in XY:
inner_cx = 0.0
inner_cy = FAIRING + WALL + INNER_L / 2
inner_cz = WALL + INNER_H / 2

cavity = (
    cq.Workplane("XY")
    .box(INNER_W, INNER_L, INNER_H + 0.2, centered=True)
    .translate((inner_cx, inner_cy, inner_cz))
)

# ── Flaschenwölbung (Boden konkav) ───────────────────────────────────────────
bottle_cyl = (
    cq.Workplane("XZ")          # XZ-Ebene → Extrusion entlang Y
    .circle(BOTTLE_R)
    .extrude(TOTAL_L + 4)
    .translate((0.0, -2.0, -(BOTTLE_R - 0.6)))
)

# ── Display-Fenster (oben) ────────────────────────────────────────────────────
disp_cx = -OUTER_W / 2 + DISP_OX + DISP_W / 2
disp_cy = DISP_OY + DISP_H / 2      # jetzt nahe Heck
disp_cz = OUTER_H - WALL / 2

disp_cut = (
    cq.Workplane("XY")
    .box(DISP_W, DISP_H, WALL + 0.4, centered=True)
    .translate((disp_cx, disp_cy, disp_cz))
    .edges("|Z").fillet(1.2)
)

# ── USB-C Kabelkanal — durch Heck-Fairing (PCB am Heck, USB-C zeigt nach hinten)
USB_H      = 5.0
# USB-C durch flache Heckwand
usb_chan_l  = WALL + 2.0
pcb_cx      = -OUTER_W / 2 + PCB_X0 + BOARD_W / 2
usb_y_start = TOTAL_L - WALL - 1.0
usb_cut = (
    cq.Workplane("XY")
    .box(USB_W, usb_chan_l, USB_H, centered=True)
    .translate((pcb_cx, usb_y_start + usb_chan_l / 2, WALL + 2.5 + USB_H / 2))
)

# JST LiPo — Stecker an Hinterkante PCB, Kabel läuft intern → kein Wandschlitz

# ── Druckausgleichsloch BMP388 (Boden Sensor-Zone, nahe Spitze) ──────────────
vent_cx = -OUTER_W / 2 + WALL + TOL + 9.0
vent_cy = SEN_Y0 + 6.5

vent_cut = (
    cq.Workplane("XY")
    .workplane(offset=-1.0)
    .moveTo(vent_cx, vent_cy)
    .circle(1.25)
    .extrude(WALL + 2.0)
)

# ── Batterie-Fach (hinter Sensor-Zone, von oben zugänglich) ──────────────────
bat_x0 = -INNER_W / 2 + (INNER_W - BAT_W) / 2     # zentriert in X
bat_cx = bat_x0 + BAT_W / 2
bat_cy = BAT_Y0 + BAT_L / 2
bat_cz = WALL + (BAT_H + 1.0) / 2

bat_pocket = (
    cq.Workplane("XY")
    .box(BAT_W + TOL * 2, BAT_L + TOL * 2, BAT_H + 1.0, centered=True)
    .translate((bat_cx, bat_cy, bat_cz))
)

# ── Kabelbinder-Schlitze (3 Paare links + rechts) ────────────────────────────
ZIPTIE_W = 5.0   # Schlitzbreite (passt für 3.5–4mm Kabelbinder)
ZIPTIE_H = 3.5   # Schlitzhöhe

ziptie_y_positions = [
    SEN_Y0 + SENSOR_L * 0.5,
    BAT_Y0 + BAT_L * 0.5,
    BRD_Y0 + BOARD_L * 0.5,
]

ziptie_cuts = []
for zy in ziptie_y_positions:
    for sx in [-OUTER_W / 2 - 0.5, OUTER_W / 2 - WALL - 0.5]:
        cut = (
            cq.Workplane("XY")
            .box(WALL + 1.0, ZIPTIE_W, ZIPTIE_H, centered=True)
            .translate((sx + (WALL + 1.0) / 2, zy, WALL + ZIPTIE_H / 2))
        )
        ziptie_cuts.append(cut)

# ── Boolesche Operationen ─────────────────────────────────────────────────────
result = (
    outer
    .cut(cavity)
    .cut(bat_pocket)
    .cut(bottle_cyl)
    .cut(disp_cut)
    .cut(usb_cut)
    .cut(vent_cut)
)
for cut in ziptie_cuts:
    result = result.cut(cut)

# ── M2-Montagebossen (4×) — PCB-Ecken laut Datenblatt ────────────────────────
# PCB-Eckbohrungen: 2.5mm von jeder Kante
M2_BOSS_OD  = 4.6   # Außendurchmesser Boss
M2_BOSS_H   = 3.0   # Höhe (PCB liegt oben auf)
M2_DRILL    = 2.2   # Durchgangsloch für M2-Schraube
M2_HEAD_D   = 4.5   # Versenkung für M2-Flachkopf (von unten)
M2_HEAD_H   = 1.2

pcb_left  = -OUTER_W / 2 + PCB_X0
pcb_front = BRD_Y0                    # PCB jetzt in Heck-Zone

m2_positions = [
    (pcb_left + 2.5,           pcb_front + 2.5),
    (pcb_left + BOARD_W - 2.5, pcb_front + 2.5),
    (pcb_left + 2.5,           pcb_front + BOARD_L - 2.5),
    (pcb_left + BOARD_W - 2.5, pcb_front + BOARD_L - 2.5),
]

for px, py in m2_positions:
    boss = (
        cq.Workplane("XY")
        .workplane(offset=WALL - 0.1)   # 0.1mm in Boden → saubere Fusion
        .moveTo(px, py)
        .circle(M2_BOSS_OD / 2)
        .extrude(M2_BOSS_H + 0.1)
    )
    result = result.union(boss)

    # Durchgangsloch für M2-Schraube (von unten durch Boss)
    drill = (
        cq.Workplane("XY")
        .workplane(offset=-0.1)
        .moveTo(px, py)
        .circle(M2_DRILL / 2)
        .extrude(WALL + M2_BOSS_H + 0.2)
    )
    result = result.cut(drill)

    # Senkkopf-Versenkung von unten (M2-Flachkopf)
    head = (
        cq.Workplane("XY")
        .workplane(offset=-0.1)
        .moveTo(px, py)
        .circle(M2_HEAD_D / 2)
        .extrude(M2_HEAD_H)
    )
    result = result.cut(head)

# ── Deckel-Montagebossen (4×) — in Seitenwände integriert, Sensor-Zone ───────
LID_BLIND_H = 5.0   # Tiefe des Blind-M2-Lochs von oben

lid_boss_positions = [
    (0.0,           NOSE_BOSS_Y, NOSE_BOSS_R, NOSE_BOSS_D),   # Spitze zentriert
    (TAIL_BOSS_LX,  TAIL_BOSS_Y, TAIL_BOSS_R, TAIL_BOSS_D),   # Heck links
    (TAIL_BOSS_RX,  TAIL_BOSS_Y, TAIL_BOSS_R, TAIL_BOSS_D),   # Heck rechts
]

for lx, ly, boss_r, boss_d in lid_boss_positions:
    boss = (
        cq.Workplane("XY")
        .workplane(offset=WALL - 0.1)
        .moveTo(lx, ly)
        .circle(boss_r)
        .extrude(OUTER_H - WALL + 0.1)
    )
    result = result.union(boss)
    blind = (
        cq.Workplane("XY")
        .workplane(offset=OUTER_H - LID_BLIND_H - 0.1)
        .moveTo(lx, ly)
        .circle(boss_d / 2)
        .extrude(LID_BLIND_H + 0.2)
    )
    result = result.cut(blind)


# ── Sensor-Trennwände (3×) mit Kabeldurchführungen ────────────────────────────
BMP_L     = 16.5   # BMP388 Länge entlang Gehäuse (-1.5mm)
MPU_L     = 14.5   # MPU6050 Länge entlang Gehäuse (-1.5mm)
DIV_T     = 1.4
DIV_H     = INNER_H * 0.75   # oben bleibt Spalt für Kabel
CBL_W     = 7.0
CBL_H     = 5.0

# Y-Mittelpunkte der drei Trennwände
div_y_centers = [
    SEN_Y0 + BMP_L + (SENSOR_L - BMP_L - MPU_L) / 2,   # BMP388 | MPU6050
    SEN_Y0 + SENSOR_L + GAP / 2,                          # Sensor | Akku
    BAT_Y0 + BAT_L + GAP / 2,                             # Akku   | PCB
]

for dy_c in div_y_centers:
    div_wall = (
        cq.Workplane("XY")
        .box(INNER_W - 2.0, DIV_T, DIV_H + 0.1, centered=True)
        .translate((0.0, dy_c, WALL - 0.1 + (DIV_H + 0.1) / 2))  # 0.1mm in Boden
    )
    cable_hole = (
        cq.Workplane("XY")
        .box(CBL_W, DIV_T + 0.2, CBL_H + 0.1, centered=True)
        .translate((0.0, dy_c, WALL + DIV_H - CBL_H / 2))
    )
    result = result.union(div_wall.cut(cable_hole))

# ── Export ────────────────────────────────────────────────────────────────────
import os
result = result.clean()   # Shape-Healing: coplanare Faces mergen, separate Bodies fixen
out_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rocket_case.step")
exporters.export(result, out_file)

print(f"Exportiert: {out_file}")
print(f"Abmessungen: {OUTER_W:.1f} x {TOTAL_L:.1f} x {OUTER_H:.1f} mm")

# ── Deckel ────────────────────────────────────────────────────────────────────
LID_H     = WALL          # Plattendicke
TONGUE_H  = 2.5           # Zunge greift ins Gehäuse
TONGUE_T  = 0.1           # Spiel Zunge → Gehäuse (Presspassung)

lid_profile = fairing_profile()

lid_outer = (
    cq.Workplane("XY")
    .polyline(lid_profile)
    .close()
    .extrude(LID_H)
    .edges("|Z").fillet(1.5)
    .edges(">Z").fillet(0.5)
)

# Zunge (greift in Gehäuse)
tongue = (
    cq.Workplane("XY")
    .box(INNER_W - TONGUE_T * 2, INNER_L - TONGUE_T * 2, TONGUE_H, centered=True)
    .translate((0.0, FAIRING + WALL + INNER_L / 2, -TONGUE_H / 2))
)
lid = lid_outer.union(tongue)

# Display-Fenster ausschneiden
disp_lid_cx = -OUTER_W / 2 + DISP_OX + DISP_W / 2
disp_lid_cy = DISP_OY + DISP_H / 2   # identisch mit Gehäuse-Fenster

disp_lid_cut = (
    cq.Workplane("XY")
    .box(DISP_W, DISP_H, LID_H + 0.4, centered=True)
    .translate((disp_lid_cx, disp_lid_cy, LID_H / 2))
    .edges("|Z").fillet(1.2)
)
lid = lid.cut(disp_lid_cut)

# M2-Durchgangslöcher + Aussparungen für Lid-Bossen (Zunge kollidiert sonst mit Bossen)
for lx, ly, boss_r, boss_d in lid_boss_positions:
    m2_lid = (
        cq.Workplane("XY")
        .workplane(offset=-TONGUE_H - 0.1)
        .moveTo(lx, ly)
        .circle(boss_d / 2)
        .extrude(LID_H + TONGUE_H + 0.2)
    )
    lid = lid.cut(m2_lid)
    boss_relief = (
        cq.Workplane("XY")
        .workplane(offset=-TONGUE_H - 0.1)
        .moveTo(lx, ly)
        .circle(boss_r + 0.2)
        .extrude(TONGUE_H + 0.2)
    )
    lid = lid.cut(boss_relief)

# Deckel neben Gehäuse platzieren (Zunge nach oben → 180° um X kippen)
lid_placed = lid.rotate((0, 0, 0), (1, 0, 0), 180).translate(
    (OUTER_W + 8, TOTAL_L, LID_H)
)

combined = cq.Compound.makeCompound([result.val(), lid_placed.val()])
combined_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rocket_tracker.step")
exporters.export(combined, combined_file)
print(f"Exportiert: {combined_file}")
print(f"Druckbett: {OUTER_W*2+8:.1f} x {TOTAL_L:.1f} mm")
