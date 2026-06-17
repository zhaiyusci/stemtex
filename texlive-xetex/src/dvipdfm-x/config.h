/* Minimal standalone config for building xdvipdfmx from ordinary C sources. */
#ifndef DVIPDFMX_STANDALONE_CONFIG_H
#define DVIPDFMX_STANDALONE_CONFIG_H

#define VERSION "20260330"
#define HAVE_LIBPAPER 1
#define HAVE_LIBPNG 1
#define HAVE_PNG_H 1
#define HAVE_ZLIB 1
#define HAVE_ZLIB_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
/* MSVC standalone build does not provide unistd.h. */
/* #undef HAVE_UNISTD_H */

#endif
