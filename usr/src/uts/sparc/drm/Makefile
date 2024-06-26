#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
# Copyright 2021 Klaus Ziegler
#

#
#	This makefile drives the production of the DRM (Direct Rendering
#	Manager) common misc module.
#
#	Path to the base of the uts directory tree (usually /usr/src/uts).
#
UTSBASE	= ../..

#
#	Define the module and object file sets.
#
MODULE		= drm
OBJECTS		= $(DRM_OBJS:%=$(OBJS_DIR)/%)
LINTS		= $(DRM_OBJS:%.o=$(LINTS_DIR)/%.ln)
ROOTMODULE	= $(ROOT_MISC_DIR)/$(MODULE)

CMN_DRM		= $(UTSBASE)/common/drm

# Includes
INC_PATH	+= -I$(CMN_DRM)

# Dependency SPARC does not jet has a gfx_private
# hashed out - notice from Randy fishel.
# LDFLAGS		+= -dy -Nmisc/gfx_private

#
#	Include common rules.
#
include $(UTSBASE)/sparc/Makefile.sparc
include $(UTSBASE)/common/io/drm/Makefile.mod

CERRWARN	+= -_gcc=-Wno-parentheses
CERRWARN	+= -_gcc=-Wno-unused-function
CERRWARN	+= -_gcc=-Wno-unused-variable

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

$(OBJS_DIR)/%.o:		$(UTSBASE)/common/io/drm/%.c
	$(COMPILE.c) -o $@ $<
	$(CTFCONVERT_O)

$(LINTS_DIR)/%.ln:		$(UTSBASE)/common/io/drm/%.c
	@($(LHEAD) $(LINT.c) $< $(LTAIL))

#
#	Include common targets.
#
include $(UTSBASE)/sparc/Makefile.targ
