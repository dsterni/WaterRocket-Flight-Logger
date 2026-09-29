// Wasserraketen-Tracker Gehäuse v4 — Aerodynamisch, leicht, Klebebefestigung
// Auf Flasche kleben: Längsachse entlang der Rakete
// Waveshare ESP32-S3-Touch-AMOLED-1.64" + BMP388 + MPU6050
// LiPo extern · kein Schnappverschluss · Rippen statt dicke Wände
//
// Drucken: 0.15mm Schichthöhe, 3 Perimeter, 15% Gyroid-Infill, PLA/PETG

// ── Hauptparameter ────────────────────────────────────────────────────────────
wall      = 1.4;   // Wandstärke (leicht aber stabil durch Rippen)
rib_w     = 1.2;   // Rippenbreite
tol       = 0.3;   // Passungsspiel

bottle_r  = 45.0;  // Flaschenradius (Standard 1,5L PET ~Ø90mm)

// PCB — Waveshare ESP32-S3-Touch-AMOLED-1.64 (Datenblatt)
board_w   = 28.6;   // PCB Breite
board_l   = 43.5;   // PCB Länge (USB-C vorne)
board_h   = 9.6;    // Gesamthöhe inkl. Display-Modul (Seitenansicht)
sensor_l  = 37.0;   // BMP388 18mm + 3mm Abstand + MPU6050 16mm (in Reihe)
gap       = 3.0;

// LiPo — 500mAh (Amazon B0DPZNZ4D6)
bat_w     = 30.0;
bat_l     = 44.0;
bat_h     = 8.0;

// Display-Fenster (Datenblatt: Glasfläche 29.12×44mm, sichtbar ~22.34×36mm)
disp_w    = 22.34;
disp_h    = 36.06;
disp_off_x =  3.1;  // (28.6 - 22.34) / 2
disp_off_y =  2.3;  // oberer Rand laut Datenblatt

// Innenraum: Battery (30mm) etwas breiter als PCB (28.6mm) → bestimmt Breite
inner_w = max(board_w, bat_w) + tol * 2;  // 30.6 mm  ← quer zur Rakete
inner_l = board_l + gap + sensor_l + gap + bat_l + tol * 2;  // ~134 mm
inner_h = board_h + 3.0;              // 12.6 mm — display bündig mit Oberkante

// Außenhülle
outer_w = inner_w + wall * 2;
outer_l = inner_l + wall * 2;
outer_h = inner_h + wall;             // oben geschlossen, unten offen (Flasche)

// Aerodynamische Verlängerung: Nase + Heck verjüngen sich
fairing  = 12.0;  // Länge der Keilverjüngung vorne + hinten
total_l  = outer_l + fairing * 2;

// Display-Fenster (oben) — aus Datenblatt
disp_ox  = disp_off_x + tol;
disp_oy  = disp_off_y + tol + fairing;

// USB-C Schlitz (vordere Stirnseite)
usb_w    = 10.0;
usb_h    =  4.0;

// JST ext. LiPo (Seite)
jst_w    =  7.0;
jst_h    =  4.0;

// ── Hilfsfunktionen ───────────────────────────────────────────────────────────
module rounded_box(w, l, h, r = 2) {
    hull()
        for (x = [r, w - r], y = [r, l - r])
            translate([x, y, 0]) cylinder(r = r, h = h, $fn = 24);
}

// Flaschenwölbung: Zylinder von unten subtrahieren
module bottle_cutout() {
    // Zylinder liegt unter dem Gehäuse, Achse quer
    translate([outer_w / 2, total_l / 2, -bottle_r + 0.6])
        rotate([90, 0, 0])
        cylinder(r = bottle_r, h = total_l + 2, center = true, $fn = 80);
}

// Aerodynamische Außenform (Draufsicht: abgerundetes Rechteck mit Spitzen v+h)
module fairing_shell(h) {
    hull() {
        // Hauptkörper
        translate([0, fairing, 0])
            rounded_box(outer_w, outer_l, h, 3);
        // Vordere Spitze (schmal, flach)
        translate([outer_w / 2 - 3, 0, 0])
            cylinder(r = 3, h = h * 0.3, $fn = 24);
        translate([outer_w / 2 - 3, 0, h * 0.3 - 0.1])
            cylinder(r1 = 3, r2 = 0.5, h = h * 0.7, $fn = 24);
        // Hintere Spitze
        translate([outer_w / 2 - 3, total_l, 0])
            cylinder(r = 3, h = h * 0.3, $fn = 24);
        translate([outer_w / 2 - 3, total_l, h * 0.3 - 0.1])
            cylinder(r1 = 3, r2 = 0.5, h = h * 0.7, $fn = 24);
    }
}

// ── Gehäuse ───────────────────────────────────────────────────────────────────
module case_body() {
    difference() {
        fairing_shell(outer_h);

        // Innenraum
        translate([wall, fairing + wall, wall])
            cube([inner_w, inner_l, outer_h]);

        // Display-Fenster (oben)
        translate([disp_ox, disp_oy, outer_h - wall - 0.1])
            rounded_box(disp_w, disp_h, wall + 0.2, 2);

        // USB-C Kabelkanal — durch gesamte Fairing-Nase bis zur Frontwand
        // PCB liegt auf 3mm Bossen → Unterkante USB-C bei z≈3.5mm
        // Kanalbreite 10mm, Höhe 5mm, auf PCB-Mitte zentriert (X=16.0mm)
        pcb_cx = wall + tol + board_w / 2;
        translate([pcb_cx - usb_w / 2, -1, wall + 2.5])
            cube([usb_w, fairing + wall + 1, 5.0]);

        // JST LiPo — Stecker an Hinterkante PCB (Mitte), Kabel läuft intern zur Batterie

        // Kabelbinder-Schlitze (3 Paare) — je links + rechts durch Seitenwand
        // Schlitz: 5mm breit × 3.5mm hoch, unten bündig mit Boden
        ziptie_w = 5.0;  ziptie_h = 3.5;
        for (zy = [fairing + wall + board_l * 0.4,
                   fairing + wall + board_l + gap + sensor_l * 0.5,
                   fairing + wall + board_l + gap + sensor_l + gap + bat_l * 0.5]) {
            // linke Wand
            translate([-1, zy - ziptie_w / 2, wall])
                cube([wall + 2, ziptie_w, ziptie_h]);
            // rechte Wand
            translate([outer_w - wall - 1, zy - ziptie_w / 2, wall])
                cube([wall + 2, ziptie_w, ziptie_h]);
        }

        // Druckausgleich BMP388 (Boden links)
        translate([wall + tol + 9, fairing + wall + board_l + gap + 6.5, -1])
            cylinder(d = 2.5, h = wall + 2, $fn = 20);

        // Batterie-Fach — hinter Sensor-Zone, von oben zugänglich
        bat_y0 = fairing + wall + board_l + gap + sensor_l + gap;
        translate([wall + tol + (max(board_w, bat_w) - bat_w) / 2,
                   bat_y0, wall])
            cube([bat_w + tol * 2, bat_l + tol * 2, bat_h + 1.0]);

        // Flaschenwölbung ausschneiden
        bottle_cutout();
    }

    // M2-Montagebossen (4×) — PCB-Eckbohrungen 2.5mm von Kante (Datenblatt)
    m2_boss_od = 4.6;
    m2_boss_h  = 3.0;
    m2_drill   = 2.2;   // Durchgangsloch M2
    m2_head_d  = 4.5;   // Senkkopf-Versenkung von unten
    m2_head_h  = 1.2;

    for (px = [wall + tol + 2.5, wall + tol + board_w - 2.5])
        for (py = [fairing + wall + tol + 2.5, fairing + wall + tol + board_l - 2.5]) {
            // Boss
            difference() {
                translate([px, py, wall])
                    cylinder(d = m2_boss_od, h = m2_boss_h, $fn = 20);
                // Durchgangsloch
                translate([px, py, -0.1])
                    cylinder(d = m2_drill, h = wall + m2_boss_h + 0.2, $fn = 16);
                // Senkkopf-Aussparung von unten
                translate([px, py, -0.1])
                    cylinder(d = m2_head_d, h = m2_head_h, $fn = 20);
            }
        }

    // Deckel-Montagebossen (4×) — in Seitenwände integriert, Sensor-Zone
    // Blind-M2-Loch von oben, 5mm tief
    lid_boss_r     = 3.0;
    lid_boss_drill = 2.4;
    lid_y1 = fairing + wall + board_l + gap + 8;
    lid_y2 = fairing + wall + board_l + gap + sensor_l - 8;

    for (by = [lid_y1, lid_y2])
        for (bx = [wall / 2, outer_w - wall / 2]) {
            difference() {
                translate([bx, by, wall])
                    cylinder(r = lid_boss_r, h = outer_h - wall, $fn = 20);
                translate([bx, by, outer_h - 5.1])
                    cylinder(d = lid_boss_drill, h = 5.2, $fn = 16);
            }
        }

    // Klebelaschen: 2× breite Flachflächen an Unterseite (bleiben nach bottle_cutout übrig)
    // → entstehen automatisch durch bottle_cutout-Radius
}

// ── Ausgabe ───────────────────────────────────────────────────────────────────
color("DodgerBlue", 0.9) case_body();

echo(str("GESAMT: ", round(outer_w*10)/10, " × ",
         round(total_l*10)/10, " × ",
         round(outer_h*10)/10, " mm"));
echo(str("Fairing je: ", fairing, " mm   Flasche R: ", bottle_r, " mm"));
echo(str("Elektronik-Zone: ", round(outer_l*10)/10, " mm"));
