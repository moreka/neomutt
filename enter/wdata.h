/**
 * @file
 * Enter Window Data
 *
 * @authors
 * Copyright (C) 2022 Richard Russon <rich@flatcap.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MUTT_ENTER_WDATA_H
#define MUTT_ENTER_WDATA_H

#include "config.h"
#include <stdbool.h>
#include <wchar.h>
#include <wctype.h>
#include "mutt.h"
#include "history/lib.h"

struct MuttWindow;

/**
 * enum EnterRedrawFlags - Redraw flags for mutt_enter_string_full()
 */
enum EnterRedrawFlags
{
  ENTER_REDRAW_NONE = 0, ///< Nothing to redraw
  ENTER_REDRAW_INIT,     ///< Go to end of line and redraw
  ENTER_REDRAW_LINE,     ///< Redraw entire line
};

/**
 * struct EnterWindowData - Data to fill the Enter Window
 */
struct EnterWindowData
{
  // Function parameters
  char *buf;                      ///< XXX
  size_t buflen;                  ///< XXX
  int col;                        ///< XXX
  CompletionFlags flags;          ///< XXX
  bool multiple;                  ///< XXX
  struct Mailbox *m;              ///< XXX
  char ***files;                  ///< XXX
  int *numfiles;                  ///< XXX
  struct EnterState *state;       ///< XXX

  // Local variables
  enum EnterRedrawFlags redraw;   ///< XXX
  bool pass;                      ///< XXX
  bool first;                     ///< XXX
  enum HistoryClass hclass;       ///< XXX
  wchar_t *tempbuf;               ///< XXX
  size_t templen;                 ///< XXX
  mbstate_t *mbstate;             ///< XXX
};

#endif /* MUTT_ENTER_WDATA_H */
