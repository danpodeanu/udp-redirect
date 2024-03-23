#ifndef __DEBUG_H

#define __DEBUG_H

#include <time.h>

/**
 * @brief The available debug levels.
 */
enum DEBUG_LEVEL {
    DEBUG_LEVEL_ERROR = 0,      ///< Error messages
    DEBUG_LEVEL_INFO = 1,       ///< Informational messages
    DEBUG_LEVEL_VERBOSE = 2,    ///< Verbose messages
    DEBUG_LEVEL_DEBUG = 3       ///< Debug messages
};

/**
 * Standard debug macro requiring a locally defined debug level.
 * Adapted from the excellent https://github.com/jleffler/soq/blob/master/src/libsoq/debug.h
 *
 * Note that __VA_ARGS__ is a GCC extension; used for convenience to support DEBUG without format arguments.
 */
#define DEBUG(lvl, fmt, ...) \
        do { \
            if ((debug_level) >= (lvl)) { \
                fprintf(stderr, "%s:%d:%d:%s(): " fmt "\n", __FILE__, \
                    __LINE__, (int)(time(NULL)), __func__, ##__VA_ARGS__); \
            } \
        } while (0)

#endif
