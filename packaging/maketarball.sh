#!/bin/sh
#
# maketarball.sh - create a tarball from the git branch HEAD
#
# Copyright (C) Michael Adam 2009
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 3 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, see <http://www.gnu.org/licenses/>.
#

#
# Create CTDB source tarball of the current git branch HEAD.
# The version is extracted from the spec file...
# The first extra argument will be added as an additional version.
#

DIRNAME=$(dirname $0)
TOPDIR=${DIRNAME}/..
RPMDIR=${DIRNAME}/RPM
SPECFILE=${RPMDIR}/ctdb.spec
SPECFILE_IN=${SPECFILE}.in

EXTRA_SUFFIX="$1"

GITHASH=".$(git log --pretty=format:%h -1)"

if test "x$USE_GITHASH" = "xno" ; then
	GITHASH=""
fi

sed -e s/GITHASH/${GITHASH}/g \
	< ${SPECFILE_IN} \
	> ${SPECFILE}

VERSION=$(grep ^Version ${SPECFILE} | sed -e 's/^Version:\ \+//')${GITHASH}

if [ "x${EXTRA_SUFFIX}" != "x" ]; then
	VERSION="${VERSION}-${EXTRA_SUFFIX}"
fi

if echo | gzip -c --rsyncable - > /dev/null 2>&1 ; then
	GZIP="gzip -9 --rsyncable"
else
	GZIP="gzip -9"
fi

TAR_PREFIX="ctdb-${VERSION}"
TAR_BASE="ctdb-${VERSION}"

TAR_BALL=${TAR_BASE}.tar
TAR_GZ_BALL=${TAR_BALL}.gz

pushd ${TOPDIR}
echo "Creating ${TAR_BASE}.tar.gz ... "
git archive --prefix=${TAR_PREFIX}/ HEAD | ( cd /tmp ; tar xf - )
RC=$?
popd
if [ $RC -ne 0 ]; then
	echo "Error calling git archive."
	exit 1
fi

pushd /tmp/${TAR_PREFIX}
./autogen.sh
RC=$?
popd
if [ $RC -ne 0 ]; then
	echo "Error calling autogen.sh."
	exit 1
fi

pushd /tmp
tar cf ${TAR_BALL} ${TAR_PREFIX}
RC=$?
if [ $RC -ne 0 ]; then
	popd
        echo "Creation of tarball failed."
        exit 1
fi

${GZIP} ${TAR_BALL}
RC=$?
if [ $RC -ne 0 ]; then
	popd
        echo "Zipping tarball failed."
        exit 1
fi

rm -rf ${TAR_PREFIX}

popd

mv /tmp/${TAR_GZ_BALL} .

echo "Done."
exit 0
