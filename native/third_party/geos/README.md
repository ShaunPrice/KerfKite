# GEOS

The native core uses GEOS 3.14.1 for its polygon operations. GEOS is free software under the GNU Lesser
General Public License, version 2.1; `COPYING` in this directory is that licence, copied from the
GEOS 3.14.1 release.

GEOS's source is not kept in this repository. The build fetches the release tarball and checks its
SHA-256 (`native/cmake/geos.cmake`):

    https://github.com/libgeos/geos/releases/download/3.14.1/geos-3.14.1.tar.bz2
    https://download.osgeo.org/geos/geos-3.14.1.tar.bz2
    SHA-256 3c20919cda9a505db07b5216baa980bacdaa0702da715b43f176fb07eff7e716

Copyright in GEOS belongs to its authors; see the `AUTHORS` file and the source headers of the release.
What distributing a program that contains GEOS requires is described in `docs/LICENSING.md`.
