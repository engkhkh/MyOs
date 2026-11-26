#!/usr/bin/env python3
"""regress.py - the MyOS regression test.

Boots MyOS in QEMU and goes through the features of every version, driving it
like a person would (keyboard + mouse through the QEMU monitor). Each step saves
a screenshot; things that can be checked automatically are checked:
files written to the disk, settings.ini, web server requests, the sound that
was played (frequency analysis of a recording), filesystem health (fsck), and
that the screen is not blank. Results go to regress/report.txt and contact sheets.
"""
import os, socket, subprocess, sys, time, wave, struct, shutil, re
import numpy as np
from PIL import Image, ImageDraw

W = '/home/claude/work'
MYOS = '/home/claude/myos'
OUT = W + '/regress'
IMG = OUT + '/test.img'
SOCK = W + '/mon.sock'

results = []          # (area, check, ok, detail)
shots = []            # (name, caption)

def check(area, what, ok, detail=''):
    results.append((area, what, bool(ok), str(detail)))
    print(('PASS ' if ok else 'FAIL ') + f'[{area}] {what} {detail}', flush=True)

# ---------------------------------------------------------------- QEMU
class VM:
    def __init__(self, iso=False, net=True):
        subprocess.run(['pkill', '-f', 'qemu-system'], capture_output=True)
        time.sleep(1)
        if os.path.exists(SOCK): os.remove(SOCK)
        boot = ['-cdrom', MYOS + '/myos.iso', '-boot', 'd'] if iso else ['-kernel', MYOS + '/myos.bin']
        args = ['qemu-system-i386'] + boot + [
            '-drive', f'file={IMG},format=raw,index=0,media=disk', '-m', '256', '-no-reboot',
            '-display', 'none', '-monitor', f'unix:{SOCK},server,nowait',
            '-audiodev', 'none,id=snd0', '-device', 'sb16,audiodev=snd0', '-machine', 'pcspk-audiodev=snd0']
        if net: args += ['-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0']
        cmd = 'mount --bind %s/resolv.conf /etc/resolv.conf && exec %s' % (W, ' '.join("'%s'" % a for a in args))
        self.p = subprocess.Popen(['unshare', '-m', 'sh', '-c', cmd], stdout=open(W + '/qemu.log', 'w'), stderr=subprocess.STDOUT)
        for _ in range(100):
            if os.path.exists(SOCK): break
            time.sleep(0.1)
        time.sleep(0.3)
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(SOCK); self.s.settimeout(2)
        self.read()
    def read(self):
        out = b''
        try:
            while True:
                d = self.s.recv(65536)
                if not d: break
                out += d
                if out.endswith(b'(qemu) '): break
        except socket.timeout: pass
        return out.decode(errors='replace')
    def mon(self, c):
        self.s.send((c + '\n').encode()); return self.read()
    def alive(self): return self.p.poll() is None
    def key(self, *keys, gap=0.12):
        for k in keys: self.mon('sendkey ' + k); time.sleep(gap)
    KEYMAP = {'"': 'shift-apostrophe', ' ': 'spc', '.': 'dot', '\\': 'backslash', '/': 'slash', '-': 'minus',
              ':': 'shift-semicolon', '\n': 'ret', '(': 'shift-9', ')': 'shift-0', '!': 'shift-1', '_': 'shift-minus',
              '=': 'equal', '*': 'shift-8', '+': 'shift-equal', '%': 'shift-5', ',': 'comma', "'": 'apostrophe',
              '\t': 'tab', '@': 'shift-2'}
    def type(self, text, settle=0.3):
        for ch in text:
            k = self.KEYMAP.get(ch) or ('shift-' + ch.lower() if ch.isupper() else ch)
            self.mon('sendkey ' + k); time.sleep(0.035)
        time.sleep(settle)
    def cmd(self, line, wait=0.8):                  # a Terminal command
        self.type(line + '\n', settle=wait)
    def move(self, x, y):                           # absolute: slam to the corner, then walk
        for _ in range(30): self.mon('mouse_move -50 -50')
        time.sleep(0.5)
        for _ in range(3): self.mon('mouse_move -50 -50')    # again: anything left over is soaked up here
        time.sleep(0.5)                             # let MyOS take the corner move first: moves that are
                                                    # still queued get added together (like a real mouse)
        while x > 0 or y > 0:
            dx, dy = min(x, 40), min(y, 40); self.mon(f'mouse_move {dx} {dy}'); x -= dx; y -= dy
            time.sleep(0.02)                        # a busy MyOS (photo wallpaper) must not lose moves
        time.sleep(0.25)
    def click(self, x, y, double=False):
        self.move(x, y); self.mon('mouse_button 1'); self.mon('mouse_button 0')
        if double: self.mon('mouse_button 1'); self.mon('mouse_button 0')
        time.sleep(0.3)
    def drag(self, x0, y0, x1, y1, steps=10):
        self.move(x0, y0); self.mon('mouse_button 1')
        for i in range(steps): self.mon(f'mouse_move {(x1 - x0) // steps} {(y1 - y0) // steps}'); time.sleep(0.03)
        time.sleep(0.2); self.mon('mouse_button 0'); time.sleep(0.4)
    def shot(self, name, caption, crop=None):
        p = f'{OUT}/{name}.ppm'
        self.mon('screendump ' + p); time.sleep(0.4)
        im = Image.open(p).convert('RGB'); os.remove(p)
        if crop: im = im.crop(crop)
        im.save(f'{OUT}/{name}.png'); shots.append((name, caption))
        return im
    def quit(self):
        try: self.mon('quit')
        except Exception: pass
        time.sleep(1)
        subprocess.run(['pkill', '-f', 'qemu-system'], capture_output=True)

def not_blank(im): a = np.asarray(im); return a.std() > 8
def mtools(*args):
    return subprocess.run(list(args), capture_output=True, text=True).stdout
def disk_part():
    subprocess.run(['dd', f'if={IMG}', f'of={OUT}/part.img', 'bs=1M', 'skip=1', 'status=none'])
    return OUT + '/part.img'
def mtype(path):
    r = subprocess.run(['mtype', '-i', OUT + '/part.img', '::' + path], capture_output=True)
    return r.stdout.decode(errors='replace') if r.returncode == 0 else None
def mexists(path):
    return subprocess.run(['mdir', '-i', OUT + '/part.img', '::' + path], capture_output=True).returncode == 0

def tones(path):
    """main frequencies found in a recording, in 0.1 s windows"""
    with wave.open(path) as w:
        n, ch, rate = w.getnframes(), w.getnchannels(), w.getframerate()
        a = np.frombuffer(w.readframes(n), np.int16).astype(float)
    if ch > 1: a = a.reshape(-1, ch).mean(1)
    found = []
    win = rate // 10
    for i in range(0, len(a) - win, win):
        seg = a[i:i + win]
        if np.abs(seg).max() < 500: continue
        f = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
        found.append(round(np.argmax(f[5:]) + 5) * rate / win)
    return found

# ---------------------------------------------------------------- the tests
def boot1():
    vm = VM()
    time.sleep(11)
    im = vm.shot('01_desktop', 'v0.4 desktop after boot (kernel boot)')
    check('boot', 'desktop is drawn', not_blank(im))
    check('boot', 'taskbar at the bottom', np.asarray(im)[745, 600].sum() < 200, np.asarray(im)[745, 600])

    # ---- v0.1-v0.3 Terminal basics
    vm.cmd('ver'); vm.cmd('mem'); vm.cmd('time'); vm.cmd('uptime'); vm.cmd('echo Hello from the regression test')
    vm.shot('02_terminal', 'v0.1-0.3 Terminal: ver, mem, time, uptime, echo', (150, 50, 772, 450))
    vm.cmd('clear'); vm.cmd('memtest', 2); vm.cmd('ps'); vm.cmd('disk')
    vm.shot('03_terminal2', 'Terminal: memtest, ps, disk', (150, 50, 772, 450))

    # ---- v0.5 files from the Terminal
    vm.cmd('clear')
    vm.cmd('write reg.txt Hello from the regression test'); vm.cmd('append reg.txt Second line')
    vm.cmd('copy reg.txt reg2.txt'); vm.cmd('ren reg2.txt reg3.txt'); vm.cmd('mkdir RegDir')
    vm.cmd('copy reg.txt RegDir/inside.txt'); vm.cmd('write gone.txt delete me'); vm.cmd('del gone.txt')
    vm.cmd('type reg.txt'); vm.cmd('dir')
    vm.shot('04_files', 'v0.5 Terminal file commands: write, append, copy, ren, mkdir, del, type, dir', (150, 50, 772, 450))

    # ---- v0.8 sound: beep + tune recorded
    vm.cmd('clear')
    vm.mon(f'wavcapture {OUT}/sound1.wav snd0'); time.sleep(0.3)
    vm.cmd('beep', 1.0); vm.cmd('tune', 4.5)
    vm.mon('stopcapture 0'); time.sleep(0.5)
    t = tones(OUT + '/sound1.wav')
    check('sound', 'beep plays 880 Hz', any(abs(f - 880) < 15 for f in t[:4]), t[:4])
    check('sound', 'tune plays several notes (392..1047 Hz)', len(set(round(f, -1) for f in t)) >= 4, sorted(set(round(f) for f in t))[:10])

    # ---- v0.9 network
    vm.cmd('ipconfig', 1.5); vm.cmd('ping 10.0.2.2', 5); vm.cmd('nslookup myos.test', 2.5)
    vm.shot('05_network', 'v0.9 ipconfig, ping, nslookup', (150, 50, 772, 450))

    # ---- v0.6/0.7 programs, Task Manager, crash isolation, Alt+F4
    vm.cmd('clear'); vm.cmd('hello', 2)
    vm.shot('06_hello', 'v0.6 hello program in its own window')
    vm.key('alt-f4'); time.sleep(0.5)
    vm.cmd('bounce', 2.5); vm.shot('07_bounce', 'v0.7 bounce animation'); vm.key('alt-f4'); time.sleep(0.5)
    vm.cmd('crash', 2); vm.type('3', 2)                         # 3 = divide by zero
    vm.shot('08_crash', 'v0.6 a program divides by zero -> error dialog, system keeps running')
    vm.key('ret'); time.sleep(0.5)
    focus_terminal(vm)
    vm.cmd('spin', 1.5); focus_terminal(vm); vm.cmd('taskmgr', 2.5)
    vm.shot('09_taskmgr', 'v0.6 Task Manager with spin running')
    vm.key('alt-f4'); time.sleep(0.5)                          # closes Task Manager
    vm.shot('10_altf4', 'v1.8 Alt+F4 closed Task Manager (Terminal in front, spin behind)')
    vm.key('alt-tab'); time.sleep(0.5)                         # the window at the back (spin) comes forward
    vm.shot('11_alttab', 'v1.8 Alt+Tab brought spin to the front')
    vm.key('alt-f4'); time.sleep(0.6)                          # ...and Alt+F4 ends it

    # ---- v0.7 Paint: draw + save
    focus_terminal(vm)
    vm.cmd('paint', 2.5)
    vm.drag(400, 300, 600, 400); vm.drag(420, 380, 560, 280)
    vm.key('ctrl-s'); time.sleep(1.5)
    vm.shot('12_paint', 'v0.7 Paint: two strokes, saved with Ctrl+S'); vm.key('alt-f4'); time.sleep(0.5)

    # ---- v0.7 games
    focus_terminal(vm)
    vm.cmd('snake', 2); vm.key('up', 'left'); time.sleep(1.5); vm.shot('13_snake', 'v0.7 Snake'); vm.key('alt-f4'); time.sleep(0.4)
    focus_terminal(vm)
    vm.cmd('mines', 2); vm.shot('14_mines', 'v0.7 Minesweeper'); vm.key('alt-f4'); time.sleep(0.4)

    # ---- v0.8 Music (records the 440 Hz test tone)
    focus_terminal(vm)
    vm.mon(f'wavcapture {OUT}/sound2.wav snd0'); time.sleep(0.3)
    vm.cmd('music /Music/Test tone 440 Hz.wav', 3)
    vm.shot('15_music', 'v0.8 Music playing the 440 Hz test tone')
    vm.mon('stopcapture 0'); time.sleep(0.3)
    t = tones(OUT + '/sound2.wav')
    check('sound', 'Music plays the 440 Hz WAV through the Sound Blaster', sum(abs(f - 440) < 12 for f in t) >= 5, t[:8])
    vm.key('alt-f4'); time.sleep(0.5)

    # ---- v1.0 Settings window
    focus_terminal(vm)
    vm.cmd('settings', 1.5); vm.shot('16_settings', 'v1.0 Settings window'); vm.key('alt-f4'); time.sleep(0.4)

    # ---- v1.1 Calculator
    focus_terminal(vm)
    vm.cmd('calc', 2); vm.type('12.5*4+7=', 0.6); vm.shot('17_calc', 'v1.1 Calculator: 12.5*4+7 = 57')
    vm.key('ctrl-c'); vm.key('alt-f4'); time.sleep(0.4)

    # ---- v1.2 Notepad + clipboard + v1.4 find / replace
    focus_terminal(vm)
    vm.cmd('notepad /Docs/regnote.txt', 1.5)
    vm.key('ctrl-v'); vm.type(' is the answer.\nThe cat sat on the mat. A cat is a cat.\n')
    vm.key('ctrl-h'); vm.type('cat'); vm.key('tab'); vm.type('dog')
    vm.key('ret'); time.sleep(0.3)
    vm.shot('18_notepad_find', 'v1.2/1.4 Notepad: pasted 57 from Calculator, Replace bar')
    # Replace all: click its button (Notepad at 240,110; client y + bar rows)
    vm.click(681, 229)                                         # the "Replace all" button
    vm.key('esc'); vm.key('ctrl-s'); time.sleep(1)
    vm.shot('19_notepad_saved', 'v1.4 Replace all (cat -> dog) then Ctrl+S')
    vm.key('alt-f4'); time.sleep(0.4)

    # ---- v1.5/1.6/1.7 Pictures: JPEG, PNG, GIF, BMP + set wallpaper
    focus_terminal(vm)
    vm.cmd('viewer /Pictures/Coast.jpg', 3); vm.shot('20_viewer_jpg', 'v1.6 progressive JPEG in Pictures')
    vm.key('right'); time.sleep(2); vm.shot('21_viewer_next', 'Pictures: next picture')
    vm.key('end'); time.sleep(2.5); vm.shot('22_viewer_last', 'Pictures: last picture (Valley.jpg)')
    vm.key('w'); time.sleep(4); vm.shot('23_wallpaper', 'v1.5 Set as wallpaper (Valley.jpg)')
    vm.key('esc'); time.sleep(0.5)
    focus_terminal(vm)
    vm.cmd('viewer /Pictures/Rainbow.gif', 2.5); vm.shot('24_viewer_gif', 'v1.7 GIF with transparency (first frame)')
    vm.key('esc'); time.sleep(0.4)

    # ---- v1.8 Explorer: icons + previews, type-to-select, copy/paste/rename/delete
    focus_terminal(vm)
    vm.cmd('explorer', 1.5)
    vm.shot('25_explorer_root', 'v1.8 File Explorer, Icons view')
    vm.type('pic', 0.3); vm.key('ret'); time.sleep(7)
    vm.shot('26_explorer_pictures', 'v1.8 picture previews (PNG, JPEG, GIF, BMP)')
    vm.type('coa', 0.2); vm.key('ctrl-c'); vm.key('backspace'); time.sleep(0.8); vm.key('ctrl-v'); time.sleep(1.5)
    vm.key('f2'); vm.key(*['backspace'] * 12); vm.type('Copied coast.jpg\n', 0.6)
    vm.shot('27_explorer_paste', 'v1.1 Explorer: Coast.jpg copied to C:\\ and renamed')
    time.sleep(1.5); vm.type('regd', 0.2); vm.key('shift-delete'); time.sleep(0.6)
    vm.shot('28_explorer_confirm', 'v2.0 Shift+Delete deletes forever: a folder with files asks first')
    vm.key('ret'); time.sleep(1)
    vm.key('ctrl-e'); time.sleep(0.6)
    vm.shot('29_explorer_list', 'v1.8 Ctrl+E switches back to the List view'); vm.key('ctrl-e'); time.sleep(0.3)
    bin_tests(vm)
    vm.key('alt-f4'); time.sleep(0.4)

    # ---- v1.7 Web with pictures
    focus_terminal(vm)
    open(W + '/web.log', 'w').close()
    vm.cmd('web myos.test/pics.html', 9)
    vm.shot('30_web', 'v1.7 Web browser with pictures')
    log = open(W + '/web.log').read()
    check('web', 'page + 6 picture requests reached the server', log.count('GET /pics') >= 6 and 'pics.html' in log, log.count('GET'))
    vm.key('pgdn', 'pgdn'); time.sleep(0.6); vm.shot('31_web_scrolled', 'Web: scrolled to the logo / GIF / missing pictures')
    vm.key('alt-f4'); time.sleep(0.4)

    # ---- v1.3 Solitaire (win), v1.4 FreeCell (Game #1 deal + win), Widget Gallery
    focus_terminal(vm)
    vm.cmd('solitaire demo', 4)
    win = window_origin(vm)
    vm.drag(win[0] + 138, win[1] + 86, win[0] + 310, win[1] + 252, 9); time.sleep(3)
    vm.shot('32_solitaire_win', 'v1.3 Solitaire: one move -> auto finish -> bouncing cards')
    vm.key('esc'); time.sleep(0.6); vm.key('esc'); time.sleep(0.3); vm.key('alt-f4'); time.sleep(0.4)
    focus_terminal(vm)
    vm.cmd('freecell 1', 4); vm.shot('33_freecell1', 'v1.4 FreeCell Game #1 (top row should be JD 2D 9H JC 5D 7H 7C 5H)')
    vm.key('alt-f4'); time.sleep(0.4)
    focus_terminal(vm)
    vm.cmd('widgets', 2); vm.shot('34_widgets', 'v1.3 Widget Gallery'); vm.key('alt-f4'); time.sleep(0.4)
    spider_tests(vm)
    notes_tests(vm)
    contacts_tests(vm)
    calendar_tests(vm)
    v27_tests(vm)
    dark_tests(vm)
    desktops_tests(vm)

    # ---- Start menu
    vm.key('meta_l'); time.sleep(0.6); vm.shot('35_start', 'Start menu (Windows key)'); vm.key('esc'); time.sleep(0.3)

    # ---- v1.9 windows
    window_tests(vm)

    # ---- settings from the Terminal (saved to settings.ini)
    focus_terminal(vm)
    vm.cmd('tz +3'); vm.cmd('volume 60')
    vm.shot('36_final', 'end of the first boot')
    vm.quit()

def focus_terminal(vm):
    """bring the Terminal to the front (or open it again): double-click its desktop icon"""
    vm.click(55, 235, double=True); time.sleep(0.5)

def window_origin(vm):
    """client-area origin of the window in front: its title bar's top row is exactly the accent blue"""
    im = np.asarray(vm.shot('_win', '')).astype(int); shots.pop(); os.remove(OUT + '/_win.png')
    acc = (im[:, :, 0] == 0x25) & (im[:, :, 1] == 0x63) & (im[:, :, 2] == 0xEB)
    rows = np.where(acc.sum(1) > 150)[0]
    if not len(rows): return (231, 100)
    y = rows[0]; xs = np.where(acc[y])[0]
    return (int(xs[0]), int(y) - 1 + 30)

def front_rect(vm):
    """x, y, width of the window in front (from its accent-blue title bar)"""
    im = np.asarray(vm.shot('_fr', '')).astype(int); shots.pop(); os.remove(OUT + '/_fr.png')
    acc = (im[:, :, 0] == 0x25) & (im[:, :, 1] == 0x63) & (im[:, :, 2] == 0xEB)
    rows = np.where(acc.sum(1) > 150)[0]
    if not len(rows): return (-1, -1, 0)
    y = int(rows[0]); xs = np.where(acc[y])[0]
    return (int(xs[0]) - 1, y - 1, int(xs[-1]) - int(xs[0]) + 3)

def window_tests(vm):
    """v1.9: maximise / restore, double-click the title, resize from a corner, snap to half the screen"""
    focus_terminal(vm)
    vm.cmd('notepad /Welcome.txt', 1.5)
    x, y, w = front_rect(vm)
    vm.click(x + w - 69, y + 15); time.sleep(0.8)                 # the maximise button
    mx_, my_, mw = front_rect(vm)
    vm.shot('37_notepad_max', 'v1.9 Notepad maximised (maximise button)')
    check('windows', 'maximise button fills the screen', mx_ == 0 and my_ == 0 and mw == 1024, (mx_, my_, mw))
    vm.click(400, 14, double=True); time.sleep(0.8)              # double-click the title: restore
    rx, ry, rw = front_rect(vm)
    check('windows', 'double-clicking the title restores the old size', (rx, ry, rw) == (x, y, w), ((rx, ry, rw), (x, y, w)))
    vm.click(rx + 120, ry + 15, double=True); time.sleep(0.8)    # ...and maximises again
    check('windows', 'double-clicking the title maximises', front_rect(vm)[2] == 1024)
    vm.key('alt-f4'); time.sleep(0.5)

    focus_terminal(vm)
    vm.cmd('explorer', 1.5)
    x, y, w = front_rect(vm)
    vm.drag(x + w - 2, y + 518, x + w + 98, y + 578)            # bottom-right corner: 100 wider, 60 taller
    nx, ny, nw = front_rect(vm)
    vm.shot('38_explorer_resized', 'v1.9 File Explorer made bigger from its bottom-right corner')
    check('windows', 'resizing from the corner', nw == w + 100 and (nx, ny) == (x, y), ((x, y, w), (nx, ny, nw)))
    vm.move(nx + 300, ny + 15); vm.mon('mouse_button 1')         # drag the title to the left edge: snap
    for _ in range(16): vm.mon('mouse_move -40 0'); time.sleep(0.03)
    time.sleep(0.4)
    vm.shot('39_snap_preview', 'v1.9 dragging to the left edge shows where the window will snap')
    vm.mon('mouse_button 0'); time.sleep(0.8)
    sx, sy, sw = front_rect(vm)
    vm.shot('40_snapped', 'v1.9 File Explorer snapped to the left side (min. width 660)')
    check('windows', 'snapping to the left edge', sx == 0 and sy == 0 and sw == 660, (sx, sy, sw))
    vm.drag(300, 15, 500, 215)                                   # pull it away: back to its old size
    ux, uy, uw = front_rect(vm)
    check('windows', 'pulling a snapped window away restores its size', uw == nw, (ux, uy, uw, nw))
    vm.key('alt-f4'); time.sleep(0.5)

    focus_terminal(vm)
    vm.cmd('viewer /Pictures/Valley.jpg', 3)
    x, y, w = front_rect(vm)
    vm.click(x + w - 69, y + 15); time.sleep(2.5)               # programs can be maximised too
    vm.shot('41_viewer_max', 'v1.9 Pictures maximised: the photo is fitted to the new size')
    check('windows', 'a program window (Pictures) maximises', front_rect(vm)[2] == 1024)
    vm.key('esc'); time.sleep(0.5)

BIN_ICON = (56, 10 + 7 * 88 + 36)                                # the Recycle Bin on the desktop

def tile(x, y, i, cols):
    """centre of tile i in Explorer's Icons view, for an Explorer window at x, y"""
    return (x + 90 + (i % cols) * 120, y + 156 + (i // cols) * 128)

def drag_shot(vm, x0, y0, x1, y1, name, caption, steps=12):
    vm.move(x0, y0); vm.mon('mouse_button 1')
    for i in range(steps): vm.mon(f'mouse_move {(x1 - x0) // steps} {(y1 - y0) // steps}'); time.sleep(0.08)
    time.sleep(0.6); vm.shot(name, caption)
    vm.mon('mouse_button 0'); time.sleep(1)

def bin_tests(vm):
    """v2.0: drag and drop in Explorer, the Recycle Bin (delete, restore, drag onto the bin)"""
    vm.type('docs', 0.2); vm.key('ret'); time.sleep(1)
    vm.key('ctrl-n'); time.sleep(0.4); vm.key(*['backspace'] * 12); vm.type('Box\n', 0.8)
    x, y, w = front_rect(vm)
    cols = max(1, (w - 2 - 12) // 120)
    # Docs: Box, Ideas for MyOS.txt, regnote.txt, shopping list.txt
    sx, sy = tile(x, y, 3, cols); bx, by = tile(x, y, 0, cols)
    drag_shot(vm, sx, sy, bx, by, '52_drag_folder', 'v2.0 dragging "shopping list.txt" onto the Box folder')
    vm.shot('53_dropped', 'v2.0 dropped: the file moved into Box')
    vm.type('ideas', 0.2); vm.key('delete'); time.sleep(1)
    vm.shot('54_to_bin', 'v2.0 Delete moves "Ideas for MyOS.txt" to the Recycle Bin (the bin icon is now full)')
    vm.click(*BIN_ICON, double=True); time.sleep(1.2)
    vm.shot('55_bin', 'v2.0 the Recycle Bin, with where each item came from')
    vm.key('down'); time.sleep(0.3)
    vm.click(x + 1 + 54 + 34, y + 30 + 18); time.sleep(1)        # Restore
    vm.shot('56_restored', 'v2.0 Restore puts it back in C:\\Docs')
    vm.click(x + 1 + 6 + 22, y + 30 + 18); time.sleep(0.8)       # Up (to C:)
    vm.type('docs', 0.2); vm.key('ret'); time.sleep(1)
    bx, by = tile(x, y, 0, cols)
    drag_shot(vm, bx, by, *BIN_ICON, '57_drag_bin', 'v2.0 dragging the Box folder onto the Recycle Bin')
    vm.shot('58_after_bin', 'v2.0 Box is in the Recycle Bin now')
    multi_tests(vm, x, y, w, cols)

def multi_tests(vm, x, y, w, cols):
    """v2.1: selecting several items (Shift+arrows, Ctrl+A, a box) and working on all of them"""
    for _ in range(3):                                           # three new text files in C:\Docs
        vm.click(x + 1 + 154 + 40, y + 30 + 18); time.sleep(0.6); vm.key('ret'); time.sleep(0.5)
    vm.key('home'); vm.key('right'); vm.key('shift-right'); vm.key('shift-right'); time.sleep(0.5)
    vm.shot('59_multi_shift', 'v2.1 Shift+Right selects three files at once (status bar: 3 selected)')
    vm.key('ctrl-x'); time.sleep(0.4)
    vm.key('ctrl-n'); time.sleep(0.4); vm.key(*['backspace'] * 12); vm.type('Sub\n', 0.8)
    vm.key('ret'); time.sleep(1)                                 # open Sub
    vm.key('ctrl-v'); time.sleep(1.5)
    vm.shot('60_multi_paste', 'v2.1 Cut + Paste moved all three into Sub')
    vm.key('ctrl-a'); time.sleep(0.3); vm.key('delete'); time.sleep(1.5)
    vm.shot('61_multi_delete', 'v2.1 Ctrl+A then Delete: three items to the Recycle Bin')
    vm.key('backspace'); time.sleep(1)                           # back to Docs: Sub, Ideas, regnote
    ex0, ey0 = tile(x, y, cols + 2, cols); ex1, ey1 = tile(x, y, 1, cols)
    drag_shot(vm, ex0, ey0, ex1, ey1, '62_box_select', 'v2.1 dragging a box over two files selects both')
    vm.key('ctrl-c'); time.sleep(0.3)
    vm.type('sub', 0.2); vm.key('ret'); time.sleep(1); vm.key('ctrl-v'); time.sleep(1.5)
    vm.shot('63_box_paste', 'v2.1 both copied into Sub with Ctrl+C / Ctrl+V')
    vm.key('ctrl-a'); time.sleep(0.3)
    sx, sy = tile(x, y, 0, cols)
    drag_shot(vm, sx, sy, *BIN_ICON, '64_drag_two', 'v2.1 dragging both onto the Recycle Bin at once')
    vm.shot('65_after_drag_two', 'v2.1 Sub is empty again')
    vm.key('backspace'); time.sleep(0.5)
    menu_tests(vm, x, y, w, cols)

def rclick(vm, x, y):
    vm.move(x, y); time.sleep(0.3); vm.mon('mouse_button 2'); vm.mon('mouse_button 0'); time.sleep(0.6)

def menu_tests(vm, x, y, w, cols):
    """v2.2: right-click menus (Explorer items and empty space, Properties, title bar, desktop, Menu key)"""
    ex, ey = tile(x, y, cols + 2, cols)                          # empty space
    rclick(vm, ex, ey)
    vm.shot('66_menu_empty', 'v2.2 right-click on empty space: View, New folder, New text file, Paste, Properties')
    vm.click(ex + 40, ey + 52); time.sleep(0.6)                  # New folder
    vm.key(*['backspace'] * 12); vm.type('Menu\n', 0.8)
    # Docs: Menu, Sub, Ideas for MyOS.txt, regnote.txt
    ix, iy = tile(x, y, 2, cols); mx_, my_ = tile(x, y, 0, cols)
    rclick(vm, ix, iy)
    vm.shot('67_menu_item', 'v2.2 right-click on a file: Open, Open with Notepad, Cut, Copy, Delete, Rename, Properties')
    vm.click(ix + 40, iy + 104); time.sleep(0.5)                 # Copy
    rclick(vm, mx_, my_)
    vm.click(mx_ + 40, my_ + 104); time.sleep(1.2)               # Paste into folder
    vm.shot('68_paste_into', 'v2.2 "Paste into folder" on the Menu folder')
    rclick(vm, ix, iy)
    vm.click(ix + 40, iy + 200); time.sleep(0.8)                 # Properties
    vm.shot('69_properties', 'v2.2 Properties of a file')
    vm.key('ret'); time.sleep(0.4)
    vm.key('alt-ret'); time.sleep(0.8)                           # Alt+Enter: Properties of the selected file
    vm.shot('70_properties_key', 'v2.2 Alt+Enter opens Properties too')
    vm.key('ret'); time.sleep(0.4)
    rclick(vm, x + 120, y + 12)                                  # the title bar
    vm.shot('71_window_menu', 'v2.2 right-click on the title bar: Restore, Minimise, Maximise, Close')
    vm.click(x + 120 + 40, y + 12 + 69); time.sleep(1)           # Maximise
    fx, fy, fw = front_rect(vm)
    check('menu', 'title bar menu maximises the window', fw >= 1020 and fy == 0, (fx, fy, fw))
    rclick(vm, 300, 12)
    vm.click(340, 12 + 17); time.sleep(1)                        # Restore
    fx, fy, fw = front_rect(vm)
    check('menu', 'title bar menu restores it', (fx, fy, fw) == (x, y, w), (fx, fy, fw))
    vm.type('regn', 0.3); vm.key('compose'); time.sleep(0.6)      # QEMU's name for the Menu key
    vm.shot('72_menu_key', 'v2.2 the Menu key opens the menu for the selected file')
    vm.key('esc'); time.sleep(0.3)
    rclick(vm, 930, 600)
    vm.shot('73_desktop_menu', 'v2.2 right-click on the desktop')
    vm.click(814 + 40, 600 - MENU_H + 4 + 5 * 26 + 9 + 13); time.sleep(1.2)   # Task Manager (the menu opens upwards here)
    fx, fy, fw = front_rect(vm)
    check('menu', 'the desktop menu opens Task Manager', (fx, fy) != (x, y), (fx, fy, fw))
    vm.shot('74_desktop_menu_taskmgr', 'v2.2 Task Manager opened from the desktop menu')
    vm.key('alt-f4'); time.sleep(0.5)
    desk_tests(vm, x, y, w, cols)

DESK = lambda row: (16 + 9 * 96 + 40, 10 + row * 88 + 40)        # desktop file icons, right-hand column
MENU_H = 8 + 9 * 26 + 2 * 9                                      # the desktop's right-click menu

def notes_tests(vm):
    """v2.5: Notes"""
    focus_terminal(vm)
    vm.cmd('notes', 3)
    vm.shot('88_notes', 'v2.5 Notes: the notes in C:\\Notes, newest editor on the right')
    vm.key('ctrl-n'); time.sleep(0.4)
    vm.type('Trip to Jeddah\nPack sunscreen and a hat, and remember to book the hotel near the sea before Friday.\nBook the hotel', 2)
    vm.shot('89_notes_typed', 'v2.5 a new note: the text wraps, and it saves itself (status: Saved)')
    vm.key('ctrl-f'); vm.type('spider', 1)
    vm.shot('90_notes_search', 'v2.5 searching for "spider" leaves only the note that mentions it')
    vm.key(*['backspace'] * 6); time.sleep(0.5)
    win = window_origin(vm)
    vm.click(win[0] + 60, win[1] + 180); time.sleep(0.5)       # the 2nd note: Book ideas
    vm.click(win[0] + 725, win[1] + 45); time.sleep(0.6)       # Delete
    vm.shot('91_notes_delete', 'v2.5 Delete asks first')
    vm.key('ret'); time.sleep(0.8)
    vm.key('alt-f4'); time.sleep(0.5)

def lock_hash(pw):
    """the lock-screen password hash (FNV-1a over "MyOS lock:" + password), as settings.ini keeps it"""
    h = 2166136261
    for ch in ('MyOS lock:' + pw).encode():
        h = ((h ^ ch) * 16777619) & 0xFFFFFFFF
    return '%08x' % h

def lum(p): return (p[0] * 3 + p[1] * 6 + p[2]) // 10

def desktops_tests(vm):
    """v2.9: virtual desktops"""
    focus_terminal(vm)
    term = front_rect(vm)
    vm.key('ctrl-meta_l-d'); time.sleep(0.5)
    vm.shot('114_new_desktop', 'v2.9 Win+Ctrl+D: a new, empty desktop ("Desktop 2" shows for a moment)')
    time.sleep(1)
    vm.shot('114b_desktop2', 'Desktop 2: no windows on the taskbar')
    a, b = vm_pixel(vm, '114b_desktop2', 220, 745), vm_pixel(vm, '114b_desktop2', 600, 745)
    check('desktops', 'a new desktop starts empty (no window buttons on the taskbar)', a == b, (a, b))
    vm.key('meta_l'); time.sleep(0.6); vm.type('clock', 0.6); vm.key('ret'); time.sleep(1.2)
    vm.key('meta_l-tab'); time.sleep(0.8)
    vm.shot('115_taskview', 'v2.9 Win+Tab: Task View - both desktops, their windows, a + for a new one')
    vm.key('esc'); vm.key('ctrl-meta_l-left'); time.sleep(1.5)
    vm.shot('116_back_to_1', 'v2.9 Win+Ctrl+Left: back to Desktop 1 - the Terminal is still here, the Clock is not')
    check('desktops', 'each desktop keeps its own windows', front_rect(vm) == term, front_rect(vm))
    rclick(vm, term[0] + 200, term[1] + 12)
    vm.shot('117_move_menu', 'v2.9 right-click a title bar: Move to desktop 2')
    vm.click(term[0] + 240, term[1] + 12 + 4 + 3 * 26 + 9 + 13); time.sleep(0.8)   # Move to desktop 2
    vm.key('ctrl-meta_l-right'); time.sleep(1.5)
    vm.shot('118_moved', 'v2.9 the Terminal moved to Desktop 2 (with the Clock)')
    check('desktops', 'a window can move to another desktop', front_rect(vm) == term, front_rect(vm))
    vm.key('ctrl-meta_l-f4'); time.sleep(1.5)
    vm.shot('119_closed', 'v2.9 Win+Ctrl+F4 closes the desktop; its windows go to the one next to it')
    vm.key('meta_l'); time.sleep(0.6); vm.type('clock', 0.6); vm.key('ret'); time.sleep(1)
    vm.key('alt-f4'); time.sleep(0.5)
    focus_terminal(vm)
    check('desktops', 'back to one desktop with the Terminal', front_rect(vm) == term, front_rect(vm))

def dark_tests(vm):
    """v2.8: dark mode"""
    focus_terminal(vm)
    vm.cmd('theme dark', 1)
    vm.cmd('settings', 2)
    x, y, w = front_rect(vm)
    vm.shot('111_dark_settings', 'v2.8 dark mode (theme dark): Settings, the Terminal behind, the menus and Start all follow it')
    p = vm_pixel(vm, '111_dark_settings', x + 300, y + 40 + 150)
    check('dark', 'theme dark: window contents turn dark', lum(p) < 60, p)
    vm.key('meta_l'); time.sleep(0.8)
    vm.shot('112_dark_start', 'v2.8 the Start menu in dark mode')
    vm.key('esc'); time.sleep(0.3)
    vm.click(x + 1 + 400, y + 30 + 176); time.sleep(1)          # the Dark mode switch
    vm.shot('113_light_again', 'v2.8 the Dark mode switch in Settings turns it off again')
    p = vm_pixel(vm, '113_light_again', x + 300, y + 40 + 150)
    check('dark', 'the Settings switch turns dark mode off', lum(p) > 200, p)
    vm.key('alt-f4'); time.sleep(0.5)

def v27_tests(vm):
    """v2.7: Start search, screenshots, notifications, calendar reminders, lock screen, Win+E / Win+D"""
    import datetime
    focus_terminal(vm)
    term = front_rect(vm)
    vm.key('meta_l'); time.sleep(0.6); vm.type('calc', 0.8)
    vm.shot('100_search_app', 'v2.7 type in Start to search: "calc" finds Calculator')
    vm.key('ret'); time.sleep(2.5)
    check('search', 'Enter on a Start search result opens it', front_rect(vm) != term, front_rect(vm))
    vm.key('alt-f4'); time.sleep(0.5)
    vm.key('meta_l'); time.sleep(0.6); vm.type('weekend', 0.8)
    vm.shot('101_search_file', 'v2.7 Start search finds files too: "weekend" -> Weekend plans.txt in C:\\Notes')
    vm.key('ret'); time.sleep(1.5)
    vm.shot('101b_search_opened', 'v2.7 Enter opens it (Notepad)')
    vm.key('alt-f4'); time.sleep(0.6)
    vm.shot('102_before_print', 'just before Print Screen')
    vm.key('print'); time.sleep(4)
    vm.shot('103_print_toast', 'v2.7 Print Screen saves a PNG and says so in a pop-up (bottom right)')
    focus_terminal(vm)
    now = datetime.datetime.utcnow()                             # MyOS runs on UTC until "tz +3" later
    for add in (10, 11):                                         # one of them is 10 minutes away whatever the second
        t = now + datetime.timedelta(minutes=add)
        vm.cmd('append /Calendar/Events.txt %s Reminder test' % t.strftime('%Y-%m-%d %H:%M'), 0.3)
    time.sleep(1.2)
    vm.shot('104_reminder', 'v2.7 a Calendar reminder pops up 10 minutes before an event (Calendar does not need to be open)')
    px = vm_pixel(vm, '104_reminder', 872, 692)
    check('notify', 'a calendar reminder pops up', px == (255, 255, 255), px)
    vm.click(1024 - 136 + 18, 745); time.sleep(0.8)
    vm.shot('105_center', 'v2.7 the bell on the taskbar opens the notification centre (reminder, screenshot, Spider win)')
    vm.key('esc'); time.sleep(0.4)
    vm.key('meta_l-e'); time.sleep(1.5)
    vm.shot('106_win_e', 'v2.7 Win+E opens File Explorer')
    vm.key('meta_l-d'); time.sleep(0.8)
    vm.shot('107_win_d', 'v2.7 Win+D shows the desktop (all windows minimised)')
    vm.key('meta_l-d'); time.sleep(0.8)
    vm.key('alt-f4'); time.sleep(0.5)                           # File Explorer
    focus_terminal(vm)
    vm.cmd('password secret', 1)
    vm.key('meta_l-l'); time.sleep(1.2)
    vm.shot('108_lock', 'v2.7 Win+L locks the screen (password set with the "password" command)')
    vm.type('wrong\n', 1)
    vm.shot('109_lock_wrong', 'v2.7 a wrong password is refused')
    vm.type('secret\n', 1)
    vm.shot('110_unlocked', 'v2.7 the right password unlocks; everything is as it was')
    check('lock', 'the right password unlocks the screen', front_rect(vm) == term, front_rect(vm))

def vm_pixel(vm, shot, x, y):
    from PIL import Image
    return Image.open(os.path.join(OUT, shot + '.png')).convert('RGB').getpixel((x, y))

def calendar_tests(vm):
    """v2.6: Calendar"""
    focus_terminal(vm)
    vm.cmd('calendar', 3)
    vm.shot('96_calendar', 'v2.6 Calendar: this month, today circled, events shown in the days')
    vm.type('14:30\tDentist check-up\n', 1)                   # typing a digit starts the time box
    vm.type('9\tStand-up\n', 1)
    vm.shot('97_calendar_added', 'v2.6 two events added to today, listed by time on the right')
    win = window_origin(vm)
    vm.click(win[0] + 854, win[1] + 24 + 66 + 17); time.sleep(0.8)   # x on the first (09:00) event
    vm.shot('98_calendar_deleted', 'v2.6 the x deletes an event')
    vm.key('esc'); vm.key('pgdn'); time.sleep(0.6)
    vm.shot('99_calendar_next', 'v2.6 PgDn shows the next month')
    vm.key('alt-f4'); time.sleep(0.6)
    vm.click(980, 750); time.sleep(3)                            # the clock on the taskbar
    vm.shot('99b_calendar_clock', 'v2.6 clicking the clock on the taskbar opens Calendar')
    vm.key('alt-f4'); time.sleep(0.5)

def contacts_tests(vm):
    """v2.5: Contacts"""
    focus_terminal(vm)
    vm.cmd('contacts', 3)
    vm.shot('92_contacts', 'v2.5 Contacts: A-Z list with initials, details on the right')
    vm.key('ctrl-n'); time.sleep(0.4)
    vm.type('Zed Test\t+1 555 1234\tbad-email\n', 1)
    vm.shot('93_contacts_check', 'v2.5 Enter saves - but first the email address is checked')
    vm.key('end'); vm.key(*['backspace'] * 9); vm.type('zed@example.com\n', 1.2)
    vm.shot('94_contacts_saved', 'v2.5 fixed and saved: Zed Test is in the list (A to Z)')
    vm.key('ctrl-f'); vm.type('cairo', 1)
    vm.shot('95_contacts_search', 'v2.5 the search looks in every field: "cairo" finds Omar')
    vm.key('alt-f4'); time.sleep(0.5)

def spider_tests(vm):
    """v2.4: Spider Solitaire"""
    focus_terminal(vm)
    vm.cmd('spider 2', 4)
    vm.shot('82_spider', 'v2.4 Spider Solitaire, 2 suits: 10 columns, stock bottom right')
    vm.key('h'); time.sleep(0.5)
    vm.shot('83_spider_hint', 'v2.4 H shows a hint (cards lifted, target outlined in yellow)')
    vm.key('d'); time.sleep(1)
    vm.shot('84_spider_deal', 'v2.4 D deals one more card onto every column (4 deals left)')
    vm.key('alt-f4'); time.sleep(0.5)
    focus_terminal(vm)
    vm.cmd('spider demo', 4)
    win = window_origin(vm)
    vm.drag(win[0] + 130, win[1] + 86, win[0] + 50, win[1] + 372, 9); time.sleep(4)
    vm.shot('85_spider_win', 'v2.4 Spider: the Ace completes the last run -> the game is won')
    vm.key('esc'); time.sleep(0.6); vm.key('esc'); time.sleep(0.3); vm.key('alt-f4'); time.sleep(0.4)

def desk_tests(vm, x, y, w, cols):
    """v2.3: files and folders on the desktop (C:\\Desktop)"""
    vm.shot('75_desktop_files', 'v2.3 "My notes.txt" lives in C:\\Desktop, so it is on the desktop (right-hand side)')
    sx, sy = tile(x, y, 1, cols)                                 # Docs: Menu, Sub, Ideas, regnote
    drag_shot(vm, sx, sy, 976, 420, '76_drag_to_desktop', 'v2.3 dragging the Sub folder out of File Explorer onto the desktop')
    ex, ey = tile(x, y, cols + 2, cols)
    drag_shot(vm, *DESK(1), ex, ey, '77_drag_from_desktop', 'v2.3 dragging "My notes.txt" from the desktop into the open folder (Docs)')
    rclick(vm, 976, 500)
    vm.shot('78_desktop_new', 'v2.3 right-click the desktop: New folder, New text file, Paste')
    vm.click(814 + 40, 500 - MENU_H + 4 + 13); time.sleep(1)    # New folder (the menu opens upwards)
    vm.key(*['backspace'] * 12); vm.type('Stuff\n', 1)
    vm.key('alt-f4'); time.sleep(0.6)                            # the Explorer it opened in
    vm.shot('79_desktop_after', 'v2.3 the desktop now has the folders Stuff and Sub')
    vm.click(*DESK(1), double=True); time.sleep(1.2)             # Stuff, Sub -> Sub is the 2nd
    vm.shot('80_desktop_open', 'v2.3 double-clicking a desktop folder opens it in File Explorer')
    vm.key('alt-f4'); time.sleep(0.6)
    vm.click(*DESK(0)); time.sleep(0.4); vm.key('delete'); time.sleep(1.2)
    vm.shot('81_desktop_delete', 'v2.3 selecting Stuff on the desktop and pressing Delete sends it to the Recycle Bin')
    drag_shot(vm, 56, 50, 16 + 3 * 96 + 40, 10 + 6 * 88 + 40, '86_icon_drag', 'v2.4 dragging the This PC icon to a new place')
    vm.shot('87_icon_moved', 'v2.4 This PC stays where it was dropped (saved in C:\\$Desktop.ini)')
    focus_terminal(vm); vm.cmd('explorer', 1.5)

def boot1_bin_checks():
    check('explorer', 'restored file is back in C:\\Docs', mexists('/Docs/Ideas for MyOS.txt'))
    check('drag', 'the dragged folder left C:\\Docs', not mexists('/Docs/Box') and not mexists('/Docs/shopping list.txt'))
    infs = [l for l in mtools('mdir', '-b', '-i', OUT + '/part.img', '::/$Recycle').split() if l.lower().endswith('.inf')]
    got = {}
    for f in infs:
        got[f.split('/')[-1]] = mtype('/$Recycle/' + f.split('/')[-1]) or ''
    box = [k for k, v in got.items() if v == '/Docs/Box']
    want = sorted(['/Docs/Box', '/Desktop/Stuff', '/Docs/Sub/New text file.txt', '/Docs/Sub/New text file (2).txt', '/Docs/Sub/New text file (3).txt',
                   '/Docs/Sub/Ideas for MyOS.txt', '/Docs/Sub/regnote.txt'])
    check('bin', 'the bin holds the 7 deleted items, each with a note of where it came from', sorted(got.values()) == want, got)
    trip = mtype('/Notes/Trip to Jeddah.txt') or ''
    check('notes', 'a new note is saved as C:\\Notes\\<its title>.txt', trip.startswith('Trip to Jeddah') and 'Book the hotel' in trip, trip[:60])
    check('notes', 'a deleted note\'s file is gone, the others stay', not mexists('/Notes/Book ideas.txt') and mexists('/Notes/Weekend plans.txt'))
    book = mtype('/Contacts/Contacts.txt') or ''
    cal = mtype('/Calendar/Events.txt') or ''
    check('calendar', 'events added and deleted are saved in C:\\Calendar\\Events.txt',
          ' 14:30 Dentist check-up' in cal and 'Stand-up' not in cal and 'Dinner with Sara' in cal, cal)
    check('contacts', 'the new contact is saved in C:\\Contacts\\Contacts.txt',
          'Name: Zed Test\nPhone: +1 555 1234\nEmail: zed@example.com' in book and 'Name: Sara Ahmed' in book, book[-120:])
    check('desktop', 'the moved icon\'s place is saved', '@This PC=3,6' in (mtype('/$Desktop.ini') or '') and '@Terminal=0,2' in (mtype('/$Desktop.ini') or ''), mtype('/$Desktop.ini'))
    check('desktop', 'dragged to and from the desktop, new folder made on it',
          mexists('/Desktop/Sub') and mexists('/Docs/My notes.txt') and not mexists('/Desktop/My notes.txt') and not mexists('/Docs/Sub'))
    check('menu', '"Paste into folder" copied the file into C:\\Docs\\Menu', mexists('/Docs/Menu/Ideas for MyOS.txt') and mexists('/Docs/Ideas for MyOS.txt'))
    check('multi', 'several items cut + pasted, copied, deleted together',
          mexists('/Desktop/Sub') and not mexists('/Docs/New text file.txt') and mexists('/Docs/Ideas for MyOS.txt')
          and mexists('/Docs/regnote.txt') and not mexists('/Docs/Sub/regnote.txt'))
    if box:
        r = box[0][:-4]
        check('drag', 'the file dragged into Box went to the bin with it', mexists(f'/$Recycle/{r}/shopping list.txt'))

def boot2_bin(vm):
    """boot 2: empty the Recycle Bin (it survived the restart)"""
    vm.click(*BIN_ICON, double=True); time.sleep(1.5)
    x, y, w = front_rect(vm)
    vm.shot('48_bin_after_restart', 'boot 2: the Recycle Bin still holds its 7 items after a restart')
    vm.click(x + 1 + 226 + 72, y + 30 + 18); time.sleep(0.6)   # Empty Recycle Bin
    vm.shot('49_bin_empty_confirm', 'v2.0 Empty Recycle Bin asks first')
    vm.key('ret'); time.sleep(1.2)
    vm.shot('49b_bin_emptied', 'v2.0 the Recycle Bin is empty (desktop icon empty too)')
    vm.key('alt-f4'); time.sleep(0.5)
    vm.shot('49c_icons_after_restart', 'boot 2: This PC is still where it was moved to')
    rclick(vm, 976, 600)
    vm.click(814 + 40, 600 - MENU_H + 4 + 6 * 26 + 2 * 9 + 13); time.sleep(1)   # Auto arrange icons
    vm.shot('49d_auto_arrange', 'v2.4 right-click > Auto arrange icons puts This PC back')

def boot2_disk():
    disk_checks('after boot 2')
    left = [l for l in mtools('mdir', '-b', '-i', OUT + '/part.img', '::/$Recycle').split() if '/R' in l]
    check('bin', 'Empty Recycle Bin removed everything', not left, left)
    check('desktop', 'Auto arrange icons forgets the saved places', not mexists('/$Desktop.ini'))
    ini2 = mtype('/settings.ini') or ''
    check('lock', 'password off removes it', 'password=' not in ini2, ini2)

def disk_checks(stage):
    p = disk_part()
    fsck = subprocess.run(['fsck.fat', '-n', p], capture_output=True, text=True)
    check('disk', f'{stage}: fsck finds no errors', fsck.returncode == 0, fsck.stdout.strip().splitlines()[-1] if fsck.stdout else '')
    return p

def boot1_disk():
    disk_checks('after boot 1')
    t = mtype('/reg.txt') or ''
    check('files', 'write + append', 'Hello from the regression test' in t and 'Second line' in t, repr(t[:60]))
    check('files', 'copy + ren (reg3.txt exists, reg2.txt gone)', mexists('/reg3.txt') and not mexists('/reg2.txt'))
    check('files', 'del (gone.txt removed)', not mexists('/gone.txt'))
    check('explorer', 'folder RegDir (with a file) deleted after confirming', not mexists('/RegDir'))
    check('explorer', 'Coast.jpg pasted into C:\\ and renamed', mexists('/Copied coast.jpg'))
    a = subprocess.run(['mtype', '-i', OUT + '/part.img', '::/Copied coast.jpg'], capture_output=True).stdout
    b = open(MYOS + '/media/Coast.jpg', 'rb').read()
    check('explorer', 'the copy is byte-for-byte the same', a == b, f'{len(a)} vs {len(b)} bytes')
    n = mtype('/Docs/regnote.txt') or ''
    check('notepad', 'clipboard paste from Calculator (57)', n.startswith('57'), repr(n[:40]))
    check('notepad', 'Replace all cat -> dog', 'dog sat' in n and 'A dog is a dog' in n and 'cat' not in n, repr(n[:80]))
    check('paint', 'painting.bmp saved', mexists('/Pictures/painting.bmp'))
    ini = mtype('/settings.ini') or ''
    check('settings', 'wallpaper picture saved', 'picture=/Pictures/Valley.jpg' in ini, ini.replace('\r\n', ' '))
    check('settings', 'tz and volume saved', 'timezone=3' in ini.replace('+', '') or 'timezone=180' in ini, '')
    check('settings', 'explorer_view saved', 'explorer_view=1' in ini)
    check('dark', 'the dark mode choice is saved in settings.ini', 'dark=0' in ini, '')
    if True:                                                     # (boot 1)
        check('lock', 'the password is kept only as a hash', 'password=' + lock_hash('secret') in ini and 'secret' not in ini, ini)
        shots = [l for l in mtools('mdir', '-b', '-i', OUT + '/part.img', '::/Pictures/Screenshots').splitlines() if l.lower().endswith('.png')]
        ok = False
        if shots:
            from PIL import Image
            import io, numpy as np
            subprocess.run(['mcopy', '-n', '-i', OUT + '/part.img', '::/Pictures/Screenshots/' + shots[0].split('/')[-1], OUT + '/shot_from_disk.png'], capture_output=True)
            try:
                a = np.array(Image.open(OUT + '/shot_from_disk.png').convert('RGB')).astype(int)
                b = np.array(Image.open(os.path.join(OUT, '102_before_print.png')).convert('RGB')).astype(int)
                same = (np.abs(a - b).sum(-1) == 0).mean()
                ok = a.shape == (768, 1024, 3) and same > 0.97
            except Exception as e:
                same = str(e)
        check('screenshot', 'Print Screen saved a PNG of the screen in C:\\Pictures\\Screenshots', ok, (shots, locals().get('same')))

    boot1_bin_checks()

def boot2():
    vm = VM()
    time.sleep(12)
    vm.shot('44_boot_locked', 'v2.7 with a password set, MyOS starts at the lock screen')
    check('lock', 'a password means the lock screen at start-up', vm_pixel(vm, '44_boot_locked', 300, 745) != vm_pixel(vm, '36_final', 300, 745), '')
    vm.type('secret\n', 1.5)
    vm.cmd('password off', 1)
    vm.shot('45_reboot', 'boot 2: wallpaper (Valley.jpg) and settings kept after a restart')
    vm.cmd('dir', 1); vm.cmd('type reg.txt', 1)
    vm.shot('46_reboot_files', 'boot 2: files are still there', (150, 50, 772, 450))
    boot2_bin(vm)
    focus_terminal(vm)
    vm.cmd('bsod', 2)
    vm.shot('47_bsod', 'v0.2 crash screen (bsod command)')
    vm.key('spc'); time.sleep(3)
    check('crash', 'a key on the crash screen restarts (QEMU exits with -no-reboot)', not vm.alive())
    vm.quit()

def boot3_iso():
    vm = VM(iso=True)
    time.sleep(20)
    im = vm.shot('50_iso', 'boot 3: from the ISO (GRUB, BIOS)')
    check('iso', 'ISO boots to the desktop', not_blank(im) and np.asarray(im)[745, 600].sum() < 200)
    vm.cmd('explorer', 1.5); vm.type('pic'); vm.key('ret'); time.sleep(8)
    vm.shot('51_iso_explorer', 'ISO boot: Explorer previews')
    vm.quit()

def host_suites():
    T = '/home/claude/pngtest'
    def run(cmd): return subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=T)
    run('gcc -O2 -w -o t t.c ../myos/image.c ../myos/jpeg.c')
    fails = []
    for s in (11, 12, 13):
        run(f'rm -rf cases && python3 -W ignore gen.py {s}'); r = run('./t cases/*.png'); fails.append(r.stdout.strip().splitlines()[-1])
    check('decoders', 'PNG: 3 x 52 generated files exact', all(f.startswith('0 of') for f in fails), fails)
    fails = []
    for s in (31, 32, 33):
        run(f'rm -rf jcases && python3 -W ignore jgen.py {s}'); r = run('./t jcases/*.jpg'); fails.append(r.stdout.strip().splitlines()[-1])
    r = run('./t jbig/*.jpg'); fails.append(r.stdout.strip().splitlines()[-1])
    check('decoders', 'JPEG: 3 x 48 random files + 5 big ones exact vs libjpeg-turbo', all(f.startswith('0 of') for f in fails), fails)
    fails = []
    for s in (41, 42):
        run(f'rm -rf gcases && python3 -W ignore ggen.py {s}'); r = run('./t gcases/*.gif'); fails.append(r.stdout.strip().splitlines()[-1])
    check('decoders', 'GIF: 2 x 40 files exact', all(f.startswith('0 of') for f in fails), fails)
    run('gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -w -o fz fuzz.c ../myos/image.c ../myos/jpeg.c')
    r = run('./fz cases/*.png jcases/*.jpg gcases/*.gif 2>&1 | tail -1')
    check('decoders', 'fuzz: damaged PNG/JPEG/GIF files never crash (ASan+UBSan)', 'no crash' in r.stdout, r.stdout.strip())

def contact_sheets():
    names = [n for n in shots if os.path.exists(f'{OUT}/{n[0]}.png')]
    per = 6
    for k in range(0, len(names), per):
        group = names[k:k + per]
        sheet = Image.new('RGB', (2 * 640, 3 * 500), 'white'); d = ImageDraw.Draw(sheet)
        for i, (n, cap) in enumerate(group):
            im = Image.open(f'{OUT}/{n}.png'); im.thumbnail((630, 470))
            x, y = (i % 2) * 640, (i // 2) * 500
            sheet.paste(im, (x + 5, y + 24)); d.text((x + 6, y + 6), f'{n}: {cap}', fill='black')
        sheet.save(f'{OUT}/sheet{k // per + 1}.png')

if __name__ == '__main__':
    if os.path.exists(OUT): shutil.rmtree(OUT)
    os.makedirs(OUT)
    shutil.copy(MYOS + '/disk.img', IMG)
    only = sys.argv[1:] or ['host', 'boot1', 'boot2', 'iso']
    if 'host' in only: host_suites()
    if 'boot1' in only: boot1(); boot1_disk()
    if 'boot2' in only: boot2(); boot2_disk()
    if 'iso' in only: boot3_iso(); disk_checks('after the ISO boot')
    contact_sheets()
    passed = sum(r[2] for r in results)
    with open(OUT + '/report.txt', 'w') as f:
        for a, w, ok, d in results: f.write(f'{"PASS" if ok else "FAIL"}  [{a}] {w}  {d}\n')
        f.write(f'\n{passed} of {len(results)} checks passed\n')
    print(f'\n{passed} of {len(results)} checks passed')
