/*
 * Rufus for Linux: bled support functions
 * Copyright © 2026 Rufus Linux port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <stddef.h>

typedef struct llist_t llist_t;

/*
 * Links from archives are not restored, exactly like on Windows (where this
 * busybox function isn't compiled in and link()/symlink() are stubs).
 */
void create_or_remember_link(llist_t** link_placeholders, const char* target, const char* linkname, int hard_link)
{
	(void)link_placeholders; (void)target; (void)linkname; (void)hard_link;
}
