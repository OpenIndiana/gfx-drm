#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2016 Gordon W. Ross
#

include ../Makefile.cmd

# Note: misc[12] dirs are tests from the top level (no subdirs).
# The others correspond to dirs under common/libdrm/libdrm-*/tests
#
# Not currently supported: amdgpu nouveau

SUBDIRS = misc1 misc2 util kms modeprint proptest modetest vbltest \
	kmstest radeon exynos tegra

ROOTCMDDIR=$(ROOT)/opt/drm-tests

all:=		TARGET= all
install:=	TARGET= install
clean:=		TARGET= clean
clobber:=	TARGET= clobber
lint:=		TARGET= lint
catalog:=       TARGET= catalog

SCRIPT=	Run_all.sh
ROOTCMD64= $(SCRIPT:%=$(ROOTCMDDIR)/%)

all clean clobber lint: $(SUBDIRS)
install: rootdirs .WAIT rootcmd $(SUBDIRS)

# These indirect names (rootdirs, rootcmd) are just so
# you can make those targets separately from subdirs.
rootdirs: $(ROOTCMDDIR64)
rootcmd: $(ROOTCMD64)

$(ROOTCMD) := FILEMODE= 755
$(ROOTCMDDIR64)/%.sh: %.sh
	$(INS.file)

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(TARGET)

FRC:

include ../Makefile.targ
