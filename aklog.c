/* 
 * $Id: aklog.c,v 1.4 1997-11-17 16:23:47 ghudson Exp $
 *
 * Copyright 1990,1991 by the Massachusetts Institute of Technology
 * For distribution and copying rights, see the file "mit-copyright.h"
 */

static const char rcsid[] = "$Id: aklog.c,v 1.4 1997-11-17 16:23:47 ghudson Exp $";

#include "aklog.h"

int main(int argc, char *argv[])
{
    aklog_params params;

    aklog_init_params(&params);
    aklog(argc, argv, &params);
}
