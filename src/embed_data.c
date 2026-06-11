// Provides symbol definitions for embedded compressed data.
// The generated headers define the actual const arrays; this file
// pulls them in so the extern declarations in sharun.h can link.

#if SHARUN_LIB4BIN
#  if __has_include("lib4bin_embedded.h")
#    include "lib4bin_embedded.h"
#  endif
#endif

