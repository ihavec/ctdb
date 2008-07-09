#!/bin/sh
# Copyright (C) John H Terpstra 1998-2002
#               Gerald (Jerry) Carter 2003
#		Jim McDonough 2007
#		Andrew Tridgell 2007
#		Ronnie Sahlberg 2008

# The following allows environment variables to override the target directories
#   the alternative is to have a file in your home directory calles .rpmmacros
#   containing the following:
#   %_topdir  /home/mylogin/redhat
#
# Note: Under this directory rpm expects to find the same directories that are under the
#   /usr/src/redhat directory
#

EXTRA_OPTIONS="$1"

[ -d packaging ] || {
    echo "Must run this from the ctdb directory"
    exit 1
}


SPECDIR=`rpm --eval %_specdir`
SRCDIR=`rpm --eval %_sourcedir`

# At this point the SPECDIR and SRCDIR vaiables must have a value!

VERSION='1.0'
REVISION=''
RPMBUILD="rpmbuild"

echo -n "Creating ctdb-${VERSION}.tar.gz ... "
git archive --prefix=ctdb-${VERSION}/ HEAD | gzip -9 --rsyncable > ${SRCDIR}/ctdb-${VERSION}.tar.gz
echo "Done."
if [ $? -ne 0 ]; then
        echo "Build failed!"
        exit 1
fi


##
## copy additional source files
##
cp -p packaging/RPM/ctdb.spec ${SPECDIR}

##
## Build
##
echo "$(basename $0): Getting Ready to build release packages"
cd ${SPECDIR}

echo "building ctdb package"
SPECFILE="ctdb.spec"
${RPMBUILD} -ba $EXTRA_OPTIONS $SPECFILE || exit 1

echo "building ctdb-dev package"
SPECFILE="ctdb-dev.spec"
${RPMBUILD} -bb --clean --rmsource $EXTRA_OPTIONS $SPECFILE || exit 1

echo "$(basename $0): Done."

exit 0
