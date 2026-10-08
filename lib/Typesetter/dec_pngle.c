/* pngle streams a PNG. Inflate comes from lib/Miniz, which already links miniz. */
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define PNGLE_NO_GAMMA_CORRECTION
#include "pngle.c"
