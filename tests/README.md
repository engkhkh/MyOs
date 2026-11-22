# MyOS tests

These run on a **Linux** PC (they were developed on Ubuntu). They need QEMU, mtools,
dosfstools (fsck.fat), Python 3 with numpy and Pillow, and gcc.

## regress.py - the full regression test
Boots MyOS in QEMU and goes through the features of every version like a person would
(keyboard and mouse through the QEMU monitor): Terminal commands, files, sound,
networking, programs, crash handling, Paint, games, Music, Settings, Calculator,
Notepad (clipboard, Find/Replace), Pictures (JPEG/PNG/GIF/BMP, wallpaper), File
Explorer (previews, copy/paste/rename/delete), the Web browser with pictures,
resizing / maximising / snapping windows,
Solitaire, FreeCell, the Widget Gallery, the Start menu, restarting (settings and files
kept), the crash screen and booting from the ISO.

It checks automatically: files written to the disk, settings.ini, web server requests,
the sound that was played (frequency analysis of a recording), fsck after every boot,
and that the screen is drawn. Screenshots are saved with contact sheets for a person
to look over. Results: regress/report.txt.

Before running: start `fakedns.py` (needs root, port 53) and `webserver.py site LOGFILE`
(port 80); the script runs QEMU in a private mount namespace whose /etc/resolv.conf
(resolv.conf here) points at the fake DNS, so `myos.test` reaches the test site.
The paths at the top of regress.py (W, MYOS) say where things are; change them for
your PC.

## decoders/ - picture decoder tests (run on the PC, not in MyOS)
- gen.py / jgen.py / ggen.py make random PNG / JPEG / GIF files together with the
  exact pixels they must decode to (JPEG and GIF are checked against Pillow, which
  uses libjpeg-turbo).
- t.c decodes them with MyOS's image.c / jpeg.c and compares: `gcc -O2 -o t t.c ../../image.c ../../jpeg.c`
- fuzz.c feeds thousands of cut-short and corrupted files to the decoders; build it with
  `-fsanitize=address,undefined` so any bad memory access is caught.
