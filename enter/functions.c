/**
 * @file
 * Enter functions
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

/**
 * @page enter_functions Enter functions
 *
 * Enter functions
 */

#include "config.h"
#include "alias/lib.h"
#include "gui/lib.h"
#include "functions.h"
#include "index/lib.h"
#include "menu/lib.h"
#include "pattern/lib.h"
#include "init.h"
#include "mutt_history.h"
#include "mutt_mailbox.h"
#include "muttlib.h"
#include "opcodes.h"
#include "protos.h"
#include "state.h"
#include "wdata.h"

struct EnterWindowData;

/* combining mark / non-spacing character */
#define COMB_CHAR(wc) (IsWPrint(wc) && !wcwidth(wc))

/**
 * replace_part - Search and replace on a buffer
 * @param state Current state of the input buffer
 * @param from  Starting point for the replacement
 * @param buf   Replacement string
 */
static void replace_part(struct EnterState *state, size_t from, const char *buf)
{
  /* Save the suffix */
  size_t savelen = state->lastchar - state->curpos;
  wchar_t *savebuf = NULL;

  if (savelen)
  {
    savebuf = mutt_mem_calloc(savelen, sizeof(wchar_t));
    memcpy(savebuf, state->wbuf + state->curpos, savelen * sizeof(wchar_t));
  }

  /* Convert to wide characters */
  state->curpos = mutt_mb_mbstowcs(&state->wbuf, &state->wbuflen, from, buf);

  if (savelen)
  {
    /* Make space for suffix */
    if (state->curpos + savelen > state->wbuflen)
    {
      state->wbuflen = state->curpos + savelen;
      mutt_mem_realloc(&state->wbuf, state->wbuflen * sizeof(wchar_t));
    }

    /* Restore suffix */
    memcpy(state->wbuf + state->curpos, savebuf, savelen * sizeof(wchar_t));
    FREE(&savebuf);
  }

  state->lastchar = state->curpos + savelen;
}

// -----------------------------------------------------------------------------

/**
 * op_editor_complete - complete filename or alias - Implements ::enter_function_t - @ingroup enter_function_api
 *
 * This function handles:
 * - OP_EDITOR_COMPLETE
 * - OP_EDITOR_COMPLETE_QUERY
 */
static int op_editor_complete(struct EnterWindowData *wdata, int op)
{
  wdata->state->tabs++;
  if (wdata->flags & MUTT_COMP_FILE_SIMPLE)
  {
    size_t i;
    for (i = wdata->state->curpos;
         (i > 0) && !mutt_mb_is_shell_char(wdata->state->wbuf[i - 1]); i--)
    {
    }
    mutt_mb_wcstombs(wdata->buf, wdata->buflen, wdata->state->wbuf + i,
                     wdata->state->curpos - i);
    if (wdata->tempbuf && (wdata->templen == (wdata->state->lastchar - i)) &&
        (memcmp(wdata->tempbuf, wdata->state->wbuf + i,
                (wdata->state->lastchar - i) * sizeof(wchar_t)) == 0))
    {
      mutt_select_file(wdata->buf, wdata->buflen, MUTT_SEL_NO_FLAGS, wdata->m, NULL, NULL);
      if (wdata->buf[0] != '\0')
        replace_part(wdata->state, i, wdata->buf);
      rc = 1;
      goto bye;
    }
    if (mutt_complete(wdata->buf, wdata->buflen) == 0)
    {
      wdata->templen = wdata->state->lastchar - i;
      mutt_mem_realloc(&wdata->tempbuf, wdata->templen * sizeof(wchar_t));
      memcpy(wdata->tempbuf, wdata->state->wbuf + i, wdata->templen * sizeof(wchar_t));
    }
    else
      mutt_beep(false);

    replace_part(wdata->state, i, wdata->buf);
  }
  else if ((wdata->flags & MUTT_COMP_ALIAS) && (op == OP_EDITOR_COMPLETE))
  {
    /* invoke the alias-menu to get more addresses */
    size_t i;
    for (i = wdata->state->curpos; (i > 0) && (wdata->state->wbuf[i - 1] != ',') &&
                                   (wdata->state->wbuf[i - 1] != ':');
         i--)
    {
    }
    for (; (i < wdata->state->lastchar) && (wdata->state->wbuf[i] == ' '); i++)
      ; // do nothing

    mutt_mb_wcstombs(wdata->buf, wdata->buflen, wdata->state->wbuf + i,
                     wdata->state->curpos - i);
    int rc2 = alias_complete(wdata->buf, wdata->buflen, NeoMutt->sub);
    replace_part(wdata->state, i, wdata->buf);
    if (rc2 != 1)
    {
      rc = 1;
      goto bye;
    }
    break;
  }
  else if ((wdata->flags & MUTT_COMP_LABEL) && (op == OP_EDITOR_COMPLETE))
  {
    size_t i;
    for (i = wdata->state->curpos; (i > 0) && (wdata->state->wbuf[i - 1] != ',') &&
                                   (wdata->state->wbuf[i - 1] != ':');
         i--)
    {
    }
    for (; (i < wdata->state->lastchar) && (wdata->state->wbuf[i] == ' '); i++)
      ; // do nothing

    mutt_mb_wcstombs(wdata->buf, wdata->buflen, wdata->state->wbuf + i,
                     wdata->state->curpos - i);
    int rc2 = mutt_label_complete(wdata->buf, wdata->buflen, wdata->state->tabs);
    replace_part(wdata->state, i, wdata->buf);
    if (rc2 != 1)
    {
      rc = 1;
      goto bye;
    }
    break;
  }
  else if ((wdata->flags & MUTT_COMP_PATTERN) && (op == OP_EDITOR_COMPLETE))
  {
    size_t i = wdata->state->curpos;
    if (i && (wdata->state->wbuf[i - 1] == '~'))
    {
      if (dlg_select_pattern(wdata->buf, wdata->buflen))
        replace_part(wdata->state, i - 1, wdata->buf);
      rc = 1;
      goto bye;
    }
    for (; (i > 0) && (wdata->state->wbuf[i - 1] != '~'); i--)
      ; // do nothing

    if ((i > 0) && (i < wdata->state->curpos) &&
        (wdata->state->wbuf[i - 1] == '~') && (wdata->state->wbuf[i] == 'y'))
    {
      i++;
      mutt_mb_wcstombs(wdata->buf, wdata->buflen, wdata->state->wbuf + i,
                       wdata->state->curpos - i);
      int rc2 = mutt_label_complete(wdata->buf, wdata->buflen, wdata->state->tabs);
      replace_part(wdata->state, i, wdata->buf);
      if (rc2 != 1)
      {
        rc = 1;
        goto bye;
      }
    }
    else
    {
      if (self_insert(wdata))
        goto bye;
    }
    break;
  }
  else if ((wdata->flags & MUTT_COMP_ALIAS) && (op == OP_EDITOR_COMPLETE_QUERY))
  {
    size_t i = wdata->state->curpos;
    if (i != 0)
    {
      for (; (i > 0) && (wdata->state->wbuf[i - 1] != ','); i--)
        ; // do nothing

      for (; (i < wdata->state->curpos) && (wdata->state->wbuf[i] == ' '); i++)
        ; // do nothing
    }

    mutt_mb_wcstombs(wdata->buf, wdata->buflen, wdata->state->wbuf + i,
                     wdata->state->curpos - i);
    struct Buffer *tmp = mutt_buffer_pool_get();
    mutt_buffer_strcpy(tmp, wdata->buf);
    query_complete(tmp, NeoMutt->sub);
    mutt_str_copy(wdata->buf, mutt_buffer_string(tmp), wdata->buflen);
    mutt_buffer_pool_release(&tmp);
    replace_part(wdata->state, i, wdata->buf);

    rc = 1;
    goto bye;
  }
  else if (wdata->flags & MUTT_COMP_COMMAND)
  {
    mutt_mb_wcstombs(wdata->buf, wdata->buflen, wdata->state->wbuf, wdata->state->curpos);
    size_t i = strlen(wdata->buf);
    if ((i != 0) && (wdata->buf[i - 1] == '=') &&
        (mutt_var_value_complete(wdata->buf, wdata->buflen, i) != 0))
    {
      wdata->state->tabs = 0;
    }
    else if (mutt_command_complete(wdata->buf, wdata->buflen, i, wdata->state->tabs) == 0)
      mutt_beep(false);
    replace_part(wdata->state, 0, wdata->buf);
  }
  else if (wdata->flags & (MUTT_COMP_FILE | MUTT_COMP_FILE_MBOX))
  {
    mutt_mb_wcstombs(wdata->buf, wdata->buflen, wdata->state->wbuf, wdata->state->curpos);

    /* see if the path has changed from the last time */
    if ((!wdata->tempbuf && !wdata->state->lastchar) ||
        (wdata->tempbuf && (wdata->templen == wdata->state->lastchar) &&
         (memcmp(wdata->tempbuf, wdata->state->wbuf,
                 wdata->state->lastchar * sizeof(wchar_t)) == 0)))
    {
      mutt_select_file(wdata->buf, wdata->buflen,
                       ((wdata->flags & MUTT_COMP_FILE_MBOX) ? MUTT_SEL_FOLDER : MUTT_SEL_NO_FLAGS) |
                           (wdata->multiple ? MUTT_SEL_MULTI : MUTT_SEL_NO_FLAGS),
                       wdata->m, wdata->files, wdata->numfiles);
      if (wdata->buf[0] != '\0')
      {
        mutt_pretty_mailbox(wdata->buf, wdata->buflen);
        if (!wdata->pass)
          mutt_hist_add(wdata->hclass, wdata->buf, true);
        rc = 0;
        goto bye;
      }

      /* file selection cancelled */
      rc = 1;
      goto bye;
    }

    if (mutt_complete(wdata->buf, wdata->buflen) == 0)
    {
      wdata->templen = wdata->state->lastchar;
      mutt_mem_realloc(&wdata->tempbuf, wdata->templen * sizeof(wchar_t));
      memcpy(wdata->tempbuf, wdata->state->wbuf, wdata->templen * sizeof(wchar_t));
    }
    else
      mutt_beep(false); /* let the user know that nothing matched */
    replace_part(wdata->state, 0, wdata->buf);
  }
#ifdef USE_NOTMUCH
  else if (wdata->flags & MUTT_COMP_NM_QUERY)
  {
    mutt_mb_wcstombs(wdata->buf, wdata->buflen, wdata->state->wbuf, wdata->state->curpos);
    size_t len = strlen(wdata->buf);
    if (!mutt_nm_query_complete(wdata->buf, wdata->buflen, len, wdata->state->tabs))
      mutt_beep(false);

    replace_part(wdata->state, 0, wdata->buf);
  }
  else if (wdata->flags & MUTT_COMP_NM_TAG)
  {
    mutt_mb_wcstombs(wdata->buf, wdata->buflen, wdata->state->wbuf, wdata->state->curpos);
    if (!mutt_nm_tag_complete(wdata->buf, wdata->buflen, wdata->state->tabs))
      mutt_beep(false);

    replace_part(wdata->state, 0, wdata->buf);
  }
#endif
  else
  {
    if (self_insert(wdata))
      goto bye;
  }
  return FR_SUCCESS;
}

/**
 * op_editor_history_down - scroll down through the history list - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_history_down(struct EnterWindowData *wdata, int op)
{
  wdata->state->curpos = wdata->state->lastchar;
  if (mutt_hist_at_scratch(wdata->hclass))
  {
    mutt_mb_wcstombs(wdata->buf, wdata->buflen, wdata->state->wbuf, wdata->state->curpos);
    mutt_hist_save_scratch(wdata->hclass, wdata->buf);
  }
  replace_part(wdata->state, 0, mutt_hist_next(wdata->hclass));
  wdata->redraw = ENTER_REDRAW_INIT;
  return FR_SUCCESS;
}

/**
 * op_editor_history_search - search through the history list - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_history_search(struct EnterWindowData *wdata, int op)
{
  wdata->state->curpos = wdata->state->lastchar;
  mutt_mb_wcstombs(wdata->buf, wdata->buflen, wdata->state->wbuf, wdata->state->curpos);
  mutt_hist_complete(wdata->buf, wdata->buflen, wdata->hclass);
  replace_part(wdata->state, 0, wdata->buf);
  rc = 1;
  goto bye;
  return FR_SUCCESS;
}

/**
 * op_editor_history_up - scroll up through the history list - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_history_up(struct EnterWindowData *wdata, int op)
{
  wdata->state->curpos = wdata->state->lastchar;
  if (mutt_hist_at_scratch(wdata->hclass))
  {
    mutt_mb_wcstombs(wdata->buf, wdata->buflen, wdata->state->wbuf, wdata->state->curpos);
    mutt_hist_save_scratch(wdata->hclass, wdata->buf);
  }
  replace_part(wdata->state, 0, mutt_hist_prev(wdata->hclass));
  wdata->redraw = ENTER_REDRAW_INIT;
  return FR_SUCCESS;
}

/**
 * op_editor_mailbox_cycle - cycle among incoming mailboxes - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_mailbox_cycle(struct EnterWindowData *wdata, int op)
{
  if (wdata->flags & MUTT_COMP_FILE_MBOX)
  {
    wdata->first = true; /* clear input if user types a real key later */
    mutt_mb_wcstombs(wdata->buf, wdata->buflen, wdata->state->wbuf, wdata->state->curpos);

    struct Buffer *pool = mutt_buffer_pool_get();
    mutt_buffer_addstr(pool, wdata->buf);
    mutt_mailbox_next(wdata->m, pool);
    mutt_str_copy(wdata->buf, mutt_buffer_string(pool), wdata->buflen);
    mutt_buffer_pool_release(&pool);

    wdata->state->curpos = wdata->state->lastchar =
        mutt_mb_mbstowcs(&wdata->state->wbuf, &wdata->state->wbuflen, 0, wdata->buf);
    break;
  }
  else if (!(wdata->flags & MUTT_COMP_FILE))
  {
    if (self_insert(wdata))
      goto bye;
    break;
  }
  /* fallthrough */
  //QWQ OP_EDITOR_COMPLETE:
  return FR_SUCCESS;
}

// -----------------------------------------------------------------------------

/**
 * op_editor_backspace - delete the char in front of the cursor - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_backspace(struct EnterWindowData *wdata, int op)
{
  if (wdata->state->curpos == 0)
  {
    // Pressing backspace when no text is in the command prompt should exit the prompt
    const bool c_abort_backspace =
        cs_subset_bool(NeoMutt->sub, "abort_backspace");
    if (c_abort_backspace && (wdata->state->lastchar == 0))
      goto bye;
    // Pressing backspace with text in the command prompt should just beep
    mutt_beep(false);
  }
  else
  {
    size_t i = wdata->state->curpos;
    while ((i > 0) && COMB_CHAR(wdata->state->wbuf[i - 1]))
      i--;
    if (i > 0)
      i--;
    memmove(wdata->state->wbuf + i, wdata->state->wbuf + wdata->state->curpos,
            (wdata->state->lastchar - wdata->state->curpos) * sizeof(wchar_t));
    wdata->state->lastchar -= wdata->state->curpos - i;
    wdata->state->curpos = i;
  }
  return FR_SUCCESS;
}

/**
 * op_editor_backward_char - move the cursor one character to the left - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_backward_char(struct EnterWindowData *wdata, int op)
{
  if (wdata->state->curpos == 0)
    mutt_beep(false);
  else
  {
    while (wdata->state->curpos && COMB_CHAR(wdata->state->wbuf[wdata->state->curpos - 1]))
      wdata->state->curpos--;
    if (wdata->state->curpos)
      wdata->state->curpos--;
  }
  return FR_SUCCESS;
}

/**
 * op_editor_backward_word - move the cursor to the beginning of the word - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_backward_word(struct EnterWindowData *wdata, int op)
{
  if (wdata->state->curpos == 0)
    mutt_beep(false);
  else
  {
    while (wdata->state->curpos && iswspace(wdata->state->wbuf[wdata->state->curpos - 1]))
      wdata->state->curpos--;
    while (wdata->state->curpos && !iswspace(wdata->state->wbuf[wdata->state->curpos - 1]))
      wdata->state->curpos--;
  }
  return FR_SUCCESS;
}

/**
 * op_editor_bol - jump to the beginning of the line - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_bol(struct EnterWindowData *wdata, int op)
{
  wdata->state->curpos = 0;
  return FR_SUCCESS;
}

/**
 * op_editor_capitalize_word - capitalize the word - Implements ::enter_function_t - @ingroup enter_function_api
 * This function handles:
 * - OP_EDITOR_CAPITALIZE_WORD
 * - OP_EDITOR_DOWNCASE_WORD
 * - OP_EDITOR_UPCASE_WORD
 */
static int op_editor_capitalize_word(struct EnterWindowData *wdata, int op)
{
  if (wdata->state->curpos == wdata->state->lastchar)
  {
    mutt_beep(false);
    break;
  }
  while (wdata->state->curpos && !iswspace(wdata->state->wbuf[wdata->state->curpos]))
    wdata->state->curpos--;
  while ((wdata->state->curpos < wdata->state->lastchar) &&
         iswspace(wdata->state->wbuf[wdata->state->curpos]))
    wdata->state->curpos++;
  while ((wdata->state->curpos < wdata->state->lastchar) &&
         !iswspace(wdata->state->wbuf[wdata->state->curpos]))
  {
    if (op == OP_EDITOR_DOWNCASE_WORD)
      wdata->state->wbuf[wdata->state->curpos] =
          towlower(wdata->state->wbuf[wdata->state->curpos]);
    else
    {
      wdata->state->wbuf[wdata->state->curpos] =
          towupper(wdata->state->wbuf[wdata->state->curpos]);
      if (op == OP_EDITOR_CAPITALIZE_WORD)
        op = OP_EDITOR_DOWNCASE_WORD;
    }
    wdata->state->curpos++;
  }
  return FR_SUCCESS;
}

/**
 * op_editor_delete_char - delete the char under the cursor - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_delete_char(struct EnterWindowData *wdata, int op)
{
  if (wdata->state->curpos == wdata->state->lastchar)
    mutt_beep(false);
  else
  {
    size_t i = wdata->state->curpos;
    while ((i < wdata->state->lastchar) && COMB_CHAR(wdata->state->wbuf[i]))
      i++;
    if (i < wdata->state->lastchar)
      i++;
    while ((i < wdata->state->lastchar) && COMB_CHAR(wdata->state->wbuf[i]))
      i++;
    memmove(wdata->state->wbuf + wdata->state->curpos, wdata->state->wbuf + i,
            (wdata->state->lastchar - i) * sizeof(wchar_t));
    wdata->state->lastchar -= i - wdata->state->curpos;
  }
  return FR_SUCCESS;
}

/**
 * op_editor_eol - jump to the end of the line - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_eol(struct EnterWindowData *wdata, int op)
{
  wdata->redraw = ENTER_REDRAW_INIT;
  return FR_SUCCESS;
}

/**
 * op_editor_forward_char - move the cursor one character to the right - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_forward_char(struct EnterWindowData *wdata, int op)
{
  if (wdata->state->curpos == wdata->state->lastchar)
    mutt_beep(false);
  else
  {
    wdata->state->curpos++;
    while ((wdata->state->curpos < wdata->state->lastchar) &&
           COMB_CHAR(wdata->state->wbuf[wdata->state->curpos]))
    {
      wdata->state->curpos++;
    }
  }
  return FR_SUCCESS;
}

/**
 * op_editor_forward_word - move the cursor to the end of the word - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_forward_word(struct EnterWindowData *wdata, int op)
{
  if (wdata->state->curpos == wdata->state->lastchar)
    mutt_beep(false);
  else
  {
    while ((wdata->state->curpos < wdata->state->lastchar) &&
           iswspace(wdata->state->wbuf[wdata->state->curpos]))
    {
      wdata->state->curpos++;
    }
    while ((wdata->state->curpos < wdata->state->lastchar) &&
           !iswspace(wdata->state->wbuf[wdata->state->curpos]))
    {
      wdata->state->curpos++;
    }
  }
  return FR_SUCCESS;
}

/**
 * op_editor_kill_eol - delete chars from cursor to end of line - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_kill_eol(struct EnterWindowData *wdata, int op)
{
  wdata->state->lastchar = wdata->state->curpos;
  return FR_SUCCESS;
}

/**
 * op_editor_kill_eow - delete chars from the cursor to the end of the word - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_kill_eow(struct EnterWindowData *wdata, int op)
{
  /* delete to end of word */

  /* wdata->first skip over whitespace */
  size_t i;
  for (i = wdata->state->curpos;
       (i < wdata->state->lastchar) && iswspace(wdata->state->wbuf[i]); i++)
    ; // do nothing

  /* if there are any characters left.. */
  if (i < wdata->state->lastchar)
  {
    /* if the current character is alphanumeric.. */
    if (iswalnum(wdata->state->wbuf[i]))
    {
      /* skip over the rest of the word consistent of only alphanumerics */
      for (; (i < wdata->state->lastchar) && iswalnum(wdata->state->wbuf[i]); i++)
        ; // do nothing
    }
    else
    {
      /* skip over one non-alphanumeric character */
      i++;
    }
  }

  memmove(wdata->state->wbuf + wdata->state->curpos, wdata->state->wbuf + i,
          (wdata->state->lastchar - i) * sizeof(wchar_t));
  wdata->state->lastchar += wdata->state->curpos - i;
  return FR_SUCCESS;
}

/**
 * op_editor_kill_line - delete all chars on the line - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_kill_line(struct EnterWindowData *wdata, int op)
{
  wdata->state->curpos = 0;
  wdata->state->lastchar = 0;
  return FR_SUCCESS;
}

/**
 * op_editor_kill_word - delete the word in front of the cursor - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_kill_word(struct EnterWindowData *wdata, int op)
{
  /* delete to beginning of word */
  if (wdata->state->curpos != 0)
  {
    size_t i = wdata->state->curpos;
    while (i && iswspace(wdata->state->wbuf[i - 1]))
      i--;
    if (i > 0)
    {
      if (iswalnum(wdata->state->wbuf[i - 1]))
      {
        for (--i; (i > 0) && iswalnum(wdata->state->wbuf[i - 1]); i--)
          ; // do nothing
      }
      else
        i--;
    }
    memmove(wdata->state->wbuf + i, wdata->state->wbuf + wdata->state->curpos,
            (wdata->state->lastchar - wdata->state->curpos) * sizeof(wchar_t));
    wdata->state->lastchar += i - wdata->state->curpos;
    wdata->state->curpos = i;
  }
  return FR_SUCCESS;
}

/**
 * op_editor_quote_char - quote the next typed key - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_quote_char(struct EnterWindowData *wdata, int op)
{
  struct KeyEvent event = { OP_NULL, OP_NULL };
  do
  {
    event = mutt_getch();
  } while (event.op == OP_TIMEOUT);
  if (event.op >= 0)
  {
    LastKey = event.op;
    if (self_insert(wdata))
      goto bye;
  }
  return FR_SUCCESS;
}

/**
 * op_editor_transpose_chars - transpose character under cursor with previous - Implements ::enter_function_t - @ingroup enter_function_api
 */
static int op_editor_transpose_chars(struct EnterWindowData *wdata, int op)
{
  if (wdata->state->lastchar < 2)
    mutt_beep(false);
  else
  {
    if (wdata->state->curpos == 0)
      wdata->state->curpos = 2;
    else if (wdata->state->curpos < wdata->state->lastchar)
      wdata->state->curpos++;

    wchar_t wc = wdata->state->wbuf[wdata->state->curpos - 2];
    wdata->state->wbuf[wdata->state->curpos - 2] =
        wdata->state->wbuf[wdata->state->curpos - 1];
    wdata->state->wbuf[wdata->state->curpos - 1] = wc;
  }
  return FR_SUCCESS;
}

// -----------------------------------------------------------------------------

/**
 * EnterFunctions - All the NeoMutt functions that Enter supports
 */
struct EnterFunction EnterFunctions[] = {
  // clang-format off
  { OP_EDITOR_BACKSPACE,          op_editor_backspace },
  { OP_EDITOR_BACKWARD_CHAR,      op_editor_backward_char },
  { OP_EDITOR_BACKWARD_WORD,      op_editor_backward_word },
  { OP_EDITOR_BOL,                op_editor_bol },
  { OP_EDITOR_CAPITALIZE_WORD,    op_editor_capitalize_word },
  { OP_EDITOR_COMPLETE,           op_editor_complete },
  { OP_EDITOR_COMPLETE_QUERY,     op_editor_complete },
  { OP_EDITOR_DELETE_CHAR,        op_editor_delete_char },
  { OP_EDITOR_DOWNCASE_WORD,      op_editor_capitalize_word },
  { OP_EDITOR_EOL,                op_editor_eol },
  { OP_EDITOR_FORWARD_CHAR,       op_editor_forward_char },
  { OP_EDITOR_FORWARD_WORD,       op_editor_forward_word },
  { OP_EDITOR_HISTORY_DOWN,       op_editor_history_down },
  { OP_EDITOR_HISTORY_SEARCH,     op_editor_history_search },
  { OP_EDITOR_HISTORY_UP,         op_editor_history_up },
  { OP_EDITOR_KILL_EOL,           op_editor_kill_eol },
  { OP_EDITOR_KILL_EOW,           op_editor_kill_eow },
  { OP_EDITOR_KILL_LINE,          op_editor_kill_line },
  { OP_EDITOR_KILL_WORD,          op_editor_kill_word },
  { OP_EDITOR_MAILBOX_CYCLE,      op_editor_mailbox_cycle },
  { OP_EDITOR_QUOTE_CHAR,         op_editor_quote_char },
  { OP_EDITOR_TRANSPOSE_CHARS,    op_editor_transpose_chars },
  { OP_EDITOR_UPCASE_WORD,        op_editor_capitalize_word },
  { 0, NULL },
  // clang-format on
};

/**
 * enter_function_dispatcher - Perform a Enter function
 * @param win Enter Window
 * @param op  Operation to perform, e.g. OP_ENTER_NEXT
 * @retval num #IndexRetval, e.g. #FR_SUCCESS
 */
int enter_function_dispatcher(struct MuttWindow *win, int op)
{
  if (!win || !win->wdata)
    return FR_UNKNOWN;

  struct EnterWindowData *wdata = win->wdata;
  int rc = FR_UNKNOWN;
  for (size_t i = 0; EnterFunctions[i].op != OP_NULL; i++)
  {
    const struct EnterFunction *fn = &EnterFunctions[i];
    if (fn->op == op)
    {
      rc = fn->function(wdata, op);
      break;
    }
  }

  if (rc == FR_UNKNOWN) // Not our function
    return rc;

  const char *result = dispacher_get_retval_name(rc);
  mutt_debug(LL_DEBUG1, "Handled %s (%d) -> %s\n", opcodes_get_name(op), op, NONULL(result));

  return rc;
}
