# Build MyOS.  `make` builds myos.bin, `make run` boots it in QEMU.
CFLAGS = -m32 -ffreestanding -fno-stack-protector -fno-pie -nostdlib -mgeneral-regs-only -O2 -Wall -Wextra
LDFLAGS = -m elf_i386 -T linker.ld

OBJS = boot.o interrupts.o kernel.o idt.o mm.o gfx.o mouse.o shell.o wm.o ata.o fat32.o proc.o sound.o e1000.o net.o settings.o image.o jpeg.o

APPS = apps/hello.elf apps/bounce.elf apps/spin.elf apps/guess.elf apps/crash.elf apps/paint.elf apps/snake.elf apps/mines.elf apps/music.elf apps/viewer.elf apps/web.elf apps/calc.elf apps/solitaire.elf apps/freecell.elf apps/spider.elf apps/widgets.elf
UCFLAGS = -m32 -ffreestanding -fno-stack-protector -fno-pie -nostdlib -fno-builtin -mgeneral-regs-only -O2 -Wall -Wextra

all: myos.bin myos-grub.bin $(APPS)

# ---- user programs (run in ring 3, loaded from C:\Apps) ----
apps/crt0.o: apps/crt0.asm
	nasm -f elf32 $< -o $@

# Calculator uses floating point (x87 FPU), so it may use FPU registers
apps/calc.o apps/fputest.o: apps/%.o: apps/%.c apps/myos.h apps/ui.h apps/cards.h syscalls.h
	gcc $(filter-out -mgeneral-regs-only,$(UCFLAGS)) -mfpmath=387 -mno-sse -c $< -o $@

apps/%.o: apps/%.c apps/myos.h apps/ui.h apps/cards.h syscalls.h
	gcc $(UCFLAGS) -c $< -o $@

# the picture decoder (image.c) is shared by the kernel and programs
apps/image.o: image.c image.h
	gcc $(UCFLAGS) -c $< -o $@
apps/jpeg.o: jpeg.c image.h
	gcc $(UCFLAGS) -c $< -o $@

# programs that use the widget toolkit (apps/ui.c)
UI_APPS = apps/solitaire.elf apps/freecell.elf apps/spider.elf apps/widgets.elf apps/viewer.elf apps/web.elf
$(UI_APPS): apps/%.elf: apps/crt0.o apps/malloc.o apps/ui.o apps/cards.o apps/image.o apps/jpeg.o apps/%.o apps/user.ld
	ld -m elf_i386 --no-warn-rwx-segments -T apps/user.ld -o $@ apps/crt0.o apps/$*.o apps/ui.o apps/cards.o apps/image.o apps/jpeg.o apps/malloc.o

apps/%.elf: apps/crt0.o apps/malloc.o apps/%.o apps/user.ld
	ld -m elf_i386 --no-warn-rwx-segments -T apps/user.ld -o $@ apps/crt0.o apps/$*.o apps/malloc.o

# For QEMU's -kernel option (graphics set up by MyOS itself)
myos.bin: $(OBJS) linker.ld
	ld $(LDFLAGS) -o $@ $(OBJS)

# For real PCs booted through GRUB (asks GRUB for a 1024x768 graphics mode)
myos-grub.bin: boot-grub.o $(filter-out boot.o,$(OBJS)) linker.ld
	ld $(LDFLAGS) -o $@ boot-grub.o $(filter-out boot.o,$(OBJS))

boot-grub.o: boot.asm
	nasm -f elf32 -DGRUB_VIDEO $< -o $@

%.o: %.asm
	nasm -f elf32 $< -o $@

%.o: %.c kernel.h font.h syscalls.h image.h
	gcc $(CFLAGS) -c $< -o $@

run: myos.bin disk.img
	qemu-system-i386 -kernel myos.bin -m 256 -audiodev pa,id=snd0 -device sb16,audiodev=snd0 -machine pcspk-audiodev=snd0 -netdev user,id=n0 -device e1000,netdev=n0 -drive file=disk.img,format=raw,index=0,media=disk

# 64 MB sample hard disk with a FAT32 partition (needs dosfstools, fdisk, mtools)
disk.img: $(APPS) mkdisk.sh mkmedia.py media/Valley.jpg media/Coast.jpg media/Rainbow.gif
	./mkdisk.sh

clean:
	rm -rf *.o apps/*.o apps/*.elf myos.bin myos-grub.bin myos.iso iso

# Bootable CD/USB image (BIOS + UEFI) using GRUB
iso: myos-grub.bin grub.cfg
	mkdir -p iso/boot/grub
	cp myos-grub.bin iso/boot/myos.bin
	cp grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso iso

run-iso: myos.iso disk.img
	qemu-system-i386 -cdrom myos.iso -boot d -m 256 -audiodev pa,id=snd0 -device sb16,audiodev=snd0 -machine pcspk-audiodev=snd0 -netdev user,id=n0 -device e1000,netdev=n0 -drive file=disk.img,format=raw,index=0,media=disk
