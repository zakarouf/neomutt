/**
 * @file
 * Nntp path manipulations
 *
 * @authors
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

#ifndef MUTT_NNTP_PATH_H
#define MUTT_NNTP_PATH_H

struct Path;
struct stat;

int nntp_path2_canon  (struct Path *path, char *user, int port);
int nntp_path2_compare(struct Path *path1, struct Path *path2);
int nntp_path2_parent (const struct Path *path, struct Path **parent);
int nntp_path2_pretty (struct Path *path, const char *folder);
int nntp_path2_probe  (struct Path *path, const struct stat *st);
int nntp_path2_tidy   (struct Path *path);

#endif /* MUTT_NNTP_PATH_H */