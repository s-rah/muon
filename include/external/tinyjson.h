/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: Seedo Paul <seedoeldhopaul@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_EXTERNAL_TINYJSON_H
#define MUON_EXTERNAL_TINYJSON_H

#include <stdbool.h>

#include "lang/workspace.h"

bool muon_json_to_dict(struct workspace *wk, char *json_str, obj *res);

#endif
