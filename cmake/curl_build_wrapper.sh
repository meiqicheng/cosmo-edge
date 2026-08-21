#!/bin/sh
# Removes host include dirs recorded by curl's configure-time checks from
# generated flags.make files (host glibc >= 2.36 stdio.h breaks gcc 9.2),
# then patches curl_config.h with capabilities the ARM GNU glibc sysroot is
# known to provide (the configure-time probes were run before the host
# include pollution was stripped, so they under-report these).
# Invoked as the curl ExternalProject BUILD_COMMAND wrapper.
set -e
build_dir="$1"

find "$build_dir" -name flags.make -exec sed -i 's| -I/usr/include||g; s| -I/usr/local/include||g' {} +

# Patch under-reported sysroot capabilities into curl_config.h.
config_h="$build_dir/lib/curl_config.h"
if [ -f "$config_h" ]; then
    sed -i \
        -e 's|/\* #undef HAVE_UNISTD_H \*/|#define HAVE_UNISTD_H 1|' \
        -e 's|/\* #undef HAVE_STRUCT_SOCKADDR_STORAGE \*/|#define HAVE_STRUCT_SOCKADDR_STORAGE 1|' \
        -e 's|/\* #undef HAVE_SOCKLEN_T \*/|#define HAVE_SOCKLEN_T 1|' \
        -e 's|/\* #undef HAVE_NETINET_IN_H \*/|#define HAVE_NETINET_IN_H 1|' \
        -e 's|/\* #undef HAVE_SYS_SOCKET_H \*/|#define HAVE_SYS_SOCKET_H 1|' \
        -e 's|/\* #undef HAVE_NETINET_TCP_H \*/|#define HAVE_NETINET_TCP_H 1|' \
        -e 's|/\* #undef HAVE_ARPA_INET_H \*/|#define HAVE_ARPA_INET_H 1|' \
        "$config_h"
fi

exec make -C "$build_dir"

