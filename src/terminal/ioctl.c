#include "../../build/feature.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "ioctl.h"
#include "util/log.h"

#if HAVE_TIOCGWINSZ || HAVE_TIOCNOTTY
# include <sys/ioctl.h>
#endif

bool term_drop_controlling_tty(void)
{
#if HAVE_TIOCNOTTY
    if (ioctl(STDIN_FILENO, TIOCNOTTY) == -1) {
        LOG_WARNING("TIOCNOTTY ioctl failed: %s", strerror(errno));
        return false;
    }
    return true;
#else
    // errno = ENOSYS;
    return false;
#endif
}

#if HAVE_TIOCGWINSZ

bool term_get_size(unsigned int *w, unsigned int *h)
{
    struct winsize ws;
    if (unlikely(ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1)) {
        LOG_ERROR("TIOCGWINSZ ioctl failed: %s", strerror(errno));
        return false;
    }
    *w = ws.ws_col;
    *h = ws.ws_row;
    return true;
}

#elif HAVE_TCGETWINSIZE

#include <termios.h>

bool term_get_size(unsigned int *w, unsigned int *h)
{
    struct winsize ws;
    if (unlikely(tcgetwinsize(STDIN_FILENO, &ws) != 0)) {
        LOG_ERROR("tcgetwinsize() failed: %s", strerror(errno));
        return false;
    }
    *w = ws.ws_col;
    *h = ws.ws_row;
    return true;
}

#else

bool term_get_size(unsigned int* UNUSED_ARG(w), unsigned int* UNUSED_ARG(h))
{
    return false;
}

#endif
