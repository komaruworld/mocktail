# FreeBSD Linuxulator

FreeBSD Linuxulator support is experimental. I tested it on FreeBSD
15.1-RELEASE-p2 with a Fedora 44 x86_64 userspace.

FreeBSD returns the wrong filesystem type for `/proc`. Roblox treats this as a
broken install and disconnects with Error 304 after about a minute.
[`linuxulator.patch`](linuxulator.patch) makes Linuxulator return the same
value as Linux.

## Patch FreeBSD

Use a clean Git checkout that exactly matches the running FreeBSD kernel. The
tested source revision is `aadd58dddcbc78f4d5594827b46b5633552b15ce`. Run
the following commands as root.

```sh
fetch -o /root/linuxulator.patch https://raw.githubusercontent.com/komaruworld/mocktail/main/packaging/freebsd/linuxulator.patch
cd /usr/src
git apply /root/linuxulator.patch
make -j2 kernel-toolchain
make -j2 buildkernel KERNCONF=GENERIC MODULES_OVERRIDE=linux64
```

Install only the rebuilt module. Keep a backup and reboot instead of unloading
a live Linuxulator module.

```sh
cp -p /boot/kernel/linux64.ko /boot/kernel/linux64.ko.mocktail-backup
install -o root -g wheel -m 0444 /usr/obj/usr/src/amd64.amd64/sys/GENERIC/modules/usr/src/sys/modules/linux64/linux64.ko /boot/kernel/linux64.ko.mocktail-new
mv /boot/kernel/linux64.ko.mocktail-new /boot/kernel/linux64.ko
kldxref /boot/kernel
reboot
```

After reboot, verify the result from the Fedora userspace. It must print
`9fa0`.

```sh
chroot /compat/linux stat -f -c %t /proc
```

## Run Mocktail

You can simply run the Mocktail AppImage from the
[latest release](https://github.com/komaruworld/mocktail/releases/latest) or
[nightly build](https://github.com/komaruworld/mocktail/releases/tag/continuous)
inside the Fedora userspace.

## Audio

Install the ALSA OSS plugin inside the Fedora userspace.

```sh
dnf install -y alsa-lib alsa-plugins-oss
```

On the FreeBSD host, use `cat /dev/sndstat` to find the correct audio device.
The number is system-specific: it may be `/dev/dsp0`, `/dev/dsp1`,
`/dev/dsp2`, or another device. Replace `/dev/dsp3` below with yours.

```sh
cat << 'EOF' > /etc/asound.conf
pcm.!default {
    type oss
    device /dev/dsp3
}
ctl.!default {
    type oss
    device /dev/dsp3
}
EOF
```

Launch Mocktail with the ALSA audio driver.

```sh
SDL_AUDIO_DRIVER=alsa ./Mocktail-x86_64.AppImage
```

Mocktail cannot override this check because Roblox reads `/proc` directly. The
Linuxulator patch fixes the value before Roblox sees it.
