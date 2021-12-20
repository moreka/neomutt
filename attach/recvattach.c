/**
 * @file
 * Attachment code
 *
 * @authors
 * Copyright (C) 1996-2000,2002,2007,2010 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 1999-2006 Thomas Roessler <roessler@does-not-exist.org>
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
 * @page attach_recvattach Attachment functions
 *
 * Attachment code
 */

#include "config.h"
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mutt/lib.h"
#include "config/lib.h"
#include "email/lib.h"
#include "core/lib.h"
#include "gui/lib.h"
#include "mutt.h"
#include "recvattach.h"
#include "menu/lib.h"
#include "ncrypt/lib.h"
#include "question/lib.h"
#include "send/lib.h"
#include "commands.h"
#include "handler.h"
#include "hook.h"
#include "mailcap.h"
#include "mutt_attach.h"
#include "mutt_thread.h"
#include "muttlib.h"
#include "opcodes.h"
#include "rfc3676.h"
#ifdef ENABLE_NLS
#include <libintl.h>
#endif

/**
 * current_attachment - Get the current attachment
 * @param actx Attachment context
 * @param menu Menu
 * @retval ptr Current Attachment
 */
struct AttachPtr *current_attachment(struct AttachCtx *actx, struct Menu *menu)
{
  const int virt = menu_get_index(menu);
  const int index = actx->v2r[virt];

  return actx->idx[index];
}

/**
 * mutt_update_v2r - Update the virtual list of attachments
 * @param actx Attachment context
 *
 * Update the record of the number of attachments and the status of the tree.
 */
static void mutt_update_v2r(struct AttachCtx *actx)
{
  int vindex, rindex, curlevel;

  vindex = 0;
  rindex = 0;

  while (rindex < actx->idxlen)
  {
    actx->v2r[vindex++] = rindex;
    if (actx->idx[rindex]->collapsed)
    {
      curlevel = actx->idx[rindex]->level;
      do
      {
        rindex++;
      } while ((rindex < actx->idxlen) && (actx->idx[rindex]->level > curlevel));
    }
    else
      rindex++;
  }

  actx->vcount = vindex;
}

/**
 * mutt_update_tree - Refresh the list of attachments
 * @param actx Attachment context
 */
void mutt_update_tree(struct AttachCtx *actx)
{
  char buf[256] = { 0 };
  char *s = NULL;

  mutt_update_v2r(actx);

  for (int vindex = 0; vindex < actx->vcount; vindex++)
  {
    const int rindex = actx->v2r[vindex];
    actx->idx[rindex]->num = vindex;
    if ((2 * (actx->idx[rindex]->level + 2)) < sizeof(buf))
    {
      if (actx->idx[rindex]->level)
      {
        s = buf + 2 * (actx->idx[rindex]->level - 1);
        *s++ = (actx->idx[rindex]->body->next) ? MUTT_TREE_LTEE : MUTT_TREE_LLCORNER;
        *s++ = MUTT_TREE_HLINE;
        *s++ = MUTT_TREE_RARROW;
      }
      else
        s = buf;
      *s = '\0';
    }

    if (actx->idx[rindex]->tree)
    {
      if (!mutt_str_equal(actx->idx[rindex]->tree, buf))
        mutt_str_replace(&actx->idx[rindex]->tree, buf);
    }
    else
      actx->idx[rindex]->tree = mutt_str_dup(buf);

    if (((2 * (actx->idx[rindex]->level + 2)) < sizeof(buf)) &&
        actx->idx[rindex]->level)
    {
      s = buf + 2 * (actx->idx[rindex]->level - 1);
      *s++ = (actx->idx[rindex]->body->next) ? '\005' : '\006';
      *s++ = '\006';
    }
  }
}

/**
 * prepend_savedir - Add `$attach_save_dir` to the beginning of a path
 * @param buf Buffer for the result
 */
static void prepend_savedir(struct Buffer *buf)
{
  if (!buf || !buf->data || (buf->data[0] == '/'))
    return;

  struct Buffer *tmp = mutt_buffer_pool_get();
  const char *const c_attach_save_dir =
      cs_subset_path(NeoMutt->sub, "attach_save_dir");
  if (c_attach_save_dir)
  {
    mutt_buffer_addstr(tmp, c_attach_save_dir);
    if (tmp->dptr[-1] != '/')
      mutt_buffer_addch(tmp, '/');
  }
  else
    mutt_buffer_addstr(tmp, "./");

  mutt_buffer_addstr(tmp, mutt_buffer_string(buf));
  mutt_buffer_copy(buf, tmp);
  mutt_buffer_pool_release(&tmp);
}

/**
 * has_a_message - Determine if the Body has a message (to save)
 * @param[in]  body Body of the message
 * @retval true Suitable for saving
 */
static bool has_a_message(struct Body *body)
{
  return (body->email && (body->encoding != ENC_BASE64) &&
          (body->encoding != ENC_QUOTED_PRINTABLE) &&
          mutt_is_message_type(body->type, body->subtype));
}

/**
 * save_attachment_flowed_helper - Helper for unstuffing attachments
 * @param fp    Attachment to work on
 * @param b     Body of email
 * @param path  Path to save the attachment
 * @param flags Flags, e.g. #MUTT_SAVE_APPEND
 * @param e     Email
 * @retval  0 Success
 * @retval -1 Failure
 *
 * This is a proxy between the mutt_save_attachment_list() calls and
 * mutt_save_attachment().  It (currently) exists solely to unstuff
 * format=flowed text attachments.
 *
 * Direct modification of mutt_save_attachment() wasn't easily possible
 * because:
 * 1) other callers of mutt_save_attachment() should not have unstuffing
 *    performed, such as replying/forwarding attachments.
 * 2) the attachment saving can append to a file, making the
 *    unstuffing inside difficult with current functions.
 * 3) we can't unstuff before-hand because decoding hasn't occurred.
 *
 * So, I apologize for this horrific proxy, but it was the most
 * straightforward method.
 */
static int save_attachment_flowed_helper(FILE *fp, struct Body *b, const char *path,
                                         enum SaveAttach flags, struct Email *e)
{
  int rc = -1;

  if (mutt_rfc3676_is_format_flowed(b))
  {
    struct Body b_fake = { 0 };

    struct Buffer *tempfile = mutt_buffer_pool_get();
    mutt_buffer_mktemp(tempfile);

    /* Pass MUTT_SAVE_NO_FLAGS to force mutt_file_fopen("w") */
    rc = mutt_save_attachment(fp, b, mutt_buffer_string(tempfile), MUTT_SAVE_NO_FLAGS, e);
    if (rc != 0)
      goto cleanup;

    mutt_rfc3676_space_unstuff_attachment(b, mutt_buffer_string(tempfile));

    /* Now "really" save it.  Send mode does this without touching anything,
     * so force send-mode. */
    memset(&b_fake, 0, sizeof(struct Body));
    b_fake.filename = tempfile->data;
    rc = mutt_save_attachment(NULL, &b_fake, path, flags, e);

    mutt_file_unlink(mutt_buffer_string(tempfile));

  cleanup:
    mutt_buffer_pool_release(&tempfile);
  }
  else
  {
    rc = mutt_save_attachment(fp, b, path, flags, e);
  }

  return rc;
}

/**
 * query_save_attachment - Ask the user if we should save the attachment
 * @param[in]  fp        File handle to the attachment (OPTIONAL)
 * @param[in]  body      Attachment
 * @param[in]  e       Email
 * @param[out] directory Where the attachment was saved
 * @retval  0 Success
 * @retval -1 Failure
 */
static int query_save_attachment(FILE *fp, struct Body *body, struct Email *e, char **directory)
{
  char *prompt = NULL;
  enum SaveAttach opt = MUTT_SAVE_NO_FLAGS;
  int rc = -1;

  struct Buffer *buf = mutt_buffer_pool_get();
  struct Buffer *tfile = mutt_buffer_pool_get();

  if (body->filename)
  {
    if (directory && *directory)
    {
      mutt_buffer_concat_path(buf, *directory, mutt_path_basename(body->filename));
    }
    else
      mutt_buffer_strcpy(buf, body->filename);
  }
  else if (has_a_message(body))
  {
    mutt_default_save(buf->data, buf->dsize, body->email);
    mutt_buffer_fix_dptr(buf);
  }

  prepend_savedir(buf);

  prompt = _("Save to file: ");
  while (prompt)
  {
    if ((mutt_get_field(prompt, buf, MUTT_COMP_FILE | MUTT_COMP_CLEAR, false,
                        NULL, NULL, NULL) != 0) ||
        mutt_buffer_is_empty(buf))
    {
      goto cleanup;
    }

    prompt = NULL;
    mutt_buffer_expand_path(buf);

    bool is_message = (fp && has_a_message(body));

    if (is_message)
    {
      struct stat st = { 0 };

      /* check to make sure that this file is really the one the user wants */
      rc = mutt_save_confirm(mutt_buffer_string(buf), &st);
      if (rc == 1)
      {
        prompt = _("Save to file: ");
        continue;
      }
      else if (rc == -1)
        goto cleanup;
      mutt_buffer_copy(tfile, buf);
    }
    else
    {
      rc = mutt_check_overwrite(body->filename, mutt_buffer_string(buf), tfile,
                                &opt, directory);
      if (rc == -1)
        goto cleanup;
      else if (rc == 1)
      {
        prompt = _("Save to file: ");
        continue;
      }
    }

    mutt_message(_("Saving..."));
    if (save_attachment_flowed_helper(fp, body, mutt_buffer_string(tfile), opt,
                                      (e || !is_message) ? e : body->email) == 0)
    {
      // This uses ngettext to avoid duplication of messages
      mutt_message(ngettext("Attachment saved", "%d attachments saved", 1), 1);
      rc = 0;
      goto cleanup;
    }
    else
    {
      prompt = _("Save to file: ");
      continue;
    }
  }

cleanup:
  mutt_buffer_pool_release(&buf);
  mutt_buffer_pool_release(&tfile);
  return rc;
}

/**
 * save_without_prompting - Save the attachment, without prompting each time.
 * @param[in]  fp   File handle to the attachment (OPTIONAL)
 * @param[in]  body Attachment
 * @param[in]  e    Email
 * @retval  0 Success
 * @retval -1 Failure
 */
static int save_without_prompting(FILE *fp, struct Body *body, struct Email *e)
{
  enum SaveAttach opt = MUTT_SAVE_NO_FLAGS;
  int rc = -1;
  struct Buffer *buf = mutt_buffer_pool_get();
  struct Buffer *tfile = mutt_buffer_pool_get();

  if (body->filename)
  {
    mutt_buffer_strcpy(buf, body->filename);
  }
  else if (has_a_message(body))
  {
    mutt_default_save(buf->data, buf->dsize, body->email);
  }

  prepend_savedir(buf);
  mutt_buffer_expand_path(buf);

  bool is_message = (fp && has_a_message(body));

  if (is_message)
  {
    mutt_buffer_copy(tfile, buf);
  }
  else
  {
    rc = mutt_check_overwrite(body->filename, mutt_buffer_string(buf), tfile, &opt, NULL);
    if (rc == -1) // abort or cancel
      goto cleanup;
  }

  rc = save_attachment_flowed_helper(fp, body, mutt_buffer_string(tfile), opt,
                                     (e || !is_message) ? e : body->email);

cleanup:
  mutt_buffer_pool_release(&buf);
  mutt_buffer_pool_release(&tfile);
  return rc;
}

/**
 * mutt_save_attachment_list - Save a list of attachments
 * @param actx Attachment context
 * @param fp   File handle for the attachment (OPTIONAL)
 * @param tag  If true, only save the tagged attachments
 * @param top  First Attachment
 * @param e  Email
 * @param menu Menu listing attachments
 */
void mutt_save_attachment_list(struct AttachCtx *actx, FILE *fp, bool tag,
                               struct Body *top, struct Email *e, struct Menu *menu)
{
  char *directory = NULL;
  int rc = 1;
  int last = menu_get_index(menu);
  FILE *fp_out = NULL;
  int saved_attachments = 0;

  struct Buffer *buf = mutt_buffer_pool_get();
  struct Buffer *tfile = mutt_buffer_pool_get();

  const bool c_attach_split = cs_subset_bool(NeoMutt->sub, "attach_split");
  const char *const c_attach_sep = cs_subset_string(NeoMutt->sub, "attach_sep");
  const bool c_attach_save_without_prompting =
      cs_subset_bool(NeoMutt->sub, "attach_save_without_prompting");

  for (int i = 0; !tag || (i < actx->idxlen); i++)
  {
    if (tag)
    {
      fp = actx->idx[i]->fp;
      top = actx->idx[i]->body;
    }
    if (!tag || top->tagged)
    {
      if (!c_attach_split)
      {
        if (mutt_buffer_is_empty(buf))
        {
          enum SaveAttach opt = MUTT_SAVE_NO_FLAGS;

          mutt_buffer_strcpy(buf, mutt_path_basename(NONULL(top->filename)));
          prepend_savedir(buf);

          if ((mutt_get_field(_("Save to file: "), buf, MUTT_COMP_FILE | MUTT_COMP_CLEAR,
                              false, NULL, NULL, NULL) != 0) ||
              mutt_buffer_is_empty(buf))
          {
            goto cleanup;
          }
          mutt_buffer_expand_path(buf);
          if (mutt_check_overwrite(top->filename, mutt_buffer_string(buf), tfile, &opt, NULL))
            goto cleanup;
          rc = save_attachment_flowed_helper(fp, top, mutt_buffer_string(tfile), opt, e);
          if ((rc == 0) && c_attach_sep && (fp_out = fopen(mutt_buffer_string(tfile), "a")))
          {
            fprintf(fp_out, "%s", c_attach_sep);
            mutt_file_fclose(&fp_out);
          }
        }
        else
        {
          rc = save_attachment_flowed_helper(fp, top, mutt_buffer_string(tfile),
                                             MUTT_SAVE_APPEND, e);
          if ((rc == 0) && c_attach_sep && (fp_out = fopen(mutt_buffer_string(tfile), "a")))
          {
            fprintf(fp_out, "%s", c_attach_sep);
            mutt_file_fclose(&fp_out);
          }
        }
      }
      else
      {
        if (tag && menu && top->aptr)
        {
          menu_set_index(menu, top->aptr->num);
          menu_queue_redraw(menu, MENU_REDRAW_MOTION);

          menu_redraw(menu);
        }
        if (c_attach_save_without_prompting)
        {
          // Save each file, with no prompting, using the configured 'AttachSaveDir'
          rc = save_without_prompting(fp, top, e);
          if (rc == 0)
            saved_attachments++;
        }
        else
        {
          // Save each file, prompting the user for the location each time.
          if (query_save_attachment(fp, top, e, &directory) == -1)
            break;
        }
      }
    }
    if (!tag)
      break;
  }

  FREE(&directory);

  if (tag && menu)
  {
    menu_set_index(menu, last);
    menu_queue_redraw(menu, MENU_REDRAW_MOTION);
  }

  if (rc == 0)
  {
    if (!c_attach_split)
      saved_attachments = 1;

    if (!c_attach_split || c_attach_save_without_prompting)
    {
      mutt_message(ngettext("Attachment saved", "%d attachments saved", saved_attachments),
                   saved_attachments);
    }
  }

cleanup:
  mutt_buffer_pool_release(&buf);
  mutt_buffer_pool_release(&tfile);
}

/**
 * query_pipe_attachment - Ask the user if we should pipe the attachment
 * @param command Command to pipe the attachment to
 * @param fp      File handle to the attachment (OPTIONAL)
 * @param body    Attachment
 * @param filter  Is this command a filter?
 */
static void query_pipe_attachment(const char *command, FILE *fp, struct Body *body, bool filter)
{
  char tfile[PATH_MAX];

  if (filter)
  {
    char warning[PATH_MAX + 256];
    snprintf(warning, sizeof(warning),
             _("WARNING!  You are about to overwrite %s, continue?"), body->filename);
    if (mutt_yesorno(warning, MUTT_NO) != MUTT_YES)
    {
      msgwin_clear_text();
      return;
    }
    mutt_mktemp(tfile, sizeof(tfile));
  }
  else
    tfile[0] = '\0';

  if (mutt_pipe_attachment(fp, body, command, tfile))
  {
    if (filter)
    {
      mutt_file_unlink(body->filename);
      mutt_file_rename(tfile, body->filename);
      mutt_update_encoding(body, NeoMutt->sub);
      mutt_message(_("Attachment filtered"));
    }
  }
  else
  {
    if (filter && tfile[0])
      mutt_file_unlink(tfile);
  }
}

/**
 * pipe_attachment - Pipe the attachment to a command
 * @param fp    File handle to the attachment (OPTIONAL)
 * @param b     Attachment
 * @param state File state for decoding the attachment
 */
static void pipe_attachment(FILE *fp, struct Body *b, struct State *state)
{
  if (!state || !state->fp_out)
    return;

  FILE *fp_in = NULL;
  FILE *fp_unstuff = NULL;
  bool is_flowed = false, unlink_unstuff = false;
  struct Buffer *unstuff_tempfile = NULL;

  if (mutt_rfc3676_is_format_flowed(b))
  {
    is_flowed = true;
    unstuff_tempfile = mutt_buffer_pool_get();
    mutt_buffer_mktemp(unstuff_tempfile);
  }

  if (fp)
  {
    state->fp_in = fp;

    if (is_flowed)
    {
      fp_unstuff = mutt_file_fopen(mutt_buffer_string(unstuff_tempfile), "w");
      if (fp_unstuff == NULL)
      {
        mutt_perror("mutt_file_fopen");
        goto bail;
      }
      unlink_unstuff = true;

      FILE *filter_fp = state->fp_out;
      state->fp_out = fp_unstuff;
      mutt_decode_attachment(b, state);
      mutt_file_fclose(&fp_unstuff);
      state->fp_out = filter_fp;

      fp_unstuff = mutt_file_fopen(mutt_buffer_string(unstuff_tempfile), "r");
      if (fp_unstuff == NULL)
      {
        mutt_perror("mutt_file_fopen");
        goto bail;
      }
      mutt_file_copy_stream(fp_unstuff, filter_fp);
      mutt_file_fclose(&fp_unstuff);
    }
    else
      mutt_decode_attachment(b, state);
  }
  else
  {
    const char *infile = NULL;

    if (is_flowed)
    {
      if (mutt_save_attachment(fp, b, mutt_buffer_string(unstuff_tempfile), 0, NULL) == -1)
        goto bail;
      unlink_unstuff = true;
      mutt_rfc3676_space_unstuff_attachment(b, mutt_buffer_string(unstuff_tempfile));
      infile = mutt_buffer_string(unstuff_tempfile);
    }
    else
      infile = b->filename;

    fp_in = fopen(infile, "r");
    if (!fp_in)
    {
      mutt_perror("fopen");
      goto bail;
    }
    mutt_file_copy_stream(fp_in, state->fp_out);
    mutt_file_fclose(&fp_in);
  }

  const char *const c_attach_sep = cs_subset_string(NeoMutt->sub, "attach_sep");
  if (c_attach_sep)
    state_puts(state, c_attach_sep);

bail:
  mutt_file_fclose(&fp_unstuff);
  mutt_file_fclose(&fp_in);

  if (unlink_unstuff)
    mutt_file_unlink(mutt_buffer_string(unstuff_tempfile));
  mutt_buffer_pool_release(&unstuff_tempfile);
}

/**
 * pipe_attachment_list - Pipe a list of attachments to a command
 * @param command Command to pipe the attachment to
 * @param actx    Attachment context
 * @param fp      File handle to the attachment (OPTIONAL)
 * @param tag     If true, only save the tagged attachments
 * @param top     First Attachment
 * @param filter  Is this command a filter?
 * @param state   File state for decoding the attachments
 */
static void pipe_attachment_list(const char *command, struct AttachCtx *actx,
                                 FILE *fp, bool tag, struct Body *top,
                                 bool filter, struct State *state)
{
  for (int i = 0; !tag || (i < actx->idxlen); i++)
  {
    if (tag)
    {
      fp = actx->idx[i]->fp;
      top = actx->idx[i]->body;
    }
    if (!tag || top->tagged)
    {
      const bool c_attach_split = cs_subset_bool(NeoMutt->sub, "attach_split");
      if (!filter && !c_attach_split)
        pipe_attachment(fp, top, state);
      else
        query_pipe_attachment(command, fp, top, filter);
    }
    if (!tag)
      break;
  }
}

/**
 * mutt_pipe_attachment_list - Pipe a list of attachments to a command
 * @param actx   Attachment context
 * @param fp     File handle to the attachment (OPTIONAL)
 * @param tag    If true, only save the tagged attachments
 * @param top    First Attachment
 * @param filter Is this command a filter?
 */
void mutt_pipe_attachment_list(struct AttachCtx *actx, FILE *fp, bool tag,
                               struct Body *top, bool filter)
{
  struct State state = { 0 };
  struct Buffer *buf = NULL;

  if (fp)
    filter = false; /* sanity check: we can't filter in the recv case yet */

  buf = mutt_buffer_pool_get();
  /* perform charset conversion on text attachments when piping */
  state.flags = MUTT_CHARCONV;

  if (mutt_get_field((filter ? _("Filter through: ") : _("Pipe to: ")), buf,
                     MUTT_COMP_FILE_SIMPLE, false, NULL, NULL, NULL) != 0)
  {
    goto cleanup;
  }

  if (mutt_buffer_len(buf) == 0)
    goto cleanup;

  mutt_buffer_expand_path(buf);

  const bool c_attach_split = cs_subset_bool(NeoMutt->sub, "attach_split");
  if (!filter && !c_attach_split)
  {
    mutt_endwin();
    pid_t pid = filter_create(mutt_buffer_string(buf), &state.fp_out, NULL, NULL);
    pipe_attachment_list(mutt_buffer_string(buf), actx, fp, tag, top, filter, &state);
    mutt_file_fclose(&state.fp_out);
    const bool c_wait_key = cs_subset_bool(NeoMutt->sub, "wait_key");
    if ((filter_wait(pid) != 0) || c_wait_key)
      mutt_any_key_to_continue(NULL);
  }
  else
    pipe_attachment_list(mutt_buffer_string(buf), actx, fp, tag, top, filter, &state);

cleanup:
  mutt_buffer_pool_release(&buf);
}

/**
 * can_print - Do we know how to print this attachment type?
 * @param actx Attachment
 * @param top  Body of email
 * @param tag  Apply to all tagged Attachments
 * @retval true (all) the Attachment(s) are printable
 */
static bool can_print(struct AttachCtx *actx, struct Body *top, bool tag)
{
  char type[256];

  for (int i = 0; !tag || (i < actx->idxlen); i++)
  {
    if (tag)
      top = actx->idx[i]->body;
    snprintf(type, sizeof(type), "%s/%s", TYPE(top), top->subtype);
    if (!tag || top->tagged)
    {
      if (!mailcap_lookup(top, type, sizeof(type), NULL, MUTT_MC_PRINT))
      {
        if (!mutt_istr_equal("text/plain", top->subtype) &&
            !mutt_istr_equal("application/postscript", top->subtype))
        {
          if (!mutt_can_decode(top))
          {
            /* L10N: s gets replaced by a MIME type, e.g. "text/plain" or
               application/octet-stream.  */
            mutt_error(_("I don't know how to print %s attachments"), type);
            return false;
          }
        }
      }
    }
    if (!tag)
      break;
  }
  return true;
}

/**
 * print_attachment_list - Print a list of Attachments
 * @param actx  Attachment context
 * @param fp    File handle to the attachment (OPTIONAL)
 * @param tag   Apply to all tagged Attachments
 * @param top   First Attachment
 * @param state File state for decoding the attachments
 */
static void print_attachment_list(struct AttachCtx *actx, FILE *fp, bool tag,
                                  struct Body *top, struct State *state)
{
  char type[256];

  for (int i = 0; !tag || (i < actx->idxlen); i++)
  {
    if (tag)
    {
      fp = actx->idx[i]->fp;
      top = actx->idx[i]->body;
    }
    if (!tag || top->tagged)
    {
      snprintf(type, sizeof(type), "%s/%s", TYPE(top), top->subtype);
      const bool c_attach_split = cs_subset_bool(NeoMutt->sub, "attach_split");
      if (!c_attach_split && !mailcap_lookup(top, type, sizeof(type), NULL, MUTT_MC_PRINT))
      {
        if (mutt_istr_equal("text/plain", top->subtype) ||
            mutt_istr_equal("application/postscript", top->subtype))
        {
          pipe_attachment(fp, top, state);
        }
        else if (mutt_can_decode(top))
        {
          /* decode and print */

          FILE *fp_in = NULL;
          struct Buffer *newfile = mutt_buffer_pool_get();

          mutt_buffer_mktemp(newfile);
          if (mutt_decode_save_attachment(fp, top, mutt_buffer_string(newfile),
                                          MUTT_PRINTING, MUTT_SAVE_NO_FLAGS) == 0)
          {
            if (!state->fp_out)
            {
              mutt_error(
                  "BUG in print_attachment_list().  Please report this. ");
              return;
            }

            fp_in = fopen(mutt_buffer_string(newfile), "r");
            if (fp_in)
            {
              mutt_file_copy_stream(fp_in, state->fp_out);
              mutt_file_fclose(&fp_in);
              const char *const c_attach_sep = cs_subset_string(NeoMutt->sub, "attach_sep");
              if (c_attach_sep)
                state_puts(state, c_attach_sep);
            }
          }
          mutt_file_unlink(mutt_buffer_string(newfile));
          mutt_buffer_pool_release(&newfile);
        }
      }
      else
        mutt_print_attachment(fp, top);
    }
    if (!tag)
      break;
  }
}

/**
 * mutt_print_attachment_list - Print a list of Attachments
 * @param actx Attachment context
 * @param fp   File handle to the attachment (OPTIONAL)
 * @param tag  Apply to all tagged Attachments
 * @param top  First Attachment
 */
void mutt_print_attachment_list(struct AttachCtx *actx, FILE *fp, bool tag, struct Body *top)
{
  char prompt[128];
  struct State state = { 0 };
  int tagmsgcount = 0;

  if (tag)
    for (int i = 0; i < actx->idxlen; i++)
      if (actx->idx[i]->body->tagged)
        tagmsgcount++;

  snprintf(prompt, sizeof(prompt),
           tag ? ngettext("Print tagged attachment?", "Print %d tagged attachments?", tagmsgcount) :
                 _("Print attachment?"),
           tagmsgcount);
  const enum QuadOption c_print = cs_subset_quad(NeoMutt->sub, "print");
  if (query_quadoption(c_print, prompt) != MUTT_YES)
    return;

  const bool c_attach_split = cs_subset_bool(NeoMutt->sub, "attach_split");
  if (c_attach_split)
  {
    print_attachment_list(actx, fp, tag, top, &state);
  }
  else
  {
    if (!can_print(actx, top, tag))
      return;
    mutt_endwin();
    const char *const c_print_command =
        cs_subset_string(NeoMutt->sub, "print_command");
    pid_t pid = filter_create(NONULL(c_print_command), &state.fp_out, NULL, NULL);
    print_attachment_list(actx, fp, tag, top, &state);
    mutt_file_fclose(&state.fp_out);
    const bool c_wait_key = cs_subset_bool(NeoMutt->sub, "wait_key");
    if ((filter_wait(pid) != 0) || c_wait_key)
      mutt_any_key_to_continue(NULL);
  }
}

/**
 * recvattach_edit_content_type - Edit the content type of an attachment
 * @param actx Attachment context
 * @param menu Menu listing Attachments
 * @param e  Email
 */
void recvattach_edit_content_type(struct AttachCtx *actx, struct Menu *menu, struct Email *e)
{
  struct AttachPtr *cur_att = current_attachment(actx, menu);
  if (!mutt_edit_content_type(e, cur_att->body, cur_att->fp))
    return;

  /* The mutt_update_recvattach_menu() will overwrite any changes
   * made to a decrypted cur_att->body, so warn the user. */
  if (cur_att->decrypted)
  {
    mutt_message(
        _("Structural changes to decrypted attachments are not supported"));
    mutt_sleep(1);
  }
  /* Editing the content type can rewrite the body structure. */
  for (int i = 0; i < actx->idxlen; i++)
    actx->idx[i]->body = NULL;
  mutt_actx_entries_free(actx);
  mutt_update_recvattach_menu(actx, menu, true);
}

/**
 * mutt_attach_display_loop - Event loop for the Attachment menu
 * @param sub  Config Subset
 * @param menu Menu listing Attachments
 * @param op   Operation, e.g. OP_ATTACHMENT_VIEW
 * @param e  Email
 * @param actx Attachment context
 * @param recv true if these are received attachments (rather than in compose)
 * @retval num Operation performed
 */
int mutt_attach_display_loop(struct ConfigSubset *sub, struct Menu *menu, int op,
                             struct Email *e, struct AttachCtx *actx, bool recv)
{
  do
  {
    switch (op)
    {
      case OP_DISPLAY_HEADERS:
        bool_str_toggle(NeoMutt->sub, "weed", NULL);
        /* fallthrough */

      case OP_ATTACHMENT_VIEW:
      {
        struct AttachPtr *cur_att = current_attachment(actx, menu);
        if (!cur_att->fp)
        {
          if (cur_att->body->type == TYPE_MULTIPART)
          {
            struct Body *b = cur_att->body->parts;
            while (b->parts)
              b = b->parts;
            cur_att = b->aptr;
          }
        }
        op = mutt_view_attachment(cur_att->fp, cur_att->body, MUTT_VA_REGULAR,
                                  e, actx, menu->win);
        break;
      }

      case OP_NEXT_ENTRY:
      case OP_MAIN_NEXT_UNDELETED: /* hack */
      {
        const int index = menu_get_index(menu) + 1;
        if (index < menu->max)
        {
          menu_set_index(menu, index);
          op = OP_ATTACHMENT_VIEW;
        }
        else
          op = OP_NULL;
        break;
      }

      case OP_PREV_ENTRY:
      case OP_MAIN_PREV_UNDELETED: /* hack */
      {
        const int index = menu_get_index(menu) - 1;
        if (index >= 0)
        {
          menu_set_index(menu, index);
          op = OP_ATTACHMENT_VIEW;
        }
        else
          op = OP_NULL;
        break;
      }

      case OP_ATTACHMENT_EDIT_TYPE:
      {
        struct AttachPtr *cur_att = current_attachment(actx, menu);
        /* when we edit the content-type, we should redisplay the attachment
         * immediately */
        mutt_edit_content_type(e, cur_att->body, cur_att->fp);
        if (recv)
          recvattach_edit_content_type(actx, menu, e);
        else
          mutt_edit_content_type(e, cur_att->body, cur_att->fp);

        menu_queue_redraw(menu, MENU_REDRAW_INDEX);
        op = OP_ATTACHMENT_VIEW;
        break;
      }
      /* functions which are passed through from the pager */
      case OP_CHECK_TRADITIONAL:
        if (!(WithCrypto & APPLICATION_PGP) || (e && e->security & PGP_TRADITIONAL_CHECKED))
        {
          op = OP_NULL;
          break;
        }
      /* fallthrough */
      case OP_ATTACHMENT_COLLAPSE:
        if (recv)
          return op;
      /* fallthrough */
      default:
        op = OP_NULL;
    }
  } while (op != OP_NULL);

  return op;
}

/**
 * mutt_generate_recvattach_list - Create a list of attachments
 * @param actx        Attachment context
 * @param e           Email
 * @param parts       Body of email
 * @param fp          File to read from
 * @param parent_type Type, e.g. #TYPE_MULTIPART
 * @param level       Attachment depth
 * @param decrypted   True if attachment has been decrypted
 */
void mutt_generate_recvattach_list(struct AttachCtx *actx, struct Email *e,
                                   struct Body *parts, FILE *fp,
                                   int parent_type, int level, bool decrypted)
{
  struct Body *m = NULL;
  struct Body *new_body = NULL;
  FILE *fp_new = NULL;
  SecurityFlags type;
  int need_secured, secured;

  for (m = parts; m; m = m->next)
  {
    need_secured = 0;
    secured = 0;

    if (((WithCrypto & APPLICATION_SMIME) != 0) && (type = mutt_is_application_smime(m)))
    {
      need_secured = 1;

      if (type & SEC_ENCRYPT)
      {
        if (!crypt_valid_passphrase(APPLICATION_SMIME))
          goto decrypt_failed;

        if (e->env)
          crypt_smime_getkeys(e->env);
      }

      secured = !crypt_smime_decrypt_mime(fp, &fp_new, m, &new_body);
      /* If the decrypt/verify-opaque doesn't generate mime output, an empty
       * text/plain type will still be returned by mutt_read_mime_header().
       * We can't distinguish an actual part from a failure, so only use a
       * text/plain that results from a single top-level part. */
      if (secured && (new_body->type == TYPE_TEXT) &&
          mutt_istr_equal("plain", new_body->subtype) && ((parts != m) || m->next))
      {
        mutt_body_free(&new_body);
        mutt_file_fclose(&fp_new);
        goto decrypt_failed;
      }

      if (secured && (type & SEC_ENCRYPT))
        e->security |= SMIME_ENCRYPT;
    }

    if (((WithCrypto & APPLICATION_PGP) != 0) &&
        (mutt_is_multipart_encrypted(m) || mutt_is_malformed_multipart_pgp_encrypted(m)))
    {
      need_secured = 1;

      if (!crypt_valid_passphrase(APPLICATION_PGP))
        goto decrypt_failed;

      secured = !crypt_pgp_decrypt_mime(fp, &fp_new, m, &new_body);

      if (secured)
        e->security |= PGP_ENCRYPT;
    }

    if (need_secured && secured)
    {
      mutt_actx_add_fp(actx, fp_new);
      mutt_actx_add_body(actx, new_body);
      mutt_generate_recvattach_list(actx, e, new_body, fp_new, parent_type, level, 1);
      continue;
    }

  decrypt_failed:
    /* Fall through and show the original parts if decryption fails */
    if (need_secured && !secured)
      mutt_error(_("Can't decrypt encrypted message"));

    /* Strip out the top level multipart */
    if ((m->type == TYPE_MULTIPART) && m->parts && !need_secured &&
        ((parent_type == -1) && !mutt_istr_equal("alternative", m->subtype) &&
         !mutt_istr_equal("multilingual", m->subtype)))
    {
      mutt_generate_recvattach_list(actx, e, m->parts, fp, m->type, level, decrypted);
    }
    else
    {
      struct AttachPtr *ap = mutt_aptr_new();
      mutt_actx_add_attach(actx, ap);

      ap->body = m;
      ap->fp = fp;
      m->aptr = ap;
      ap->parent_type = parent_type;
      ap->level = level;
      ap->decrypted = decrypted;

      if (m->type == TYPE_MULTIPART)
        mutt_generate_recvattach_list(actx, e, m->parts, fp, m->type, level + 1, decrypted);
      else if (mutt_is_message_type(m->type, m->subtype))
      {
        mutt_generate_recvattach_list(actx, m->email, m->parts, fp, m->type,
                                      level + 1, decrypted);
        e->security |= m->email->security;
      }
    }
  }
}

/**
 * mutt_attach_init - Create a new Attachment context
 * @param actx Attachment context
 */
void mutt_attach_init(struct AttachCtx *actx)
{
  /* Collapse the attachments if '$digest_collapse' is set AND if...
   * the outer container is of type 'multipart/digest' */
  bool digest = mutt_istr_equal(actx->email->body->subtype, "digest");

  const bool c_digest_collapse =
      cs_subset_bool(NeoMutt->sub, "digest_collapse");
  for (int i = 0; i < actx->idxlen; i++)
  {
    actx->idx[i]->body->tagged = false;

    /* OR an inner container is of type 'multipart/digest' */
    actx->idx[i]->collapsed =
        (c_digest_collapse &&
         (digest || ((actx->idx[i]->body->type == TYPE_MULTIPART) &&
                     mutt_istr_equal(actx->idx[i]->body->subtype, "digest"))));
  }
}

/**
 * mutt_update_recvattach_menu - Update the Attachment Menu
 * @param actx Attachment context
 * @param menu Menu listing Attachments
 * @param init If true, create a new Attachments context
 */
void mutt_update_recvattach_menu(struct AttachCtx *actx, struct Menu *menu, bool init)
{
  if (init)
  {
    mutt_generate_recvattach_list(actx, actx->email, actx->email->body,
                                  actx->fp_root, -1, 0, 0);
    mutt_attach_init(actx);
  }

  mutt_update_tree(actx);

  menu->max = actx->vcount;

  const int index = menu_get_index(menu);
  if (index >= menu->max)
    menu_set_index(menu, menu->max - 1);
  menu_queue_redraw(menu, MENU_REDRAW_INDEX);
}

/**
 * ba_add_tagged - Get an array of tagged Attachments
 * @param ba   Empty BodyArray to populate
 * @param actx List of Attachments
 * @param menu Menu
 * @retval num Number of selected Attachments
 * @retval -1  Error
 */
int ba_add_tagged(struct BodyArray *ba, struct AttachCtx *actx, struct Menu *menu)
{
  if (!ba || !actx || !menu)
    return -1;

  if (menu->tag_prefix)
  {
    for (int i = 0; i < actx->idxlen; i++)
    {
      struct Body *b = actx->idx[i]->body;
      if (b->tagged)
      {
        ARRAY_ADD(ba, b);
      }
    }
  }
  else
  {
    struct AttachPtr *cur = current_attachment(actx, menu);
    if (!cur)
      return -1;

    ARRAY_ADD(ba, cur->body);
  }

  return ARRAY_SIZE(ba);
}
