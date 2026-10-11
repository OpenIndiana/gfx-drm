#
# Copyright (c) 2026 Toasty.  All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
#

#
#	virtio-gpu DRM driver for illumos.
#
UTSBASE	= ../..

#
#	Define the module and object file sets.
#
MODULE		= virtio_gpu
OBJECTS		= $(VIRTIO_GPU_OBJS:%=$(OBJS_DIR)/%)
LINTS		= $(VIRTIO_GPU_OBJS:%.o=$(LINTS_DIR)/%.ln)
ROOTMODULE	= $(ROOT_DRV_DIR)/$(MODULE)

CMN_DRM		= $(UTSBASE)/common/drm

# Includes -- DRM headers, virtio framework, driver source
INC_PATH	+= -I$(CMN_DRM)
INC_PATH	+= -I$(UTSBASE)/common/io/virtio
INC_PATH	+= -I$(UTSBASE)/common/io/virtio_gpu

# Dependencies -- DRM misc module and virtio framework
LDFLAGS		+= -dy -Nmisc/drm -Nmisc/virtio

#
#	Include common rules.
#
include $(UTSBASE)/intel/Makefile.intel
include $(UTSBASE)/common/io/virtio_gpu/Makefile.mod

CERRWARN	+= -_gcc=-Wno-parentheses
CERRWARN	+= -_gcc=-Wno-unused-function
CERRWARN	+= -_gcc=-Wno-unused-variable
CERRWARN	+= -_gcc=-Wno-unused-but-set-variable

#
#	Define targets
#
ALL_TARGET	= $(BINARY)
LINT_TARGET	= $(MODULE).lint
INSTALL_TARGET	= $(BINARY) $(ROOTMODULE)

#
#	Default build targets.
#
.KEEP_STATE:

def:		$(DEF_DEPS)

all:		$(ALL_DEPS)

clean:		$(CLEAN_DEPS)

clobber:	$(CLOBBER_DEPS)

lint:		$(LINT_DEPS)

modlintlib:	$(MODLINTLIB_DEPS)

clean.lint:	$(CLEAN_LINT_DEPS)

install:	$(INSTALL_DEPS)

$(OBJS_DIR)/%.o:		$(UTSBASE)/common/io/virtio_gpu/%.c
	$(COMPILE.c) -o $@ $<
	$(CTFCONVERT_O)

$(LINTS_DIR)/%.ln:		$(UTSBASE)/common/io/virtio_gpu/%.c
	@($(LHEAD) $(LINT.c) $< $(LTAIL))

#
#	Include common targets.
#
include $(UTSBASE)/intel/Makefile.targ
