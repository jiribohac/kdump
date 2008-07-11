/*
 * (c) 2008, Bernhard Walle <bwalle@suse.de>, SUSE LINUX Products GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#include "progress.h"

//{{{ Terminal -----------------------------------------------------------------

// -----------------------------------------------------------------------------
Terminal::Size Terminal::getSize() const
    throw ()
{
    Terminal::Size sz;
    struct winsize winsize;

    int err = ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsize);
    if (err != 0) {
        sz.width = 0;
        sz.height = 0;
        return sz;
    }

    sz.width = winsize.ws_col;
    sz.height = winsize.ws_row;

    return sz;
}

//}}}

// vim: set sw=4 ts=4 fdm=marker et: :collapseFolds=1:
