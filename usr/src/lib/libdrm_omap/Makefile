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
# Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
#

# include library definitions
include ../Makefile.lib

include $(SRC)/common/libdrm/Makefile.drm

#SUBDIRS = $(MACH)
$(BUILD64)SUBDIRS += $(MACH64)

all :=		TARGET= all
clean :=	TARGET= clean
clobber :=	TARGET= clobber
install :=	TARGET= install
lint :=		TARGET= lint

DRM_HDRS= \
	omap_drmif.h

OMAP_HDRS= \
	omap_drm.h

ROOT_DRM_HDRS=		$(DRM_HDRS:%=$(ROOTHDRDIR)/libdrm/%)
ROOT_OMAP_HDRS=		$(OMAP_HDRS:%=$(ROOTHDRDIR)/omap/%)

.KEEP_STATE:

install: all .WAIT $(SUBDIRS)

all clean clobber lint: $(SUBDIRS)

install_h: $(ROOT_DRM_HDRS) $(ROOT_OMAP_HDRS)

# need to mkdir...
$(ROOT_OMAP_HDRS) : $(ROOTHDRDIR)/omap
$(ROOTHDRDIR)/omap :
	$(INS.dir) $@

check:	$(CHECKHDRS)

_msg: # no msgfiles here

$(POFILE):      pofile_MSGFILES

# install rules for install_h target
$(ROOTHDRDIR)/libdrm/%: $(LIBDRM_CMN_DIR)/omap/%
	$(INS.file)

$(ROOTHDRDIR)/omap/%: $(LIBDRM_CMN_DIR)/omap/%
	$(INS.file)

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(TARGET)

FRC:

include ../Makefile.targ
