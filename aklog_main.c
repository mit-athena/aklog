/* 
 * $Id: aklog_main.c,v 1.31 1998-03-30 19:01:11 danw Exp $
 *
 * Copyright 1990,1991 by the Massachusetts Institute of Technology
 * For distribution and copying rights, see the file "mit-copyright.h"
 */

static const char rcsid[] = "$Id: aklog_main.c,v 1.31 1998-03-30 19:01:11 danw Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/param.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <krb.h>

#include <afs/param.h>
#include <afs/auth.h>
#include <afs/cellconfig.h>
#include <afs/vice.h>
#include <afs/venus.h>
#include <afs/ptserver.h>

#include "aklog.h"
#include "linked_list.h"

#define AFSKEY "afs"
#define AFSINST ""

#define AKLOG_SUCCESS 0
#define AKLOG_USAGE 1
#define AKLOG_SOMETHINGSWRONG 2
#define AKLOG_AFS 3
#define AKLOG_KERBEROS 4
#define AKLOG_TOKEN 5
#define AKLOG_BADPATH 6
#define AKLOG_MISC 7

#ifndef NULL
#define NULL 0
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#define DIR '/'			/* Character that divides directories */
#define DIRSTRING "/"		/* String form of above */
#define VOLMARKER ':'		/* Character separating cellname from mntpt */
#define VOLMARKERSTRING ":"	/* String form of above */

typedef struct {
    char cell[BUFSIZ];
    char realm[REALM_SZ];
} cellinfo_t;


struct afsconf_cell ak_cellconfig; /* General information about the cell */

static aklog_params params;	/* Various aklog functions */
static char msgbuf[BUFSIZ];	/* String for constructing error messages */
static char *progname = NULL;	/* Name of this program */
static int dflag = FALSE;	/* Give debugging information */
static int noauth = FALSE;	/* If true, don't try to get tokens */
static int zsubs = FALSE;	/* Are we keeping track of zephyr subs? */
static int hosts = FALSE;	/* Are we keeping track of hosts? */
static int noprdb = FALSE;	/* Skip resolving name to id? */
static int force = FALSE;	/* Bash identical tokens? */
static linked_list zsublist;	/* List of zephyr subscriptions */
static linked_list hostlist;	/* List of host addresses */
static linked_list authedcells;	/* List of cells already logged to */


static char *afs_realm_of_cell(cellconfig)
    struct afsconf_cell *cellconfig;
{
    char krbhst[MAX_HSTNM];
    static char krbrlm[REALM_SZ+1];

    if (!cellconfig)
	return 0;
    
    strcpy(krbrlm, (char *)krb_realmofhost(cellconfig->hostName[0]));
    if (krb_get_admhst(krbhst, krbrlm, 1) != KSUCCESS) {
	char *s = krbrlm;
	char *t = cellconfig->name;
	int c;

	while (c = *t++) {
	    if (islower(c)) c=toupper(c);
	    *s++ = c;
	}
	*s++ = 0;
    }
    return krbrlm;
}

static char *copy_cellinfo(cellinfo_t *cellinfo)
{
    cellinfo_t *new_cellinfo;

    if (new_cellinfo = (cellinfo_t *)malloc(sizeof(cellinfo_t)))
	memcpy(new_cellinfo, cellinfo, sizeof(cellinfo_t));
    
    return ((char *)new_cellinfo);
}


static char *copy_string(char *string)    
{
    char *new_string;

    if (new_string = (char *)calloc(strlen(string) + 1, sizeof(char))) 
	(void) strcpy(new_string, string);

    return (new_string);
}


static int get_cellconfig(char *cell, struct afsconf_cell *cellconfig,
			  char *local_cell)
{
    int status = AKLOG_SUCCESS;
    struct afsconf_dir *configdir;

    memset(local_cell, 0, sizeof(local_cell));
    memset(cellconfig, 0, sizeof(*cellconfig));

    if (!(configdir = afsconf_Open(AFSCONF_CLIENTNAME))) {
	sprintf(msgbuf, 
		"%s: can't get afs configuration (afsconf_Open(%s))\n",
		progname, AFSCONF_CLIENTNAME);
	params.pstderr(msgbuf);
	params.exitprog(AKLOG_AFS);
    }

    if (afsconf_GetLocalCell(configdir, local_cell, MAXCELLCHARS)) {
	sprintf(msgbuf, "%s: can't determine local cell.\n", progname);
	params.pstderr(msgbuf);
	params.exitprog(AKLOG_AFS);
    }

    if ((cell == NULL) || (cell[0] == 0))
	cell = local_cell;

    if (afsconf_GetCellInfo(configdir, cell, NULL, cellconfig)) {
	sprintf(msgbuf, "%s: Can't get information about cell %s.\n",
		progname, cell);
	params.pstderr(msgbuf);
	status = AKLOG_AFS;
    }

    (void) afsconf_Close(configdir);

    return(status);
}


/* 
 * Log to a cell.  If the cell has already been logged to, return without
 * doing anything.  Otherwise, log to it and mark that it has been logged
 * to.
 */
static int auth_to_cell(char *cell, char *realm)
{
    int status = AKLOG_SUCCESS;
    char username[BUFSIZ];	/* To hold client username structure */
    long viceId;		/* AFS uid of user */

    char name[ANAME_SZ];	/* Name of afs key */
    char instance[INST_SZ];	/* Instance of afs key */
    char realm_of_user[REALM_SZ]; /* Kerberos realm of user */
    char realm_of_cell[REALM_SZ]; /* Kerberos realm of cell */
    char local_cell[MAXCELLCHARS+1];
    char cell_to_use[MAXCELLCHARS+1]; /* Cell to authenticate to */

    int i,j;

    CREDENTIALS c;
    struct ktc_principal aserver;
    struct ktc_principal aclient;
    struct ktc_token atoken, btoken;
    
    /* try to avoid an expensive call to get_cellconfig */
    if (cell && ll_string(&authedcells, ll_s_check, cell)) {
	if (dflag) {
	    sprintf(msgbuf, "Already authenticated to %s (or tried to)\n", 
		    cell);
	    params.pstdout(msgbuf);
	}
	return(AKLOG_SUCCESS);
    }

    memset(name, 0, sizeof(name));
    memset(instance, 0, sizeof(instance));
    memset(realm_of_user, 0, sizeof(realm_of_user));
    memset(realm_of_cell, 0, sizeof(realm_of_cell));

    /* NULL or empty cell returns information on local cell */
    if (status = get_cellconfig(cell, &ak_cellconfig, local_cell))
	return(status);

    strncpy(cell_to_use, ak_cellconfig.name, MAXCELLCHARS);
    cell_to_use[MAXCELLCHARS] = 0;

    if (ll_string(&authedcells, ll_s_check, cell_to_use)) {
	if (dflag) {
	    sprintf(msgbuf, "Already authenticated to %s (or tried to)\n", 
		    cell_to_use);
	    params.pstdout(msgbuf);
	}
	return(AKLOG_SUCCESS);
    }

    /* 
     * Record that we have attempted to log to this cell.  We do this
     * before we try rather than after so that we will not try
     * and fail repeatedly for one cell.
     */
    (void)ll_string(&authedcells, ll_s_add, cell_to_use);

    /* 
     * Record this cell in the list of zephyr subscriptions.  We may
     * want zephyr subscriptions even if authentication fails.
     * If this is done after we attempt to get tokens, aklog -zsubs
     * can return something different depending on whether or not we
     * are in -noauth mode.
     */
    if (ll_string(&zsublist, ll_s_add, cell_to_use) == LL_FAILURE) {
	sprintf(msgbuf, 
		"%s: failure adding cell %s to zephyr subscriptions list.\n",
		progname, cell_to_use);
	params.pstderr(msgbuf);
	params.exitprog(AKLOG_MISC);
    }
    if (ll_string(&zsublist, ll_s_add, local_cell) == LL_FAILURE) {
	sprintf(msgbuf, 
		"%s: failure adding cell %s to zephyr subscriptions list.\n",
		progname, local_cell);
	params.pstderr(msgbuf);
	params.exitprog(AKLOG_MISC);
    }

    if (!noauth) {
	if (dflag) {
	    sprintf(msgbuf, "Authenticating to cell %s.\n", cell_to_use);
	    params.pstdout(msgbuf);
	}
	
	if (realm && realm[0])
	    strcpy(realm_of_cell, realm);
	else
	    strcpy(realm_of_cell, afs_realm_of_cell(&ak_cellconfig));

	/* We use the afs.<cellname> convention here... */
	strcpy(name, AFSKEY);
	strncpy(instance, cell_to_use, sizeof(instance));
	instance[sizeof(instance)-1] = '\0';
	
	/* 
	 * Extract the session key from the ticket file and hand-frob an
	 * afs style authenticator.
	 */

	/*
	 * Try to obtain AFS tickets.  Because there are two valid service
	 * names, we will try both, but trying the more specific first.
	 *
	 * 	afs.<cell>@<realm>
	 * 	afs@<realm>
	 */
	if (dflag) {
	    sprintf(msgbuf, "Getting tickets: %s.%s@%s\n", name, instance, 
		    realm_of_cell);
	    params.pstdout(msgbuf);
	}
	status = params.get_cred(name, instance, realm_of_cell, &c);
	if (status == KDC_PR_UNKNOWN) {
	    if (dflag) {
		sprintf(msgbuf, "Getting tickets: %s@%s\n", name,
			realm_of_cell);
		params.pstdout(msgbuf);
	    }
	    status = params.get_cred(name, "", realm_of_cell, &c);
	}

	if (status != KSUCCESS) {
	    if (dflag) {
		sprintf(msgbuf, 
			"Kerberos error code returned by get_cred: %d\n",
			status);
		params.pstdout(msgbuf);
	    }
	    sprintf(msgbuf, "%s: Couldn't get %s AFS tickets: %s\n",
		    progname, cell_to_use, krb_err_txt[status]);
	    params.pstderr(msgbuf);
	    return(AKLOG_KERBEROS);
	}
	
	strncpy(aserver.name, AFSKEY, MAXKTCNAMELEN - 1);
	strncpy(aserver.instance, AFSINST, MAXKTCNAMELEN - 1);
	strncpy(aserver.cell, cell_to_use, MAXKTCREALMLEN - 1);

	strcpy (username, c.pname);
	if (c.pinst[0]) {
	    strcat(username, ".");
	    strcat(username, c.pinst);
	}

	atoken.kvno = c.kvno;
	atoken.startTime = c.issue_date;
	/* ticket lifetime is in five-minutes blocks. */
	atoken.endTime = c.issue_date + ((unsigned char)c.lifetime * 5 * 60);
	memcpy(&atoken.sessionKey, c.session, 8);
	atoken.ticketLen = c.ticket_st.length;
	memcpy(atoken.ticket, c.ticket_st.dat, atoken.ticketLen);
	
	if (!force &&
	    !ktc_GetToken(&aserver, &btoken, sizeof(btoken), &aclient) &&
	    atoken.kvno == btoken.kvno &&
	    atoken.ticketLen == btoken.ticketLen &&
	    !memcmp(&atoken.sessionKey, &btoken.sessionKey, sizeof(atoken.sessionKey)) &&
	    !memcmp(atoken.ticket, btoken.ticket, atoken.ticketLen)) {

	    if (dflag) {
		sprintf(msgbuf, "Identical tokens already exist; skipping.\n");
		params.pstdout(msgbuf);
	    }
	    return 0;
	}

	if (noprdb) {
	    if (dflag) {
		sprintf(msgbuf, "Not resolving name %s to id (-noprdb set)\n",
			username);
		params.pstdout(msgbuf);
	    }
	}
	else {
	    if ((status = params.get_user_realm(realm_of_user)) != KSUCCESS) {
		sprintf(msgbuf, "%s: Couldn't determine realm of user: %s)",
			progname, krb_err_txt[status]);
		params.pstderr(msgbuf);
		return(AKLOG_KERBEROS);
	    }
	    if (strcmp(realm_of_user, realm_of_cell)) {
		strcat(username, "@");
		strcat(username, realm_of_user);
	    }

	    if (dflag) {
		sprintf(msgbuf, "About to resolve name %s to id\n", 
			username);
		params.pstdout(msgbuf);
	    }
	    
	    if (!pr_Initialize (0, AFSCONF_CLIENTNAME, aserver.cell))
		    status = pr_SNameToId (username, &viceId);
	    
	    if (dflag) {
		if (status) 
		    sprintf(msgbuf, "Error %d\n", status);
		else
		    sprintf(msgbuf, "Id %d\n", viceId);
		params.pstdout(msgbuf);
	    }
	    
		/*
		 * This is a crock, but it is Transarc's crock, so
		 * we have to play along in order to get the
		 * functionality.  The way the afs id is stored is
		 * as a string in the username field of the token.
		 * Contrary to what you may think by looking at
		 * the code for tokens, this hack (AFS ID %d) will
		 * not work if you change %d to something else.
		 */
	    if ((status == 0) && (viceId != ANONYMOUSID))
		sprintf (username, "AFS ID %d", viceId);
	}
	
	if (dflag) {
	    sprintf(msgbuf, "Set username to %s\n", username);
	    params.pstdout(msgbuf);
	}

	/* Reset the "aclient" structure before we call ktc_SetToken.
	 * This structure was first set by the ktc_GetToken call when
	 * we were comparing whether identical tokens already existed.
	 */
	strncpy(aclient.name, username, MAXKTCNAMELEN - 1);
	strcpy(aclient.instance, "");
	strncpy(aclient.cell, c.realm, MAXKTCREALMLEN - 1);

	if (dflag) {
	    sprintf(msgbuf, "Getting tokens.\n");
	    params.pstdout(msgbuf);
	}
	if (status = ktc_SetToken(&aserver, &atoken, &aclient, 0)) {
	    sprintf(msgbuf, 
		    "%s: unable to obtain tokens for cell %s (status: %d).\n",
		    progname, cell_to_use, status);
	    params.pstderr(msgbuf);
	    status = AKLOG_TOKEN;
	}
    }
    else
	if (dflag) {
	    sprintf(msgbuf, "Noauth mode; not authenticating.\n");
	    params.pstdout(msgbuf);
	}
	
    return(status);
}

static int get_afs_mountpoint(char *file, char *mountpoint, int size)
{
    char our_file[MAXPATHLEN + 1];
    char *parent_dir;
    char *last_component;
    struct ViceIoctl vio;
    char cellname[BUFSIZ];

    memset(our_file, 0, sizeof(our_file));
    strcpy(our_file, file);

    if (last_component = strrchr(our_file, DIR)) {
	*last_component++ = 0;
	parent_dir = our_file;
    }
    else {
	last_component = our_file;
	parent_dir = ".";
    }    
    
    memset(cellname, 0, sizeof(cellname));

    vio.in = last_component;
    vio.in_size = strlen(last_component)+1;
    vio.out_size = size;
    vio.out = mountpoint;

    if (!pioctl(parent_dir, VIOC_AFS_STAT_MT_PT, &vio, 0)) {
	if (strchr(mountpoint, VOLMARKER) == NULL) {
	    vio.in = file;
	    vio.in_size = strlen(file) + 1;
	    vio.out_size = sizeof(cellname);
	    vio.out = cellname;
	    
	    if (!pioctl(file, VIOC_FILE_CELL_NAME, &vio, 1)) {
		strcat(cellname, VOLMARKERSTRING);
		strcat(cellname, mountpoint + 1);
		memset(mountpoint + 1, 0, size - 1);
		strcpy(mountpoint + 1, cellname);
	    }
	}
	return(TRUE);
    }
    else
	return(FALSE);
}

/* 
 * This routine each time it is called returns the next directory 
 * down a pathname.  It resolves all symbolic links.  The first time
 * it is called, it should be called with the name of the path
 * to be descended.  After that, it should be called with the arguemnt
 * NULL.
 */
static char *next_path(char *origpath)
{
    static char path[MAXPATHLEN + 1];
    static char pathtocheck[MAXPATHLEN + 1];

    int link = FALSE;		/* Is this a symbolic link? */
    char linkbuf[MAXPATHLEN + 1];
    char tmpbuf[MAXPATHLEN + 1];

    static char *last_comp;	/* last component of directory name */
    static char *elast_comp;	/* End of last component */
    char *t;
    int len;
    
    static int symlinkcount = 0; /* We can't exceed MAXSYMLINKS */
    
    /* If we are given something for origpath, we are initializing only. */
    if (origpath) {
	memset(path, 0, sizeof(path));
	memset(pathtocheck, 0, sizeof(pathtocheck));
	strcpy(path, origpath);
	last_comp = path;
	symlinkcount = 0;
	return(NULL);
    }

    /* We were not given origpath; find then next path to check */
    
    /* If we've gotten all the way through already, return NULL */
    if (last_comp == NULL)
	return(NULL);

    do {
	while (*last_comp == DIR)
	    strncat(pathtocheck, last_comp++, 1);
	len = (elast_comp = strchr(last_comp, DIR)) 
	    ? elast_comp - last_comp : strlen(last_comp);
	strncat(pathtocheck, last_comp, len);
	memset(linkbuf, 0, sizeof(linkbuf));
	if (link = (params.readlink(pathtocheck, linkbuf, 
				    sizeof(linkbuf)) > 0)) {
	    if (++symlinkcount > MAXSYMLINKS) {
		sprintf(msgbuf, "%s: %s\n", progname, strerror(ELOOP));
		params.pstderr(msgbuf);
		params.exitprog(AKLOG_BADPATH);
	    }
	    memset(tmpbuf, 0, sizeof(tmpbuf));
	    if (elast_comp)
		strcpy(tmpbuf, elast_comp);
	    if (linkbuf[0] == DIR) {
		/* 
		 * If this is a symbolic link to an absolute path, 
		 * replace what we have by the absolute path.
		 */
		memset(path, 0, strlen(path));
		memcpy(path, linkbuf, sizeof(linkbuf));
		strcat(path, tmpbuf);
		last_comp = path;
		elast_comp = NULL;
		memset(pathtocheck, 0, sizeof(pathtocheck));
	    }
	    else {
		/* 
		 * If this is a symbolic link to a relative path, 
		 * replace only the last component with the link name.
		 */
		strncpy(last_comp, linkbuf, strlen(linkbuf) + 1);
		strcat(path, tmpbuf);
		elast_comp = NULL;
		if (t = strrchr(pathtocheck, DIR)) {
		    t++;
		    memset(t, 0, strlen(t));
		}
		else
		    memset(pathtocheck, 0, sizeof(pathtocheck));
	    }
	}
	else
	    last_comp = elast_comp;
    }
    while(link);

    return(pathtocheck);
}
	
static void add_hosts(char *file)
{
    struct ViceIoctl vio;
    char outbuf[BUFSIZ];
    long *phosts;
    int i;
    struct hostent *hp;
    struct in_addr in;
    
    memset(outbuf, 0, sizeof(outbuf));

    vio.out_size = sizeof(outbuf);
    vio.in_size = 0;
    vio.out = outbuf;

    if (dflag) {
	sprintf(msgbuf, "Getting list of hosts for %s\n", file);
	params.pstdout(msgbuf);
    }
    /* Don't worry about errors. */
    if (!pioctl(file, VIOCWHEREIS, &vio, 1)) {
	phosts = (long *) outbuf;

	/*
	 * Lists hosts that we care about.  If ALLHOSTS is defined,
	 * then all hosts that you ever may possible go through are
	 * included in this list.  If not, then only hosts that are
	 * the only ones appear.  That is, if a volume you must use
	 * is replaced on only one server, that server is included.
	 * If it is replicated on many servers, then none are included.
	 * This is not perfect, but the result is that people don't
	 * get subscribed to a lot of instances of FILSRV that they
	 * probably won't need which reduces the instances of 
	 * people getting messages that don't apply to them.
	 */
#ifndef ALLHOSTS
	if (phosts[1] != '\0')
	    return;
#endif
	for (i = 0; phosts[i]; i++) {
	    if (hosts) {
		in.s_addr = phosts[i];
		if (dflag) {
		    sprintf(msgbuf, "Got host %s\n", inet_ntoa(in));
		    params.pstdout(msgbuf);
		}
		ll_string(&hostlist, ll_s_add, (char *)inet_ntoa(in));
	    }
	    if (zsubs && (hp=gethostbyaddr(&phosts[i],sizeof(long),AF_INET))) {
		if (dflag) {
		    sprintf(msgbuf, "Got host %s\n", hp->h_name);
		    params.pstdout(msgbuf);
		}
		ll_string(&zsublist, ll_s_add, hp->h_name);
	    }
	}
    }
}
    
/*
 * This routine descends through a path to a directory, logging to 
 * every cell it encounters along the way.
 */
static int auth_to_path(char *path)
{
    int status = AKLOG_SUCCESS;
    int auth_to_cell_status = AKLOG_SUCCESS;

    char *nextpath;
    char pathtocheck[MAXPATHLEN + 1];
    char mountpoint[MAXPATHLEN + 1];

    char *cell;
    char *endofcell;

    u_char isdir;

    /* Initialize */
    if (path[0] == DIR)
	strcpy(pathtocheck, path);
    else {
	if (params.getcwd(pathtocheck, sizeof(pathtocheck)) == NULL) {
	    sprintf(msgbuf, "Unable to find current working directory:\n");
	    params.pstderr(msgbuf);
	    sprintf(msgbuf, "%s\n", pathtocheck);
	    params.pstderr(msgbuf);
	    sprintf(msgbuf, "Try an absolute pathname.\n");
	    params.pstderr(msgbuf);
	    params.exitprog(AKLOG_BADPATH);
	}
	else {
	    strcat(pathtocheck, DIRSTRING);
	    strcat(pathtocheck, path);
	}
    }
    next_path(pathtocheck);

    /* Go on to the next level down the path */
    while (nextpath = next_path(NULL)) {
	strcpy(pathtocheck, nextpath);
	if (dflag) {
	    sprintf(msgbuf, "Checking directory %s\n", pathtocheck);
	    params.pstdout(msgbuf);
	}
	/* 
	 * If this is an afs mountpoint, determine what cell from 
	 * the mountpoint name which is of the form 
	 * #cellname:volumename or %cellname:volumename.
	 */
	if (get_afs_mountpoint(pathtocheck, mountpoint, sizeof(mountpoint))) {
	    /* skip over the '#' or '%' */
	    cell = mountpoint + 1;
	    /* Add this (cell:volumename) to the list of zsubs */
	    if (zsubs)
		ll_string(&zsublist, ll_s_add, cell);
	    if (zsubs || hosts)
		add_hosts(pathtocheck);
	    if (endofcell = strchr(mountpoint, VOLMARKER)) {
		*endofcell = '\0';
		if (auth_to_cell_status = auth_to_cell(cell, NULL)) {
		    if (status == AKLOG_SUCCESS)
			status = auth_to_cell_status;
		    else if (status != auth_to_cell_status)
			status = AKLOG_SOMETHINGSWRONG;
		}
	    }
	}
	else {
	    if (params.isdir(pathtocheck, &isdir) < 0) {
		/*
		 * If we've logged and still can't stat, there's
		 * a problem... 
		 */
		sprintf(msgbuf, "%s: stat(%s): %s\n", progname, 
			pathtocheck, strerror(errno));
		params.pstderr(msgbuf);
		return(AKLOG_BADPATH);
	    }
	    else if (! isdir) {
		/* Allow only directories */
		sprintf(msgbuf, "%s: %s: %s\n", progname, pathtocheck,
			strerror(ENOTDIR));
		params.pstderr(msgbuf);
		return(AKLOG_BADPATH);
	    }
	}
    }
    

    return(status);
}

/* Print usage message and exit */
static void usage(void)
{
    sprintf(msgbuf, "\nUsage: %s %s%s%s\n", progname,
	    "[-d] [[-cell | -c] cell [-k krb_realm]] ",
	    "[[-p | -path] pathname]\n",
	    "    [-zsubs] [-hosts] [-noauth] [-noprdb]\n");
    params.pstderr(msgbuf);
    sprintf(msgbuf, "    -d gives debugging information.\n");
    params.pstderr(msgbuf);
    sprintf(msgbuf, "    krb_realm is the kerberos realm of a cell.\n");
    params.pstderr(msgbuf);
    sprintf(msgbuf, "    pathname is the name of a directory to which ");
    params.pstderr(msgbuf);
    sprintf(msgbuf, "you wish to authenticate.\n");
    params.pstderr(msgbuf);
    sprintf(msgbuf, "    -zsubs gives zephyr subscription information.\n");
    params.pstderr(msgbuf);
    sprintf(msgbuf, "    -hosts gives host address information.\n");
    params.pstderr(msgbuf);
    sprintf(msgbuf, "    -noauth does not attempt to get tokens.\n");
    params.pstderr(msgbuf);
    sprintf(msgbuf, "    -noprdb means don't try to determine AFS ID.\n");
    params.pstderr(msgbuf);
    sprintf(msgbuf, "    No commandline arguments means ");
    params.pstderr(msgbuf);
    sprintf(msgbuf, "authenticate to the local cell.\n");
    params.pstderr(msgbuf);
    sprintf(msgbuf, "\n");
    params.pstderr(msgbuf);
    params.exitprog(AKLOG_USAGE);
}

void aklog(int argc, char *argv[], aklog_params *a_params)
{
    int status = AKLOG_SUCCESS;
    int i;
    int somethingswrong = FALSE;

    cellinfo_t cellinfo;

    extern char *progname;	/* Name of this program */

    extern int dflag;		/* Debug mode */

    int cmode = FALSE;		/* Cellname mode */
    int pmode = FALSE;		/* Path name mode */

    char realm[REALM_SZ];	/* Kerberos realm of afs server */
    char cell[BUFSIZ];		/* Cell to which we are authenticating */
    char path[MAXPATHLEN + 1];		/* Path length for path mode */

    linked_list cells;		/* List of cells to log to */
    linked_list paths;		/* List of paths to log to */
    ll_node *cur_node;

    memset(&cellinfo, 0, sizeof(cellinfo));

    memset(realm, 0, sizeof(realm));
    memset(cell, 0, sizeof(cell));
    memset(path, 0, sizeof(path));

    ll_init(&cells);
    ll_init(&paths);

    ll_init(&zsublist);
    ll_init(&hostlist);

    /* Store the program name here for error messages */
    if (progname = strrchr(argv[0], DIR))
	progname++;
    else
	progname = argv[0];

    memcpy(&params, a_params, sizeof(aklog_params));

    /* Initialize list of cells to which we have authenticated */
    (void)ll_init(&authedcells);

    /* Parse commandline arguments and make list of what to do. */
    for (i = 1; i < argc; i++) {
	if (strcmp(argv[i], "-d") == 0)
	    dflag++;
	else if (strcmp(argv[i], "-noauth") == 0) 
	    noauth++;
	else if (strcmp(argv[i], "-zsubs") == 0)
	    zsubs++;
	else if (strcmp(argv[i], "-hosts") == 0)
	    hosts++;
	else if (strcmp(argv[i], "-noprdb") == 0)
	    noprdb++;
	else if (strcmp(argv[i], "-force") == 0)
	    force++;
	else if (((strcmp(argv[i], "-cell") == 0) ||
		  (strcmp(argv[i], "-c") == 0)) && !pmode)
	    if (++i < argc) {
		cmode++;
		strcpy(cell, argv[i]);
	    }
	    else
		usage();
	else if (((strcmp(argv[i], "-path") == 0) ||
		  (strcmp(argv[i], "-p") == 0)) && !cmode)
	    if (++i < argc) {
		pmode++;
		strcpy(path, argv[i]);
	    }
	    else
		usage();
	else if (argv[i][0] == '-')
	    usage();
	else if (!pmode && !cmode) {
	    if (strchr(argv[i], DIR) || (strcmp(argv[i], ".") == 0) ||
		(strcmp(argv[i], "..") == 0)) {
		pmode++;
		strcpy(path, argv[i]);
	    }
	    else { 
		cmode++;
		strcpy(cell, argv[i]);
	    }
	}
	else
	    usage();

	if (cmode) {
	    if (((i + 1) < argc) && (strcmp(argv[i + 1], "-k") == 0)) {
		i+=2;
		if (i < argc)
		    strcpy(realm, argv[i]);
		else
		    usage();
	    }
	    /* Add this cell to list of cells */
	    strcpy(cellinfo.cell, cell);
	    strcpy(cellinfo.realm, realm);
	    if (cur_node = ll_add_node(&cells, ll_tail)) {
		char *new_cellinfo;
		if (new_cellinfo = copy_cellinfo(&cellinfo))
		    ll_add_data(cur_node, new_cellinfo);
		else {
		    sprintf(msgbuf, 
			    "%s: failure copying cellinfo.\n", progname);
		    params.pstderr(msgbuf);
		    params.exitprog(AKLOG_MISC);
		}
	    }
	    else {
		sprintf(msgbuf, "%s: failure adding cell to cells list.\n",
			progname);
		params.pstderr(msgbuf);
		params.exitprog(AKLOG_MISC);
	    }
	    memset(&cellinfo, 0, sizeof(cellinfo));
	    cmode = FALSE;
	    memset(cell, 0, sizeof(cell));
	    memset(realm, 0, sizeof(realm));
	}
	else if (pmode) {
	    /* Add this path to list of paths */
	    if (cur_node = ll_add_node(&paths, ll_tail)) {
		char *new_path; 
		if (new_path = copy_string(path)) 
		    ll_add_data(cur_node, new_path);
		else {
		    sprintf(msgbuf, "%s: failure copying path name.\n",
			    progname);
		    params.pstderr(msgbuf);
		    params.exitprog(AKLOG_MISC);
		}
	    }
	    else {
		sprintf(msgbuf, "%s: failure adding path to paths list.\n",
			progname);
		params.pstderr(msgbuf);
		params.exitprog(AKLOG_MISC);
	    }
	    pmode = FALSE;
	    memset(path, 0, sizeof(path));
	}
    }

    /* If nothing was given, log to the local cell. */
    if ((cells.nelements + paths.nelements) == 0)
	status = auth_to_cell(NULL, NULL);
    else {
	/* Log to all cells in the cells list first */
	for (cur_node = cells.first; cur_node; cur_node = cur_node->next) {
	    memcpy(&cellinfo, cur_node->data, sizeof(cellinfo));
	    if (status = auth_to_cell(cellinfo.cell, cellinfo.realm))
		somethingswrong++;
	}
	
	/* Then, log to all paths in the paths list */
	for (cur_node = paths.first; cur_node; cur_node = cur_node->next) {
	    if (status = auth_to_path(cur_node->data))
		somethingswrong++;
	}
	
	/* 
	 * If only one thing was logged to, we'll return the status 
	 * of the single call.  Otherwise, we'll return a generic
	 * something failed status.
	 */
	if (somethingswrong && ((cells.nelements + paths.nelements) > 1))
	    status = AKLOG_SOMETHINGSWRONG;
    }

    /* If we are keeping track of zephyr subscriptions, print them. */
    if (zsubs) 
	for (cur_node = zsublist.first; cur_node; cur_node = cur_node->next) {
	    sprintf(msgbuf, "zsub: %s\n", cur_node->data);
	    params.pstdout(msgbuf);
	}

    /* If we are keeping track of host information, print it. */
    if (hosts)
	for (cur_node = hostlist.first; cur_node; cur_node = cur_node->next) {
	    sprintf(msgbuf, "host: %s\n", cur_node->data);
	    params.pstdout(msgbuf);
	}

    params.exitprog(status);
}
