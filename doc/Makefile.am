XSLTPROC = /usr/bin/xsltproc

EXTRA_DIST = fuse-nfs.1 fuse-nfs.xml

# Manpages
man1_MANS = fuse-nfs.1

fuse-nfs.1:fuse-nfs.xml
	-test -z "$(XSLTPROC)" || $(XSLTPROC) -o fuse-nfs.1 http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl fuse-nfs.xml
