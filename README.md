# Wasserraketen-Tracker

Autonomer Höhen- und Flugdatenschreiber für Wasserraketen auf Basis eines
**ESP32-S3** mit AMOLED-Touchdisplay und WLAN-Webinterface.

Der Tracker misst zyklisch Luftdruck und Beschleunigung, berechnet daraus in
Echtzeit die Flugkenngrößen (Höhe, Geschwindigkeit, G-Kraft, Stabilität) und
stellt sie auf dem Display sowie über ein eigenes WLAN im Browser dar. Flüge
werden dauerhaft im Flash gespeichert (Bestenliste).

## Hardware

| Baustein | Funktion | Anbindung |
|---|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-1.64 | Rechenkern, WLAN, Display | — |
| BMP388 | Luftdruck → Höhe, Temperatur | I²C 0x77 / 0x76 |
| MPU6050 | Beschleunigung (±16 g), Drehrate | I²C 0x68 |
| CO5300 AMOLED 280×456 | Anzeige | QSPI |
| FT3168 | Kapazitiver Touch | I²C 0x38 |
| LiPo-Akku 1000 mAh | Stromversorgung | ADC an GPIO 4 |

Pinbelegung: I²C SDA=47 / SCL=48, Display QSPI CLK=10 / D0–D3=11–14 / CS=9 / RST=21.

## Software

- Framework: **PlatformIO** (Arduino, `espressif32@6.9.0`)
- Display: LovyanGFX (Panel_CO5300 über QSPI)
- Web: ESPAsyncWebServer + AsyncTCP, Live-Daten per Server-Sent-Events

### Bauen & Flashen

```bash
pio run --target upload
pio device monitor
```

## Funktionen

- 5 Displayseiten (Höhe, Flugpfad-Graph, Max-Werte, Scharfstellen, Historie)
- Automatische Flugphasen-Erkennung (Start, Schub, Steig, Fallschirm, Landung)
- Geschwindigkeit aus der Höhenmessung abgeleitet + "Tempo 10 m"
- WLAN-AP je Gerät eindeutig (`WaterRocket-XXXX`), Web-Diagramm & Bestenliste
- Standby (Deep-Sleep) mit Touch-Aufwecken

## Projektstruktur

```
src/
  main.cpp            Hauptlogik: Sensorik, Flugauswertung, Display, WLAN
  display_config.h    LovyanGFX-Panelkonfiguration (CO5300 / QSPI)
  web_page.h          Eingebettete Weboberfläche (HTML/JS)
case/                 3D-Druck-Gehäuse (STL / STEP / SCAD)
platformio.ini        Build-Konfiguration
Wasserraketen-Tracker_Doku.pdf   Technische Dokumentation
```

## Dokumentation

Die vollständige technische Dokumentation (Messwertberechnung, Genauigkeit,
Schaltung, Schub-/Geschwindigkeitstabellen) liegt als
[PDF](Wasserraketen-Tracker_Doku.pdf) bei.

## Autor

DI. Mag. David Sternberger · david@sternberger.at
