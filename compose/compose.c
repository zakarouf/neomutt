/**
 * @file
 * GUI editor for an email's headers
 *
 * @authors
 * Copyright (C) 1996-2000,2002,2007,2010,2012 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2004 g10 Code GmbH
 * Copyright (C) 2019 Pietro Cerutti <gahr@gahr.ch>
 * Copyright (C) 2020 Richard Russon <rich@flatcap.org>
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
 * @page compose_compose GUI editor for an email's headers
 *
 * GUI editor for an email's headers
 */

#include "config.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mutt/lib.h"
#include "address/lib.h"
#include "config/lib.h"
#include "email/lib.h"
#include "core/lib.h"
#include "alias/lib.h"
#include "conn/lib.h"
#include "gui/lib.h"
#include "mutt.h"
#include "lib.h"
#include "ncrypt/lib.h"
#include "send/lib.h"
#include "browser.h"
#include "commands.h"
#include "context.h"
#include "format_flags.h"
#include "hook.h"
#include "index.h"
#include "init.h"
#include "keymap.h"
#include "mutt_attach.h"
#include "mutt_globals.h"
#include "mutt_header.h"
#include "mutt_logging.h"
#include "mutt_menu.h"
#include "muttlib.h"
#include "mx.h"
#include "opcodes.h"
#include "options.h"
#include "protos.h"
#include "recvattach.h"
#include "rfc3676.h"
#include "sort.h"
#ifdef ENABLE_NLS
#include <libintl.h>
#endif
#ifdef MIXMASTER
#include "remailer.h"
#endif
#ifdef USE_NNTP
#include "nntp/lib.h"
#endif
#ifdef USE_POP
#include "pop/lib.h"
#endif
#ifdef USE_IMAP
#include "imap/lib.h"
#endif
#ifdef USE_AUTOCRYPT
#include "autocrypt/lib.h"
#endif

/// Maximum number of rows to use for the To:, Cc:, Bcc: fields
#define MAX_ADDR_ROWS 5

/// Maximum number of rows to use for the Headers: field
#define MAX_USER_HDR_ROWS 5

static const char *There_are_no_attachments = N_("There are no attachments");

static void compose_status_line(char *buf, size_t buflen, size_t col, int cols,
                                struct Menu *menu, const char *p);

/**
 * struct ComposeRedrawData - Keep track when the compose screen needs redrawing
 */
struct ComposeRedrawData
{
  struct Email *email;
  struct Buffer *fcc;

  struct ListHead to_list;
  struct ListHead cc_list;
  struct ListHead bcc_list;

  short to_rows;
  short cc_rows;
  short bcc_rows;
  short sec_rows;

#ifdef USE_AUTOCRYPT
  enum AutocryptRec autocrypt_rec;
  int autocrypt_rec_override;
#endif
  struct MuttWindow *win_envelope; ///< Envelope: From, To, etc
  struct MuttWindow *win_abar;     ///< Attachments label
  struct MuttWindow *win_attach;   ///< List of Attachments
  struct MuttWindow *win_cbar;     ///< Compose bar

  struct ConfigSubset *sub; ///< Inherited config items
};

#define CHECK_COUNT                                                            \
  if (actx->idxlen == 0)                                                       \
  {                                                                            \
    mutt_error(_(There_are_no_attachments));                                   \
    break;                                                                     \
  }

#define CUR_ATTACH actx->idx[actx->v2r[menu->current]]

/**
 * enum HeaderField - Ordered list of headers for the compose screen
 *
 * The position of various fields on the compose screen.
 */
enum HeaderField
{
  HDR_FROM,    ///< "From:" field
  HDR_TO,      ///< "To:" field
  HDR_CC,      ///< "Cc:" field
  HDR_BCC,     ///< "Bcc:" field
  HDR_SUBJECT, ///< "Subject:" field
  HDR_REPLYTO, ///< "Reply-To:" field
  HDR_FCC,     ///< "Fcc:" (save folder) field
#ifdef MIXMASTER
  HDR_MIX, ///< "Mix:" field (Mixmaster chain)
#endif
  HDR_CRYPT,     ///< "Security:" field (encryption/signing info)
  HDR_CRYPTINFO, ///< "Sign as:" field (encryption/signing info)
#ifdef USE_AUTOCRYPT
  HDR_AUTOCRYPT, ///< "Autocrypt:" and "Recommendation:" fields
#endif
#ifdef USE_NNTP
  HDR_NEWSGROUPS, ///< "Newsgroups:" field
  HDR_FOLLOWUPTO, ///< "Followup-To:" field
  HDR_XCOMMENTTO, ///< "X-Comment-To:" field
#endif
  HDR_CUSTOM_HEADERS, ///< "Headers:" field
  HDR_ATTACH_TITLE,   ///< The "-- Attachments" line
};

static int HeaderPadding[HDR_ATTACH_TITLE] = { 0 };
static int MaxHeaderWidth = 0;

static const char *const Prompts[] = {
  /* L10N: Compose menu field.  May not want to translate. */
  N_("From: "),
  /* L10N: Compose menu field.  May not want to translate. */
  N_("To: "),
  /* L10N: Compose menu field.  May not want to translate. */
  N_("Cc: "),
  /* L10N: Compose menu field.  May not want to translate. */
  N_("Bcc: "),
  /* L10N: Compose menu field.  May not want to translate. */
  N_("Subject: "),
  /* L10N: Compose menu field.  May not want to translate. */
  N_("Reply-To: "),
  /* L10N: Compose menu field.  May not want to translate. */
  N_("Fcc: "),
#ifdef MIXMASTER
  /* L10N: "Mix" refers to the MixMaster chain for anonymous email */
  N_("Mix: "),
#endif
  /* L10N: Compose menu field.  Holds "Encrypt", "Sign" related information */
  N_("Security: "),
  /* L10N: This string is used by the compose menu.
     Since it is hidden by default, it does not increase the indentation of
     other compose menu fields.  However, if possible, it should not be longer
     than the other compose menu fields.  Since it shares the row with "Encrypt
     with:", it should not be longer than 15-20 character cells.  */
  N_("Sign as: "),
#ifdef USE_AUTOCRYPT
  // L10N: The compose menu autocrypt line
  N_("Autocrypt: "),
#endif
#ifdef USE_NNTP
  /* L10N: Compose menu field.  May not want to translate. */
  N_("Newsgroups: "),
  /* L10N: Compose menu field.  May not want to translate. */
  N_("Followup-To: "),
  /* L10N: Compose menu field.  May not want to translate. */
  N_("X-Comment-To: "),
#endif
  N_("Headers: "),
};

/// Help Bar for the Compose dialog
static const struct Mapping ComposeHelp[] = {
  // clang-format off
  { N_("Send"),        OP_COMPOSE_SEND_MESSAGE },
  { N_("Abort"),       OP_EXIT },
  /* L10N: compose menu help line entry */
  { N_("To"),          OP_COMPOSE_EDIT_TO },
  /* L10N: compose menu help line entry */
  { N_("CC"),          OP_COMPOSE_EDIT_CC },
  /* L10N: compose menu help line entry */
  { N_("Subj"),        OP_COMPOSE_EDIT_SUBJECT },
  { N_("Attach file"), OP_COMPOSE_ATTACH_FILE },
  { N_("Descrip"),     OP_COMPOSE_EDIT_DESCRIPTION },
  { N_("Help"),        OP_HELP },
  { NULL, 0 },
  // clang-format on
};

#ifdef USE_NNTP
/// Help Bar for the News Compose dialog
static const struct Mapping ComposeNewsHelp[] = {
  // clang-format off
  { N_("Send"),        OP_COMPOSE_SEND_MESSAGE },
  { N_("Abort"),       OP_EXIT },
  { N_("Newsgroups"),  OP_COMPOSE_EDIT_NEWSGROUPS },
  { N_("Subj"),        OP_COMPOSE_EDIT_SUBJECT },
  { N_("Attach file"), OP_COMPOSE_ATTACH_FILE },
  { N_("Descrip"),     OP_COMPOSE_EDIT_DESCRIPTION },
  { N_("Help"),        OP_HELP },
  { NULL, 0 },
  // clang-format on
};
#endif

#ifdef USE_AUTOCRYPT
static const char *AutocryptRecUiFlags[] = {
  /* L10N: Autocrypt recommendation flag: off.
   * This is displayed when Autocrypt is turned off. */
  N_("Off"),
  /* L10N: Autocrypt recommendation flag: no.
   * This is displayed when Autocrypt cannot encrypt to the recipients. */
  N_("No"),
  /* L10N: Autocrypt recommendation flag: discouraged.
   * This is displayed when Autocrypt believes encryption should not be used.
   * This might occur if one of the recipient Autocrypt Keys has not been
   * used recently, or if the only key available is a Gossip Header key. */
  N_("Discouraged"),
  /* L10N: Autocrypt recommendation flag: available.
   * This is displayed when Autocrypt believes encryption is possible, but
   * leaves enabling it up to the sender.  Probably because "prefer encrypt"
   * is not set in both the sender and recipient keys. */
  N_("Available"),
  /* L10N: Autocrypt recommendation flag: yes.
   * This is displayed when Autocrypt would normally enable encryption
   * automatically. */
  N_("Yes"),
};
#endif

/**
 * calc_header_width_padding - Calculate the width needed for the compose labels
 * @param idx      Store the result at this index of HeaderPadding
 * @param header   Header string
 * @param calc_max If true, calculate the maximum width
 */
static void calc_header_width_padding(int idx, const char *header, bool calc_max)
{
  int width;

  HeaderPadding[idx] = mutt_str_len(header);
  width = mutt_strwidth(header);
  if (calc_max && (MaxHeaderWidth < width))
    MaxHeaderWidth = width;
  HeaderPadding[idx] -= width;
}

/**
 * init_header_padding - Calculate how much padding the compose table will need
 *
 * The padding needed for each header is strlen() + max_width - strwidth().
 *
 * calc_header_width_padding sets each entry in HeaderPadding to strlen -
 * width.  Then, afterwards, we go through and add max_width to each entry.
 */
static void init_header_padding(void)
{
  static bool done = false;

  if (done)
    return;
  done = true;

  for (int i = 0; i < HDR_ATTACH_TITLE; i++)
  {
    if (i == HDR_CRYPTINFO)
      continue;
    calc_header_width_padding(i, _(Prompts[i]), true);
  }

  /* Don't include "Sign as: " in the MaxHeaderWidth calculation.  It
   * doesn't show up by default, and so can make the indentation of
   * the other fields look funny. */
  calc_header_width_padding(HDR_CRYPTINFO, _(Prompts[HDR_CRYPTINFO]), false);

  for (int i = 0; i < HDR_ATTACH_TITLE; i++)
  {
    HeaderPadding[i] += MaxHeaderWidth;
    if (HeaderPadding[i] < 0)
      HeaderPadding[i] = 0;
  }
}

/**
 * snd_make_entry - Format a menu item for the attachment list - Implements Menu::make_entry()
 */
static void snd_make_entry(char *buf, size_t buflen, struct Menu *menu, int line)
{
  struct AttachCtx *actx = menu->mdata;

  const struct ComposeRedrawData *rd = (const struct ComposeRedrawData *) menu->redraw_data;
  const struct ConfigSubset *sub = rd->sub;

  const char *c_attach_format = cs_subset_string(sub, "attach_format");
  mutt_expando_format(buf, buflen, 0, menu->win_index->state.cols, NONULL(c_attach_format),
                      attach_format_str, (intptr_t)(actx->idx[actx->v2r[line]]),
                      MUTT_FORMAT_STAT_FILE | MUTT_FORMAT_ARROWCURSOR);
}

#ifdef USE_AUTOCRYPT
/**
 * autocrypt_compose_menu - Autocrypt compose settings
 * @param e Email
 * @param sub ConfigSubset
 */
static void autocrypt_compose_menu(struct Email *e, const struct ConfigSubset *sub)
{
  /* L10N: The compose menu autocrypt prompt.
     (e)ncrypt enables encryption via autocrypt.
     (c)lear sets cleartext.
     (a)utomatic defers to the recommendation.  */
  const char *prompt = _("Autocrypt: (e)ncrypt, (c)lear, (a)utomatic?");

  e->security |= APPLICATION_PGP;

  /* L10N: The letter corresponding to the compose menu autocrypt prompt
     (e)ncrypt, (c)lear, (a)utomatic */
  const char *letters = _("eca");

  int choice = mutt_multi_choice(prompt, letters);
  switch (choice)
  {
    case 1:
      e->security |= (SEC_AUTOCRYPT | SEC_AUTOCRYPT_OVERRIDE);
      e->security &= ~(SEC_ENCRYPT | SEC_SIGN | SEC_OPPENCRYPT | SEC_INLINE);
      break;
    case 2:
      e->security &= ~SEC_AUTOCRYPT;
      e->security |= SEC_AUTOCRYPT_OVERRIDE;
      break;
    case 3:
    {
      e->security &= ~SEC_AUTOCRYPT_OVERRIDE;
      const bool c_crypt_opportunistic_encrypt =
          cs_subset_bool(sub, "crypt_opportunistic_encrypt");
      if (c_crypt_opportunistic_encrypt)
        e->security |= SEC_OPPENCRYPT;
      break;
    }
  }
}
#endif

/**
 * draw_floating - Draw a floating label
 * @param win  Window to draw on
 * @param col  Column to draw at
 * @param row  Row to draw at
 * @param text Text to display
 */
static void draw_floating(struct MuttWindow *win, int col, int row, const char *text)
{
  mutt_curses_set_color(MT_COLOR_COMPOSE_HEADER);
  mutt_window_mvprintw(win, col, row, "%s", text);
  mutt_curses_set_color(MT_COLOR_NORMAL);
}

/**
 * draw_header - Draw an aligned label
 * @param win   Window to draw on
 * @param row   Row to draw at
 * @param field Field to display, e.g. #HDR_FROM
 */
static void draw_header(struct MuttWindow *win, int row, enum HeaderField field)
{
  mutt_curses_set_color(MT_COLOR_COMPOSE_HEADER);
  mutt_window_mvprintw(win, 0, row, "%*s", HeaderPadding[field], _(Prompts[field]));
  mutt_curses_set_color(MT_COLOR_NORMAL);
}

/**
 * draw_header_content - Draw content on a separate line aligned to header prompt
 * @param win     Window to draw on
 * @param row     Row to draw at
 * @param field   Field to display, e.g. #HDR_FROM
 * @param content Text to display
 *
 * Content will be truncated if it is wider than the window.
 */
static void draw_header_content(struct MuttWindow *win, int row,
                                enum HeaderField field, const char *content)
{
  mutt_window_move(win, HeaderPadding[field], row);
  mutt_paddstr(win->state.cols - HeaderPadding[field], content);
}

/**
 * calc_address - Calculate how many rows an AddressList will need
 * @param[in]  al    Address List
 * @param[out] slist String list
 * @param[in]  cols Screen columns available
 * @param[out] srows Rows needed
 * @retval num Rows needed
 *
 * @note Number of rows is capped at #MAX_ADDR_ROWS
 */
static int calc_address(struct AddressList *al, struct ListHead *slist,
                        short cols, short *srows)
{
  mutt_list_free(slist);
  mutt_addrlist_write_list(al, slist);

  int rows = 1;
  int addr_len;
  int width_left = cols;
  struct ListNode *next = NULL;
  struct ListNode *np = NULL;
  STAILQ_FOREACH(np, slist, entries)
  {
    next = STAILQ_NEXT(np, entries);
    addr_len = mutt_strwidth(np->data);
    if (next)
      addr_len += 2; // ", "

  try_again:
    if (addr_len >= width_left)
    {
      if (width_left == cols)
        break;

      rows++;
      width_left = cols;
      goto try_again;
    }

    if (addr_len < width_left)
      width_left -= addr_len;
  }

  *srows = MIN(rows, MAX_ADDR_ROWS);
  return *srows;
}

/**
 * calc_security - Calculate how many rows the security info will need
 * @param e    Email
 * @param rows Rows needed
 * @param sub  ConfigSubset
 * @retval num Rows needed
 */
static int calc_security(struct Email *e, short *rows, const struct ConfigSubset *sub)
{
  if ((WithCrypto & (APPLICATION_PGP | APPLICATION_SMIME)) == 0)
    *rows = 0; // Neither PGP nor SMIME are built into NeoMutt
  else if ((e->security & (SEC_ENCRYPT | SEC_SIGN)) != 0)
    *rows = 2; // 'Security:' and 'Sign as:'
  else
    *rows = 1; // Just 'Security:'

#ifdef USE_AUTOCRYPT
  const bool c_autocrypt = cs_subset_bool(sub, "autocrypt");
  if (c_autocrypt)
    *rows += 1;
#endif

  return *rows;
}

/**
 * calc_user_hdrs - Calculate how many rows are needed for user-defined headers
 * @param hdrs Header List
 * @retval num Rows needed, limited to #MAX_USER_HDR_ROWS
 */
static int calc_user_hdrs(const struct ListHead *hdrs)
{
  int rows = 0; /* Don't print at all if no custom headers*/
  struct ListNode *np = NULL;
  STAILQ_FOREACH(np, hdrs, entries)
  {
    if (rows == MAX_USER_HDR_ROWS)
      break;
    rows++;
  }
  return rows;
}

/**
 * calc_envelope - Calculate how many rows the envelope will need
 * @param rd Email and other compose data
 * @retval num Rows needed
 */
static int calc_envelope(struct ComposeRedrawData *rd)
{
  int rows = 4; // 'From:', 'Subject:', 'Reply-To:', 'Fcc:'
#ifdef MIXMASTER
  rows++;
#endif

  struct Email *e = rd->email;
  struct Envelope *env = e->env;
  const int cols = rd->win_envelope->state.cols - MaxHeaderWidth;

#ifdef USE_NNTP
  if (OptNewsSend)
  {
    rows += 2; // 'Newsgroups:' and 'Followup-To:'
    const bool c_x_comment_to = cs_subset_bool(rd->sub, "x_comment_to");
    if (c_x_comment_to)
      rows++;
  }
  else
#endif
  {
    rows += calc_address(&env->to, &rd->to_list, cols, &rd->to_rows);
    rows += calc_address(&env->cc, &rd->cc_list, cols, &rd->cc_rows);
    rows += calc_address(&env->bcc, &rd->bcc_list, cols, &rd->bcc_rows);
  }
  rows += calc_security(e, &rd->sec_rows, rd->sub);
  const bool c_compose_show_user_headers =
      cs_subset_bool(rd->sub, "compose_show_user_headers");
  if (c_compose_show_user_headers)
    rows += calc_user_hdrs(&env->userhdrs);

  return rows;
}

/**
 * redraw_crypt_lines - Update the encryption info in the compose window
 * @param rd  Email and other compose data
 * @param row Window row to start drawing
 */
static int redraw_crypt_lines(struct ComposeRedrawData *rd, int row)
{
  struct Email *e = rd->email;

  draw_header(rd->win_envelope, row++, HDR_CRYPT);

  if ((WithCrypto & (APPLICATION_PGP | APPLICATION_SMIME)) == 0)
    return 0;

  // We'll probably need two lines for 'Security:' and 'Sign as:'
  int used = 2;
  if ((e->security & (SEC_ENCRYPT | SEC_SIGN)) == (SEC_ENCRYPT | SEC_SIGN))
  {
    mutt_curses_set_color(MT_COLOR_COMPOSE_SECURITY_BOTH);
    mutt_window_addstr(_("Sign, Encrypt"));
  }
  else if (e->security & SEC_ENCRYPT)
  {
    mutt_curses_set_color(MT_COLOR_COMPOSE_SECURITY_ENCRYPT);
    mutt_window_addstr(_("Encrypt"));
  }
  else if (e->security & SEC_SIGN)
  {
    mutt_curses_set_color(MT_COLOR_COMPOSE_SECURITY_SIGN);
    mutt_window_addstr(_("Sign"));
  }
  else
  {
    /* L10N: This refers to the encryption of the email, e.g. "Security: None" */
    mutt_curses_set_color(MT_COLOR_COMPOSE_SECURITY_NONE);
    mutt_window_addstr(_("None"));
    used = 1; // 'Sign as:' won't be needed
  }
  mutt_curses_set_color(MT_COLOR_NORMAL);

  if ((e->security & (SEC_ENCRYPT | SEC_SIGN)))
  {
    if (((WithCrypto & APPLICATION_PGP) != 0) && (e->security & APPLICATION_PGP))
    {
      if ((e->security & SEC_INLINE))
        mutt_window_addstr(_(" (inline PGP)"));
      else
        mutt_window_addstr(_(" (PGP/MIME)"));
    }
    else if (((WithCrypto & APPLICATION_SMIME) != 0) && (e->security & APPLICATION_SMIME))
      mutt_window_addstr(_(" (S/MIME)"));
  }

  const bool c_crypt_opportunistic_encrypt =
      cs_subset_bool(rd->sub, "crypt_opportunistic_encrypt");
  if (c_crypt_opportunistic_encrypt && (e->security & SEC_OPPENCRYPT))
    mutt_window_addstr(_(" (OppEnc mode)"));

  mutt_window_clrtoeol(rd->win_envelope);

  if (((WithCrypto & APPLICATION_PGP) != 0) &&
      (e->security & APPLICATION_PGP) && (e->security & SEC_SIGN))
  {
    draw_header(rd->win_envelope, row++, HDR_CRYPTINFO);
    const char *c_pgp_sign_as = cs_subset_string(rd->sub, "pgp_sign_as");
    mutt_window_printf("%s", c_pgp_sign_as ? c_pgp_sign_as : _("<default>"));
  }

  if (((WithCrypto & APPLICATION_SMIME) != 0) &&
      (e->security & APPLICATION_SMIME) && (e->security & SEC_SIGN))
  {
    draw_header(rd->win_envelope, row++, HDR_CRYPTINFO);
    const char *c_smime_sign_as = cs_subset_string(rd->sub, "pgp_sign_as");
    mutt_window_printf("%s", c_smime_sign_as ? c_smime_sign_as : _("<default>"));
  }

  const char *c_smime_encrypt_with =
      cs_subset_string(rd->sub, "smime_encrypt_with");
  if (((WithCrypto & APPLICATION_SMIME) != 0) && (e->security & APPLICATION_SMIME) &&
      (e->security & SEC_ENCRYPT) && c_smime_encrypt_with)
  {
    draw_floating(rd->win_envelope, 40, row - 1, _("Encrypt with: "));
    mutt_window_printf("%s", NONULL(c_smime_encrypt_with));
  }

#ifdef USE_AUTOCRYPT
  const bool c_autocrypt = cs_subset_bool(rd->sub, "autocrypt");
  if (c_autocrypt)
  {
    draw_header(rd->win_envelope, row, HDR_AUTOCRYPT);
    if (e->security & SEC_AUTOCRYPT)
    {
      mutt_curses_set_color(MT_COLOR_COMPOSE_SECURITY_ENCRYPT);
      mutt_window_addstr(_("Encrypt"));
    }
    else
    {
      mutt_curses_set_color(MT_COLOR_COMPOSE_SECURITY_NONE);
      mutt_window_addstr(_("Off"));
    }

    /* L10N: The autocrypt compose menu Recommendation field.
       Displays the output of the recommendation engine
       (Off, No, Discouraged, Available, Yes) */
    draw_floating(rd->win_envelope, 40, row, _("Recommendation: "));
    mutt_window_printf("%s", _(AutocryptRecUiFlags[rd->autocrypt_rec]));

    used++;
  }
#endif
  return used;
}

/**
 * update_crypt_info - Update the crypto info
 * @param rd Email and other compose data
 */
static void update_crypt_info(struct ComposeRedrawData *rd)
{
  struct Email *e = rd->email;

  const bool c_crypt_opportunistic_encrypt =
      cs_subset_bool(rd->sub, "crypt_opportunistic_encrypt");
  if (c_crypt_opportunistic_encrypt)
    crypt_opportunistic_encrypt(e);

#ifdef USE_AUTOCRYPT
  const bool c_autocrypt = cs_subset_bool(rd->sub, "autocrypt");
  if (c_autocrypt)
  {
    rd->autocrypt_rec = mutt_autocrypt_ui_recommendation(e, NULL);

    /* Anything that enables SEC_ENCRYPT or SEC_SIGN, or turns on SMIME
     * overrides autocrypt, be it oppenc or the user having turned on
     * those flags manually. */
    if (e->security & (SEC_ENCRYPT | SEC_SIGN | APPLICATION_SMIME))
      e->security &= ~(SEC_AUTOCRYPT | SEC_AUTOCRYPT_OVERRIDE);
    else
    {
      if (!(e->security & SEC_AUTOCRYPT_OVERRIDE))
      {
        if (rd->autocrypt_rec == AUTOCRYPT_REC_YES)
        {
          e->security |= (SEC_AUTOCRYPT | APPLICATION_PGP);
          e->security &= ~(SEC_INLINE | APPLICATION_SMIME);
        }
        else
          e->security &= ~SEC_AUTOCRYPT;
      }
    }
  }
#endif
}

#ifdef MIXMASTER
/**
 * redraw_mix_line - Redraw the Mixmaster chain
 * @param chain List of chain links
 * @param rd    Email and other compose data
 * @param row   Window row to start drawing
 */
static void redraw_mix_line(struct ListHead *chain, struct ComposeRedrawData *rd, int row)
{
  char *t = NULL;

  draw_header(rd->win_envelope, row, HDR_MIX);

  if (STAILQ_EMPTY(chain))
  {
    mutt_window_addstr(_("<no chain defined>"));
    mutt_window_clrtoeol(rd->win_envelope);
    return;
  }

  int c = 12;
  struct ListNode *np = NULL;
  STAILQ_FOREACH(np, chain, entries)
  {
    t = np->data;
    if (t && (t[0] == '0') && (t[1] == '\0'))
      t = "<random>";

    if (c + mutt_str_len(t) + 2 >= rd->win_envelope->state.cols)
      break;

    mutt_window_addstr(NONULL(t));
    if (STAILQ_NEXT(np, entries))
      mutt_window_addstr(", ");

    c += mutt_str_len(t) + 2;
  }
}
#endif

/**
 * check_attachments - Check if any attachments have changed or been deleted
 * @param actx Attachment context
 * @param sub  ConfigSubset
 * @retval  0 Success
 * @retval -1 Error
 */
static int check_attachments(struct AttachCtx *actx, struct ConfigSubset *sub)
{
  int rc = -1;
  struct stat st;
  struct Buffer *pretty = NULL, *msg = NULL;

  for (int i = 0; i < actx->idxlen; i++)
  {
    if (actx->idx[i]->body->type == TYPE_MULTIPART)
      continue;
    if (stat(actx->idx[i]->body->filename, &st) != 0)
    {
      if (!pretty)
        pretty = mutt_buffer_pool_get();
      mutt_buffer_strcpy(pretty, actx->idx[i]->body->filename);
      mutt_buffer_pretty_mailbox(pretty);
      /* L10N: This message is displayed in the compose menu when an attachment
         doesn't stat.  %d is the attachment number and %s is the attachment
         filename.  The filename is located last to avoid a long path hiding
         the error message.  */
      mutt_error(_("Attachment #%d no longer exists: %s"), i + 1, mutt_b2s(pretty));
      goto cleanup;
    }

    if (actx->idx[i]->body->stamp < st.st_mtime)
    {
      if (!pretty)
        pretty = mutt_buffer_pool_get();
      mutt_buffer_strcpy(pretty, actx->idx[i]->body->filename);
      mutt_buffer_pretty_mailbox(pretty);

      if (!msg)
        msg = mutt_buffer_pool_get();
      /* L10N: This message is displayed in the compose menu when an attachment
         is modified behind the scenes.  %d is the attachment number and %s is
         the attachment filename.  The filename is located last to avoid a long
         path hiding the prompt question.  */
      mutt_buffer_printf(msg, _("Attachment #%d modified. Update encoding for %s?"),
                         i + 1, mutt_b2s(pretty));

      enum QuadOption ans = mutt_yesorno(mutt_b2s(msg), MUTT_YES);
      if (ans == MUTT_YES)
        mutt_update_encoding(actx->idx[i]->body, sub);
      else if (ans == MUTT_ABORT)
        goto cleanup;
    }
  }

  rc = 0;

cleanup:
  mutt_buffer_pool_release(&pretty);
  mutt_buffer_pool_release(&msg);
  return rc;
}

/**
 * draw_envelope_addr - Write addresses to the compose window
 * @param field     Field to display, e.g. #HDR_FROM
 * @param al        Address list to write
 * @param win       Window
 * @param row       Window row to start drawing
 * @param max_lines How many lines may be used
 */
static int draw_envelope_addr(int field, struct AddressList *al,
                              struct MuttWindow *win, int row, size_t max_lines)
{
  draw_header(win, row, field);

  struct ListHead list = STAILQ_HEAD_INITIALIZER(list);
  int count = mutt_addrlist_write_list(al, &list);

  int lines_used = 1;
  int width_left = win->state.cols - MaxHeaderWidth;
  char more[32];
  int more_len = 0;

  char *sep = NULL;
  struct ListNode *next = NULL;
  struct ListNode *np = NULL;
  STAILQ_FOREACH(np, &list, entries)
  {
    next = STAILQ_NEXT(np, entries);
    int addr_len = mutt_strwidth(np->data);
    if (next)
    {
      sep = ", ";
      addr_len += 2;
    }
    else
    {
      sep = "";
    }

    count--;
  try_again:
    more_len = snprintf(more, sizeof(more),
                        ngettext("(+%d more)", "(+%d more)", count), count);
    mutt_debug(LL_DEBUG3, "text: '%s'  len: %d\n", more, more_len);

    int reserve = ((count > 0) && (lines_used == max_lines)) ? more_len : 0;
    mutt_debug(LL_DEBUG3, "processing: %s (al:%ld, wl:%d, r:%d, lu:%ld)\n",
               np->data, addr_len, width_left, reserve, lines_used);
    if (addr_len >= (width_left - reserve))
    {
      mutt_debug(LL_DEBUG3, "not enough space\n");
      if (lines_used == max_lines)
      {
        mutt_debug(LL_DEBUG3, "no more lines\n");
        mutt_debug(LL_DEBUG3, "truncating: %s\n", np->data);
        mutt_paddstr(width_left, np->data);
        break;
      }

      if (width_left == (win->state.cols - MaxHeaderWidth))
      {
        mutt_debug(LL_DEBUG3, "couldn't print: %s\n", np->data);
        mutt_paddstr(width_left, np->data);
        break;
      }

      mutt_debug(LL_DEBUG3, "start a new line\n");
      mutt_window_clrtoeol(win);
      row++;
      lines_used++;
      width_left = win->state.cols - MaxHeaderWidth;
      mutt_window_move(win, MaxHeaderWidth, row);
      goto try_again;
    }

    if (addr_len < width_left)
    {
      mutt_debug(LL_DEBUG3, "space for: %s\n", np->data);
      mutt_window_addstr(np->data);
      mutt_window_addstr(sep);
      width_left -= addr_len;
    }
    mutt_debug(LL_DEBUG3, "%ld addresses remaining\n", count);
    mutt_debug(LL_DEBUG3, "%ld lines remaining\n", max_lines - lines_used);
  }
  mutt_list_free(&list);

  if (count > 0)
  {
    mutt_window_move(win, win->state.cols - more_len, row);
    mutt_curses_set_color(MT_COLOR_BOLD);
    mutt_window_addstr(more);
    mutt_curses_set_color(MT_COLOR_NORMAL);
    mutt_debug(LL_DEBUG3, "%ld more (len %d)\n", count, more_len);
  }
  else
  {
    mutt_window_clrtoeol(win);
  }

  for (int i = lines_used; i < max_lines; i++)
  {
    mutt_window_move(win, 0, row + i);
    mutt_window_clrtoeol(win);
  }

  mutt_debug(LL_DEBUG3, "used %d lines\n", lines_used);
  return lines_used;
}

/**
 * draw_envelope_user_hdrs - Write user-defined headers to the compose window
 * @param rd  Email and other compose data
 * @param row Window row to start drawing from
 */
static int draw_envelope_user_hdrs(const struct ComposeRedrawData *rd, int row)
{
  const char *overflow_text = "...";
  int rows_used = 0;

  struct ListNode *first = STAILQ_FIRST(&rd->email->env->userhdrs);
  if (!first)
    return rows_used;

  /* Draw first entry on same line as prompt */
  draw_header(rd->win_envelope, row, HDR_CUSTOM_HEADERS);
  mutt_paddstr(rd->win_envelope->state.cols -
                   (HeaderPadding[HDR_CUSTOM_HEADERS] +
                    mutt_strwidth(_(Prompts[HDR_CUSTOM_HEADERS]))),
               first->data);
  rows_used++;

  /* Draw any following entries on their own line */
  struct ListNode *np = STAILQ_NEXT(first, entries);
  if (!np)
    return rows_used;

  STAILQ_FOREACH_FROM(np, &rd->email->env->userhdrs, entries)
  {
    if ((rows_used == (MAX_USER_HDR_ROWS - 1)) && STAILQ_NEXT(np, entries))
    {
      draw_header_content(rd->win_envelope, row + rows_used, HDR_CUSTOM_HEADERS, overflow_text);
      rows_used++;
      break;
    }
    draw_header_content(rd->win_envelope, row + rows_used, HDR_CUSTOM_HEADERS, np->data);
    rows_used++;
  }
  return rows_used;
}

/**
 * draw_envelope - Write the email headers to the compose window
 * @param rd  Email and other compose data
 */
static void draw_envelope(struct ComposeRedrawData *rd)
{
  struct Email *e = rd->email;
  const char *fcc = mutt_b2s(rd->fcc);
  const int cols = rd->win_envelope->state.cols - MaxHeaderWidth;

  mutt_window_clear(rd->win_envelope);
  int row = draw_envelope_addr(HDR_FROM, &e->env->from, rd->win_envelope, 0, 1);

#ifdef USE_NNTP
  if (OptNewsSend)
  {
    draw_header(rd->win_envelope, row++, HDR_NEWSGROUPS);
    mutt_paddstr(cols, NONULL(e->env->newsgroups));

    draw_header(rd->win_envelope, row++, HDR_FOLLOWUPTO);
    mutt_paddstr(cols, NONULL(e->env->followup_to));

    const bool c_x_comment_to = cs_subset_bool(rd->sub, "x_comment_to");
    if (c_x_comment_to)
    {
      draw_header(rd->win_envelope, row++, HDR_XCOMMENTTO);
      mutt_paddstr(cols, NONULL(e->env->x_comment_to));
    }
  }
  else
#endif
  {
    row += draw_envelope_addr(HDR_TO, &e->env->to, rd->win_envelope, row, rd->to_rows);
    row += draw_envelope_addr(HDR_CC, &e->env->cc, rd->win_envelope, row, rd->cc_rows);
    row += draw_envelope_addr(HDR_BCC, &e->env->bcc, rd->win_envelope, row, rd->bcc_rows);
  }

  draw_header(rd->win_envelope, row++, HDR_SUBJECT);
  mutt_paddstr(cols, NONULL(e->env->subject));

  row += draw_envelope_addr(HDR_REPLYTO, &e->env->reply_to, rd->win_envelope, row, 1);

  draw_header(rd->win_envelope, row++, HDR_FCC);
  mutt_paddstr(cols, fcc);

  if (WithCrypto)
    row += redraw_crypt_lines(rd, row);

#ifdef MIXMASTER
  redraw_mix_line(&e->chain, rd, row++);
#endif
  const bool c_compose_show_user_headers =
      cs_subset_bool(rd->sub, "compose_show_user_headers");
  if (c_compose_show_user_headers)
    row += draw_envelope_user_hdrs(rd, row);

  mutt_curses_set_color(MT_COLOR_STATUS);
  mutt_window_mvaddstr(rd->win_abar, 0, 0, _("-- Attachments"));
  mutt_window_clrtoeol(rd->win_abar);
  mutt_curses_set_color(MT_COLOR_NORMAL);
}

/**
 * edit_address_list - Let the user edit the address list
 * @param[in]     field Field to edit, e.g. #HDR_FROM
 * @param[in,out] al    AddressList to edit
 * @retval bool true if the address list was changed
 */
static bool edit_address_list(int field, struct AddressList *al)
{
  char buf[8192] = { 0 }; /* needs to be large for alias expansion */
  char old_list[8192] = { 0 };

  mutt_addrlist_to_local(al);
  mutt_addrlist_write(al, buf, sizeof(buf), false);
  mutt_str_copy(old_list, buf, sizeof(buf));
  if (mutt_get_field(_(Prompts[field]), buf, sizeof(buf), MUTT_ALIAS) == 0)
  {
    mutt_addrlist_clear(al);
    mutt_addrlist_parse2(al, buf);
    mutt_expand_aliases(al);
  }

  char *err = NULL;
  if (mutt_addrlist_to_intl(al, &err) != 0)
  {
    mutt_error(_("Bad IDN: '%s'"), err);
    mutt_refresh();
    FREE(&err);
  }

  return !mutt_str_equal(buf, old_list);
}

/**
 * delete_attachment - Delete an attachment
 * @param actx Attachment context
 * @param x    Index number of attachment
 * @retval  0 Success
 * @retval -1 Error
 */
static int delete_attachment(struct AttachCtx *actx, int x)
{
  struct AttachPtr **idx = actx->idx;
  int rindex = actx->v2r[x];

  if ((rindex == 0) && (actx->idxlen == 1))
  {
    mutt_error(_("You may not delete the only attachment"));
    idx[rindex]->body->tagged = false;
    return -1;
  }

  for (int y = 0; y < actx->idxlen; y++)
  {
    if (idx[y]->body->next == idx[rindex]->body)
    {
      idx[y]->body->next = idx[rindex]->body->next;
      break;
    }
  }

  idx[rindex]->body->next = NULL;
  /* mutt_make_message_attach() creates body->parts, shared by
   * body->email->body.  If we NULL out that, it creates a memory
   * leak because mutt_free_body() frees body->parts, not
   * body->email->body.
   *
   * Other mutt_send_message() message constructors are careful to free
   * any body->parts, removing depth:
   *  - mutt_prepare_template() used by postponed, resent, and draft files
   *  - mutt_copy_body() used by the recvattach menu and $forward_attachments.
   *
   * I believe it is safe to completely remove the "body->parts =
   * NULL" statement.  But for safety, am doing so only for the case
   * it must be avoided: message attachments.
   */
  if (!idx[rindex]->body->email)
    idx[rindex]->body->parts = NULL;
  mutt_body_free(&(idx[rindex]->body));
  FREE(&idx[rindex]->tree);
  FREE(&idx[rindex]);
  for (; rindex < actx->idxlen - 1; rindex++)
    idx[rindex] = idx[rindex + 1];
  idx[actx->idxlen - 1] = NULL;
  actx->idxlen--;

  return 0;
}

/**
 * mutt_gen_compose_attach_list - Generate the attachment list for the compose screen
 * @param actx        Attachment context
 * @param m           Attachment
 * @param parent_type Attachment type, e.g #TYPE_MULTIPART
 * @param level       Nesting depth of attachment
 */
static void mutt_gen_compose_attach_list(struct AttachCtx *actx, struct Body *m,
                                         int parent_type, int level)
{
  for (; m; m = m->next)
  {
    if ((m->type == TYPE_MULTIPART) && m->parts &&
        (!(WithCrypto & APPLICATION_PGP) || !mutt_is_multipart_encrypted(m)))
    {
      mutt_gen_compose_attach_list(actx, m->parts, m->type, level);
    }
    else
    {
      struct AttachPtr *ap = mutt_mem_calloc(1, sizeof(struct AttachPtr));
      mutt_actx_add_attach(actx, ap);
      ap->body = m;
      m->aptr = ap;
      ap->parent_type = parent_type;
      ap->level = level;

      /* We don't support multipart messages in the compose menu yet */
    }
  }
}

/**
 * mutt_update_compose_menu - Redraw the compose window
 * @param actx Attachment context
 * @param menu Current menu
 * @param init If true, initialise the attachment list
 */
static void mutt_update_compose_menu(struct AttachCtx *actx, struct Menu *menu, bool init)
{
  if (init)
  {
    mutt_gen_compose_attach_list(actx, actx->email->body, -1, 0);
    mutt_attach_init(actx);
    menu->mdata = actx;
  }

  mutt_update_tree(actx);

  menu->max = actx->vcount;
  if (menu->max)
  {
    if (menu->current >= menu->max)
      menu->current = menu->max - 1;
  }
  else
    menu->current = 0;

  menu->redraw |= REDRAW_INDEX | REDRAW_STATUS;
}

/**
 * update_idx - Add a new attchment to the message
 * @param menu Current menu
 * @param actx Attachment context
 * @param ap   Attachment to add
 */
static void update_idx(struct Menu *menu, struct AttachCtx *actx, struct AttachPtr *ap)
{
  ap->level = (actx->idxlen > 0) ? actx->idx[actx->idxlen - 1]->level : 0;
  if (actx->idxlen)
    actx->idx[actx->idxlen - 1]->body->next = ap->body;
  ap->body->aptr = ap;
  mutt_actx_add_attach(actx, ap);
  mutt_update_compose_menu(actx, menu, false);
  menu->current = actx->vcount - 1;
}

/**
 * insert_idx - Insert a new attachment into the message at specified index position
 * @param menu Current menu
 * @param actx Attachment context
 * @param ap   Attachment to add
 * @param aidx Index position to insert attachment
 */
static void insert_idx(struct Menu *menu, struct AttachCtx *actx, struct AttachPtr *ap, short aidx)
{
  if (aidx > 0 && ap->level == actx->idx[aidx - 1]->level)
    actx->idx[aidx - 1]->body->next = ap->body;
  if (aidx < actx->idxlen && ap->level == actx->idx[aidx]->level)
    ap->body->next = actx->idx[aidx]->body;
  ap->body->aptr = ap;
  mutt_actx_ins_attach(actx, ap, aidx);
  mutt_update_compose_menu(actx, menu, false);
  menu->current = aidx;
}

/**
 * compose_custom_redraw - Redraw the compose menu - Implements Menu::custom_redraw()
 */
static void compose_custom_redraw(struct Menu *menu)
{
  struct ComposeRedrawData *rd = menu->redraw_data;
  if (!rd)
    return;

  if (menu->redraw & REDRAW_FLOW)
  {
    rd->win_envelope->req_rows = calc_envelope(rd);
    mutt_window_reflow(dialog_find(rd->win_envelope));
  }

  if (menu->redraw & REDRAW_FULL)
  {
    menu_redraw_full(menu);
    draw_envelope(rd);
    menu->pagelen = menu->win_index->state.rows;
  }

  menu_check_recenter(menu);

  if (menu->redraw & REDRAW_STATUS)
  {
    char buf[1024];
    const char *c_compose_format = cs_subset_string(rd->sub, "compose_format");
    compose_status_line(buf, sizeof(buf), 0, menu->win_ibar->state.cols, menu,
                        NONULL(c_compose_format));
    mutt_window_move(menu->win_ibar, 0, 0);
    mutt_curses_set_color(MT_COLOR_STATUS);
    mutt_draw_statusline(menu->win_ibar->state.cols, buf, sizeof(buf));
    mutt_curses_set_color(MT_COLOR_NORMAL);
    menu->redraw &= ~REDRAW_STATUS;
  }

  if (menu->redraw & REDRAW_INDEX)
    menu_redraw_index(menu);
  else if (menu->redraw & (REDRAW_MOTION | REDRAW_MOTION_RESYNC))
    menu_redraw_motion(menu);
  else if (menu->redraw == REDRAW_CURRENT)
    menu_redraw_current(menu);
  menu->redraw = REDRAW_NO_FLAGS;
}

/**
 * compose_attach_swap - Swap two adjacent entries in the attachment list
 * @param[in]  msg   Body of email
 * @param[out] idx   Array of Attachments
 * @param[in]  first Index of first attachment to swap
 */
static void compose_attach_swap(struct Body *msg, struct AttachPtr **idx, short first)
{
  /* Reorder Body pointers.
   * Must traverse msg from top since Body has no previous ptr.  */
  for (struct Body *part = msg; part; part = part->next)
  {
    if (part->next == idx[first]->body)
    {
      idx[first]->body->next = idx[first + 1]->body->next;
      idx[first + 1]->body->next = idx[first]->body;
      part->next = idx[first + 1]->body;
      break;
    }
  }

  /* Reorder index */
  struct AttachPtr *saved = idx[first];
  idx[first] = idx[first + 1];
  idx[first + 1] = saved;

  /* Swap ptr->num */
  int i = idx[first]->num;
  idx[first]->num = idx[first + 1]->num;
  idx[first + 1]->num = i;
}

/**
 * cum_attachs_size - Cumulative Attachments Size
 * @param menu Menu listing attachments
 * @retval num Bytes in attachments
 *
 * Returns the total number of bytes used by the attachments in the attachment
 * list _after_ content-transfer-encodings have been applied.
 */
static unsigned long cum_attachs_size(struct Menu *menu)
{
  size_t s = 0;
  struct AttachCtx *actx = menu->mdata;
  struct AttachPtr **idx = actx->idx;
  struct Content *info = NULL;
  struct Body *b = NULL;
  struct ComposeRedrawData *rd = (struct ComposeRedrawData *) menu->redraw_data;
  struct ConfigSubset *sub = rd->sub;

  for (unsigned short i = 0; i < actx->idxlen; i++)
  {
    b = idx[i]->body;

    if (!b->content)
      b->content = mutt_get_content_info(b->filename, b, sub);

    info = b->content;
    if (info)
    {
      switch (b->encoding)
      {
        case ENC_QUOTED_PRINTABLE:
          s += 3 * (info->lobin + info->hibin) + info->ascii + info->crlf;
          break;
        case ENC_BASE64:
          s += (4 * (info->lobin + info->hibin + info->ascii + info->crlf)) / 3;
          break;
        default:
          s += info->lobin + info->hibin + info->ascii + info->crlf;
          break;
      }
    }
  }

  return s;
}

/**
 * compose_format_str - Create the status bar string for compose mode - Implements ::format_t
 *
 * | Expando | Description
 * |:--------|:--------------------------------------------------------
 * | \%a     | Total number of attachments
 * | \%h     | Local hostname
 * | \%l     | Approximate size (in bytes) of the current message
 * | \%v     | NeoMutt version string
 */
static const char *compose_format_str(char *buf, size_t buflen, size_t col, int cols,
                                      char op, const char *src, const char *prec,
                                      const char *if_str, const char *else_str,
                                      intptr_t data, MuttFormatFlags flags)
{
  char fmt[128], tmp[128];
  bool optional = (flags & MUTT_FORMAT_OPTIONAL);
  struct Menu *menu = (struct Menu *) data;

  *buf = '\0';
  switch (op)
  {
    case 'a': /* total number of attachments */
      snprintf(fmt, sizeof(fmt), "%%%sd", prec);
      snprintf(buf, buflen, fmt, menu->max);
      break;

    case 'h': /* hostname */
      snprintf(fmt, sizeof(fmt), "%%%ss", prec);
      snprintf(buf, buflen, fmt, NONULL(ShortHostname));
      break;

    case 'l': /* approx length of current message in bytes */
      snprintf(fmt, sizeof(fmt), "%%%ss", prec);
      mutt_str_pretty_size(tmp, sizeof(tmp), menu ? cum_attachs_size(menu) : 0);
      snprintf(buf, buflen, fmt, tmp);
      break;

    case 'v':
      snprintf(buf, buflen, "%s", mutt_make_version());
      break;

    case 0:
      *buf = '\0';
      return src;

    default:
      snprintf(buf, buflen, "%%%s%c", prec, op);
      break;
  }

  if (optional)
    compose_status_line(buf, buflen, col, cols, menu, if_str);
  else if (flags & MUTT_FORMAT_OPTIONAL)
    compose_status_line(buf, buflen, col, cols, menu, else_str);

  return src;
}

/**
 * compose_status_line - Compose the string for the status bar
 * @param[out] buf    Buffer in which to save string
 * @param[in]  buflen Buffer length
 * @param[in]  col    Starting column
 * @param[in]  cols   Number of screen columns
 * @param[in]  menu   Current menu
 * @param[in]  src    Printf-like format string
 */
static void compose_status_line(char *buf, size_t buflen, size_t col, int cols,
                                struct Menu *menu, const char *src)
{
  mutt_expando_format(buf, buflen, col, cols, src, compose_format_str,
                      (intptr_t) menu, MUTT_FORMAT_NO_FLAGS);
}

/**
 * compose_config_observer - Listen for config changes affecting the Compose menu - Implements ::observer_t
 */
static int compose_config_observer(struct NotifyCallback *nc)
{
  if (!nc->event_data || !nc->global_data)
    return -1;
  if (nc->event_type != NT_CONFIG)
    return 0;

  struct EventConfig *ec = nc->event_data;
  struct MuttWindow *dlg = nc->global_data;

  if (!mutt_str_equal(ec->name, "status_on_top"))
    return 0;

  struct MuttWindow *win_ebar = mutt_window_find(dlg, WT_INDEX_BAR);
  if (!win_ebar)
    return 0;

  TAILQ_REMOVE(&dlg->children, win_ebar, entries);

  const bool c_status_on_top = cs_subset_bool(ec->sub, "status_on_top");
  if (c_status_on_top)
    TAILQ_INSERT_HEAD(&dlg->children, win_ebar, entries);
  else
    TAILQ_INSERT_TAIL(&dlg->children, win_ebar, entries);

  mutt_window_reflow(dlg);
  return 0;
}

/**
 * compose_header_observer - Listen for header changes - Implements ::observer_t
 */
static int compose_header_observer(struct NotifyCallback *nc)
{
  if ((nc->event_type != NT_HEADER) || !nc->event_data || !nc->global_data)
    return -1;

  const struct EventHeader *event = nc->event_data;
  struct ComposeRedrawData *rd = nc->global_data;
  struct Envelope *env = rd->email->env;
  struct MuttWindow *dlg = rd->win_envelope->parent;

  if ((nc->event_subtype == NT_HEADER_ADD) || (nc->event_subtype == NT_HEADER_CHANGE))
  {
    header_set(&env->userhdrs, event->header);
    mutt_window_reflow(dlg);
    return 0;
  }
  if (nc->event_subtype == NT_HEADER_REMOVE)
  {
    struct ListNode *removed = header_find(&env->userhdrs, event->header);
    if (removed)
    {
      header_free(&env->userhdrs, removed);
      mutt_window_reflow(dlg);
    }
    return 0;
  }

  return -1;
}

/**
 * mutt_compose_menu - Allow the user to edit the message envelope
 * @param e      Email to fill
 * @param fcc    Buffer to save FCC
 * @param e_cur  Current message
 * @param flags  Flags, e.g. #MUTT_COMPOSE_NOFREEHEADER
 * @param sub    ConfigSubset
 * @retval  1 Message should be postponed
 * @retval  0 Normal exit
 * @retval -1 Abort message
 */
int mutt_compose_menu(struct Email *e, struct Buffer *fcc, struct Email *e_cur,
                      uint8_t flags, struct ConfigSubset *sub)
{
  char buf[PATH_MAX];
  int rc = -1;
  bool loop = true;
  bool fcc_set = false; /* has the user edited the Fcc: field ? */
  struct ComposeRedrawData redraw = { 0 };

  STAILQ_INIT(&redraw.to_list);
  STAILQ_INIT(&redraw.cc_list);
  STAILQ_INIT(&redraw.bcc_list);

  struct ComposeRedrawData *rd = &redraw;
#ifdef USE_NNTP
  bool news = OptNewsSend; /* is it a news article ? */
#endif

  init_header_padding();

  struct MuttWindow *dlg =
      mutt_window_new(WT_DLG_COMPOSE, MUTT_WIN_ORIENT_VERTICAL, MUTT_WIN_SIZE_MAXIMISE,
                      MUTT_WIN_SIZE_UNLIMITED, MUTT_WIN_SIZE_UNLIMITED);

  struct MuttWindow *envelope =
      mutt_window_new(WT_CUSTOM, MUTT_WIN_ORIENT_VERTICAL, MUTT_WIN_SIZE_FIXED,
                      MUTT_WIN_SIZE_UNLIMITED, HDR_ATTACH_TITLE - 1);

  struct MuttWindow *abar =
      mutt_window_new(WT_CUSTOM, MUTT_WIN_ORIENT_VERTICAL, MUTT_WIN_SIZE_FIXED,
                      MUTT_WIN_SIZE_UNLIMITED, 1);

  struct MuttWindow *attach =
      mutt_window_new(WT_INDEX, MUTT_WIN_ORIENT_VERTICAL, MUTT_WIN_SIZE_MAXIMISE,
                      MUTT_WIN_SIZE_UNLIMITED, MUTT_WIN_SIZE_UNLIMITED);

  struct MuttWindow *ebar =
      mutt_window_new(WT_INDEX_BAR, MUTT_WIN_ORIENT_VERTICAL,
                      MUTT_WIN_SIZE_FIXED, MUTT_WIN_SIZE_UNLIMITED, 1);

  rd->email = e;
  rd->fcc = fcc;
  rd->win_envelope = envelope;
  rd->win_cbar = ebar;
  rd->win_attach = attach;
  rd->win_abar = abar;
  rd->sub = sub;

  const bool c_status_on_top = cs_subset_bool(sub, "status_on_top");
  if (c_status_on_top)
  {
    mutt_window_add_child(dlg, ebar);
    mutt_window_add_child(dlg, envelope);
    mutt_window_add_child(dlg, abar);
    mutt_window_add_child(dlg, attach);
  }
  else
  {
    mutt_window_add_child(dlg, envelope);
    mutt_window_add_child(dlg, abar);
    mutt_window_add_child(dlg, attach);
    mutt_window_add_child(dlg, ebar);
  }

  notify_observer_add(NeoMutt->notify, NT_CONFIG, compose_config_observer, dlg);
  notify_observer_add(NeoMutt->notify, NT_HEADER, compose_header_observer, rd);
  dialog_push(dlg);

#ifdef USE_NNTP
  if (news)
    dlg->help_data = ComposeNewsHelp;
  else
#endif
    dlg->help_data = ComposeHelp;
  dlg->help_menu = MENU_COMPOSE;

  envelope->req_rows = calc_envelope(rd);
  mutt_window_reflow(dlg);

  struct Menu *menu = mutt_menu_new(MENU_COMPOSE);

  menu->pagelen = attach->state.rows;
  menu->win_index = attach;
  menu->win_ibar = ebar;

  menu->make_entry = snd_make_entry;
  menu->tag = attach_tag;
  menu->custom_redraw = compose_custom_redraw;
  menu->redraw_data = rd;
  mutt_menu_push_current(menu);

  struct AttachCtx *actx = mutt_actx_new();
  actx->email = e;
  mutt_update_compose_menu(actx, menu, true);

  update_crypt_info(rd);

  /* Since this is rather long lived, we don't use the pool */
  struct Buffer fname = mutt_buffer_make(PATH_MAX);

  bool redraw_env = false;
  while (loop)
  {
    if (redraw_env)
    {
      redraw_env = false;
      envelope->req_rows = calc_envelope(rd);
      mutt_window_reflow(dlg);
    }

#ifdef USE_NNTP
    OptNews = false; /* for any case */
#endif
    const int op = mutt_menu_loop(menu);
    if (op >= 0)
      mutt_debug(LL_DEBUG1, "Got op %s (%d)\n", OpStrings[op][0], op);
    switch (op)
    {
      case OP_COMPOSE_EDIT_FROM:
        if (edit_address_list(HDR_FROM, &e->env->from))
        {
          update_crypt_info(rd);
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          redraw_env = true;
        }
        break;

      case OP_COMPOSE_EDIT_TO:
      {
#ifdef USE_NNTP
        if (news)
          break;
#endif
        if (edit_address_list(HDR_TO, &e->env->to))
        {
          update_crypt_info(rd);
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          redraw_env = true;
        }
        break;
      }

      case OP_COMPOSE_EDIT_BCC:
      {
#ifdef USE_NNTP
        if (news)
          break;
#endif
        if (edit_address_list(HDR_BCC, &e->env->bcc))
        {
          update_crypt_info(rd);
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          redraw_env = true;
        }
        break;
      }

      case OP_COMPOSE_EDIT_CC:
      {
#ifdef USE_NNTP
        if (news)
          break;
#endif
        if (edit_address_list(HDR_CC, &e->env->cc))
        {
          update_crypt_info(rd);
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          redraw_env = true;
        }
        break;
      }

#ifdef USE_NNTP
      case OP_COMPOSE_EDIT_NEWSGROUPS:
        if (!news)
          break;
        mutt_str_copy(buf, e->env->newsgroups, sizeof(buf));
        if (mutt_get_field(Prompts[HDR_NEWSGROUPS], buf, sizeof(buf), MUTT_COMP_NO_FLAGS) == 0)
        {
          mutt_str_replace(&e->env->newsgroups, buf);
          redraw_env = true;
        }
        break;

      case OP_COMPOSE_EDIT_FOLLOWUP_TO:
        if (!news)
          break;
        mutt_str_copy(buf, e->env->followup_to, sizeof(buf));
        if (mutt_get_field(Prompts[HDR_FOLLOWUPTO], buf, sizeof(buf), MUTT_COMP_NO_FLAGS) == 0)
        {
          mutt_str_replace(&e->env->followup_to, buf);
          redraw_env = true;
        }
        break;

      case OP_COMPOSE_EDIT_X_COMMENT_TO:
      {
        const bool c_x_comment_to = cs_subset_bool(sub, "x_comment_to");
        if (!(news && c_x_comment_to))
          break;
        mutt_str_copy(buf, e->env->x_comment_to, sizeof(buf));
        if (mutt_get_field(Prompts[HDR_XCOMMENTTO], buf, sizeof(buf), MUTT_COMP_NO_FLAGS) == 0)
        {
          mutt_str_replace(&e->env->x_comment_to, buf);
          redraw_env = true;
        }
        break;
      }
#endif

      case OP_COMPOSE_EDIT_SUBJECT:
        mutt_str_copy(buf, e->env->subject, sizeof(buf));
        if (mutt_get_field(Prompts[HDR_SUBJECT], buf, sizeof(buf), MUTT_COMP_NO_FLAGS) == 0)
        {
          if (!mutt_str_equal(e->env->subject, buf))
          {
            mutt_str_replace(&e->env->subject, buf);
            redraw_env = true;
            mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          }
        }
        break;

      case OP_COMPOSE_EDIT_REPLY_TO:
        if (edit_address_list(HDR_REPLYTO, &e->env->reply_to))
        {
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          redraw_env = true;
        }
        break;

      case OP_COMPOSE_EDIT_FCC:
        mutt_buffer_copy(&fname, fcc);
        if (mutt_buffer_get_field(Prompts[HDR_FCC], &fname, MUTT_FILE | MUTT_CLEAR) == 0)
        {
          if (!mutt_str_equal(fcc->data, fname.data))
          {
            mutt_buffer_copy(fcc, &fname);
            mutt_buffer_pretty_mailbox(fcc);
            fcc_set = true;
            redraw_env = true;
            mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          }
        }
        break;

      case OP_COMPOSE_EDIT_MESSAGE:
      {
        const bool c_edit_headers = cs_subset_bool(sub, "edit_headers");
        if (!c_edit_headers)
        {
          mutt_rfc3676_space_unstuff(e);
          const char *c_editor = cs_subset_string(sub, "editor");
          mutt_edit_file(c_editor, e->body->filename);
          mutt_rfc3676_space_stuff(e);
          mutt_update_encoding(e->body, sub);
          menu->redraw = REDRAW_FULL;
          /* Unconditional hook since editor was invoked */
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          break;
        }
      }
        /* fallthrough */

      case OP_COMPOSE_EDIT_HEADERS:
      {
        mutt_rfc3676_space_unstuff(e);
        const char *tag = NULL;
        char *err = NULL;
        mutt_env_to_local(e->env);
        const char *c_editor = cs_subset_string(sub, "editor");
        mutt_edit_headers(NONULL(c_editor), e->body->filename, e, fcc);
        if (mutt_env_to_intl(e->env, &tag, &err))
        {
          mutt_error(_("Bad IDN in '%s': '%s'"), tag, err);
          FREE(&err);
        }
        update_crypt_info(rd);
        redraw_env = true;

        mutt_rfc3676_space_stuff(e);
        mutt_update_encoding(e->body, sub);

        /* attachments may have been added */
        if (actx->idxlen && actx->idx[actx->idxlen - 1]->body->next)
        {
          mutt_actx_entries_free(actx);
          mutt_update_compose_menu(actx, menu, true);
        }

        menu->redraw = REDRAW_FULL;
        /* Unconditional hook since editor was invoked */
        mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
        break;
      }

      case OP_COMPOSE_ATTACH_KEY:
      {
        if (!(WithCrypto & APPLICATION_PGP))
          break;
        struct AttachPtr *ap = mutt_mem_calloc(1, sizeof(struct AttachPtr));
        ap->body = crypt_pgp_make_key_attachment();
        if (ap->body)
        {
          update_idx(menu, actx, ap);
          menu->redraw |= REDRAW_INDEX;
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
        }
        else
          FREE(&ap);

        menu->redraw |= REDRAW_STATUS;
        break;
      }

      case OP_COMPOSE_MOVE_UP:
        if (menu->current == 0)
        {
          mutt_error(_("Attachment is already at top"));
          break;
        }
        if (menu->current == 1)
        {
          mutt_error(_("The fundamental part can't be moved"));
          break;
        }
        compose_attach_swap(e->body, actx->idx, menu->current - 1);
        menu->redraw |= REDRAW_INDEX;
        menu->current--;
        break;

      case OP_COMPOSE_MOVE_DOWN:
        if (menu->current == (actx->idxlen - 1))
        {
          mutt_error(_("Attachment is already at bottom"));
          break;
        }
        if (menu->current == 0)
        {
          mutt_error(_("The fundamental part can't be moved"));
          break;
        }
        compose_attach_swap(e->body, actx->idx, menu->current);
        menu->redraw |= REDRAW_INDEX;
        menu->current++;
        break;

      case OP_COMPOSE_GROUP_ALTS:
      {
        if (menu->tagged < 2)
        {
          mutt_error(
              _("Grouping 'alternatives' requires at least 2 tagged messages"));
          break;
        }

        struct Body *group = mutt_body_new();
        group->type = TYPE_MULTIPART;
        group->subtype = mutt_str_dup("alternative");
        group->disposition = DISP_INLINE;

        struct Body *alts = NULL;
        /* group tagged message into a multipart/alternative */
        struct Body *bptr = e->body;
        int gidx = 0;
        int glastidx = 0;
        int glevel = 0;
        for (int i = 0; bptr;)
        {
          if (bptr->tagged)
          {
            bptr->tagged = false;
            bptr->disposition = DISP_INLINE;

            /* for first match, set group desc according to match */
#define ALTS_TAG "Alternatives for \"%s\""
            if (!group->description)
            {
              char *p = bptr->description ? bptr->description : bptr->filename;
              if (p)
              {
                group->description =
                    mutt_mem_calloc(1, strlen(p) + strlen(ALTS_TAG) + 1);
                sprintf(group->description, ALTS_TAG, p);
              }
            }

            // append bptr to the alts list, and remove from the e->body list
            if (alts)
            {
              alts->next = bptr;
              bptr = bptr->next;
              alts = alts->next;
              alts->next = NULL;
              // make grouped attachments consecutive
              if (i > glastidx + 1)
              {
                struct AttachPtr *saved = actx->idx[i];
                int saved_num = actx->idx[i]->num;
                for (int j = i; j > glastidx + 1; j--)
                {
                  actx->idx[j]->num += 1;
                  actx->idx[j] = actx->idx[j - 1];
                }
                actx->idx[glastidx + 1] = saved;
                actx->idx[glastidx + 1]->num = saved_num;
                if (actx->idxlen - 1 > i)
                  actx->idx[i]->body->next = actx->idx[i + 1]->body;
                else
                  actx->idx[i]->body->next = NULL;
              }
              glastidx++;
            }
            else
            {
              gidx = i;
              glastidx = i;
              glevel = actx->idx[i]->level;
              group->parts = bptr;
              alts = bptr;
              bptr = bptr->next;
              alts->next = NULL;
            }

            actx->idx[glastidx]->level = glevel + 1;
          }
          else
          {
            bptr = bptr->next;
          }
          i++;
        }

        if (actx->idxlen - 1 > glastidx)
          group->next = actx->idx[glastidx + 1]->body;
        else
          group->next = NULL;
        mutt_generate_boundary(&group->parameter);

        /* if no group desc yet, make one up */
        if (!group->description)
          group->description = mutt_str_dup("unknown alternative group");

        struct AttachPtr *gptr = mutt_mem_calloc(1, sizeof(struct AttachPtr));
        gptr->body = group;
        gptr->level = glevel;
        insert_idx(menu, actx, gptr, gidx);

        /* update e->body pointer */
        e->body = actx->idx[0]->body;

        menu->redraw |= REDRAW_INDEX;
        menu->current = gidx;
        break;
      }

      case OP_COMPOSE_GROUP_LINGUAL:
      {
        if (menu->tagged < 2)
        {
          mutt_error(
              _("Grouping 'multilingual' requires at least 2 tagged messages"));
          break;
        }

        /* traverse to see whether all the parts have Content-Language: set */
        int tagged_with_lang_num = 0;
        for (struct Body *b = e->body; b; b = b->next)
          if (b->tagged && b->language && *b->language)
            tagged_with_lang_num++;

        if (menu->tagged != tagged_with_lang_num)
        {
          if (mutt_yesorno(
                  _("Not all parts have 'Content-Language' set, continue?"), MUTT_YES) != MUTT_YES)
          {
            mutt_message(_("Not sending this message"));
            break;
          }
        }

        struct Body *group = mutt_body_new();
        group->type = TYPE_MULTIPART;
        group->subtype = mutt_str_dup("multilingual");
        group->disposition = DISP_INLINE;

        struct Body *alts = NULL;
        /* group tagged message into a multipart/multilingual */
        struct Body *bptr = e->body;
        for (int i = 0; bptr;)
        {
          if (bptr->tagged)
          {
            bptr->tagged = false;
            bptr->disposition = DISP_INLINE;

            /* for first match, set group desc according to match */
#define LINGUAL_TAG "Multilingual part for \"%s\""
            if (!group->description)
            {
              char *p = bptr->description ? bptr->description : bptr->filename;
              if (p)
              {
                group->description =
                    mutt_mem_calloc(1, strlen(p) + strlen(LINGUAL_TAG) + 1);
                sprintf(group->description, LINGUAL_TAG, p);
              }
            }

            // append bptr to the alts list, and remove from the e->body list
            if (alts)
            {
              alts->next = bptr;
              bptr = bptr->next;
              alts = alts->next;
              alts->next = NULL;
            }
            else
            {
              group->parts = bptr;
              alts = bptr;
              bptr = bptr->next;
              alts->next = NULL;
            }

            for (int j = i; j < actx->idxlen - 1; j++)
            {
              actx->idx[j] = actx->idx[j + 1];
              actx->idx[j + 1] = NULL; /* for debug reason */
            }
            actx->idxlen--;
          }
          else
          {
            bptr = bptr->next;
            i++;
          }
        }

        group->next = NULL;
        mutt_generate_boundary(&group->parameter);

        /* if no group desc yet, make one up */
        if (!group->description)
          group->description = mutt_str_dup("unknown multilingual group");

        struct AttachPtr *gptr = mutt_mem_calloc(1, sizeof(struct AttachPtr));
        gptr->body = group;
        update_idx(menu, actx, gptr);
        menu->redraw |= REDRAW_INDEX;
        break;
      }

      case OP_COMPOSE_ATTACH_FILE:
      {
        char *prompt = _("Attach file");
        int numfiles = 0;
        char **files = NULL;

        mutt_buffer_reset(&fname);
        if ((mutt_buffer_enter_fname_full(prompt, &fname, false, true, &files,
                                          &numfiles, MUTT_SEL_MULTI) == -1) ||
            mutt_buffer_is_empty(&fname))
        {
          for (int i = 0; i < numfiles; i++)
            FREE(&files[i]);

          FREE(&files);
          break;
        }

        bool error = false;
        bool added_attachment = false;
        if (numfiles > 1)
        {
          mutt_message(ngettext("Attaching selected file...",
                                "Attaching selected files...", numfiles));
        }
        for (int i = 0; i < numfiles; i++)
        {
          char *att = files[i];
          struct AttachPtr *ap = mutt_mem_calloc(1, sizeof(struct AttachPtr));
          ap->unowned = true;
          ap->body = mutt_make_file_attach(att, sub);
          if (ap->body)
          {
            added_attachment = true;
            update_idx(menu, actx, ap);
          }
          else
          {
            error = true;
            mutt_error(_("Unable to attach %s"), att);
            FREE(&ap);
          }
          FREE(&files[i]);
        }

        FREE(&files);
        if (!error)
          mutt_clear_error();

        menu->redraw |= REDRAW_INDEX | REDRAW_STATUS;
        if (added_attachment)
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
        break;
      }

      case OP_COMPOSE_ATTACH_MESSAGE:
#ifdef USE_NNTP
      case OP_COMPOSE_ATTACH_NEWS_MESSAGE:
#endif
      {
        mutt_buffer_reset(&fname);
        char *prompt = _("Open mailbox to attach message from");

#ifdef USE_NNTP
        OptNews = false;
        if (Context && (op == OP_COMPOSE_ATTACH_NEWS_MESSAGE))
        {
          const char *c_news_server = cs_subset_string(sub, "news_server");
          CurrentNewsSrv = nntp_select_server(Context->mailbox, c_news_server, false);
          if (!CurrentNewsSrv)
            break;

          prompt = _("Open newsgroup to attach message from");
          OptNews = true;
        }
#endif

        if (Context)
        {
#ifdef USE_NNTP
          if ((op == OP_COMPOSE_ATTACH_MESSAGE) ^ (Context->mailbox->type == MUTT_NNTP))
#endif
          {
            mutt_buffer_strcpy(&fname, mailbox_path(Context->mailbox));
            mutt_buffer_pretty_mailbox(&fname);
          }
        }

        if ((mutt_buffer_enter_fname(prompt, &fname, true) == -1) ||
            mutt_buffer_is_empty(&fname))
        {
          break;
        }

#ifdef USE_NNTP
        if (OptNews)
          nntp_expand_path(fname.data, fname.dsize, &CurrentNewsSrv->conn->account);
        else
#endif
          mutt_buffer_expand_path(&fname);
#ifdef USE_IMAP
        if (imap_path_probe(mutt_b2s(&fname), NULL) != MUTT_IMAP)
#endif
#ifdef USE_POP
          if (pop_path_probe(mutt_b2s(&fname), NULL) != MUTT_POP)
#endif
#ifdef USE_NNTP
            if (!OptNews && (nntp_path_probe(mutt_b2s(&fname), NULL) != MUTT_NNTP))
#endif
              if (mx_path_probe(mutt_b2s(&fname)) != MUTT_NOTMUCH)
              {
                /* check to make sure the file exists and is readable */
                if (access(mutt_b2s(&fname), R_OK) == -1)
                {
                  mutt_perror(mutt_b2s(&fname));
                  break;
                }
              }

        menu->redraw = REDRAW_FULL;

        struct Mailbox *m = mx_path_resolve(mutt_b2s(&fname));
        bool old_readonly = m->readonly;
        struct Context *ctx = mx_mbox_open(m, MUTT_READONLY);
        if (!ctx)
        {
          mutt_error(_("Unable to open mailbox %s"), mutt_b2s(&fname));
          mx_fastclose_mailbox(m);
          m = NULL;
          break;
        }

        if (ctx->mailbox->msg_count == 0)
        {
          mx_mbox_close(&ctx);
          mutt_error(_("No messages in that folder"));
          break;
        }

        struct Context *ctx_cur = Context; /* remember current folder and sort methods */
        const enum SortType old_sort = cs_subset_sort(sub, "sort"); /* `$sort`, `$sort_aux` could be changed in mutt_index_menu() */
        const enum SortType old_sort_aux = cs_subset_sort(sub, "sort_aux");

        Context = ctx;
        OptAttachMsg = true;
        mutt_message(_("Tag the messages you want to attach"));
        struct MuttWindow *dlgindex = index_pager_init();
        dialog_push(dlgindex);
        mutt_index_menu(dlgindex);
        dialog_pop();
        index_pager_shutdown(dlgindex);
        mutt_window_free(&dlgindex);
        OptAttachMsg = false;

        if (!Context)
        {
          /* go back to the folder we started from */
          Context = ctx_cur;
          /* Restore old $sort and $sort_aux */
          if (old_sort != cs_subset_sort(sub, "sort"))
            cs_subset_str_native_set(sub, "sort", old_sort, NULL);
          if (old_sort_aux != cs_subset_sort(sub, "sort_aux"))
            cs_subset_str_native_set(sub, "sort_aux", old_sort_aux, NULL);
          menu->redraw |= REDRAW_INDEX | REDRAW_STATUS;
          break;
        }

        bool added_attachment = false;
        for (int i = 0; i < Context->mailbox->msg_count; i++)
        {
          if (!Context->mailbox->emails[i])
            break;
          if (!message_is_tagged(Context, Context->mailbox->emails[i]))
            continue;

          struct AttachPtr *ap = mutt_mem_calloc(1, sizeof(struct AttachPtr));
          ap->body = mutt_make_message_attach(Context->mailbox,
                                              Context->mailbox->emails[i], true, sub);
          if (ap->body)
          {
            added_attachment = true;
            update_idx(menu, actx, ap);
          }
          else
          {
            mutt_error(_("Unable to attach"));
            FREE(&ap);
          }
        }
        menu->redraw |= REDRAW_FULL;

        Context->mailbox->readonly = old_readonly;
        mx_fastclose_mailbox(Context->mailbox);
        ctx_free(&Context);

        /* go back to the folder we started from */
        Context = ctx_cur;
        /* Restore old $sort and $sort_aux */
        if (old_sort != cs_subset_sort(sub, "sort"))
          cs_subset_str_native_set(sub, "sort", old_sort, NULL);
        if (old_sort_aux != cs_subset_sort(sub, "sort_aux"))
          cs_subset_str_native_set(sub, "sort_aux", old_sort_aux, NULL);
        if (added_attachment)
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
        break;
      }

      case OP_DELETE:
        CHECK_COUNT;
        if (CUR_ATTACH->unowned)
          CUR_ATTACH->body->unlink = false;
        if (delete_attachment(actx, menu->current) == -1)
          break;
        mutt_update_compose_menu(actx, menu, false);
        if (menu->current == 0)
          e->body = actx->idx[0]->body;

        mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
        break;

      case OP_COMPOSE_TOGGLE_RECODE:
      {
        CHECK_COUNT;
        if (!mutt_is_text_part(CUR_ATTACH->body))
        {
          mutt_error(_("Recoding only affects text attachments"));
          break;
        }
        CUR_ATTACH->body->noconv = !CUR_ATTACH->body->noconv;
        if (CUR_ATTACH->body->noconv)
          mutt_message(_("The current attachment won't be converted"));
        else
          mutt_message(_("The current attachment will be converted"));
        menu->redraw |= REDRAW_CURRENT;
        mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
        break;
      }

      case OP_COMPOSE_EDIT_DESCRIPTION:
        CHECK_COUNT;
        mutt_str_copy(buf, CUR_ATTACH->body->description, sizeof(buf));
        /* header names should not be translated */
        if (mutt_get_field("Description: ", buf, sizeof(buf), MUTT_COMP_NO_FLAGS) == 0)
        {
          if (!mutt_str_equal(CUR_ATTACH->body->description, buf))
          {
            mutt_str_replace(&CUR_ATTACH->body->description, buf);
            menu->redraw |= REDRAW_CURRENT;
            mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          }
        }
        break;

      case OP_COMPOSE_UPDATE_ENCODING:
      {
        CHECK_COUNT;
        bool encoding_updated = false;
        if (menu->tagprefix)
        {
          struct Body *top = NULL;
          for (top = e->body; top; top = top->next)
          {
            if (top->tagged)
            {
              encoding_updated = true;
              mutt_update_encoding(top, sub);
            }
          }
          menu->redraw = REDRAW_FULL;
        }
        else
        {
          mutt_update_encoding(CUR_ATTACH->body, sub);
          encoding_updated = true;
          menu->redraw |= REDRAW_CURRENT | REDRAW_STATUS;
        }
        if (encoding_updated)
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
        break;
      }

      case OP_COMPOSE_TOGGLE_DISPOSITION:
        /* toggle the content-disposition between inline/attachment */
        CUR_ATTACH->body->disposition =
            (CUR_ATTACH->body->disposition == DISP_INLINE) ? DISP_ATTACH : DISP_INLINE;
        menu->redraw |= REDRAW_CURRENT;
        break;

      case OP_EDIT_TYPE:
        CHECK_COUNT;
        {
          if (mutt_edit_content_type(NULL, CUR_ATTACH->body, NULL))
          {
            /* this may have been a change to text/something */
            mutt_update_encoding(CUR_ATTACH->body, sub);
            menu->redraw |= REDRAW_CURRENT;
            mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          }
        }
        break;

      case OP_COMPOSE_EDIT_LANGUAGE:
        CHECK_COUNT;
        mutt_str_copy(buf, CUR_ATTACH->body->language, sizeof(buf));
        if (mutt_get_field("Content-Language: ", buf, sizeof(buf), MUTT_COMP_NO_FLAGS) == 0)
        {
          if (!mutt_str_equal(CUR_ATTACH->body->language, buf))
          {
            CUR_ATTACH->body->language = mutt_str_dup(buf);
            menu->redraw |= REDRAW_CURRENT | REDRAW_STATUS;
            mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          }
          mutt_clear_error();
        }
        else
          mutt_warning(_("Empty 'Content-Language'"));
        break;

      case OP_COMPOSE_EDIT_ENCODING:
        CHECK_COUNT;
        mutt_str_copy(buf, ENCODING(CUR_ATTACH->body->encoding), sizeof(buf));
        if ((mutt_get_field("Content-Transfer-Encoding: ", buf, sizeof(buf),
                            MUTT_COMP_NO_FLAGS) == 0) &&
            (buf[0] != '\0'))
        {
          int enc = mutt_check_encoding(buf);
          if ((enc != ENC_OTHER) && (enc != ENC_UUENCODED))
          {
            if (enc != CUR_ATTACH->body->encoding)
            {
              CUR_ATTACH->body->encoding = enc;
              menu->redraw |= REDRAW_CURRENT | REDRAW_STATUS;
              mutt_clear_error();
              mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
            }
          }
          else
            mutt_error(_("Invalid encoding"));
        }
        break;

      case OP_COMPOSE_SEND_MESSAGE:
        /* Note: We don't invoke send2-hook here, since we want to leave
         * users an opportunity to change settings from the ":" prompt.  */
        if (check_attachments(actx, sub) != 0)
        {
          menu->redraw = REDRAW_FULL;
          break;
        }

#ifdef MIXMASTER
        if (!STAILQ_EMPTY(&e->chain) && (mix_check_message(e) != 0))
          break;
#endif

        if (!fcc_set && !mutt_buffer_is_empty(fcc))
        {
          const enum QuadOption c_copy = cs_subset_quad(sub, "copy");
          enum QuadOption ans =
              query_quadoption(c_copy, _("Save a copy of this message?"));
          if (ans == MUTT_ABORT)
            break;
          else if (ans == MUTT_NO)
            mutt_buffer_reset(fcc);
        }

        loop = false;
        rc = 0;
        break;

      case OP_COMPOSE_EDIT_FILE:
      {
        CHECK_COUNT;
        const char *c_editor = cs_subset_string(sub, "editor");
        mutt_edit_file(NONULL(c_editor), CUR_ATTACH->body->filename);
        mutt_update_encoding(CUR_ATTACH->body, sub);
        menu->redraw |= REDRAW_CURRENT | REDRAW_STATUS;
        /* Unconditional hook since editor was invoked */
        mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
        break;
      }

      case OP_COMPOSE_TOGGLE_UNLINK:
        CHECK_COUNT;
        CUR_ATTACH->body->unlink = !CUR_ATTACH->body->unlink;

        menu->redraw |= REDRAW_INDEX;
        /* No send2hook since this doesn't change the message. */
        break;

      case OP_COMPOSE_GET_ATTACHMENT:
        CHECK_COUNT;
        if (menu->tagprefix)
        {
          for (struct Body *top = e->body; top; top = top->next)
          {
            if (top->tagged)
              mutt_get_tmp_attachment(top);
          }
          menu->redraw |= REDRAW_FULL;
        }
        else if (mutt_get_tmp_attachment(CUR_ATTACH->body) == 0)
          menu->redraw |= REDRAW_CURRENT;

        /* No send2hook since this doesn't change the message. */
        break;

      case OP_COMPOSE_RENAME_ATTACHMENT:
      {
        CHECK_COUNT;
        char *src = NULL;
        if (CUR_ATTACH->body->d_filename)
          src = CUR_ATTACH->body->d_filename;
        else
          src = CUR_ATTACH->body->filename;
        mutt_buffer_strcpy(&fname, mutt_path_basename(NONULL(src)));
        int ret = mutt_buffer_get_field(_("Send attachment with name: "), &fname, MUTT_FILE);
        if (ret == 0)
        {
          /* As opposed to RENAME_FILE, we don't check buf[0] because it's
           * valid to set an empty string here, to erase what was set */
          mutt_str_replace(&CUR_ATTACH->body->d_filename, mutt_b2s(&fname));
          menu->redraw |= REDRAW_CURRENT;
        }
        break;
      }

      case OP_COMPOSE_RENAME_FILE:
        CHECK_COUNT;
        mutt_buffer_strcpy(&fname, CUR_ATTACH->body->filename);
        mutt_buffer_pretty_mailbox(&fname);
        if ((mutt_buffer_get_field(_("Rename to: "), &fname, MUTT_FILE) == 0) &&
            !mutt_buffer_is_empty(&fname))
        {
          struct stat st;
          if (stat(CUR_ATTACH->body->filename, &st) == -1)
          {
            /* L10N: "stat" is a system call. Do "man 2 stat" for more information. */
            mutt_error(_("Can't stat %s: %s"), mutt_b2s(&fname), strerror(errno));
            break;
          }

          mutt_buffer_expand_path(&fname);
          if (mutt_file_rename(CUR_ATTACH->body->filename, mutt_b2s(&fname)))
            break;

          mutt_str_replace(&CUR_ATTACH->body->filename, mutt_b2s(&fname));
          menu->redraw |= REDRAW_CURRENT;

          if (CUR_ATTACH->body->stamp >= st.st_mtime)
            mutt_stamp_attachment(CUR_ATTACH->body);
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
        }
        break;

      case OP_COMPOSE_NEW_MIME:
      {
        mutt_buffer_reset(&fname);
        if ((mutt_buffer_get_field(_("New file: "), &fname, MUTT_FILE) != 0) ||
            mutt_buffer_is_empty(&fname))
        {
          continue;
        }
        mutt_buffer_expand_path(&fname);

        /* Call to lookup_mime_type () ?  maybe later */
        char type[256] = { 0 };
        if ((mutt_get_field("Content-Type: ", type, sizeof(type), MUTT_COMP_NO_FLAGS) != 0) ||
            (type[0] == '\0'))
        {
          continue;
        }

        char *p = strchr(type, '/');
        if (!p)
        {
          mutt_error(_("Content-Type is of the form base/sub"));
          continue;
        }
        *p++ = 0;
        enum ContentType itype = mutt_check_mime_type(type);
        if (itype == TYPE_OTHER)
        {
          mutt_error(_("Unknown Content-Type %s"), type);
          continue;
        }
        struct AttachPtr *ap = mutt_mem_calloc(1, sizeof(struct AttachPtr));
        /* Touch the file */
        FILE *fp = mutt_file_fopen(mutt_b2s(&fname), "w");
        if (!fp)
        {
          mutt_error(_("Can't create file %s"), mutt_b2s(&fname));
          FREE(&ap);
          continue;
        }
        mutt_file_fclose(&fp);

        ap->body = mutt_make_file_attach(mutt_b2s(&fname), sub);
        if (!ap->body)
        {
          mutt_error(_("What we have here is a failure to make an attachment"));
          FREE(&ap);
          continue;
        }
        update_idx(menu, actx, ap);

        CUR_ATTACH->body->type = itype;
        mutt_str_replace(&CUR_ATTACH->body->subtype, p);
        CUR_ATTACH->body->unlink = true;
        menu->redraw |= REDRAW_INDEX | REDRAW_STATUS;

        if (mutt_compose_attachment(CUR_ATTACH->body))
        {
          mutt_update_encoding(CUR_ATTACH->body, sub);
          menu->redraw = REDRAW_FULL;
        }
        mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
        break;
      }

      case OP_COMPOSE_EDIT_MIME:
        CHECK_COUNT;
        if (mutt_edit_attachment(CUR_ATTACH->body))
        {
          mutt_update_encoding(CUR_ATTACH->body, sub);
          menu->redraw = REDRAW_FULL;
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
        }
        break;

      case OP_VIEW_ATTACH:
      case OP_DISPLAY_HEADERS:
        CHECK_COUNT;
        mutt_attach_display_loop(menu, op, NULL, actx, false);
        menu->redraw = REDRAW_FULL;
        /* no send2hook, since this doesn't modify the message */
        break;

      case OP_SAVE:
        CHECK_COUNT;
        mutt_save_attachment_list(actx, NULL, menu->tagprefix, CUR_ATTACH->body, NULL, menu);
        /* no send2hook, since this doesn't modify the message */
        break;

      case OP_PRINT:
        CHECK_COUNT;
        mutt_print_attachment_list(actx, NULL, menu->tagprefix, CUR_ATTACH->body);
        /* no send2hook, since this doesn't modify the message */
        break;

      case OP_PIPE:
      case OP_FILTER:
        CHECK_COUNT;
        mutt_pipe_attachment_list(actx, NULL, menu->tagprefix, CUR_ATTACH->body,
                                  (op == OP_FILTER));
        if (op == OP_FILTER) /* cte might have changed */
          menu->redraw |= menu->tagprefix ? REDRAW_FULL : REDRAW_CURRENT;
        menu->redraw |= REDRAW_STATUS;
        mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
        break;

      case OP_EXIT:
      {
        const enum QuadOption c_postpone = cs_subset_quad(sub, "postpone");
        enum QuadOption ans =
            query_quadoption(c_postpone, _("Save (postpone) draft message?"));
        if (ans == MUTT_NO)
        {
          for (int i = 0; i < actx->idxlen; i++)
            if (actx->idx[i]->unowned)
              actx->idx[i]->body->unlink = false;

          if (!(flags & MUTT_COMPOSE_NOFREEHEADER))
          {
            for (int i = 0; i < actx->idxlen; i++)
            {
              /* avoid freeing other attachments */
              actx->idx[i]->body->next = NULL;
              /* See the comment in delete_attachment() */
              if (!actx->idx[i]->body->email)
                actx->idx[i]->body->parts = NULL;
              mutt_body_free(&actx->idx[i]->body);
            }
          }
          rc = -1;
          loop = false;
          break;
        }
        else if (ans == MUTT_ABORT)
          break; /* abort */
      }
        /* fallthrough */

      case OP_COMPOSE_POSTPONE_MESSAGE:
        if (check_attachments(actx, sub) != 0)
        {
          menu->redraw = REDRAW_FULL;
          break;
        }

        loop = false;
        rc = 1;
        break;

      case OP_COMPOSE_ISPELL:
      {
        endwin();
        const char *c_ispell = cs_subset_string(sub, "ispell");
        snprintf(buf, sizeof(buf), "%s -x %s", NONULL(c_ispell), e->body->filename);
        if (mutt_system(buf) == -1)
          mutt_error(_("Error running \"%s\""), buf);
        else
        {
          mutt_update_encoding(e->body, sub);
          menu->redraw |= REDRAW_STATUS;
        }
        break;
      }

      case OP_COMPOSE_WRITE_MESSAGE:
        mutt_buffer_reset(&fname);
        if (Context)
        {
          mutt_buffer_strcpy(&fname, mailbox_path(Context->mailbox));
          mutt_buffer_pretty_mailbox(&fname);
        }
        if (actx->idxlen)
          e->body = actx->idx[0]->body;
        if ((mutt_buffer_enter_fname(_("Write message to mailbox"), &fname, true) != -1) &&
            !mutt_buffer_is_empty(&fname))
        {
          mutt_message(_("Writing message to %s ..."), mutt_b2s(&fname));
          mutt_buffer_expand_path(&fname);

          if (e->body->next)
            e->body = mutt_make_multipart(e->body);

          if (mutt_write_fcc(mutt_b2s(&fname), e, NULL, false, NULL, NULL, sub) == 0)
            mutt_message(_("Message written"));

          e->body = mutt_remove_multipart(e->body);
        }
        break;

      case OP_COMPOSE_PGP_MENU:
      {
        const SecurityFlags old_flags = e->security;
        if (!(WithCrypto & APPLICATION_PGP))
          break;
        if (!crypt_has_module_backend(APPLICATION_PGP))
        {
          mutt_error(_("No PGP backend configured"));
          break;
        }
        if (((WithCrypto & APPLICATION_SMIME) != 0) && (e->security & APPLICATION_SMIME))
        {
          if (e->security & (SEC_ENCRYPT | SEC_SIGN))
          {
            if (mutt_yesorno(_("S/MIME already selected. Clear and continue?"), MUTT_YES) != MUTT_YES)
            {
              mutt_clear_error();
              break;
            }
            e->security &= ~(SEC_ENCRYPT | SEC_SIGN);
          }
          e->security &= ~APPLICATION_SMIME;
          e->security |= APPLICATION_PGP;
          update_crypt_info(rd);
        }
        e->security = crypt_pgp_send_menu(e);
        update_crypt_info(rd);
        if (old_flags != e->security)
        {
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          redraw_env = true;
        }
        break;
      }

      case OP_FORGET_PASSPHRASE:
        crypt_forget_passphrase();
        break;

      case OP_COMPOSE_SMIME_MENU:
      {
        const SecurityFlags old_flags = e->security;
        if (!(WithCrypto & APPLICATION_SMIME))
          break;
        if (!crypt_has_module_backend(APPLICATION_SMIME))
        {
          mutt_error(_("No S/MIME backend configured"));
          break;
        }

        if (((WithCrypto & APPLICATION_PGP) != 0) && (e->security & APPLICATION_PGP))
        {
          if (e->security & (SEC_ENCRYPT | SEC_SIGN))
          {
            if (mutt_yesorno(_("PGP already selected. Clear and continue?"), MUTT_YES) != MUTT_YES)
            {
              mutt_clear_error();
              break;
            }
            e->security &= ~(SEC_ENCRYPT | SEC_SIGN);
          }
          e->security &= ~APPLICATION_PGP;
          e->security |= APPLICATION_SMIME;
          update_crypt_info(rd);
        }
        e->security = crypt_smime_send_menu(e);
        update_crypt_info(rd);
        if (old_flags != e->security)
        {
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          redraw_env = true;
        }
        break;
      }

#ifdef MIXMASTER
      case OP_COMPOSE_MIX:
        dlg_select_mixmaster_chain(rd->win_envelope, &e->chain,
                                   rd->win_envelope->state.cols);
        mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
        redraw_env = true;
        break;
#endif

#ifdef USE_AUTOCRYPT
      case OP_COMPOSE_AUTOCRYPT_MENU:
      {
        const SecurityFlags old_flags = e->security;
        const bool c_autocrypt = cs_subset_bool(sub, "autocrypt");
        if (!c_autocrypt)
          break;

        if ((WithCrypto & APPLICATION_SMIME) && (e->security & APPLICATION_SMIME))
        {
          if (e->security & (SEC_ENCRYPT | SEC_SIGN))
          {
            if (mutt_yesorno(_("S/MIME already selected. Clear and continue?"), MUTT_YES) != MUTT_YES)
            {
              mutt_clear_error();
              break;
            }
            e->security &= ~(SEC_ENCRYPT | SEC_SIGN);
          }
          e->security &= ~APPLICATION_SMIME;
          e->security |= APPLICATION_PGP;
          update_crypt_info(rd);
        }
        autocrypt_compose_menu(e, sub);
        update_crypt_info(rd);
        if (old_flags != e->security)
        {
          mutt_message_hook(NULL, e, MUTT_SEND2_HOOK);
          redraw_env = true;
        }
        break;
      }
#endif
    }
  }

  mutt_buffer_dealloc(&fname);

#ifdef USE_AUTOCRYPT
  /* This is a fail-safe to make sure the bit isn't somehow turned
   * on.  The user could have disabled the option after setting SEC_AUTOCRYPT,
   * or perhaps resuming or replying to an autocrypt message.  */
  const bool c_autocrypt = cs_subset_bool(sub, "autocrypt");
  if (!c_autocrypt)
    e->security &= ~SEC_AUTOCRYPT;
#endif

  mutt_menu_pop_current(menu);
  mutt_menu_free(&menu);
  dialog_pop();
  notify_observer_remove(NeoMutt->notify, compose_config_observer, dlg);
  notify_observer_remove(NeoMutt->notify, compose_header_observer, rd);
  mutt_window_free(&dlg);

  if (actx->idxlen)
    e->body = actx->idx[0]->body;
  else
    e->body = NULL;

  mutt_actx_free(&actx);

  mutt_list_free(&redraw.to_list);
  mutt_list_free(&redraw.cc_list);
  mutt_list_free(&redraw.bcc_list);
  return rc;
}
