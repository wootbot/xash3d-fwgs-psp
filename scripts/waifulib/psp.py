# encoding: utf-8
# psp.py -- PSP EBOOT task
# Copyright (C) 2023 Sergey Galushko
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

from waflib import Task, TaskGen

# the PSP has no dynamic linker: these libraries are linked into the engine
# as relocatable objects (see xshlib.py), only game libraries are PRX modules
PSP_STATIC_LIBS = ['filesystem_stdio', 'menu']
PSP_RENDERERS = {
	'NULL': ['ref_null'],
	'SW':   ['ref_soft'],
	'HW':   ['ref_gu'],
	'ALL':  ['ref_soft', 'ref_gu'],
}

def configure(conf):
	static_libs = PSP_STATIC_LIBS + PSP_RENDERERS[conf.env.PSP_RENDER_TYPE]

	# what xshlib.configure does with --static-linking, it already ran before xcompile set the toolchain up
	conf.find_program('psp-ld', var='LD')
	conf.find_program('psp-objcopy', var='OBJCOPY')
	conf.env.STATIC_LINKING = ','.join(static_libs)

	# EBOOT.PBP generation parameters
	conf.env.PSP_EBOOT_TITLE = 'Xash3D FWGS'
	conf.env.PSP_EBOOT_ICON = 'NULL'
	conf.env.PSP_EBOOT_ICON1 = 'NULL'
	conf.env.PSP_EBOOT_UNKPNG = 'NULL'
	conf.env.PSP_EBOOT_PIC1 = 'NULL'
	conf.env.PSP_EBOOT_SND0 = 'NULL'
	conf.env.PSP_EBOOT_PSAR = 'NULL'

	conf.msg('PSP static libraries', conf.env.STATIC_LINKING)

##################################
#            PSP Tools           #
##################################
class psp_fixup(Task.Task):
	run_str = '${FIXUP} -o ${TGT} ${SRC}'
	color   = 'BLUE'

class psp_prxgen(Task.Task):
	run_str = '${PRXGEN} ${SRC} ${TGT}'
	color   = 'BLUE'

class psp_strip(Task.Task):
	run_str = '${STRIP} -o ${TGT} ${SRC}'
	color   = 'BLUE'

class psp_mksfo(Task.Task):
	# MEMSIZE=1 unlocks the extra memory of the 64 MB models
	run_str = '${MKSFO} -d MEMSIZE=1 ${PSP_EBOOT_TITLE} ${TGT}'
	color   = 'YELLOW'

	def scan(self):
		return ([], [self.env.PSP_EBOOT_TITLE])

class psp_packpbp(Task.Task):
	run_str = '${PACK_PBP} ${TGT} ${SRC[1].abspath()} ${PSP_EBOOT_ICON} ${PSP_EBOOT_ICON1} ${PSP_EBOOT_UNKPNG} ${PSP_EBOOT_PIC1} ${PSP_EBOOT_SND0} ${SRC[0].abspath()} ${PSP_EBOOT_PSAR}'
	color   = 'GREEN'

def is_psp_build(self):
	return self.env.DEST_OS == 'psp' and self.bld.__class__.__name__ != 'ConfigurationContext' and getattr(self, 'link_task', None)

@TaskGen.feature('cshlib', 'cxxshlib')
@TaskGen.after_method('apply_link')
def build_module(self):
	if not is_psp_build(self):
		return

	# statically linked libraries are turned into relocatable objects by xshlib
	if self.name in (self.env.STATIC_LINKING or '').split(','):
		return

	link_output = self.link_task.outputs[0]
	fixup_output = self.path.find_or_declare(link_output.name + '_fixup')
	prxgen_output = self.path.find_or_declare(link_output.change_ext('.prx').name)

	self.create_task('psp_fixup', src=link_output, tgt=fixup_output)
	self.create_task('psp_prxgen', src=fixup_output, tgt=prxgen_output)

	if getattr(self, 'install_task', None):
		self.install_task.inputs = [prxgen_output]

@TaskGen.feature('cprogram', 'cxxprogram', 'cprogram_static', 'cxxprogram_static')
@TaskGen.after_method('apply_link')
def build_eboot(self):
	if not is_psp_build(self) or self.target != 'xash':
		return

	finalobj_ext = '.elf'
	finalobj_tool = 'psp_strip'
	if self.env.PSP_BUILD_PRX:
		finalobj_ext = '.prx'
		finalobj_tool = 'psp_prxgen'

	link_output = self.link_task.outputs[0]
	fixup_output = self.path.find_or_declare(link_output.name + '_fixup')
	finalobj_output = self.path.find_or_declare(link_output.name + finalobj_ext)

	mksfo_output = self.path.find_or_declare('PARAM.SFO')
	packpbp_output = self.path.find_or_declare('EBOOT.PBP')

	self.create_task('psp_fixup', src=link_output, tgt=fixup_output)
	self.create_task(finalobj_tool, src=fixup_output, tgt=finalobj_output)
	self.create_task('psp_mksfo', tgt=mksfo_output)
	self.create_task('psp_packpbp', src=[finalobj_output, mksfo_output], tgt=packpbp_output)

	if getattr(self, 'install_task', None):
		self.install_task.inputs = [packpbp_output, finalobj_output]
