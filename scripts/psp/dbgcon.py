#!/usr/bin/env python3
"""dbgcon.py -- PC side of the PSP Wi-Fi debug console (engine/platform/psp/dbg_psp.c)

Start the game with -dbgcon (e.g. in start.cmd). It joins Wi-Fi at startup and listens on
UDP 27999. The PC always sends first, so its firewall only sees replies.

  dbgcon.py listen <psp ip> [log]      keepalives, console output -> log (default build-psp/dbgcon.log),
                                       commands from <log>.cmd (one per line) -> PSP. Runs until killed.
                                       When the PSP is silent for 10 s the keepalives are broadcast too, and
                                       the listener follows the PSP to a new address (DHCP), written to <log>.ip
  dbgcon.py cmd "<command>" [log]      queue a console command for the running listener
  dbgcon.py update <psp ip> <eboot> [dest]
                                       send psp_update, push the EBOOT over TCP; the game restarts with it.
                                       With dest (relative to the game directory, e.g.
                                       valve/cl_dlls/client_psp_mipsel.prx) the file is written there instead
"""
import os
import socket
import sys
import time

PORT = 27999
DEFAULT_LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build-psp', 'dbgcon.log')


def listen(ip, log):
	cmdfile = log + '.cmd'
	ipfile = log + '.ip'
	sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
	sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
	sock.bind(('0.0.0.0', 0))
	sock.settimeout(0.2)
	last_keepalive = 0.0
	last_heard = time.time()
	with open(log, 'ab', buffering=0) as out:
		out.write(('\n[dbgcon.py: listening for %s at %s]\n' % (ip, time.strftime('%H:%M:%S'))).encode())
		while True:
			now = time.time()
			if now - last_keepalive >= 2.0:
				sock.sendto(b'\xff', (ip, PORT))
				# after a restart the PSP may come back with another address: the game
				# answers whoever sent the last packet, so a broadcast finds it
				if now - last_heard > 10.0:
					try:
						sock.sendto(b'\xff', ('255.255.255.255', PORT))
					except OSError:
						pass
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
				data, addr = sock.recvfrom(4096)
				if addr[1] == PORT:
					last_heard = time.time()
					if addr[0] != ip:
						ip = addr[0]
						out.write(('\n[dbgcon.py: the PSP is at %s now]\n' % ip).encode())
						with open(ipfile, 'w') as f:
							f.write(ip + '\n')
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


def update(ip, eboot, dest=None):
	data = open(eboot, 'rb').read()
	try:
		# where the listener last heard the PSP
		with open(DEFAULT_LOG + '.ip') as f:
			found = f.read().strip()
		if found and found != ip:
			print('using %s, where the listener found the PSP' % found)
			ip = found
	except OSError:
		pass
	command = 'psp_update' + (' ' + dest if dest else '')
	# through the listener if it runs: the game sends its output to whoever sent the
	# last packet, a packet from here would send the psp_update messages nowhere
	if cmd(command, DEFAULT_LOG) != 0:
		udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
		udp.sendto(command.encode(), (ip, PORT))
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
	elif len(a) in (3, 4) and a[0] == 'update':
		return update(a[1], a[2], a[3] if len(a) > 3 else None)
	else:
		print(__doc__)
		return 2


if __name__ == '__main__':
	sys.exit(main() or 0)
