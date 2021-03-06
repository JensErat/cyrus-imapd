/* dav_db.c -- implementation of per-user DAV database
 *
 * Copyright (c) 1994-2012 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <config.h>

#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "assert.h"
#include "caldav_alarm.h"
#include "cyrusdb.h"
#include "dav_db.h"
#include "global.h"
#include "util.h"
#include "xmalloc.h"

#define CMD_CREATE_CAL                                                  \
    "CREATE TABLE IF NOT EXISTS ical_objs ("                            \
    " rowid INTEGER PRIMARY KEY,"                                       \
    " creationdate INTEGER,"                                            \
    " mailbox TEXT NOT NULL,"                                           \
    " resource TEXT NOT NULL,"                                          \
    " imap_uid INTEGER,"                                                \
    " modseq INTEGER,"                                                  \
    " lock_token TEXT,"                                                 \
    " lock_owner TEXT,"                                                 \
    " lock_ownerid TEXT,"                                               \
    " lock_expire INTEGER,"                                             \
    " comp_type INTEGER,"                                               \
    " ical_uid TEXT,"                                                   \
    " organizer TEXT,"                                                  \
    " dtstart TEXT,"                                                    \
    " dtend TEXT,"                                                      \
    " comp_flags INTEGER,"                                              \
    " sched_tag TEXT,"                                                  \
    " alive INTEGER,"                                                   \
    " UNIQUE( mailbox, resource ) );"                                   \
    "CREATE INDEX IF NOT EXISTS idx_ical_uid ON ical_objs ( ical_uid );"

#define CMD_CREATE_CARD                                                 \
    "CREATE TABLE IF NOT EXISTS vcard_objs ("                           \
    " rowid INTEGER PRIMARY KEY,"                                       \
    " creationdate INTEGER,"                                            \
    " mailbox TEXT NOT NULL,"                                           \
    " resource TEXT NOT NULL,"                                          \
    " imap_uid INTEGER,"                                                \
    " modseq INTEGER,"                                                  \
    " lock_token TEXT,"                                                 \
    " lock_owner TEXT,"                                                 \
    " lock_ownerid TEXT,"                                               \
    " lock_expire INTEGER,"                                             \
    " version INTEGER,"                                                 \
    " vcard_uid TEXT,"                                                  \
    " kind INTEGER,"                                                    \
    " fullname TEXT,"                                                   \
    " name TEXT,"                                                       \
    " nickname TEXT,"                                                   \
    " alive INTEGER,"                                                   \
    " UNIQUE( mailbox, resource ) );"                                   \
    "CREATE INDEX IF NOT EXISTS idx_vcard_fn ON vcard_objs ( fullname );" \
    "CREATE INDEX IF NOT EXISTS idx_vcard_uid ON vcard_objs ( vcard_uid );"

#define CMD_CREATE_EM                                                   \
    "CREATE TABLE IF NOT EXISTS vcard_emails ("                         \
    " rowid INTEGER PRIMARY KEY,"                                       \
    " objid INTEGER,"                                                   \
    " pos INTEGER NOT NULL," /* for sorting */                          \
    " email TEXT NOT NULL COLLATE NOCASE,"                              \
    " ispref INTEGER NOT NULL DEFAULT 0,"                               \
    " FOREIGN KEY (objid) REFERENCES vcard_objs (rowid) ON DELETE CASCADE );" \
    "CREATE INDEX IF NOT EXISTS idx_vcard_email ON vcard_emails ( email COLLATE NOCASE );"

#define CMD_CREATE_GR                                                   \
    "CREATE TABLE IF NOT EXISTS vcard_groups ("                         \
    " rowid INTEGER PRIMARY KEY,"                                       \
    " objid INTEGER,"                                                   \
    " pos INTEGER NOT NULL," /* for sorting */                          \
    " member_uid TEXT NOT NULL,"                                        \
    " otheruser TEXT NOT NULL DEFAULT \"\","                            \
    " FOREIGN KEY (objid) REFERENCES vcard_objs (rowid) ON DELETE CASCADE );"

#define CMD_CREATE_OBJS                                                 \
    "CREATE TABLE IF NOT EXISTS dav_objs ("                             \
    " rowid INTEGER PRIMARY KEY,"                                       \
    " creationdate INTEGER,"                                            \
    " mailbox TEXT NOT NULL,"                                           \
    " resource TEXT NOT NULL,"                                          \
    " imap_uid INTEGER,"                                                \
    " modseq INTEGER,"                                                  \
    " lock_token TEXT,"                                                 \
    " lock_owner TEXT,"                                                 \
    " lock_ownerid TEXT,"                                               \
    " lock_expire INTEGER,"                                             \
    " filename TEXT,"                                                   \
    " type TEXT,"                                                       \
    " subtype TEXT,"                                                    \
    " res_uid TEXT,"                                                    \
    " ref_count INTEGER,"                                               \
    " alive INTEGER,"                                                   \
    " UNIQUE( mailbox, resource ) );"                                   \
    "CREATE INDEX IF NOT EXISTS idx_res_uid ON dav_objs ( res_uid );"


#define CMD_CREATE CMD_CREATE_CAL CMD_CREATE_CARD CMD_CREATE_EM CMD_CREATE_GR \
                   CMD_CREATE_OBJS

/* leaves these unused columns around, but that's life.  A dav_reconstruct
 * will fix them */
#define CMD_DBUPGRADEv2                                         \
    "ALTER TABLE ical_objs ADD COLUMN comp_flags INTEGER;"      \
    "UPDATE ical_objs SET comp_flags = recurring + 2 * transp;"

#define CMD_DBUPGRADEv3                                         \
    "ALTER TABLE ical_objs ADD COLUMN modseq INTEGER;"          \
    "UPDATE ical_objs SET modseq = 1;"                          \
    "ALTER TABLE vcard_objs ADD COLUMN modseq INTEGER;"         \
    "UPDATE vcard_objs SET modseq = 1;"

#define CMD_DBUPGRADEv4                                         \
    "ALTER TABLE ical_objs ADD COLUMN alive INTEGER;"           \
    "UPDATE ical_objs SET alive = 1;"                           \
    "ALTER TABLE vcard_objs ADD COLUMN alive INTEGER;"          \
    "UPDATE vcard_objs SET alive = 1;"

#define CMD_DBUPGRADEv5                                         \
    "ALTER TABLE vcard_emails ADD COLUMN ispref INTEGER NOT NULL DEFAULT 0;"    \
    "ALTER TABLE vcard_groups ADD COLUMN otheruser TEXT NOT NULL DEFAULT \"\";"

#define CMD_DBUPGRADEv6 CMD_CREATE_OBJS

struct sqldb_upgrade davdb_upgrade[] = {
  { 2, CMD_DBUPGRADEv2, NULL },
  { 3, CMD_DBUPGRADEv3, NULL },
  { 4, CMD_DBUPGRADEv4, NULL },
  { 5, CMD_DBUPGRADEv5, NULL },
  { 6, CMD_DBUPGRADEv6, NULL },
  { 0, NULL, NULL }
};

#define DB_VERSION 6

static int in_reconstruct = 0;

EXPORTED sqldb_t *dav_open_userid(const char *userid)
{
    sqldb_t *db = NULL;
    struct buf fname = BUF_INITIALIZER;
    dav_getpath_byuserid(&fname, userid);
    if (in_reconstruct) buf_printf(&fname, ".NEW");
    db = sqldb_open(buf_cstring(&fname), CMD_CREATE, DB_VERSION, davdb_upgrade);
    buf_free(&fname);
    return db;
}

EXPORTED sqldb_t *dav_open_mailbox(struct mailbox *mailbox)
{
    sqldb_t *db = NULL;
    struct buf fname = BUF_INITIALIZER;
    dav_getpath(&fname, mailbox);
    if (in_reconstruct) buf_printf(&fname, ".NEW");
    db = sqldb_open(buf_cstring(&fname), CMD_CREATE, DB_VERSION, davdb_upgrade);
    buf_free(&fname);
    return db;
}

/*
 * mboxlist_usermboxtree() callback function to create DAV DB entries for a mailbox
 */
static int _dav_reconstruct_mb(const mbentry_t *mbentry, void *rock __attribute__((unused)))
{
    int r = 0;

    signals_poll();

#ifdef WITH_DAV
    if (mbentry->mbtype & MBTYPES_DAV) {
        struct mailbox *mailbox = NULL;
        /* Open/lock header */
        r = mailbox_open_irl(mbentry->name, &mailbox);
        if (!r) r = mailbox_add_dav(mailbox);
        mailbox_close(&mailbox);
    }
#endif

    return r;
}

static void run_audit_tool(const char *tool, const char *srcdb, const char *dstdb)
{
    pid_t pid = fork();
    if (pid < 0)
        return;

    if (pid == 0) {
        /* child */
        execl(tool, tool, srcdb, dstdb, (void *)NULL);
        exit(-1);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0);
}

EXPORTED int dav_reconstruct_user(const char *userid, const char *audit_tool)
{
    syslog(LOG_NOTICE, "dav_reconstruct_user: %s", userid);

    struct buf fname = BUF_INITIALIZER;
    dav_getpath_byuserid(&fname, userid);

    struct buf newfname = BUF_INITIALIZER;
    dav_getpath_byuserid(&newfname, userid);
    buf_printf(&newfname, ".NEW");

    /* XXX - this still means that alarms can go missing if this
     * task is interrupted, but we can't afford to keep the
     * alarm database locked for the entire time, it's a single
     * blocking database over the entire server */
    sqldb_t *alarmdb = caldav_alarm_open();
    caldav_alarm_delete_user(alarmdb, userid);
    caldav_alarm_close(alarmdb);

    in_reconstruct = 1;

    sqldb_t *userdb = dav_open_userid(userid);
    sqldb_begin(userdb, "reconstruct");
    int r = mboxlist_usermboxtree(userid, _dav_reconstruct_mb, NULL, 0);
    if (r)
        sqldb_rollback(userdb, "reconstruct");
    else
        sqldb_commit(userdb, "reconstruct");
    sqldb_close(&userdb);

    in_reconstruct = 0;

    /* this actually works before close according to the internets */
    if (r) {
        syslog(LOG_ERR, "dav_reconstruct_user: %s FAILED %s", userid, error_message(r));
        if (audit_tool) {
            printf("Not auditing %s, reconstruct failed %s\n", userid, error_message(r));
        }
        unlink(buf_cstring(&newfname));
    }
    else {
        syslog(LOG_NOTICE, "dav_reconstruct_user: %s SUCCEEDED", userid);
        if (audit_tool) {
            run_audit_tool(audit_tool, buf_cstring(&fname), buf_cstring(&newfname));
            unlink(buf_cstring(&newfname));
        }
        else {
            rename(buf_cstring(&newfname), buf_cstring(&fname));
        }
    }

    buf_free(&newfname);
    buf_free(&fname);

    return 0;
}
