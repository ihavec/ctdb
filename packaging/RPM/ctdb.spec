%define initdir %{_sysconfdir}/init.d

Summary: Clustered TDB
Vendor: Samba Team
Packager: Samba Team <samba@samba.org>
Name: ctdb
Version: 1.0
Release: 2
Epoch: 0
License: GNU GPL version 2
Group: System Environment/Daemons
URL: http://ctdb.samba.org/

Source: ctdb-%{version}.tar.bz2

Prereq: /sbin/chkconfig /bin/mktemp /usr/bin/killall
Prereq: fileutils sed /etc/init.d

Provides: ctdb = %{version}

Prefix: /usr
BuildRoot: %{_tmppath}/%{name}-%{version}-root

%description
ctdb is the clustered database used by samba


#######################################################################

%prep
%setup -q
# setup the init script and sysconfig file
%setup -T -D -n ctdb-%{version} -q

%build

CC="gcc"

## always run autogen.sh
./autogen.sh

CFLAGS="$RPM_OPT_FLAGS $EXTRA -D_GNU_SOURCE" ./configure \
	--prefix=%{_prefix} \
	--sysconfdir=%{_sysconfdir} \
	--localstatedir="/var"

make showflags
make   

%install
# Clean up in case there is trash left from a previous build
rm -rf $RPM_BUILD_ROOT

# Create the target build directory hierarchy
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}/sysconfig
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}/init.d

make DESTDIR=$RPM_BUILD_ROOT install

install -m644 config/ctdb.sysconfig $RPM_BUILD_ROOT%{_sysconfdir}/sysconfig/ctdb
install -m755 config/ctdb.init $RPM_BUILD_ROOT%{initdir}/ctdb

# Remove "*.old" files
find $RPM_BUILD_ROOT -name "*.old" -exec rm -f {} \;

%clean
rm -rf $RPM_BUILD_ROOT

%post
[ -x /sbin/chkconfig ] && /sbin/chkconfig --add ctdb

%preun
if [ $1 = 0 ] ; then
    [ -x /sbin/chkconfig ] && /sbin/chkconfig --del ctdb
fi
exit 0

%postun
if [ "$1" -ge "1" ]; then
	%{initdir}/ctdb restart >/dev/null 2>&1
fi	


#######################################################################
## Files section                                                     ##
#######################################################################

%files
%defattr(-,root,root)

%config(noreplace) %{_sysconfdir}/sysconfig/ctdb
%attr(755,root,root) %config %{initdir}/ctdb

%{_sysconfdir}/ctdb/events
%{_sysconfdir}/ctdb/functions
%{_sysconfdir}/ctdb/events.d/10.interface
%{_sysconfdir}/ctdb/events.d/40.vsftpd
%{_sysconfdir}/ctdb/events.d/50.samba
%{_sysconfdir}/ctdb/events.d/59.nfslock
%{_sysconfdir}/ctdb/events.d/60.nfs
%{_sysconfdir}/ctdb/statd-callout
%{_sbindir}/ctdbd
%{_bindir}/ctdb
%{_bindir}/onnode.ssh
%{_bindir}/onnode.rsh
%{_bindir}/onnode
%{_includedir}/ctdb.h
%{_includedir}/ctdb_private.h
