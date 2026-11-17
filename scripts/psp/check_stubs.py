#!/usr/bin/env python3
# check_stubs.py -- verify that every PSP import stub is registered in .lib.stub
#
# psp-fixup-imports only registers the first contiguous run of a library's import stubs.
# If a library's stubs end up split (e.g. because the library was listed twice on the link
# command line), the rest stay as `jr ra; nop` and silently return garbage at runtime.
#
# Usage: check_stubs.py <elf> [<elf>...]   (run on the output of psp-fixup-imports)
# Exit code is non-zero if any stub isn't covered by a .lib.stub entry.

import struct
import sys

def read_sections(data):
	if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
		raise ValueError('not a 32-bit little-endian ELF')
	e_shoff, = struct.unpack_from('<I', data, 0x20)
	e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x2e)
	shdrs = []
	for i in range(e_shnum):
		shdrs.append(struct.unpack_from('<IIIIIIIIII', data, e_shoff + i * e_shentsize))
	strtab = shdrs[e_shstrndx]
	sections = {}
	for sh in shdrs:
		off = strtab[4] + sh[0]
		name = data[off:data.index(b'\0', off)].decode()
		sections[name] = sh
	return sections, shdrs

def read_symbols(data, shdrs):
	syms = {}
	for sh in shdrs:
		if sh[1] != 2: # SHT_SYMTAB
			continue
		strsh = shdrs[sh[6]]
		for i in range(sh[5] // 16):
			st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from('<IIIBBH', data, sh[4] + i * 16)
			if st_name:
				off = strsh[4] + st_name
				syms.setdefault(st_value, data[off:data.index(b'\0', off)].decode())
	return syms

def vaddr_to_str(data, sections, addr):
	for sh in sections.values():
		if sh[1] == 8: # SHT_NOBITS
			continue
		if sh[3] <= addr < sh[3] + sh[5]:
			off = sh[4] + addr - sh[3]
			return data[off:data.index(b'\0', off)].decode(errors='replace')
	return '<0x%08x>' % addr

def check(path):
	with open(path, 'rb') as f:
		data = f.read()
	sections, shdrs = read_sections(data)

	stub_text = sections.get('.sceStub.text')
	lib_stub = sections.get('.lib.stub')
	if not stub_text:
		print('%s: no .sceStub.text, nothing imported' % path)
		return True
	if not lib_stub:
		print('%s: .sceStub.text present but no .lib.stub!' % path)
		return False

	text_addr, text_size = stub_text[3], stub_text[5]
	covered = [None] * (text_size // 8)

	off, end = lib_stub[4], lib_stub[4] + lib_stub[5]
	libs = 0
	while off < end:
		libname, version, attr, entsize, varcount, stubcount, nidtable, stubtable = struct.unpack_from('<IHHBBHII', data, off)
		if entsize < 5:
			print('%s: bad .lib.stub entry size %d' % (path, entsize))
			return False
		name = vaddr_to_str(data, sections, libname)
		for i in range(stubcount):
			slot = (stubtable + i * 8 - text_addr) // 8
			if 0 <= slot < len(covered):
				covered[slot] = name
		libs += 1
		off += entsize * 4

	syms = read_symbols(data, shdrs)
	missing = [i for i, c in enumerate(covered) if c is None]
	for i in missing:
		addr = text_addr + i * 8
		print('%s: UNRESOLVED import stub at 0x%08x %s' % (path, addr, syms.get(addr, '')))

	print('%s: %d libraries, %d stubs, %d unresolved' % (path, libs, len(covered), len(missing)))
	return not missing

if __name__ == '__main__':
	if len(sys.argv) < 2:
		print(__doc__ if __doc__ else 'usage: check_stubs.py <elf>...')
		sys.exit(2)
	ok = True
	for p in sys.argv[1:]:
		ok = check(p) and ok
	sys.exit(0 if ok else 1)
