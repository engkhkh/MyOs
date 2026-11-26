/* syscalls.h - System call numbers, shared by the kernel and user programs.
 * A program puts the number in EAX, arguments in EBX, ECX, EDX, ESI, EDI,
 * executes "int 0x80", and finds the result in EAX. */
#ifndef SYSCALLS_H
#define SYSCALLS_H
#define SYS_EXIT       0   /* exit(code) */
#define SYS_PRINT      1   /* print(text): console-style output in the window */
#define SYS_GETKEY     2   /* getkey(): wait for a key press */
#define SYS_SLEEP      3   /* sleep(ms) */
#define SYS_TICKS      4   /* ticks(): milliseconds since boot */
#define SYS_FILL       5   /* fill(x, y, w, h, colour) */
#define SYS_TEXT       6   /* text(x, y, string, colour [, scale 2-4 for big text]) */
#define SYS_CLEAR      7   /* clear(colour): blank the window, cursor to top */
#define SYS_GETPID     8   /* getpid() */
#define SYS_READFILE   9   /* readfile(path, buffer, max) -> bytes or -1 */
#define SYS_WRITEFILE 10   /* writefile(path, buffer, length) -> bytes or -1 */
#define SYS_TIME      11   /* time(int out[6]): hour, min, sec, day, month, year */
#define SYS_YIELD     12   /* yield(): let other programs run */
#define SYS_POLLKEY   13   /* pollkey(): key press or -1, never waits */
#define SYS_CANVAS    14   /* canvas(): (width << 16) | height of the window area */
#define SYS_SETCOLOR  15   /* setcolor(text colour, background colour) */
#define SYS_SETTITLE  16   /* settitle(text) */
#define SYS_GETEVENT  17   /* getevent(struct event *): wait for a key or mouse event */
#define SYS_POLLEVENT 18   /* pollevent(struct event *): 1 if an event was waiting, else 0 */
#define SYS_SBRK      19   /* sbrk(bytes): grow the program's heap, returns old end or -1 */
#define SYS_RESIZE    20   /* resize(width, height): change the window's drawing area */
#define SYS_BLIT      21   /* blit(x|y<<16, w|h<<16, pixels, stride): copy an image to the window */

#define SYS_TONE      22   /* tone(freq, ms): queue a PC-speaker beep (freq 0 = pause) */
#define SYS_PLAY      23   /* play(pcm, bytes, rate, bits | channels << 8): Sound Blaster playback */
#define SYS_STOPSOUND 24   /* stopsound() */
#define SYS_SOUNDPOS  25   /* soundpos(): ms played so far, -1 if nothing is playing */
#define SYS_GETARG    26   /* getarg(buf, max): the file/argument the program was started with */
#define SYS_LISTDIR   27   /* listdir(path, struct dirent *, max): number of entries or -1 */
#define SYS_VOLUME    28   /* volume(v): set 0..100 (or -1 to just read); returns the volume */

#define SYS_RESOLVE   29   /* resolve(name, unsigned *ip): 1 found, 0 still looking (ask again), -1 failed */
#define SYS_TCPOPEN   30   /* tcpopen(ip, port): connection number, or -1 */
#define SYS_TCPSTATUS 31   /* tcpstatus(c): 0 connecting, 1 open, 2 other side done sending, -1 failed */
#define SYS_TCPSEND   32   /* tcpsend(c, data, len): bytes accepted */
#define SYS_TCPRECV   33   /* tcprecv(c, buf, max): bytes, 0 = nothing yet, -1 = finished */
#define SYS_TCPCLOSE  34   /* tcpclose(c) */
#define SYS_NETINFO   35   /* netinfo(unsigned out[4]): ip, mask, gateway, dns; returns 1 if online */
#define SYS_CLIPSET   36   /* clipset(text, len): put text on the clipboard (shared with Notepad etc.) */
#define SYS_FONT      38   /* font(buf, bold): copy the 8x16 system font (128 chars x 16 bytes) into buf */
#define SYS_WALLPAPER 39   /* wallpaper(path): use this .png / .bmp as the desktop background; 0 = ok */
#define SYS_RESIZABLE 40   /* resizable(min_w, min_h): the user may resize / maximise the window; EV_RESIZE tells the new size */
#define SYS_DELETE    41   /* deletefile(path): delete a file (not a folder); 0 = ok, -1 = error */
#define SYS_MKDIR     42   /* makedir(path): make a folder; 0 = ok (or it already exists), -1 = error */
#define SYS_NOTIFY    43   /* notify(title, text): a pop-up notification (also kept in the notification centre) */
#define SYS_CLIPGET   37   /* clipget(buf, max): copy clipboard text into buf; returns its full length */

/* Folder entries returned by listdir() */
struct dirent { char name[64]; int is_dir; unsigned size; };

/* Events delivered to programs */
#define EV_KEY        1    /* key = key code */
#define EV_MOUSE_DOWN 2    /* x, y inside the window; buttons: 1 = left, 2 = right, 4 = middle */
#define EV_MOUSE_UP   3
#define EV_MOUSE_MOVE 4
#define EV_RESIZE     5    /* the user resized the window: x = new width, y = new height (the canvas is already that size) */
struct event { int type, key, x, y, buttons; };
#endif
