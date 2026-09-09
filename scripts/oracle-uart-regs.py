"""Read the oracle's PL011 registers while its guest sits at the shell prompt.

The oracle answers questions the emulator cannot answer about itself: what FR,
CR, IMSC and RIS actually read back at an idle prompt, and whether a line typed
there is delivered (it is, even with RXE clear -- the driver clears RXE before
every transmit). Boot the fixture under QEMU with a monitor socket, ask the
monitor, print the answers. Usage: scripts/oracle-uart-regs.py [initramfs]
"""
import os, socket, subprocess, threading, time
if not os.path.exists("/tmp/orin.fifo"): os.mkfifo("/tmp/orin.fifo")
cmd = ["qemu-system-aarch64","-machine","virt,virtualization=off","-cpu","cortex-a53","-m","256",
  "-kernel","guest/build/Image","-initrd","guest/build/initramfs.cpio",
  "-append","console=ttyAMA0 earlycon=pl011,0x9000000 panic=-1 rdinit=/init",
  "-display","none","-no-reboot","-serial","stdio",
  "-chardev","socket,id=mon,path=/tmp/mon.sock,server=on,wait=off","-monitor","chardev:mon"]
log = open("/home/oyx/proj/oemu/.dsh2/logs/oracle_regs.log","wb")
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.DEVNULL)
def feed():
    time.sleep(33); p.stdin.write(b"echo SHELL_ALIVE\n"); p.stdin.flush()
threading.Thread(target=feed, daemon=True).start()
time.sleep(24)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect("/tmp/mon.sock")
s.settimeout(0.5)
def drain():
    buf=b""
    try:
        while True:
            d=s.recv(200000)
            if not d: break
            buf+=d
    except Exception: pass
    return buf
def q(cmd):
    drain(); s.sendall(cmd.encode()+b"\n"); time.sleep(0.9)
    return drain().decode(errors="replace")
def regs(tag):
    print("== %s ==" % tag)
    print(q("xp/1xw 0x9000018").strip())   # FR
    print(q("xp/1xw 0x9000030").strip())   # CR
    print(q("xp/1xw 0x9000038").strip())   # IMSC
    print(q("xp/1xw 0x900003c").strip())   # RIS
    print(q("xp/1xw 0x9000040").strip())   # MIS
regs("oracle BEFORE the feed")
time.sleep(12)
regs("oracle AFTER the feed")
s.sendall(b"quit\n"); p.terminate()
