/*
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 * Functions to interface with the SSH_AUTHENTICATION_FD socket.
 *
 * As far as I am concerned, the code I have written for this software
 * can be used freely for any purpose.  Any derived versions of this
 * software must be clearly marked as such, and if the derived work is
 * incompatible with the protocol description in the RFC file, it must be
 * called by a name other than "ssh" or "Secure Shell".
 */

/* $OpenBSD: auth-options.h,v 1.9 2001/06/26 06:32:47 itojun Exp $ */

#ifndef AUTH_OPTIONS_H
#define AUTH_OPTIONS_H

/* Linked list of custom environment strings */
struct envstring {
	struct envstring *next;
	char   *s;
};

/* Flags that may be set in authorized_keys options. */
extern int no_port_forwarding_flag;
extern int no_agent_forwarding_flag;
extern int no_x11_forwarding_flag;
extern int no_pty_flag;
extern char *forced_command;
extern struct envstring *custom_environment;

/*
 * return 1 if access is granted, 0 if not.
 * side effect: sets key option flags
 */
int
auth_parse_options(struct passwd *, char *, char *, u_long);

/* reset options flags */
void	auth_clear_options(void);

#endif
