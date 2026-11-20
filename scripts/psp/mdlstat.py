#!/usr/bin/env python3
# mdlstat.py -- print the size of GoldSrc studio models (v10): bones, triangles, textures
# usage: mdlstat.py file.mdl [...]
import struct
import sys


def tris_of(data, triindex):
	# tricmds: count (negative = fan), then count * 4 shorts, until 0
	n = 0
	off = triindex
	while True:
		(c,) = struct.unpack_from('<h', data, off)
		off += 2
		if c == 0:
			return n
		c = abs(c)
		n += c - 2
		off += c * 8


def stat(path):
	data = open(path, 'rb').read()
	ident, version = struct.unpack_from('<4si', data, 0)
	if ident != b'IDST' or version != 10:
		return '%s: not a v10 studio model (%r %d)' % (path, ident, version)
	(numbones, boneindex, numbonecontrollers, bonecontrollerindex, numhitboxes, hitboxindex,
		numseq, seqindex, numseqgroups, seqgroupindex, numtextures, textureindex, texturedataindex,
		numskinref, numskinfamilies, skinindex, numbodyparts, bodypartindex, numattachments,
		attachmentindex) = struct.unpack_from('<20i', data, 0x8C)
	tris = 0
	verts = 0
	worst = 0
	for b in range(numbodyparts):
		name, nummodels, base, modelindex = struct.unpack_from('<64siii', data, bodypartindex + b * 76)
		best = 0
		for m in range(nummodels):
			mo = modelindex + m * 112
			(nummesh, meshindex, numverts) = struct.unpack_from('<iii', data, mo + 64 + 8)
			t = 0
			for k in range(nummesh):
				numtris, triindex = struct.unpack_from('<ii', data, meshindex + k * 20)
				t += tris_of(data, triindex)
			best = max(best, t)
			if m == 0:
				verts += numverts
		tris += best
	texbytes = 0
	texsizes = []
	for t in range(numtextures):
		name, flags, w, h, index = struct.unpack_from('<64siiii', data, textureindex + t * 80)
		texbytes += w * h
		texsizes.append('%dx%d' % (w, h))
	return '%-28s bones %3d seq %3d tris %5d (worst body combo) verts %4d tex %2d (%d KB: %s) attach %d ctrl %d' % (
		path.replace('\\', '/').split('/')[-1], numbones, numseq, tris, verts, numtextures, texbytes // 1024,
		' '.join(texsizes[:6]) + (' ...' if len(texsizes) > 6 else ''), numattachments, numbonecontrollers)


for p in sys.argv[1:]:
	try:
		print(stat(p))
	except Exception as e:
		print('%s: %s' % (p, e))
