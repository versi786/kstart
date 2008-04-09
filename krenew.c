/* $Id$
 *
 * Automatically renew a Kerberos v5 ticket.
 *
 * Similar to k5start, krenew can run as a daemon or run a specified program
 * and wait until it completes.  Rather than obtaining fresh Kerberos
 * credentials, however, it uses an existing Kerberos ticket cache and tries
 * to renew the tickets until it exits or until the ticket cannot be renewed
 * any longer.
 *
 * Written by Russ Allbery <rra@stanford.edu>
 * Copyright 2006, 2007, 2008
 *     Board of Trustees, Leland Stanford Jr. University
 *
 * See LICENSE for licensing terms.
*/

#include <config.h>
#include <portable/system.h>
#include <portable/kafs.h>
#include <portable/time.h>

#include <errno.h>

#include <util/util.h>

/*
 * The number of seconds of fudge to add to the check for whether we need to
 * obtain a new ticket.  This is here to make sure that we don't wake up just
 * as the ticket is expiring.
 */
#define EXPIRE_FUDGE (2 * 60)

/* The usage message. */
const char usage_message[] = "\
Usage: krenew [options] [command]\n\
   -b                   Fork and run in the background\n\
   -c <file>            Write child process ID (PID) to <file>\n\
   -h                   Display this usage message and exit\n\
   -K <interval>        Run as daemon, renew ticket every <interval> minutes\n\
                        (implies -q unless -v is given)\n\
   -k <file>            Use <file> as the ticket cache\n\
   -p <file>            Write process ID (PID) to <file>\n\
   -t                   Get AFS token via aklog or KINIT_PROG\n\
   -v                   Verbose\n\
\n\
If the environment variable KINIT_PROG is set to a program (such as aklog)\n\
then this program will be executed when requested by the -t flag.\n\
Otherwise, %s.\n";


/*
 * Print out the usage message and then exit with the status given as the
 * only argument.  If status is zero, the message is printed to standard
 * output; otherwise, it is sent to standard error.
 */
static void
usage(int status)
{
    fprintf((status == 0) ? stdout : stderr, usage_message,
            ((PATH_AKLOG[0] == '\0')
             ? "using -t is an error"
             : "the default program to run is " PATH_AKLOG));
    exit(status);
}


/*
 * Given a context and a principal, get the realm.  This works differently in
 * MIT Kerberos and Heimdal, unfortunately.
 */
static char *
get_realm(krb5_context ctx UNUSED, krb5_principal princ)
{
#ifdef HAVE_KRB5_REALM
    krb5_realm *realm;

    realm = krb5_princ_realm(ctx, princ);
    if (realm == NULL)
        die("cannot get local Kerberos realm");
    return krb5_realm_data(*realm);
#else
    krb5_data *data;

    data = krb5_princ_realm(ctx, princ);
    if (data == NULL)
        die("cannot get local Kerberos realm");
    return data->data;
#endif
}


/*
 * Get the principal name for the krbtgt ticket for the local realm.  The
 * caller is responsible for freeing the principal.  Takes an existing
 * principal to get the realm from and returns a Kerberos v5 error on
 * failure.
 */
static krb5_error_code
get_krbtgt_princ(krb5_context ctx, krb5_principal user, krb5_principal *princ)
{
    char *realm;

    realm = get_realm(ctx, user);
    return krb5_build_principal(ctx, princ, strlen(realm), realm, "krbtgt",
                                realm, (const char *) NULL);
}


/*
 * Check whether a ticket will expire within the given number of minutes.
 * Takes the cache and the number of minutes.  Returns a Kerberos status code.
 */
static krb5_error_code
ticket_expired(krb5_context ctx, krb5_ccache cache, int keep_ticket)
{
    krb5_creds increds, *outcreds = NULL;
    time_t now, then;
    krb5_error_code status;

    /* Obtain the ticket. */
    memset(&increds, 0, sizeof(increds));
    status = krb5_cc_get_principal(ctx, cache, &increds.client);
    if (status != 0)
        die_krb5(ctx, status, "error reading cache");
    status = get_krbtgt_princ(ctx, increds.client, &increds.server);
    if (status != 0)
        die_krb5(ctx, status, "error building ticket name");
    status = krb5_get_credentials(ctx, 0, cache, &increds, &outcreds);
    if (status != 0)
        die_krb5(ctx, status, "cannot get current credentials");

    /* Check the expiration time. */
    if (status == 0) {
        now = time(NULL);
        then = outcreds->times.endtime;
        if (then < now + 60 * keep_ticket + EXPIRE_FUDGE)
            status = KRB5KRB_AP_ERR_TKT_EXPIRED;
    }

    /* Free memory. */
    krb5_free_cred_contents(ctx, &increds);
    if (outcreds != NULL)
        krb5_free_creds(ctx, outcreds);

    return status;
}


/*
 * Renew the user's tickets, exiting with an error if this isn't possible.
 */
static void
renew(krb5_context ctx, krb5_ccache cache, int verbose)
{
    krb5_error_code status;
    krb5_principal user;
    krb5_creds creds, *out;
#ifndef HAVE_KRB5_GET_RENEWED_CREDS
    krb5_kdc_flags flags;
    krb5_creds in, *old = NULL;
#endif

    memset(&creds, 0, sizeof(creds));
    status = krb5_cc_get_principal(ctx, cache, &user);
    if (status != 0)
        die_krb5(ctx, status, "error reading cache");
    if (verbose) {
        char *name;

        status = krb5_unparse_name(ctx, user, &name);
        if (status != 0)
            warn_krb5(ctx, status, "error unparsing name");
        else {
            notice("renewing credentials for %s", name);
            free(name);
        }
    }
#ifdef HAVE_KRB5_GET_RENEWED_CREDS
    status = krb5_get_renewed_creds(ctx, &creds, user, cache, NULL);
    out = &creds;
#else
    flags.i = 0;
    flags.b.renewable = 1;
    flags.b.renew = 1;
    memset(&in, 0, sizeof(in));
    status = krb5_copy_principal(ctx, user, &in.client);
    if (status != 0)
        die_krb5(ctx, status, "error copying principal");
    status = get_krbtgt_princ(ctx, in.client, &in.server);
    if (status != 0)
        die_krb5(ctx, status, "error building ticket name");
    status = krb5_get_credentials(ctx, 0, cache, &in, &old);
    if (status != 0)
        die_krb5(ctx, status, "cannot get current credentials");
    status = krb5_get_kdc_cred(ctx, cache, flags, NULL, NULL, old, &out);
#endif
    if (status != 0)
        die_krb5(ctx, status, "error renewing credentials");
    
    status = krb5_cc_initialize(ctx, cache, user);
    if (status != 0)
        die_krb5(ctx, status, "error reinitializing cache");
    status = krb5_cc_store_cred(ctx, cache, out);
    if (status != 0)
        die_krb5(ctx, status, "error storing credentials");
    krb5_free_principal(ctx, user);
#ifdef HAVE_KRB5_GET_RENEWED_CREDS
    krb5_free_cred_contents(ctx, &creds);
#else
    krb5_free_cred_contents(ctx, &in);
    if (old != NULL)
        krb5_free_creds(ctx, old);
    if (out != NULL)
        krb5_free_creds(ctx, out);
#endif
}


int
main(int argc, char *argv[])
{
    int option, result;
    char *cachename = NULL;
    char **command = NULL;
    char *childfile = NULL;
    char *pidfile = NULL;
    int background = 0;
    int keep_ticket = 0;
    int do_aklog = 0;
    int verbose = 0;
    const char *aklog = NULL;
    krb5_context ctx;
    krb5_ccache cache;
    int status = 0;
    krb5_error_code code;
    pid_t child = 0;

    /* Initialize logging. */
    message_program_name = "krenew";

    /* Parse command-line options. */
    while ((option = getopt(argc, argv, "bc:hK:k:p:qtv")) != EOF)
        switch (option) {
        case 'b': background = 1;               break;
        case 'c': childfile = optarg;           break;
        case 'h': usage(0);                     break;
        case 'p': pidfile = optarg;             break;
        case 't': do_aklog = 1;                 break;
        case 'v': verbose = 1;                  break;

        case 'K':
            keep_ticket = atoi(optarg);
            if (keep_ticket <= 0)
                die("-K interval argument %s out of range", optarg);
            break;
        case 'k':
            cachename = concat("FILE:", optarg, (char *) 0);
            break;

        default:
            usage(1);
            break;
        }

    /* Parse arguments.  If any are given, they will be the command to run. */
    argc -= optind;
    argv += optind;
    if (argc > 0)
        command = argv;

    /* Check the arguments for consistency. */
    if (background && keep_ticket == 0 && command == NULL)
        die("-b only makes sense with -K or a command to run");
    if (childfile != NULL && command == NULL)
        die("-c option only makes sense with a command to run");

    /* Set aklog from KINIT_PROG or the compiled-in default. */
    aklog = getenv("KINIT_PROG");
    if (aklog == NULL)
        aklog = PATH_AKLOG;
    if (aklog[0] == '\0' && do_aklog)
        die("set KINIT_PROG to specify the path to aklog");

    /* Establish a K5 context and set the ticket cache. */
    code = krb5_init_context(&ctx);
    if (code != 0)
        die_krb5(ctx, code, "error initializing Kerberos");
    if (cachename == NULL)
        code = krb5_cc_default(ctx, &cache);
    else {
        char *env;

        if (xasprintf(&env, "KRB5CCNAME=%s", cachename) < 0)
            die("cannot format KRB5CCNAME environment variable");
        putenv(env);
        code = krb5_cc_resolve(ctx, cachename, &cache);
    }
    if (code != 0)
        die_krb5(ctx, code, "error initializing ticket cache");

    /*
     * If built with setpag support and we're running a command, create the
     * new PAG now before the first authentication.
     */
    if (command != NULL && do_aklog) {
        if (k_hasafs()) {
            if (k_setpag() < 0)
                sysdie("unable to create PAG");
        } else {
            die("cannot create PAG: AFS support is not available");
        }
    }

    /*
     * Now, do the initial ticket renewal even if it's not necessary so that
     * we can catch any problems.
     */
    renew(ctx, cache, verbose);

    /* If requested, run the aklog program. */
    if (do_aklog)
        command_run(aklog, verbose);

    /*
     * If told to background, background ourselves.  We do this late so that
     * we can report initial errors.  We have to do this before spawning the
     * command, though, since we want to background the command as well and
     * since otherwise we wouldn't be able to wait for the child process.
     */
    if (background)
        daemon(0, 0);

    /*
     * Write out the PID file.  Note that we can't report failures usefully,
     * since this is generally used with -b.
     */
    if (pidfile != NULL) {
        FILE *file;

        file = fopen(pidfile, "w");
        if (file != NULL) {
            fprintf(file, "%lu\n", (unsigned long) getpid());
            fclose(file);
        }
    }

    /* Spawn the external command, if we were told to run one. */
    if (command != NULL) {
        child = command_start(command[0], command);
        if (child < 0)
            sysdie("unable to run command %s", command[0]);
        if (keep_ticket == 0)
            keep_ticket = 60;
    }

    /* Write out the child PID file.  Again, no useful error reporting. */
    if (childfile != NULL) {
        FILE *file;

        file = fopen(childfile, "w");
        if (file != NULL) {
            fprintf(file, "%lu\n", (unsigned long) child);
            fclose(file);
        }
    }

    /* Loop if we're running as a daemon. */
    if (keep_ticket > 0) {
        struct timeval timeout;

        while (1) {
            if (command != NULL) {
                result = command_finish(child, &status);
                if (result < 0)
                    sysdie("waitpid for %lu failed", (unsigned long) child);
                if (result > 0)
                    break;
            }
            timeout.tv_sec = keep_ticket * 60;
            timeout.tv_usec = 0;
            select(0, NULL, NULL, NULL, &timeout);
            if (ticket_expired(ctx, cache, keep_ticket)) {
                renew(ctx, cache, verbose);
                if (do_aklog)
                    command_run(aklog, verbose);
            }
        }
    }

    /* All done. */
    exit(status);
}
