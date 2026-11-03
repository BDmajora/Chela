#include "chela.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool db_error(ChStore *s, int rc, ChError **error) {
    if (rc == SQLITE_OK || rc == SQLITE_DONE)
        return true;
    ch_set_error(error, CH_ERR_DB, rc, "Session database: %s", sqlite3_errmsg(s->db));
    return false;
}
ChStore *ch_store_open(const char *path, ChError **error) {
    ChStore *s = ch_new0(ChStore, 1);
    int rc = sqlite3_open_v2(path, &s->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (!db_error(s, rc, error)) {
        ch_store_close(s);
        return NULL;
    }
    if (ch_strcmp0(path, ":memory:"))
        chmod(path, 0600);
    sqlite3_busy_timeout(s->db, 3000);
    rc = sqlite3_exec(s->db,
                      "PRAGMA foreign_keys=ON; CREATE TABLE IF NOT EXISTS sessions (id TEXT PRIMARY KEY, "
                      "title TEXT NOT NULL, project TEXT NOT NULL, model TEXT NOT NULL, updated INTEGER NOT "
                      "NULL, messages TEXT NOT NULL); "
                      "CREATE TABLE IF NOT EXISTS session_options (id TEXT PRIMARY KEY REFERENCES "
                      "sessions(id) ON DELETE CASCADE, interaction INTEGER NOT NULL, mode INTEGER NOT NULL, "
                      "code INTEGER NOT NULL); PRAGMA user_version=2;",
                      NULL, NULL, NULL);
    if (!db_error(s, rc, error)) {
        ch_store_close(s);
        return NULL;
    }
    return s;
}
void ch_store_close(ChStore *s) {
    if (s) {
        if (s->db)
            sqlite3_close(s->db);
        free(s);
    }
}
void ch_session_free(void *data) {
    ChSession *s = data;
    if (s) {
        free(s->id);
        free(s->title);
        free(s->project);
        free(s->model);
        free(s);
    }
}
bool ch_store_save(ChStore *s, const ChSession *v, json_object *messages, ChError **error) {
    if (!v->id || !*v->id || !json_object_is_type(messages, json_type_array) || v->interaction < CH_AGENT ||
        v->interaction > CH_CHAT || v->mode < CH_ASK || v->mode > CH_ACCEPT_EDITS) {
        ch_fail_literal(error, "Invalid session");
        return false;
    }
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_exec(s->db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (!db_error(s, rc, error))
        return false;
    rc = sqlite3_prepare_v2(s->db,
                            "INSERT INTO sessions VALUES (?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
                            "title=excluded.title,project=excluded.project,model=excluded.model,updated="
                            "excluded.updated,messages=excluded.messages",
                            -1, &q, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(q, 1, v->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 2, v->title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 3, v->project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 4, v->model, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(q, 5, ch_real_us());
        sqlite3_bind_text(q, 6, json_object_to_json_string_ext(messages, JSON_C_TO_STRING_PLAIN), -1,
                          SQLITE_TRANSIENT);
        rc = sqlite3_step(q);
    }
    sqlite3_finalize(q);
    q = NULL;
    if (rc == SQLITE_DONE)
        rc = sqlite3_prepare_v2(s->db,
                                "INSERT INTO session_options VALUES (?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
                                "interaction=excluded.interaction,mode=excluded.mode,code=excluded.code",
                                -1, &q, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(q, 1, v->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(q, 2, v->interaction);
        sqlite3_bind_int(q, 3, v->mode);
        sqlite3_bind_int(q, 4, !!v->code);
        rc = sqlite3_step(q);
    }
    sqlite3_finalize(q);
    bool ok = db_error(s, rc, error);
    if (ok)
        ok = db_error(s, sqlite3_exec(s->db, "COMMIT", NULL, NULL, NULL), error);
    if (!ok)
        sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
    return ok;
}
ChVec *ch_store_list(ChStore *s, const char *search, ChError **error) {
    ChVec *out = ch_vec_new(ch_session_free);
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_prepare_v2(
        s->db,
        "SELECT s.id,title,project,model,updated,COALESCE(o.interaction,0),COALESCE(o.mode,0),"
        "COALESCE(o.code,1) FROM sessions s LEFT JOIN session_options o ON s.id=o.id "
        "ORDER BY updated DESC",
        -1, &q, NULL);
    char *needle = ch_utf8_casefold(search ? search : "");
    if (rc == SQLITE_OK)
        while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
            const char *title = (const char *)sqlite3_column_text(q, 1),
                       *project = (const char *)sqlite3_column_text(q, 2);
            char *a = ch_utf8_casefold(title), *b = ch_utf8_casefold(project);
            bool match = strstr(a, needle) || strstr(b, needle);
            free(a);
            free(b);
            if (!match)
                continue;
            ChSession *v = ch_new0(ChSession, 1);
            v->id = ch_strdup((const char *)sqlite3_column_text(q, 0));
            v->title = ch_strdup(title);
            v->project = ch_strdup(project);
            v->model = ch_strdup((const char *)sqlite3_column_text(q, 3));
            v->updated = sqlite3_column_int64(q, 4);
            v->interaction = sqlite3_column_int(q, 5) == CH_CHAT ? CH_CHAT : CH_AGENT;
            int mode = sqlite3_column_int(q, 6);
            v->mode = mode >= CH_ASK && mode <= CH_ACCEPT_EDITS ? mode : CH_ASK;
            v->code = sqlite3_column_int(q, 7) != 0;
            ch_vec_push(out, v);
        }
    free(needle);
    sqlite3_finalize(q);
    if (!db_error(s, rc, error)) {
        ch_vec_free(out);
        return NULL;
    }
    return out;
}
json_object *ch_store_load(ChStore *s, const char *id, ChError **error) {
    sqlite3_stmt *q = NULL;
    json_object *out = NULL;
    int rc = sqlite3_prepare_v2(s->db, "SELECT messages FROM sessions WHERE id=?", -1, &q, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(q, 1, id, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(q);
        if (rc == SQLITE_ROW) {
            out = ch_json_parse((const char *)sqlite3_column_text(q, 0), error);
            if (out && !json_object_is_type(out, json_type_array)) {
                json_object_put(out);
                out = NULL;
                ch_fail_literal(error, "Corrupt session history");
            }
        } else if (rc == SQLITE_DONE)
            ch_fail_literal(error, "Session no longer exists");
        else
            db_error(s, rc, error);
    } else
        db_error(s, rc, error);
    sqlite3_finalize(q);
    return out;
}
bool ch_store_delete(ChStore *s, const char *id, ChError **error) {
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_prepare_v2(s->db, "DELETE FROM sessions WHERE id=?", -1, &q, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(q, 1, id, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(q);
    }
    sqlite3_finalize(q);
    return db_error(s, rc, error);
}
