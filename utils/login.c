/* Hurdish login

   Copyright (C) 1995 Free Software Foundation, Inc.

   Written by Miles Bader <miles@gnu.ai.mit.edu>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#include <hurd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <paths.h>
#include <ctype.h>
#include <utmp.h>
#include <pwd.h>
#include <grp.h>

#include <sys/fcntl.h>

#include <argp.h>
#include <argz.h>
#include <envz.h>
#include <idvec.h>
#include <error.h>

#define DEFAULT_NOBODY "login"
#define DEFAULT_UMASK 0

#define _PATH_MOTD "/etc/motd"	/* XXX */

/* Which shell to use if we can't find the default one.  */
#define FAILURE_SHELL _PATH_BSHELL

/* Defaults for various login parameters.  */
char *default_args[] = {
  "SHELL=" _PATH_BSHELL,
  "HOME=/u/login",
  "USER=login",
  "NAME=Not logged in",
  "HUSHLOGIN=.hushlogin",	/* Looked up relative new home dir.  */
  "MOTD=" _PATH_MOTD,
  0
};
/* Default values for the new environment.  */
char *default_env[] = {
  "PATH=" _PATH_DEFPATH,
  0
};

/* Which things are copied from the login parameters into the environment. */
char *copied_args[] = { "SHELL", "HOME", "NAME", "VIA", 0 };

static struct argp_option options[] =
{
  {"arg0",	'0', "ARG",   0, "Make ARG the shell's argv[0]"},
  {"environ",	'e', "ENTRY", 0, "Add ENTRY to the environment"},
  {"environ-default", 'E', "ENTRY", 0, "Use ENTRY as a default environment variable"},
  {"no-args",	'x', 0,	      0, "Don't login args into the environment"},
  {"arg",	'a', "ARG",   0, "Add login parameter ARG"},
  {"arg-default", 'A', "ARG", 0, "Use ARG as a default login parameter"},
  {"no-environ", 'X', 0,      0, "Don't add the parent environment as default login params"},
  {"user",	'u', "USER",  0, "Add USER to the effective uids"},
  {"avail-user",'U', "USER",  0, "Add USER to the available uids"},
  {"group",     'g', "GROUP", 0, "Add GROUP to the effective groups"},
  {"avail-group",'G',"GROUP", 0, "Add GROUP to the available groups"},
  {"umask",	'm', "MASK",  0, "Use a umask of MASK"},
  {"nobody",	'n', "USER",  0, "Use USER's passwd entry to fetch the default"
     "login params if there are no uids (default `" DEFAULT_NOBODY "')"}, 
  {"inherit-environ", 'p', 0, 0, "Inherit the parent's environment"},
  {"via",	'h',  "HOST", 0, "This login is from HOST"},
  {"no-passwd", 'f', 0,       0, "Don't ask for passwords"},
  {"no-utmp",   'z', 0,       0, "Don't put an entry in utmp"},
  {0, 0}
};
static char *args_doc = "[USER [ARG...]]";
static char *doc =
"To give args to the shell without specifying a user, use - for USER.\n"
"Current login parameters include HOME, SHELL, USER, NAME, and ROOT.";

/* Outputs whatever can be read from file descriptor FD to standard output,
   and then close it.  If FD is < 0, assumes an error happened, and prints an
   error message using ERRNO and STR.  */
static void
cat (int fd, char *str)
{
  int rd = fd < 0 ? -1 : 1;
  while (rd > 0)
    {
      char buf[4096];
      rd = read (fd, buf, sizeof (buf));
      if (rd > 0)
	write (0, buf, rd);
    }
  if (rd < 0)
    error (0, errno, "%s", str);
}

/* Add a utmp entry based on the parameters in ARGS & ARGS_LEN, from tty
   TTY_FD.  */
static void
add_utmp_entry (char *args, unsigned args_len, int tty_fd)
{
  extern void login (struct utmp *); /* in libutil */
  struct utmp utmp;
  char bogus_tty[sizeof (_PATH_TTY) + 2];
  char *tty = ttyname (0), *tail;

  if (! tty)
    {
      sprintf (bogus_tty, "%s??", _PATH_TTY);
      tty = bogus_tty;
    }

  tail = rindex (tty, '/');
  if (tail)
    tty = tail + 1;

  bzero (&utmp, sizeof (utmp));

  time (&utmp.ut_time);
  strncpy (utmp.ut_name, envz_get (args, args_len, "USER") ?: "",
	   sizeof (utmp.ut_name));
  strncpy (utmp.ut_host, envz_get (args, args_len, "VIA") ?: "",
	   sizeof (utmp.ut_host));
  strncpy (utmp.ut_line, tty, sizeof (utmp.ut_line));

  login (&utmp);
}

void 
main(int argc, char *argv[])
{
  int i, fd;
  error_t err = 0;
  int umask = DEFAULT_UMASK;
  char *nobody = DEFAULT_NOBODY; /* Where to get params if there is no user. */
  char *args = 0;		/* The login parameters */
  unsigned args_len = 0;
  char *passwd = 0;		/* Login parameters from /etc/passwd */
  unsigned passwd_len = 0;
  char *args_defs = 0;		/* Defaults for login parameters.  */
  unsigned args_defs_len = 0;
  char *env = 0;		/* The new environment.  */
  unsigned env_len = 0;
  char *env_defs = 0;		/* Defaults for the environment.  */
  unsigned env_defs_len = 0;
  char *parent_env = 0;		/* The environment we got from our parent */
  unsigned parent_env_len = 0;
  int no_environ = 0;		/* If false, use the env as default params. */
  int no_args = 0;		/* If false, put login params in the env. */
  int inherit_environ = 0;	/* True if we shouldn't clear our env.  */
  int saw_user_arg = 0;		/* True if we've seen the USER argument.  */
  int no_passwd = 0;		/* Don't bother verifying what we're doing.  */
  int no_utmp = 0;		/* Don't put an entry in utmp.  */
  struct idvec *uids = make_idvec (); /* The UIDs of the new shell.  */
  struct idvec *gids = make_idvec (); /* The GIDs.  */
  struct idvec *aux_uids = make_idvec (); /* The aux UIDs of the new shell.  */
  struct idvec *aux_gids = make_idvec (); /* The aux GIDs.  */
  char *shell = 0;		/* The shell program to run.  */
  char *home = 0;		/* The new home directory.  */
  char *root = 0;		/* The optional new root directory.  */
  char *hushlogin = 0;		/* The hushlogin file.  */
  char *sh_arg0 = 0;		/* The shell's argv[0].  */
  char *sh_args = 0;		/* The args to the shell.  */
  unsigned sh_args_len = 0;
  mach_port_t exec_node;	/* The shell executable.  */
  mach_port_t home_node;	/* The home directory node.  */
  mach_port_t root_node;	/* The root node.  */
  mach_port_t ports[INIT_PORT_MAX]; /* Init ports for the new process.  */
  int ints[INIT_INT_MAX];	/* Init ints for it.  */
  mach_port_t dtable[3];	/* File descriptors passed. */
  mach_port_t auth;		/* The auth port for the new shell.  */
  mach_port_t auth_server = getauth ();
  mach_port_t proc_server = getproc ();

  /* Returns a copy of the io/proc object PORT reauthenticated in AUTH.  If an
     error occurs, NAME is used to print an error message, and the program
     exited.  */
  mach_port_t
    reauth (mach_port_t port, int is_proc, auth_t auth, char *name)
      {
	if (port)
	  {
	    mach_port_t rend = mach_reply_port (), new_port;
	    error_t err =
	      is_proc ?
		proc_reauthenticate (port, rend, MACH_MSG_TYPE_MAKE_SEND)
		  : io_reauthenticate (port, rend, MACH_MSG_TYPE_MAKE_SEND);

	    if (err)
	      error (12, err, "reauthenticating %s", name);

	    err =
	      auth_user_authenticate (auth, port,
				      rend, MACH_MSG_TYPE_MAKE_SEND,
				      &new_port);
	    if (err)
	      error (13, err, "reauthenticating %s", name);

	    if (is_proc)
	      /* proc_reauthenticate modifies the existing port.  */
	      {
		/* We promised to make a copy, so do so... */
		mach_port_mod_refs (mach_task_self (), MACH_PORT_RIGHT_SEND,
				    port, 1);
		if (new_port != MACH_PORT_NULL)
		  mach_port_deallocate (mach_task_self (), new_port);
	      }
	    else
	      port = new_port;

	    mach_port_destroy (mach_task_self (), rend);
	  }
	return port;
      }

  /* Add the `=' separated environment entry ENTRY to ENV & ENV_LEN, exiting
     with an error message if we can't.  */
  void add_entry (char **env, unsigned *env_len, char *entry)
    {
      char *name = strsep (&entry, "=");
      err = envz_add (env, env_len, name, entry);
      if (err)
	error (8, err, "Adding %s", entry);
    }

  /* Make sure the user should be allowed to do this.  */
  void verify_passwd (const char *name, const char *password)
    {
      if (! no_passwd && password && *password)
	{
	  extern char *crypt (const char salt[2], const char *string);
	  char *prompt, *unencrypted, *encrypted;

	  asprintf (&prompt, "Password for %s:", name);
	  unencrypted = getpass (prompt);
	  encrypted = crypt (unencrypted, password);
	  /* Paranoia may destroya.  */
	  memset (unencrypted, 0, strlen (unencrypted));

	  if (strcmp (encrypted, password) != 0)
	    error (50, 0, "%s: Invalid password", name);
	}
    }

  /* Parse our options...  */
  error_t parse_opt (int key, char *arg, struct argp_state *state)
    {
      switch (key)
	{
	case 'n': nobody = arg; break;
	case 'm': umask = strtoul (arg, 0, 8); break;
	case 'p': inherit_environ = 1; break;
	case 'x': no_args = 1; break;
	case 'X': no_environ = 1; break;
	case 'e': add_entry (&env, &env_len, arg); break;
	case 'E': add_entry (&env_defs, &env_defs_len, arg); break;
	case 'a': add_entry (&args, &args_len, arg); break;
	case 'A': add_entry (&args_defs, &args_defs_len, arg); break;
	case '0': sh_arg0 = arg; break;
	case 'h': envz_add (&args, &args_len, "VIA", arg); break;
	case 'f': no_passwd = 1; break;
	case 'z': no_utmp = 1; break;

	case ARGP_KEY_ARG:
	  if (saw_user_arg)
	    {
	      err = argz_add (&sh_args, &sh_args_len, arg);
	      if (err)
		error (9, err, "Adding %s", arg);
	      break;
	    }

	  saw_user_arg = 1;
	  if (strcmp (arg, "-") == 0)
	    arg = 0;		/* Just like there weren't any args at all.  */
	  /* Fall through to deal with adding the user.  */

	case 'u':
	case 'U':
	case ARGP_KEY_NO_ARGS:
	  {
	    /* USER is whom to look up.  If it's 0, then we hit the end of
	       the sh_args without seeing a user, so we want to add defaults
	       values for `nobody'.  */
	    char *user = arg ?: nobody;
	    struct passwd *pw =
	      isdigit (*user) ? getpwuid (atoi (user)) : getpwnam (user);

	    if (! pw)
	      if (! arg)
		/* It was nobody anyway.  Just use the defaults.  */
		break;
	      else
		error (10, 0, "%s: Unknown user", user);

	    if (arg
		&& ! idvec_contains (uids, pw->pw_uid)
		&& ! idvec_contains (aux_uids, pw->pw_uid))
	      /* Check for a password, but only if we haven't already, and
		 it's not nobody.  */
	      verify_passwd (pw->pw_name, pw->pw_passwd);

	    if (key == 'U')
	      /* Add aux-ids instead of real ones.  */
	      {
		idvec_add (aux_uids, pw->pw_uid);
		idvec_add (aux_uids, pw->pw_gid);
	      }
	    else
	      {
		if (key == ARGP_KEY_ARG || uids->num == 0)
		  /* If it's the argument (as opposed to option) specifying a
		     user, or the first option user, then we get defaults for
		     various things from the password entry.  */
		  {
		    envz_add (&passwd, &passwd_len, "HOME", pw->pw_dir);
		    envz_add (&passwd, &passwd_len, "SHELL", pw->pw_shell);
		    envz_add (&passwd, &passwd_len, "NAME", pw->pw_gecos);
		    envz_add (&passwd, &passwd_len, "USER", pw->pw_name);
		  }
		if (arg)	/* A real user.  */
		  {
		    idvec_add (uids, pw->pw_uid);
		    idvec_add (gids, pw->pw_gid);

		    if (key == ARGP_KEY_ARG)
		      /* The real user arg; make sure this is the first id in
			 the aux ids set (i.e. the `real' id).  */
		      {
			idvec_insert (aux_uids, 0, pw->pw_uid);
			idvec_insert (aux_gids, 0, pw->pw_gid);
		      }
		  }
	      }
	  }
	  break;

	case 'g':
	case 'G':
	  {
	    struct group *gr =
	      isdigit (*arg) ? getgrgid (atoi (arg)) : getgrnam (arg);
	    if (! gr)
	      error (11, 0, "%s: Unknown group", arg);

	    if (! idvec_contains (gids, gr->gr_gid)
		&& ! idvec_contains (aux_gids, gr->gr_gid))
	      /* Check for a password, but only if we haven't already.  */
	      verify_passwd (gr->gr_name, gr->gr_passwd);

	    idvec_add (key == 'g' ? gids : aux_gids, gr->gr_gid);
	  }
	  break;

	default: return EINVAL;
	}
      return 0;
    }
  struct argp argp = {options, parse_opt, args_doc, doc};

  /* Don't allow logins if the nologin file exists.  */
  fd = open (_PATH_NOLOGIN, O_RDONLY);
  if (fd >= 0)
    {
      cat (fd, _PATH_NOLOGIN);
      exit (40);
    }

  /* Put in certain last-ditch defaults.  */
  err = argz_create (default_args, &args_defs, &args_defs_len);
  if (! err)
    err = argz_create (default_env, &env_defs, &env_defs_len);
  if (err)
    error (23, err, "adding defaults");

  err = argz_create (environ, &parent_env, &parent_env_len);

  /* Parse our options.  */
  argp_parse (&argp, argc, argv, 0, 0);

  /* Now that we've parsed the command line, put together all these
     environments we've gotten from various places.  There are two targets:
     (1) the login parameters, and (2) the child environment.

     The login parameters come from these sources (in priority order):
      a) User specified (with the --arg option)
      b) From the passwd file entry for the user being logged in as
      c) From the parent environment, if --no-environ wasn't specified
      d) From the user-specified defaults (--arg-default)
      e) From last-ditch defaults given by the DEFAULT_* defines above

     The child environment is from:
      a) User specified (--environ)
      b) From the login parameters (if --no-args wasn't specified)
      c) From the parent environment, if --inherit-environ was specified
      d) From the user-specified default env values (--environ-default)
      e) From last-ditch defaults given by the DEFAULT_* defines above
   */

  /* Merge the login parameters.  */
  err = envz_merge (&args, &args_len, passwd, passwd_len, 0);
  if (! err && ! no_environ)
    err = envz_merge (&args, &args_len, parent_env, parent_env_len, 0);
  if (! err)
    err = envz_merge (&args, &args_len, args_defs, args_defs_len, 0);
  if (err)
    error (24, err, "merging parameters");

  /* Verify the shell and home dir parameters.  We make a copy of SHELL, as
     we may frob ARGS ahead, and mess up where it's pointing.  */
  shell = strdup (envz_get (args, args_len, "SHELL"));
  home = envz_get (args, args_len, "HOME");
  root = envz_get (args, args_len, "ROOT");

  exec_node = file_name_lookup (shell, O_EXEC, 0);
  if (exec_node == MACH_PORT_NULL)
    {
      err = errno;		/* Save original lookup errno. */

      if (strcmp (shell, FAILURE_SHELL) != 0)
	exec_node = file_name_lookup (FAILURE_SHELL, O_EXEC, 0);

      /* Give the error message, but only exit if we couldn't default. */
      error (exec_node == MACH_PORT_NULL ? 1 : 0, err, "%s", shell);

      /* If we get here, we looked up the default shell ok.  */
      shell = FAILURE_SHELL;
      error (0, 0, "Using SHELL=%s", shell);
      envz_add (&args, &args_len, "SHELL", shell);
    }

  if (home && *home)
    {
      home_node = file_name_lookup (home, O_RDONLY, 0);
      if (home_node == MACH_PORT_NULL)
	{
	  error (0, errno, "%s", home);
	  error (0, 0, "Using HOME=/");
	  home_node = getcrdir ();
	  envz_add (&args, &args_len, "HOME", "/");
	}
    }
  else
    home_node = getcwdir ();

  if (root && *root)
    {
      root_node = file_name_lookup (root, O_RDONLY, 0);
      if (root_node == MACH_PORT_NULL)
	error (40, errno, "%s", root);
    }
  else
    root_node = getcrdir ();

  /* Build the child environment.  */
  if (! no_args)
    /* We can't just merge ARGS, because it may contain the parent
       environment, which we don't always want in the child environment, so
       we pick out only those values of args which actually *are* args.  */
    {
      char **name;
      for (name = copied_args; *name && !err; name++)
	if (! envz_get (env, env_len, *name))
	  {
	    char *val = envz_get (args, args_len, *name);
	    if (val && *val)
	      err = envz_add (&env, &env_len, *name, val);
	  }

      if (!err && !envz_entry (args, args_len, "LOGNAME"))
	/* Copy the user arg into the environment as LOGNAME.  */
	{
	  char *user = envz_get (args, args_len, "USER");
	  if (user)
	    err = envz_add (&env, &env_len, "LOGNAME", user);
	}
    }
  if (! err && inherit_environ)
    err = envz_merge (&env, &env_len, parent_env, parent_env_len, 0);
  if (! err)
    err = envz_merge (&env, &env_len, env_defs, env_defs_len, 0);
  if (err)
    error (24, err, "building environment");

  if (! sh_arg0)
    /* The shells argv[0] defaults to the basename of the shell.  */
    {
      char *shell_base = rindex (shell, '/');
      if (shell_base)
	shell_base++;
      else
	shell_base = shell;

      sh_arg0 = malloc (strlen (shell_base) + 2);
      if (! sh_arg0)
	err = ENOMEM;
      else
	/* Prepend the name with a `-', as is the odd custom.  */
	{
	  sh_arg0[0] = '-';
	  strcpy (sh_arg0 + 1, shell_base);
	}
    }
  if (! err)
    err = argz_insert (&sh_args, &sh_args_len, sh_args, sh_arg0);
  if (err)
    error (21, err, "consing arguments");

  err =
    auth_makeauth (auth_server, 0, MACH_MSG_TYPE_COPY_SEND, 0,
		   uids->ids, uids->num, aux_uids->ids, aux_uids->num,
		   gids->ids, gids->num, aux_gids->ids, aux_gids->num,
		   &auth);
  if (err)
    error (3, err, "Can't authenticate");

  proc_make_login_coll (proc_server);
  if (aux_uids->num > 0)
    proc_setowner (proc_server, aux_uids->ids[0]);
  else if (uids->num > 0)
    proc_setowner (proc_server, uids->ids[0]);

  /* Output the message of the day.  */
  hushlogin = envz_get (args, args_len, "HUSHLOGIN");
  if (hushlogin && *hushlogin)
    {
      mach_port_t hush_login_node =
	file_name_lookup_under (home_node, hushlogin, O_RDONLY, 0);
      if (hush_login_node == MACH_PORT_NULL)
	{
	  char *motd = envz_get (args, args_len, "MOTD");
	  if (motd && *motd)
	    {
	      fd = open (motd, O_RDONLY);
	      if (fd >= 0)
		cat (fd, motd);
	    }
	}
      else
	mach_port_deallocate (mach_task_self (), hush_login_node);
    }

  /* Get rid of any accumulated null entries in env.  */
  envz_strip (&env, &env_len);

  bzero (ints, sizeof (*ints) * INIT_INT_MAX);
  ints[INIT_UMASK] = umask;

  dtable[0] = reauth (getdport (0), 0, auth, "standard input");
  dtable[1] = reauth (getdport (1), 0, auth, "standard output");
  dtable[2] = reauth (getdport (2), 0, auth, "standard error");

  for (i = 0; i < INIT_PORT_MAX; i++)
    ports[i] = MACH_PORT_NULL;
  ports[INIT_PORT_CRDIR] = reauth (root_node, 0, auth, "root directory");
  ports[INIT_PORT_CWDIR] = reauth (home_node, 0, auth, "home directory");
  ports[INIT_PORT_AUTH] = auth;
  ports[INIT_PORT_PROC] = reauth (proc_server, 1, auth, "process port");

  /* No more authentications to fail, so cross our fingers and add our utmp
     entry.  */
  if (! no_utmp)
    add_utmp_entry (args, args_len, 0);

  err = file_exec (exec_node, mach_task_self (),
		   EXEC_NEWTASK | EXEC_DEFAULTS | EXEC_SECURE,
		   sh_args, sh_args_len, env, env_len,
		   dtable, MACH_MSG_TYPE_COPY_SEND, 3,
		   ports, MACH_MSG_TYPE_COPY_SEND, INIT_PORT_MAX,
		   ints, INIT_INT_MAX,
		   0, 0, 0, 0);
  if (err)
    error(5, err, "%s", shell);

  exit(0);
}
