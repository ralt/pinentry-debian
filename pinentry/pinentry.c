/* pinentry.c - The PIN entry support library
   Copyright (C) 2002, 2003 g10 Code GmbH
   
   This file is part of PINENTRY.
   
   PINENTRY is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
 
   PINENTRY is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
 
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
   02111-1307, USA  */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <locale.h>
#include <iconv.h>
#include <langinfo.h>
#include <limits.h>

#include "assuan.h"
#include "memory.h"
#include "secmem-util.h"
#include "pinentry.h"


/* Keep the name of our program here. */
static char this_pgmname[50]; 


struct pinentry pinentry =
  {
    NULL,	/* Description.  */
    NULL,	/* Error.  */
    NULL,	/* Prompt.  */
    NULL,	/* Ok button.  */
    NULL,	/* Cancel button.  */
    NULL,	/* PIN.  */
    2048,	/* PIN length.  */
    0,		/* Display.  */
    0,		/* TTY name.  */
    0,		/* TTY type.  */
    0,		/* TTY LC_CTYPE.  */
    0,		/* TTY LC_MESSAGES.  */
    0,		/* Debug mode.  */
    0,		/* Enhanced mode.  */
    1,		/* Global grab.  */
    0,		/* Parent Window ID.  */
    0		/* Result.  */
  };


char *
pinentry_utf8_to_local (char *lc_ctype, char *text)
{
  iconv_t cd;
  char *input = text;
  size_t input_len = strlen (text) + 1;
  char *output;
  size_t output_len;
  char *output_buf;
  size_t processed;

  /* If no locale setting could be determined, simply copy the
     string.  */
  if (!lc_ctype)
    return strdup (text);

  /* This is overkill, but simplifies the iconv invocation greatly.  */
  output_len = input_len * MB_LEN_MAX;
  output_buf = output = malloc (output_len);
  if (!output)
    return NULL;

  cd = iconv_open (lc_ctype, "UTF-8");
  if (cd == (iconv_t) -1)
    {
      free (output);
      return NULL;
    }
  processed = iconv (cd, &input, &input_len, &output, &output_len);
  iconv_close (cd);
  if (processed == (size_t) -1 || input_len)
    {
      free (output_buf);
      return NULL;
    }
  return output_buf;
}

/* Convert TEXT whcih is encoded according to LC_CTYPE to UTF-8.  With
   SECURE set to true, use secure memory for the returned buffer.
   Return NULL on error. */
char *
pinentry_local_to_utf8 (char *lc_ctype, char *text, int secure)
{
  char *old_ctype;
  char *source_encoding;
  iconv_t cd;
  char *input = text;
  size_t input_len = strlen (text) + 1;
  char *output;
  size_t output_len;
  char *output_buf;
  size_t processed;

  /* If no locale setting could be determined, simply copy the
     string.  */
  if (!lc_ctype)
    {
      output_buf = secure? secmem_malloc (input_len) : malloc (input_len);
      if (output_buf)
        strcpy (output_buf, input);
      return output_buf;
    }

  old_ctype = strdup (setlocale (LC_CTYPE, NULL));
  if (!old_ctype)
    return NULL;
  setlocale (LC_CTYPE, lc_ctype);
  source_encoding = nl_langinfo (CODESET);
  setlocale (LC_CTYPE, old_ctype);
  free (old_ctype);

  /* This is overkill, but simplifies the iconv invocation greatly.  */
  output_len = input_len * MB_LEN_MAX;
  output_buf = output = secure? secmem_malloc (output_len):malloc (output_len);
  if (!output)
    return NULL;

  cd = iconv_open ("UTF-8", source_encoding);
  if (cd == (iconv_t) -1)
    {
      if (secure)
        secmem_free (output);
      else
        free (output);
      return NULL;
    }
  processed = iconv (cd, &input, &input_len, &output, &output_len);
  iconv_close (cd);
  if (processed == (size_t) -1 || input_len)
    {
      if (secure)
        secmem_free (output);
      else
        free (output_buf);
      return NULL;
    }
  return output_buf;
}

/* Try to make room for at least LEN bytes in the pinentry.  Returns
   new buffer on success and 0 on failure or when the old buffer is
   sufficient.  */
char *
pinentry_setbufferlen (pinentry_t pin, int len)
{
  char *newp;
  if (len < pinentry.pin_len)
    return NULL;
  newp = secmem_realloc (pin->pin, 2 * pin->pin_len);
  if (newp)
    {
      pin->pin = newp;
      pin->pin_len *= 2;
    }
  else
    {
      secmem_free (pin->pin);
      pin->pin = 0;
      pin->pin_len = 0;
    }
  return newp;
}


/* Initialize the secure memory subsystem, drop privileges and return.
   Must be called early. */
void
pinentry_init (const char *pgmname)
{
  /* Store away our name. */
  if (strlen (pgmname) > sizeof this_pgmname - 2)
    abort ();
  strcpy (this_pgmname, pgmname);

  /* Initialize secure memory.  1 is too small, so the default size
     will be used.  */
  secmem_init (1);
  secmem_set_flags (SECMEM_WARN);
  drop_privs ();

  if (atexit (secmem_term))
    /* FIXME: Could not register at-exit function, bail out.  */
    ;

  assuan_set_malloc_hooks (secmem_malloc, secmem_realloc, secmem_free);
}

/* Simple test to check whether DISPLAY is set or the option --display
   was given.  Used to decide whether the GUI or curses should be
   initialized.  */
int
pinentry_have_display (int argc, char **argv)
{
  if (getenv ("DISPLAY"))
    return 1;
  for (; argc; argc--, argv++)
    if (!strcmp (*argv, "--display"))
      return 1;
  return 0;
}



static void 
usage (void)
{
  /* FIXME: replace the "?" by the real program name. */
  fprintf (stderr, "Usage: %s [OPTION]...\n\
Ask securely for a secret and print it to stdout.\n\
\n\
      --display DISPLAY Set the X display\n\
      --ttyname PATH    Set the tty terminal node name\n\
      --ttytype NAME    Set the tty terminal type\n\
      --lc-ctype        Set the tty LC_CTYPE value\n\
      --lc-messages     Set the tty LC_MESSAGES value\n\
  -e, --enhanced        Ask for timeout and insurance, too\n\
  -g, --no-global-grab  Grab keyboard only while window is focused\n\
      --parent-wid	Parent window ID (for positioning)\n\
  -d, --debug           Turn on debugging output\n\
      --help            Display this help and exit\n\
      --version         Output version information and exit\n", this_pgmname);
}


/* Parse the command line options.  Returns 1 if user should print
   version and exit.  Can exit the program if only help output is
   requested.  */
int
pinentry_parse_opts (int argc, char *argv[])
{
  int opt;
  int opt_help = 0;
  int opt_version = 0;
  struct option opts[] =
    {{ "debug", no_argument, &pinentry.debug, 1 },
     { "display", required_argument, 0, 'D' },
     { "ttyname", required_argument, 0, 'T' },
     { "ttytype", required_argument, 0, 'N' },
     { "lc-ctype", required_argument, 0, 'C' },
     { "lc-messages", required_argument, 0, 'M' },
     { "enhanced", no_argument, &pinentry.enhanced, 1 },
     { "no-global-grab", no_argument, &pinentry.grab, 0 },
     { "parent-wid", required_argument, 0, 'W' },
     { "help", no_argument, &opt_help, 1 },
     { "version", no_argument, &opt_version, 1 },
     { NULL, 0, NULL, 0 }};
  
  while ((opt = getopt_long (argc, argv, "deg", opts, NULL)) != -1)
    {
      switch (opt)
        {
        case 0:
        case '?':
          break;
	case 'D':
	  pinentry.display = strdup (optarg);
	  if (!pinentry.display)
	    {
	      fprintf (stderr, "%s: %s\n", this_pgmname, strerror (errno));
	      exit (EXIT_FAILURE);
	    }
	  break;
	case 'T':
	  pinentry.ttyname = strdup (optarg);
	  if (!pinentry.ttyname)
	    {
	      fprintf (stderr, "%s: %s\n", this_pgmname, strerror (errno));
	      exit (EXIT_FAILURE);
	    }
	  break;
	case 'N':
	  pinentry.ttytype = strdup (optarg);
	  if (!pinentry.ttytype)
	    {
	      fprintf (stderr, "%s: %s\n", this_pgmname, strerror (errno));
	      exit (EXIT_FAILURE);
	    }
	  break;
	case 'C':
	  pinentry.lc_ctype = strdup (optarg);
	  if (!pinentry.lc_ctype)
	    {
	      fprintf (stderr, "%s: %s\n", this_pgmname, strerror (errno));
	      exit (EXIT_FAILURE);
	    }
	  break;
	case 'M':
	  pinentry.lc_messages = strdup (optarg);
	  if (!pinentry.lc_messages)
	    {
	      fprintf (stderr, "%s: %s\n", this_pgmname, strerror (errno));
	      exit (EXIT_FAILURE);
	    }
	  break;
	case 'W':
	  pinentry.parent_wid = atoi (optarg);
	  /* FIXME: Add some error handling.  Use strtol.  */
	  break;
        default:
          /* XXX Should never happen.  */
	  break;
        }
    }
  if (opt_version) 
    return 1;
  if (opt_help) 
    {
      usage ();
      exit (EXIT_SUCCESS);
    }
  return 0;
}


static int
option_handler (ASSUAN_CONTEXT ctx, const char *key, const char *value)
{
  if (!strcmp (key, "no-grab") && !*value)
    pinentry.grab = 0;
  else if (!strcmp (key, "grab") && !*value)
    pinentry.grab = 1;
  else if (!strcmp (key, "debug-wait"))
    {
      fprintf (stderr, "%s: waiting for debugger - my pid is %u ...\n",
	       this_pgmname, (unsigned int) getpid());
      sleep (*value?atoi (value):5);
      fprintf (stderr, "%s: ... okay\n", this_pgmname);
    }
  else if (!strcmp (key, "display"))
    {
      if (pinentry.display)
	free (pinentry.display);
      pinentry.display = strdup (value);
      if (!pinentry.display)
	return ASSUAN_Out_Of_Core;
    }
  else if (!strcmp (key, "ttyname"))
    {
      if (pinentry.ttyname)
	free (pinentry.ttyname);
      pinentry.ttyname = strdup (value);
      if (!pinentry.ttyname)
	return ASSUAN_Out_Of_Core;
    }
  else if (!strcmp (key, "ttytype"))
    {
      if (pinentry.ttytype)
	free (pinentry.ttytype);
      pinentry.ttytype = strdup (value);
      if (!pinentry.ttytype)
	return ASSUAN_Out_Of_Core;
    }
  else if (!strcmp (key, "lc-ctype"))
    {
      if (pinentry.lc_ctype)
	free (pinentry.lc_ctype);
      pinentry.lc_ctype = strdup (value);
      if (!pinentry.lc_ctype)
	return ASSUAN_Out_Of_Core;
    }
  else if (!strcmp (key, "lc-messages"))
    {
      if (pinentry.lc_messages)
	free (pinentry.lc_messages);
      pinentry.lc_messages = strdup (value);
      if (!pinentry.lc_messages)
	return ASSUAN_Out_Of_Core;
    }
  else if (!strcmp (key, "parent-wid"))
    {
      pinentry.parent_wid = atoi (value);
      /* FIXME: Use strtol and add some error handling.  */
    }
  else
    return ASSUAN_Invalid_Option;
  return 0;
}


/* note, that it is sufficient to allocate the target string D as
   long as the source string S, i.e.: strlen(s)+1; */
static void
strcpy_escaped (char *d, const unsigned char *s)
{
  while (*s)
    {
      if (*s == '%' && s[1] && s[2])
        { 
          s++;
          *d++ = xtoi_2 ( s);
          s += 2;
        }
      else
        *d++ = *s++;
    }
  *d = 0; 
}


static int
cmd_setdesc (ASSUAN_CONTEXT ctx, char *line)
{
  char *newd;
  newd = malloc (strlen (line) + 1);

  if (!newd)
    return ASSUAN_Out_Of_Core;

  strcpy_escaped (newd, line);
  if (pinentry.description)
    free (pinentry.description);
  pinentry.description = newd;
  return 0;
}


static int
cmd_setprompt (ASSUAN_CONTEXT ctx, char *line)
{
  char *newp;
  newp = malloc (strlen (line) + 1);

  if (!newp)
    return ASSUAN_Out_Of_Core;

  strcpy_escaped (newp, line);
  if (pinentry.prompt)
    free (pinentry.prompt);
  pinentry.prompt = newp;
  return 0;
}


static int
cmd_seterror (ASSUAN_CONTEXT ctx, char *line)
{
  char *newe;
  newe = malloc (strlen (line) + 1);

  if (!newe)
    return ASSUAN_Out_Of_Core;

  strcpy_escaped (newe, line);
  if (pinentry.error)
    free (pinentry.error);
  pinentry.error = newe;
  return 0;
}


static int
cmd_setok (ASSUAN_CONTEXT ctx, char *line)
{
  char *newo;
  newo = malloc (strlen (line) + 1);

  if (!newo)
    return ASSUAN_Out_Of_Core;

  strcpy_escaped (newo, line);
  if (pinentry.ok)
    free (pinentry.ok);
  pinentry.ok = newo;
  return 0;
}


static int
cmd_setcancel (ASSUAN_CONTEXT ctx, char *line)
{
  char *newc;
  newc = malloc (strlen (line) + 1);

  if (!newc)
    return ASSUAN_Out_Of_Core;

  strcpy_escaped (newc, line);
  if (pinentry.cancel)
    free (pinentry.cancel);
  pinentry.cancel = newc;
  return 0;
}


static int
cmd_getpin (ASSUAN_CONTEXT ctx, char *line)
{
  int result;
  int set_prompt = 0;

  pinentry.pin = secmem_malloc (pinentry.pin_len);
  if (!pinentry.pin)
    return ASSUAN_Out_Of_Core;
  if (!pinentry.prompt)
    {
      pinentry.prompt = "PIN:";
      set_prompt = 1;
    }

  result = (*pinentry_cmd_handler) (&pinentry);
  if (pinentry.error)
    {
      free (pinentry.error);
      pinentry.error = NULL;
    }
  if (set_prompt)
    pinentry.prompt = NULL;

  if (result < 0)
    {
      if (pinentry.pin)
	{
	  secmem_free (pinentry.pin);
	  pinentry.pin = NULL;
	}
      return ASSUAN_Canceled;
    }

  if (result)
    {
      result = assuan_send_data (ctx, pinentry.pin, result);
      if (!result)
	result = assuan_send_data (ctx, NULL, 0);
    }

  if (pinentry.pin)
    {
      secmem_free (pinentry.pin);
      pinentry.pin = NULL;
    }

  return result;
}


static int
cmd_confirm (ASSUAN_CONTEXT ctx, char *line)
{
  int result;

  result = (*pinentry_cmd_handler) (&pinentry);
  if (pinentry.error)
    {
      free (pinentry.error);
      pinentry.error = NULL;
    }

  return result ? 0 : ASSUAN_Not_Confirmed;
}


/* Tell the assuan library about our commands.  */
static int
register_commands (ASSUAN_CONTEXT ctx)
{
  static struct
  {
    const char *name;
    int cmd_id;
    int (*handler) (ASSUAN_CONTEXT, char *line);
  } table[] =
    {
      { "SETDESC",    0,  cmd_setdesc },
      { "SETPROMPT",  0,  cmd_setprompt },
      { "SETERROR",   0,  cmd_seterror },
      { "SETOK",      0,  cmd_setok },
      { "SETCANCEL",  0,  cmd_setcancel },
      { "GETPIN",     0,  cmd_getpin },
      { "CONFIRM",    0,  cmd_confirm },
      { NULL }
    };
  int i, j, rc;

  for (i = j = 0; table[i].name; i++)
    {
      rc = assuan_register_command (ctx,
                                    table[i].cmd_id ? table[i].cmd_id
                                                   : (ASSUAN_CMD_USER + j++),
                                    table[i].name, table[i].handler);
      if (rc)
        return rc;
    } 
  return 0;
}


/* Start the pinentry event loop.  The program will start to process
   Assuan commands until it is finished or an error occurs.  If an
   error occurs, -1 is returned.  Otherwise, 0 is returned.  */
int
pinentry_loop (void)
{
  int rc;
  int filedes[2];
  ASSUAN_CONTEXT ctx;

  /* Extra check to make sure we have dropped privs. */
  if (getuid() != geteuid())
    abort ();

  /* For now we use a simple pipe based server so that we can work
     from scripts.  We will later add options to run as a daemon and
     wait for requests on a Unix domain socket.  */
  filedes[0] = 0;
  filedes[1] = 1;
  rc = assuan_init_pipe_server (&ctx, filedes);
  if (rc)
    {
      fprintf (stderr, "%s: failed to initialize the server: %s\n",
               this_pgmname, assuan_strerror(rc));
      return -1;
    }
  rc = register_commands (ctx);
  if (rc)
    {
      fprintf (stderr, "%s: failed to the register commands with Assuan: %s\n",
               this_pgmname, assuan_strerror(rc));
      return -1;
    }

  assuan_register_option_handler (ctx, option_handler);
#if 0
  assuan_set_log_stream (ctx, stderr);
#endif
  
  for (;;)
    {
      rc = assuan_accept (ctx);
      if (rc == -1)
          break;
      else if (rc)
        {
          fprintf (stderr, "%s: Assuan accept problem: %s\n",
                   this_pgmname, assuan_strerror (rc));
          break;
        }
      
      rc = assuan_process (ctx);
      if (rc)
        {
          fprintf (stderr, "%s: Assuan processing failed: %s\n",
                   this_pgmname, assuan_strerror (rc));
          continue;
        }
    }

  assuan_deinit_server (ctx);
  return 0;
}
