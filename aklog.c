/* 
 * $Id: aklog.c,v 1.1 1990-06-22 18:03:34 qjb Exp $
 * $Source: /afs/dev.mit.edu/source/repository/athena/bin/aklog/aklog.c,v $
 * $Author: qjb $
 *
 */

#if !defined(lint) && !defined(SABER)
static char *rcsid = "$Id: aklog.c,v 1.1 1990-06-22 18:03:34 qjb Exp $";
#endif /* lint || SABER */

#include <aklog.h>

#ifdef __STDC__
main(int argc, char *argv[])
#else
main(argc, argv)
  int argc;
  char *argv[];
#endif /* __STDC__ */
{
    aklog_params params;

    aklog_init_params(&params);
    aklog(argc, argv, &params);
}
