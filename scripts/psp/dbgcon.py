#!/usr/bin/env python3
"""dbgcon.py -- PC side of the PSP Wi-Fi debug console (engine/platform/psp/dbg_psp.c)

Start the game with -dbgcon (e.g. in start.cmd). It joins Wi-Fi at startup and listens on
UDP 27999. The PC always sends first, so its firewall only sees replies.

  dbgcon.py listen <psp ip> [log]      keepalives, console output -> log (default build-psp/dbgcon.log),
                                       commands from <log>.cmd (one per line) -> PSP. Runs until killed.
  dbgcon.py cmd "<command>" [log]      queue a console command for the running listener
  dbgcon.py update <psp ip> <eboot>    send psp_update, push the EBOOT over TCP; the game restarts with it
"""
import os
import socket
import sys
import time

PORT = 27999
DEFAULT_LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build-psp', 'dbgcon.log')


def listen(ip, log):
	cmdfile = log + '.cmd'
	sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
	sock.bind(('0.0.0.0', 0))
	sock.settimeout(0.2)
	last_keepalive = 0.0
	with open(log, 'ab', buffering=0) as out:
		out.write(('\n[dbgcon.py: listening for %s at %s]\n' % (ip, time.strftime('%H:%M:%S'))).encode())
		while True:
			now = time.time()
			if now - last_keepalive >= 2.0:
				sock.sendto(b'\xff', (ip, PORT))
				last_keepalive = now
			if os.path.exists(cmdfile):
				try:
					with open(cmdfile, 'rb') as f:
						cmds = f.read().splitlines()
					os.remove(cmdfile)
				except OSError:
					cmds = []
				for c in cmds:
					if c.strip():
						sock.sendto(c, (ip, PORT))
						out.write(b'[dbgcon.py: > ' + c + b']\n')
			try:
				data, _ = sock.recvfrom(4096)
				out.write(data)
			except socket.timeout:
				pass
			except OSError:  # e.g. ICMP port unreachable while the game isn't up
				time.sleep(0.2)


def cmd(command, log):
	cmdfile = log + '.cmd'
	with open(cmdfile, 'ab') as f:
		f.write(command.encode() + b'\n')
	for _ in range(50):
		if not os.path.exists(cmdfile):
			return 0
		time.sleep(0.1)
	print('listener did not pick up the command (is "dbgcon.py listen" running?)')
	return 1


def update(ip, eboot):
	data = open(eboot, 'rb').read()
	udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
	udp.sendto(b'psp_update', (ip, PORT))
	for attempt in range(40):
		try:
			tcp = socket.create_connection((ip, PORT), timeout=3)
			break
		except OSError:
			time.sleep(0.5)
	else:
		print('PSP never accepted the connection')
		return 1
	tcp.settimeout(60)
	tcp.sendall(b'%d\n' % len(data) + data)
	reply = tcp.recv(16)
	print('sent %d bytes, PSP says %r' % (len(data), reply))
	return 0 if reply.startswith(b'OK') else 1


def main():
	a = sys.argv[1:]
	if len(a) >= 2 and a[0] == 'listen':
		listen(a[1], a[2] if len(a) > 2 else DEFAULT_LOG)
	elif len(a) >= 2 and a[0] == 'cmd':
		return cmd(a[1], a[2] if len(a) > 2 else DEFAULT_LOG)
	elif len(a) == 3 and a[0] == 'update':
		return update(a[1], a[2])
	else:
		print(__doc__)
		return 2


if __name__ == '__main__':
	sys.exit(main() or 0)
