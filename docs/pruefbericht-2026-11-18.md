# Sicherheits-, Stabilitäts- und Leistungstest vom 18. November 2026

Geprüft wurden das native Windows-Studio und der Java-Player. Der Schwerpunkt lag auf lokalen Bibliotheken, USB-Importen, Kartendownloads, GPS-Eingaben, Wiederherstellung nach Fehlern und der Reaktionszeit bei langen Routen.

## Befunde und Änderungen

| Bereich | Befund und Ergebnis |
| --- | --- |
| Zugangsdaten | Der Player verwendete einen ungesalzenen SHA-256-Prüfwert. Bei der nächsten erfolgreichen Anmeldung migriert er den Wert und die rollierende Konfigurationssicherung auf PBKDF2-HMAC-SHA-256 mit zufälligem Salt und 600.000 Iterationen. Ältere, separat archivierte Backups können den früheren Wert enthalten. Das Studio nutzt den höheren Aufwand beim Anlegen oder erneuten Speichern der Zugangsdaten. Beide Studio-Dateiformate wurden mit korrekten Codes geprüft. |
| USB-Pakete | Das Studio prüft ZIP-Metadaten und Größen vor großen Speicher- oder Schreibvorgängen. Audioprüfsummen entstehen beim Kopieren. Für die Ersetzungsbestätigung hält es genau den bereits geprüften Datenstand bereit. Das Abbrechen verwirft ihn, eine bestätigte Übernahme benötigt den Quell-Stick nicht mehr. Im Player entfallen zusätzliche vollständige Lesevorgänge beim Entpacken. Die abschließende Integritätsprüfung bleibt erhalten. |
| Kartendownloads und Kartendateien | Das Studio verhindert Weiterleitungen von HTTPS auf HTTP sowie Anbieterwechsel beim Einsatz eines API-Schlüssels. Anonyme Downloads senden keine Windows-Anmeldedaten. Bei XYZ-Downloads hält das Studio die Zielordner während des Schreibens offen und legt temporäre Dateien exklusiv an. Der Vektorkartenleser verarbeitet Lesefehler und kann laufende Renderaufträge abbrechen. |
| Player und GPS | Vor der Wiederherstellung prüft der Player die Journaldaten. Die GPS-Verarbeitung vermeidet wiederholte Kopien der gesamten Route. Überlange serielle Datensätze werden bis zum Zeilenende verworfen. Das gilt auch dann, wenn der Empfänger zwischen zwei Teilen einer Zeile pausiert. |
| Bedienung | Audioimporte laufen im Hintergrund. Beim Punktwechsel behält das Studio die vorhandenen Kartenmarker. Bei dichten Routen zeichnet es die Linien in Gruppen und blendet sich überlagernde Beschriftungen aus. Der ausgewählte Punkt wird zuletzt gezeichnet, damit er sichtbar bleibt. |
| Build und Bibliotheken | Der Studio-Build verwendet Stackschutz mit GCC und die Windows-Schutzmerkmale ASLR und DEP. Die Java-CI verwendet auf Commits festgelegte Actions und gewährt dem Job ausschließlich Lesezugriff auf die Repository-Inhalte. Die C-Bibliotheken entsprechen den geprüften Upstream-Revisionsständen von [yyjson 0.13.0](https://github.com/ibireme/yyjson/releases/tag/0.13.0) und [miniz 3.1.2](https://github.com/richgel999/miniz/releases/tag/3.1.2). Die Releases beheben Fehler beim Lesen von Zahlen beziehungsweise ZIP-Daten. |

Für die 13 ausgelieferten Java-Laufzeitbibliotheken und die zwei Python-Pakete meldete die versionsgenaue [OSV-Abfrage](https://google.github.io/osv.dev/post-v1-querybatch/) zum Prüfzeitpunkt keine Treffer. Der zusätzliche Abgleich mit den Veröffentlichungen der C-Projekte war entscheidend: Die Abfrage der alten C-Commits allein zeigte die Aktualisierungen nicht an.

## Messungen auf dem Windows-Entwicklungsrechner

| Messfall | Vorher | Nachher |
| --- | ---: | ---: |
| Java, 200 gültige GPS-Updates außerhalb des Auslöseradius, Route mit 1.000 Punkten, Median aus drei warmen Läufen ohne Swing und GPS-Hardware | 184,2 ms | 5,4 ms |
| Studio, 30 vollständige GDI-Kartenzeichnungen bei 640 × 480 Pixeln mit 5.000 dicht liegenden Punkten und wechselnder Auswahl | 8,95 s | 0,16 s |
| Studio, 1.200 vorhandene Routen mit gemeinsam genutzter 32-MiB-WAV ersetzen, einschließlich Vorbereitung vor der Bestätigung, Median aus drei warmen Läufen auf lokalem Speicher | 266 ms | 141 ms |

Bei zwei räumlich getrennten Punkten blieb die Kartengrafik im Vergleich pixelgleich. Auch 270 gerenderte Monaco-Kacheln hatten dieselben Bildprüfsummen. Die Zahlen beziehen sich auf diese Arbeitslasten und diese Maschine.

## Verifikation und verbleibende Grenzen

- Java bestand 105 Unit-Tests, den Distributionstest, sechs Python-GPS-Tests und den Starttest der Swing-Oberfläche. Ein echtes C-Paket ließ sich nach dem Import erneut öffnen, mit unveränderten Punkten und WAV-Daten.
- Der native Release-Build bestand den CTest für Speichern, Wiederöffnen, Paketexport, Import und Ersetzung. Der Test trennt die Paketquelle vor der Bestätigung. Die ausgelieferte EXE bestand den Oberflächentest für Zugangsdaten, Routenerstellung, Kartenklick, Punktreihenfolge und Wiederöffnen. Ein lokaler HTTP-Test prüfte unter anderem Download und Wiederaufnahme.
- Bei einem Teststart blockierte die Windows-Anwendungssteuerung die frühere Studio-EXE. Die aktualisierte Auslieferungsdatei ließ sich auf demselben Rechner starten. Sie ist weiterhin ohne Authenticode-Signatur. Für eine verbindliche Herstellerprüfung auf verwalteten PCs fehlt eine vertrauenswürdige Codesignatur.
- Die App-Codes steuern den Zugriff in der Oberfläche. Windows- oder Linux-Dateirechte schützen die Bibliothek und konfigurierbare GPS-Skripte. Die SHA-256-Werte in Routenpaketen prüfen die Audiointegrität. Sie belegen die Herkunft des Pakets nicht.
- Tests am tatsächlichen Raspberry Pi mit GPS-Empfänger und Audioausgang, lange Fahrten und ein echter Stromausfall während eines SD-Karten-Schreibvorgangs stehen aus. Die Tests simulierten Quellverlust und Lesefehler. Ein physisches Abziehen eines laufenden Kartendatenträgers wurde nicht erprobt.
