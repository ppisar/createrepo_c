/* createrepo_c - Library of routines for manipulation with repodata
 * Copyright (C) 2012      Tomas Mlcoch
 * Copyright (C) 2008-2011 James Antill
 * Copyright (C) 2006-2007 James Bowes
 * Copyright (C) 2006-2007 Paul Nasrat
 * Copyright (C) 2006      Tambet Ingo
 * Copyright (C) 2006-2010 Seth Vidal
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/*
TODO:
- Fix possible license issue of this module!!!
  - Relicensins whole project to GPLv2?
  - Rewrite sqlite module?
*/

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include "logging.h"
#include "misc.h"
#include "sqlite.h"

#undef MODULE
#define MODULE "sqlite: "

#define ENCODED_PACKAGE_FILE_FILES 2048
#define ENCODED_PACKAGE_FILE_TYPES 60


GQuark
cr_db_error_quark (void)
{
    static GQuark quark;

    if (!quark)
        quark = g_quark_from_static_string("cr_db_error");

    return quark;
}


struct _DbPrimaryStatements {
    sqlite3 *db;
    sqlite3_stmt *pkg_handle;
    sqlite3_stmt *provides_handle;
    sqlite3_stmt *conflicts_handle;
    sqlite3_stmt *obsoletes_handle;
    sqlite3_stmt *requires_handle;
    sqlite3_stmt *files_handle;
};

struct _DbFilelistsStatements {
    sqlite3 *db;
    sqlite3_stmt *package_id_handle;
    sqlite3_stmt *filelists_handle;
};

struct _DbOtherStatements {
    sqlite3 *db;
    sqlite3_stmt *package_id_handle;
    sqlite3_stmt *changelog_handle;
};

/*
 * Base DB operation
 *  - Open db
 *  - Creation of tables
 *  - Tweaking of db settings
 *  - Creation of info table
 *  - Creation of index
 *  - Close db
 */

static sqlite3 *
open_sqlite_db(const char *path, GError **err)
{
    int rc;
    sqlite3 *db = NULL;

    rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        g_set_error(err, CR_DB_ERROR, CR_DB_ERROR,
                    "Can not open SQL database: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        db = NULL;
    }

    return db;
}


static void
db_create_dbinfo_table(sqlite3 *db, GError **err)
{
    int rc;
    const char *sql;

    sql = "CREATE TABLE db_info (dbversion INTEGER, checksum TEXT)";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error(err, CR_DB_ERROR, CR_DB_ERROR,
                    "Can not create db_info table: %s",
                     sqlite3_errmsg (db));
    }
}


static void
db_create_primary_tables(sqlite3 *db, GError **err)
{
    int rc;
    const char *sql;

    sql =
        "CREATE TABLE packages ("
        "  pkgKey INTEGER PRIMARY KEY,"
        "  pkgId TEXT,"
        "  name TEXT,"
        "  arch TEXT,"
        "  version TEXT,"
        "  epoch TEXT,"
        "  release TEXT,"
        "  summary TEXT,"
        "  description TEXT,"
        "  url TEXT,"
        "  time_file INTEGER,"
        "  time_build INTEGER,"
        "  rpm_license TEXT,"
        "  rpm_vendor TEXT,"
        "  rpm_group TEXT,"
        "  rpm_buildhost TEXT,"
        "  rpm_sourcerpm TEXT,"
        "  rpm_header_start INTEGER,"
        "  rpm_header_end INTEGER,"
        "  rpm_packager TEXT,"
        "  size_package INTEGER,"
        "  size_installed INTEGER,"
        "  size_archive INTEGER,"
        "  location_href TEXT,"
        "  location_base TEXT,"
        "  checksum_type TEXT)";

    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create packages table: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql =
        "CREATE TABLE files ("
        "  name TEXT,"
        "  type TEXT,"
        "  pkgKey INTEGER)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create files table: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql =
        "CREATE TABLE %s ("
        "  name TEXT,"
        "  flags TEXT,"
        "  epoch TEXT,"
        "  version TEXT,"
        "  release TEXT,"
        "  pkgKey INTEGER %s)";

    const char *deps[] = { "requires", "provides", "conflicts", "obsoletes", NULL };
    int i;

    for (i = 0; deps[i]; i++) {
        const char *prereq;
        char *query;

        if (!strcmp(deps[i], "requires")) {
            prereq = ", pre BOOLEAN DEFAULT FALSE";
        } else
            prereq = "";

        query = g_strdup_printf (sql, deps[i], prereq);
        rc = sqlite3_exec (db, query, NULL, NULL, NULL);
        g_free (query);

        if (rc != SQLITE_OK) {
            g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                         "Can not create %s table: %s",
                         deps[i], sqlite3_errmsg (db));
            return;
        }
    }

    sql =
        "CREATE TRIGGER removals AFTER DELETE ON packages"
        "  BEGIN"
        "    DELETE FROM files WHERE pkgKey = old.pkgKey;"
        "    DELETE FROM requires WHERE pkgKey = old.pkgKey;"
        "    DELETE FROM provides WHERE pkgKey = old.pkgKey;"
        "    DELETE FROM conflicts WHERE pkgKey = old.pkgKey;"
        "    DELETE FROM obsoletes WHERE pkgKey = old.pkgKey;"
        "  END;";

    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create removals trigger: %s",
                     sqlite3_errmsg (db));
        return;
    }
}


void
db_create_filelists_tables(sqlite3 *db, GError **err)
{
    int rc;
    const char *sql;

    sql =
        "CREATE TABLE packages ("
        "  pkgKey INTEGER PRIMARY KEY,"
        "  pkgId TEXT)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create packages table: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql =
        "CREATE TABLE filelist ("
        "  pkgKey INTEGER,"
        "  dirname TEXT,"
        "  filenames TEXT,"
        "  filetypes TEXT)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create filelist table: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql =
        "CREATE TRIGGER remove_filelist AFTER DELETE ON packages"
        "  BEGIN"
        "    DELETE FROM filelist WHERE pkgKey = old.pkgKey;"
        "  END;";

    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create remove_filelist trigger: %s",
                     sqlite3_errmsg (db));
        return;
    }
}


void
db_create_other_tables (sqlite3 *db, GError **err)
{
    int rc;
    const char *sql;

    sql =
        "CREATE TABLE packages ("
        "  pkgKey INTEGER PRIMARY KEY,"
        "  pkgId TEXT)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create packages table: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql =
        "CREATE TABLE changelog ("
        "  pkgKey INTEGER,"
        "  author TEXT,"
        "  date INTEGER,"
        "  changelog TEXT)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create changelog table: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql =
        "CREATE TRIGGER remove_changelogs AFTER DELETE ON packages"
        "  BEGIN"
        "    DELETE FROM changelog WHERE pkgKey = old.pkgKey;"
        "  END;";

    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create remove_changelogs trigger: %s",
                     sqlite3_errmsg (db));
        return;
    }
}


static void
db_tweak(sqlite3 *db, GError **err)
{
    CR_UNUSED(err);

    // Do not wait for disk writes to be fully
    // written to disk before continuing

    sqlite3_exec (db, "PRAGMA synchronous = OFF", NULL, NULL, NULL);

    sqlite3_exec (db, "PRAGMA journal_mode = MEMORY", NULL, NULL, NULL);

    sqlite3_exec (db, "PRAGMA temp_store = MEMORY", NULL, NULL, NULL);
}


void
db_index_primary_tables (sqlite3 *db, GError **err)
{
    int rc;
    const char *sql;

    sql = "CREATE INDEX IF NOT EXISTS packagename ON packages (name)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create packagename index: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql = "CREATE INDEX IF NOT EXISTS packageId ON packages (pkgId)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create packageId index: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql = "CREATE INDEX IF NOT EXISTS filenames ON files (name)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create filenames index: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql = "CREATE INDEX IF NOT EXISTS pkgfiles ON files (pkgKey)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create index on files table: %s",
                     sqlite3_errmsg (db));
        return;
    }

    const char *deps[] = { "requires", "provides", "conflicts", "obsoletes", NULL };
    int i;

    const char *pkgindexsql = "CREATE INDEX IF NOT EXISTS pkg%s on %s (pkgKey)";
    const char *nameindexsql = "CREATE INDEX IF NOT EXISTS %sname ON %s (name)";

    for (i = 0; deps[i]; i++) {
        char *query;

        query = g_strdup_printf(pkgindexsql, deps[i], deps[i]);
        rc = sqlite3_exec (db, query, NULL, NULL, NULL);
        g_free (query);

        if (rc != SQLITE_OK) {
            g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                         "Can not create index on %s table: %s",
                         deps[i], sqlite3_errmsg (db));
            return;
        }

        if (i < 2) {
            query = g_strdup_printf(nameindexsql, deps[i], deps[i]);
            rc = sqlite3_exec (db, query, NULL, NULL, NULL);
            g_free(query);
            if (rc != SQLITE_OK) {
                g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                             "Can not create %sname index: %s",
                             deps[i], sqlite3_errmsg (db));
                return;
            }
        }
    }
}


void
db_index_filelists_tables (sqlite3 *db, GError **err)
{
    int rc;
    const char *sql;

    sql = "CREATE INDEX IF NOT EXISTS keyfile ON filelist (pkgKey)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create keyfile index: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql = "CREATE INDEX IF NOT EXISTS pkgId ON packages (pkgId)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create pkgId index: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql = "CREATE INDEX IF NOT EXISTS dirnames ON filelist (dirname)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create dirnames index: %s",
                     sqlite3_errmsg (db));
        return;
    }
}


void
db_index_other_tables (sqlite3 *db, GError **err)
{
    int rc;
    const char *sql;

    sql = "CREATE INDEX IF NOT EXISTS keychange ON changelog (pkgKey)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create keychange index: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql = "CREATE INDEX IF NOT EXISTS pkgId ON packages (pkgId)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not create pkgId index: %s",
                     sqlite3_errmsg (db));
        return;
    }
}


sqlite3 *
cr_open_db(const char *path, cr_DatabaseType db_type, GError **err)
{
    GError *tmp_err = NULL;
    sqlite3 *db = NULL;

    if (!path || path[0] == '\0')
        return db;

    sqlite3_enable_shared_cache(1);

    db = open_sqlite_db(path, &tmp_err);
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return db;
    }

    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);

    db_tweak(db, &tmp_err);
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return db;
    }

    db_create_dbinfo_table(db, &tmp_err);
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return db;
    }

    switch (db_type) {
        case CR_DB_PRIMARY:
            db_create_primary_tables(db, &tmp_err); break;
        case CR_DB_FILELISTS:
            db_create_filelists_tables(db, &tmp_err); break;
        case CR_DB_OTHER:
            db_create_other_tables(db, &tmp_err); break;
    }
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return db;
    }

    return db;
}


void
cr_close_db(sqlite3 *db, cr_DatabaseType db_type, GError **err)
{
    GError *tmp_err = NULL;

    if (!db)
        return;

    switch (db_type) {
        case CR_DB_PRIMARY:
            db_index_primary_tables(db, &tmp_err); break;
        case CR_DB_FILELISTS:
            db_index_filelists_tables(db, &tmp_err); break;
        case CR_DB_OTHER:
            db_index_other_tables(db, &tmp_err); break;
    }

    if (tmp_err) {
        g_propagate_error(err, tmp_err);
    }

    sqlite3_exec (db, "COMMIT", NULL, NULL, NULL);

    sqlite3_close(db);
}


/*
 * Package insertion stuff
 */


void
cr_dbinfo_update(sqlite3 *db, const char *checksum, GError **err)
{
    int rc;
    char *sql;

    sql = g_strdup_printf
        ("INSERT INTO db_info (dbversion, checksum) VALUES (%d, '%s')",
         CR_SQLITE_CACHE_DBVERSION, checksum);

     rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
     if (rc != SQLITE_OK)
         g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                      "Can not update dbinfo table: %s",
                       sqlite3_errmsg (db));
    g_free (sql);
}


/*
 * primary.sqlite
 */


sqlite3_stmt *
db_package_prepare (sqlite3 *db, GError **err)
{
    int rc;
    sqlite3_stmt *handle = NULL;
    const char *query;

    query =
        "INSERT INTO packages ("
        "  pkgId, name, arch, version, epoch, release, summary, description,"
        "  url, time_file, time_build, rpm_license, rpm_vendor, rpm_group,"
        "  rpm_buildhost, rpm_sourcerpm, rpm_header_start, rpm_header_end,"
        "  rpm_packager, size_package, size_installed, size_archive,"
        "  location_href, location_base, checksum_type) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
        "  ?, ?, ?, ?, ?, ?, ?)";

    rc = sqlite3_prepare_v2 (db, query, -1, &handle, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not prepare packages insertion: %s",
                     sqlite3_errmsg (db));
        sqlite3_finalize (handle);
        handle = NULL;
    }

    return handle;
}


inline const char *
prevent_null(const char *str)
{
    if (!str)
        return "";
    else
        return str;
}

inline const char *
force_null(const char *str)
{
    if (!str || str[0] == '\0')
        return NULL;
    else
        return str;
}


void
db_package_write (sqlite3 *db, sqlite3_stmt *handle, cr_Package *p)
{
    int rc;

    sqlite3_bind_text (handle, 1,  p->pkgId, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 2,  p->name, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 3,  p->arch, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 4,  p->version, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 5,  p->epoch, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 6,  p->release, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 7,  p->summary, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 8,  p->description, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 9,  force_null(p->url), -1, SQLITE_STATIC);  // {null}
    sqlite3_bind_int  (handle, 10, p->time_file);
    sqlite3_bind_int  (handle, 11, p->time_build);
    sqlite3_bind_text (handle, 12, p->rpm_license, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 13, prevent_null(p->rpm_vendor), -1, SQLITE_STATIC);  // ""
    sqlite3_bind_text (handle, 14, p->rpm_group, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 15, p->rpm_buildhost, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 16, prevent_null(p->rpm_sourcerpm), -1, SQLITE_STATIC); // ""
    sqlite3_bind_int  (handle, 17, p->rpm_header_start);
    sqlite3_bind_int  (handle, 18, p->rpm_header_end);
    sqlite3_bind_text (handle, 19, force_null(p->rpm_packager), -1, SQLITE_STATIC);  // {null}
    sqlite3_bind_int64(handle, 20, p->size_package);
    sqlite3_bind_int64(handle, 21, p->size_installed);
    sqlite3_bind_int64(handle, 22, p->size_archive);
    sqlite3_bind_text (handle, 23, p->location_href, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 24, force_null(p->location_base), -1, SQLITE_STATIC);  // {null}
    sqlite3_bind_text (handle, 25, p->checksum_type, -1, SQLITE_STATIC);

    rc = sqlite3_step (handle);
    sqlite3_reset (handle);

    if (rc != SQLITE_DONE) {
        g_critical ("Error adding package to SQL: %s",
                    sqlite3_errmsg (db));
    } else
        p->pkgKey = sqlite3_last_insert_rowid (db);
}


sqlite3_stmt *
db_dependency_prepare (sqlite3 *db, const char *table, GError **err)
{
    int rc;
    sqlite3_stmt *handle = NULL;
    char *query;

    const char *pre_name = "";
    const char *pre_value = "";

    if (!strcmp (table, "requires")) {
        pre_name = ", pre";
        pre_value = ", ?";
    }

    query = g_strdup_printf
        ("INSERT INTO %s (name, flags, epoch, version, release, pkgKey%s) "
         "VALUES (?, ?, ?, ?, ?, ?%s)", table, pre_name, pre_value);

    rc = sqlite3_prepare_v2 (db, query, -1, &handle, NULL);
    g_free (query);

    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not prepare dependency insertion: %s",
                     sqlite3_errmsg (db));
        sqlite3_finalize (handle);
        handle = NULL;
    }

    return handle;
}

void
db_dependency_write (sqlite3 *db,
                     sqlite3_stmt *handle,
                     gint64 pkgKey,
                     cr_Dependency *dep,
                     gboolean isRequirement)
{
    int rc;

    sqlite3_bind_text (handle, 1, dep->name,    -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 2, dep->flags,   -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 3, dep->epoch,   -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 4, dep->version, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 5, dep->release, -1, SQLITE_STATIC);
    sqlite3_bind_int  (handle, 6, pkgKey);

    if (isRequirement) {
        if (dep->pre)
            sqlite3_bind_text (handle, 7, "TRUE", -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_text (handle, 7, "FALSE", -1, SQLITE_TRANSIENT);
    }

    rc = sqlite3_step (handle);
    sqlite3_reset (handle);

    if (rc != SQLITE_DONE)
        g_critical ("Error adding dependency to SQL: %s",
                    sqlite3_errmsg (db));
}

sqlite3_stmt *
db_file_prepare (sqlite3 *db, GError **err)
{
    int rc;
    sqlite3_stmt *handle = NULL;
    const char *query;

    query = "INSERT INTO files (name, type, pkgKey) VALUES (?, ?, ?)";

    rc = sqlite3_prepare_v2 (db, query, -1, &handle, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not prepare file insertion: %s",
                     sqlite3_errmsg (db));
        sqlite3_finalize (handle);
        handle = NULL;
    }

    return handle;

}

void
db_file_write (sqlite3 *db,
               sqlite3_stmt *handle,
               gint64 pkgKey,
               cr_PackageFile *file)
{
    int rc;

    gchar *fullpath = g_strconcat(file->path, file->name, NULL);
    if (!fullpath)
        return; // Nothing to do

    if (!cr_is_primary(fullpath)) {
        g_free(fullpath);
        return;
    }

    const char* file_type = file->type;
    if (!file_type || file_type[0] == '\0') {
        file_type = "file";
    }

    sqlite3_bind_text (handle, 1, fullpath, -1, SQLITE_TRANSIENT);
    g_free(fullpath);
    sqlite3_bind_text (handle, 2, file_type, -1, SQLITE_STATIC);
    sqlite3_bind_int  (handle, 3, pkgKey);

    rc = sqlite3_step (handle);
    sqlite3_reset (handle);

    if (rc != SQLITE_DONE)
        g_critical ("Error adding package file to SQL: %s",
                    sqlite3_errmsg (db));
}


/*
 * filelists.sqlite
 */


sqlite3_stmt *
db_filelists_prepare (sqlite3 *db, GError **err)
{
    int rc;
    sqlite3_stmt *handle = NULL;
    const char *query;

    query =
        "INSERT INTO filelist (pkgKey, dirname, filenames, filetypes) "
        " VALUES (?, ?, ?, ?)";

    rc = sqlite3_prepare_v2 (db, query, -1, &handle, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not prepare filelist insertion: %s",
                     sqlite3_errmsg (db));
        sqlite3_finalize (handle);
        handle = NULL;
    }

    return handle;
}


typedef struct {
    GString *files;
    GString *types;
} EncodedPackageFile;


static EncodedPackageFile *
encoded_package_file_new (void)
{
    EncodedPackageFile *enc;

    enc = g_new0 (EncodedPackageFile, 1);
    enc->files = g_string_sized_new (ENCODED_PACKAGE_FILE_FILES);
    enc->types = g_string_sized_new (ENCODED_PACKAGE_FILE_TYPES);

    return enc;
}


static void
encoded_package_file_free (EncodedPackageFile *file)
{
    g_string_free (file->files, TRUE);
    g_string_free (file->types, TRUE);
    g_free (file);
}


static GHashTable *
package_files_to_hash (GSList *files)
{
    GHashTable *hash;
    GSList *iter;

    hash = g_hash_table_new_full (g_str_hash, g_str_equal,
                                  NULL,
                                  (GDestroyNotify) encoded_package_file_free);

    for (iter = files; iter; iter = iter->next) {
        cr_PackageFile *file;
        EncodedPackageFile *enc;
        char *dir;
        char *name;

        file = (cr_PackageFile *) iter->data;

        dir = file->path;
        name = file->name;

        enc = (EncodedPackageFile *) g_hash_table_lookup (hash, dir);
        if (!enc) {
            enc = encoded_package_file_new ();
            g_hash_table_insert (hash, dir, enc);
        }

        if (enc->files->len)
            g_string_append_c (enc->files, '/');

        if (!name || name[0] == '\0')
            // Root directory '/' has empty name
            g_string_append_c (enc->files, '/');
        else
            g_string_append (enc->files, name);


        if (!(file->type) || file->type[0] == '\0' || !strcmp (file->type, "file"))
            g_string_append_c (enc->types, 'f');
        else if (!strcmp (file->type, "dir"))
            g_string_append_c (enc->types, 'd');
        else if (!strcmp (file->type, "ghost"))
            g_string_append_c (enc->types, 'g');
    }

    return hash;
}


typedef struct {
    sqlite3 *db;
    sqlite3_stmt *handle;
    gint64 pkgKey;
} FileWriteInfo;


static void
write_file (gpointer key, gpointer value, gpointer user_data)
{
    EncodedPackageFile *file = (EncodedPackageFile *) value;
    FileWriteInfo *info = (FileWriteInfo *) user_data;
    int rc;

    sqlite3_bind_int  (info->handle, 1, info->pkgKey);
    sqlite3_bind_text (info->handle, 2, (const char *) key, -1, SQLITE_STATIC);
    sqlite3_bind_text (info->handle, 3, file->files->str, -1, SQLITE_STATIC);
    sqlite3_bind_text (info->handle, 4, file->types->str, -1, SQLITE_STATIC);

    rc = sqlite3_step (info->handle);
    sqlite3_reset (info->handle);

    if (rc != SQLITE_DONE) {
        g_critical ("Error adding file to SQL: %s",
                    sqlite3_errmsg (info->db));
    }
}


void
db_filelists_write (sqlite3 *db, sqlite3_stmt *handle, cr_Package *p)
{
    GHashTable *hash;
    FileWriteInfo info;

    info.db = db;
    info.handle = handle;
    info.pkgKey = p->pkgKey;

    hash = package_files_to_hash (p->files);
    g_hash_table_foreach (hash, write_file, &info);
    g_hash_table_destroy (hash);
}



/*
 * other.sqlite
 */


sqlite3_stmt *
db_changelog_prepare (sqlite3 *db, GError **err)
{
    int rc;
    sqlite3_stmt *handle = NULL;
    const char *query;

    query =
        "INSERT INTO changelog (pkgKey, author, date, changelog) "
        " VALUES (?, ?, ?, ?)";

    rc = sqlite3_prepare_v2 (db, query, -1, &handle, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not prepare changelog insertion: %s",
                     sqlite3_errmsg (db));
        sqlite3_finalize (handle);
        handle = NULL;
    }

    return handle;
}


// Stuff common for both filelists.sqlite and other.sqlite


sqlite3_stmt *
db_package_ids_prepare(sqlite3 *db, GError **err)
{
    int rc;
    sqlite3_stmt *handle = NULL;
    const char *query;

    query = "INSERT INTO packages (pkgId) VALUES (?)";
    rc = sqlite3_prepare_v2 (db, query, -1, &handle, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, CR_DB_ERROR, CR_DB_ERROR,
                     "Can not prepare package ids insertion: %s",
                      sqlite3_errmsg (db));
        sqlite3_finalize (handle);
        handle = NULL;
    }
    return handle;
}


void
db_package_ids_write (sqlite3 *db, sqlite3_stmt *handle, cr_Package *pkg)
{
    int rc;

    sqlite3_bind_text (handle, 1,  pkg->pkgId, -1, SQLITE_STATIC);
    rc = sqlite3_step (handle);
    sqlite3_reset (handle);

    if (rc != SQLITE_DONE) {
        g_critical ("Error adding package to SQL: %s",
                    sqlite3_errmsg (db));
    } else
        pkg->pkgKey = sqlite3_last_insert_rowid (db);
}


/*
 * Module interface
 */


// Primary.sqlite interface

cr_DbPrimaryStatements
cr_prepare_primary_db_statements(sqlite3 *db, GError **err)
{
    GError *tmp_err = NULL;
    cr_DbPrimaryStatements ret = malloc(sizeof(*ret));

    ret->db               = db;
    ret->pkg_handle       = NULL;
    ret->provides_handle  = NULL;
    ret->conflicts_handle = NULL;
    ret->obsoletes_handle = NULL;
    ret->requires_handle  = NULL;
    ret->files_handle     = NULL;

    ret->pkg_handle = db_package_prepare(db, &tmp_err);
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return ret;
    }

    ret->provides_handle = db_dependency_prepare(db, "provides", &tmp_err);
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return ret;
    }

    ret->conflicts_handle = db_dependency_prepare(db, "conflicts", &tmp_err);
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return ret;
    }

    ret->obsoletes_handle = db_dependency_prepare(db, "obsoletes", &tmp_err);
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return ret;
    }

    ret->requires_handle = db_dependency_prepare(db, "requires", &tmp_err);
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return ret;
    }

    ret->files_handle = db_file_prepare(db, &tmp_err);
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return ret;
    }

    return ret;
}


void
cr_destroy_primary_db_statements(cr_DbPrimaryStatements stmts)
{
    if (!stmts)
        return;

    if (stmts->pkg_handle)
        sqlite3_finalize(stmts->pkg_handle);
    if (stmts->provides_handle)
        sqlite3_finalize(stmts->provides_handle);
    if (stmts->conflicts_handle)
        sqlite3_finalize(stmts->conflicts_handle);
    if (stmts->obsoletes_handle)
        sqlite3_finalize(stmts->obsoletes_handle);
    if (stmts->requires_handle)
        sqlite3_finalize(stmts->requires_handle);
    if (stmts->files_handle)
        sqlite3_finalize(stmts->files_handle);
    free(stmts);
}


void
cr_add_primary_pkg_db(cr_DbPrimaryStatements stmts, cr_Package *pkg)
{
    GSList *iter;

//    sqlite3_exec(stmts->db, "BEGIN", NULL, NULL, NULL);

    db_package_write(stmts->db, stmts->pkg_handle, pkg);

    for (iter = pkg->provides; iter; iter = iter->next)
        db_dependency_write (stmts->db,
                             stmts->provides_handle,
                             pkg->pkgKey,
                             (cr_Dependency *) iter->data,
                             FALSE);

    for (iter = pkg->conflicts; iter; iter = iter->next)
        db_dependency_write (stmts->db,
                             stmts->conflicts_handle,
                             pkg->pkgKey,
                             (cr_Dependency *) iter->data,
                             FALSE);

    for (iter = pkg->obsoletes; iter; iter = iter->next)
        db_dependency_write (stmts->db,
                             stmts->obsoletes_handle,
                             pkg->pkgKey,
                             (cr_Dependency *) iter->data,
                             FALSE);

    for (iter = pkg->requires; iter; iter = iter->next)
        db_dependency_write (stmts->db,
                             stmts->requires_handle,
                             pkg->pkgKey,
                             (cr_Dependency *) iter->data,
                             TRUE);

    for (iter = pkg->files; iter; iter = iter->next)
        db_file_write (stmts->db, stmts->files_handle, pkg->pkgKey,
                       (cr_PackageFile *) iter->data);

//    sqlite3_exec (stmts->db, "COMMIT", NULL, NULL, NULL);
}


// filelists.sqlite interface


cr_DbFilelistsStatements
cr_prepare_filelists_db_statements(sqlite3 *db, GError **err)
{
    GError *tmp_err = NULL;
    cr_DbFilelistsStatements ret = malloc(sizeof(*ret));

    ret->db                = db;
    ret->package_id_handle = NULL;
    ret->filelists_handle  = NULL;

    ret->package_id_handle = db_package_ids_prepare(db, &tmp_err);
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return ret;
    }

    ret->filelists_handle = db_filelists_prepare(db, &tmp_err);
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return ret;
    }

    return ret;
}


void
cr_destroy_filelists_db_statements(cr_DbFilelistsStatements stmts)
{
    if (!stmts)
        return;

    if (stmts->package_id_handle)
        sqlite3_finalize(stmts->package_id_handle);
    if (stmts->filelists_handle)
        sqlite3_finalize(stmts->filelists_handle);
    free(stmts);
}


void
cr_add_filelists_pkg_db(cr_DbFilelistsStatements stmts, cr_Package *pkg)
{
    // Add package record into the filelists.sqlite
    db_package_ids_write(stmts->db, stmts->package_id_handle, pkg);
    db_filelists_write(stmts->db, stmts->filelists_handle, pkg);
}


// other.sqlite interface


cr_DbOtherStatements
cr_prepare_other_db_statements(sqlite3 *db, GError **err)
{
    GError *tmp_err = NULL;
    cr_DbOtherStatements ret = malloc(sizeof(*ret));

    ret->db                = db;
    ret->package_id_handle = NULL;
    ret->changelog_handle  = NULL;

    ret->package_id_handle = db_package_ids_prepare(db, &tmp_err);
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return ret;
    }

    ret->changelog_handle = db_changelog_prepare(db, &tmp_err);
    if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return ret;
    }

    return ret;
}


void
cr_destroy_other_db_statements(cr_DbOtherStatements stmts)
{
    if (!stmts)
        return;

    if (stmts->package_id_handle)
        sqlite3_finalize(stmts->package_id_handle);
    if (stmts->changelog_handle)
        sqlite3_finalize(stmts->changelog_handle);
    free(stmts);
}


void
cr_add_other_pkg_db(cr_DbOtherStatements stmts, cr_Package *pkg)
{
    GSList *iter;
    cr_ChangelogEntry *entry;
    int rc;

    sqlite3_stmt *handle = stmts->changelog_handle;

    // Add package record into the other.sqlite

    db_package_ids_write(stmts->db, stmts->package_id_handle, pkg);

    for (iter = pkg->changelogs; iter; iter = iter->next) {
        entry = (cr_ChangelogEntry *) iter->data;

        sqlite3_bind_int  (handle, 1, pkg->pkgKey);
        sqlite3_bind_text (handle, 2, entry->author, -1, SQLITE_STATIC);
        sqlite3_bind_int  (handle, 3, entry->date);
        sqlite3_bind_text (handle, 4, entry->changelog, -1, SQLITE_STATIC);

        rc = sqlite3_step (handle);
        sqlite3_reset (handle);

        if (rc != SQLITE_DONE) {
            g_critical ("Error adding changelog to SQL: %s",
                        sqlite3_errmsg (stmts->db));
        }
    }
}