/* Permission modes: what goes ahead without asking.
 *
 * Auto decides with rules rather than by asking the model. A model that has been talked
 * into something would be judging itself, and a review per command would be a round trip
 * to the inference computer. The sandbox does the holding (sandbox.c). These rules add a
 * question before what it cannot stop or undo: pushing, publishing, deleting a folder,
 * handing work to a daemon that runs outside it.
 *
 * Reading a command is a judgement made to catch what an agent does by mistake, and it
 * can be fooled on purpose, which is why the kernel's rules are the limit and these are
 * not. A command that cannot be read gets the same answer as a risky one: ask. */
#define _GNU_SOURCE
#include "chela.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

const char *ch_mode_name(ChMode mode) {
    switch (mode) {
    case CH_AUTO: return "Auto";
    case CH_PLAN: return "Plan";
    case CH_BYPASS: return "Bypass permissions";
    case CH_ACCEPT_EDITS: return "Accept edits";
    default: return "Manual";
    }
}

/* Whether word is one of the space-separated names. */
static bool listed(const char *names, const char *word) {
    size_t n = strlen(word);
    if (!n)
        return false;
    for (const char *p = names; (p = strstr(p, word)); p += n)
        if ((p == names || p[-1] == ' ') && (!p[n] || p[n] == ' '))
            return true;
    return false;
}
static bool has(char **args, size_t n, const char *names) {
    for (size_t i = 0; i < n; i++)
        if (listed(names, args[i]))
            return true;
    return false;
}
static const char *base(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static const char *const SHELLS = "sh bash dash zsh ksh fish";
static const char *const INTERPRETERS = "python python2 python3 perl ruby node nodejs deno bun php lua";
static const char *const WRAPPERS = "env command builtin exec nice ionice nohup time timeout stdbuf setsid xargs parallel";
static const char *const KEYWORDS = "if then elif else fi for while until do done case esac function { } !";
/* Reading and reporting. These change nothing, so no sandbox is needed for them. */
static const char *const READ_ONLY =
    "ls dir cat tac head tail less more wc grep egrep fgrep rg ag ack tree pwd echo printf stat file du df which type "
    "whereis whoami id groups uname date hostname printenv basename dirname realpath readlink nl cut tr uniq comm cmp "
    "diff sha1sum sha256sum sha512sum md5sum b2sum cksum jq true false test [ [[ seq ps free uptime lscpu lsblk nproc "
    "sleep hexdump xxd od strings column fold expand rev sort find";
static const struct {
    const char *names, *why;
} RISKY[] = {
    {"sudo doas su pkexec run0", "it runs as another user"},
    {"dd shred wipefs mkfs fdisk parted sgdisk", "it can erase a disk or a file for good"},
    {"ssh scp sftp telnet ftp nc ncat netcat socat mosh", "it connects to another computer"},
    {"gh glab kubectl helm terraform tofu pulumi ansible ansible-playbook aws gcloud gsutil az doctl flyctl vercel "
     "netlify heroku wrangler", "it acts on a shared service"},
    /* A daemon does the work, so the sandbox on this command does not reach it. */
    {"docker podman nerdctl systemctl systemd-run loginctl machinectl dbus-send gdbus busctl qdbus flatpak-spawn at "
     "batch crontab tmux screen", "it hands work to a service outside the sandbox"},
    {"apt apt-get aptitude dpkg dnf yum rpm pacman zypper apk snap flatpak brew nix-env",
     "it changes the software installed on this computer"},
    {"kill pkill killall shutdown reboot poweroff halt", "it stops other programs or the computer"},
    {"mount umount nsenter unshare chroot", "it changes what the system can reach"},
};
static const struct {
    const char *program, *actions;
} PUBLISHING[] = {
    {"npm", "publish unpublish"}, {"pnpm", "publish"},  {"yarn", "publish"},   {"cargo", "publish"},
    {"twine", "upload"},          {"gem", "push"},      {"poetry", "publish"}, {"uv", "publish"},
    {"hatch", "publish"},         {"dotnet", "push"},   {"mvn", "deploy"},     {"gradle", "publish"},
};
static const char *const LOST_WORK = "it can discard work that git cannot bring back";
static const char *const UNREADABLE = "it could not be read with confidence";

typedef struct {
    ChRisk risk;
    const char *why;
} Verdict;
static void weigh(Verdict *v, ChRisk risk, const char *why) {
    if (risk > v->risk) {
        v->risk = risk;
        v->why = why;
    }
}
typedef struct {
    Verdict *verdict;
    int depth;
    ChVec words;
    ChBuf word;
    bool in_word;
    bool target, discard; /* the next word is where output goes; is input, not an argument */
    bool writes;          /* this command sends output into a file */
    bool fetched;         /* something earlier in this pipeline came from the network */
    char *heredoc;        /* its text starts on the next line and is data */
    bool heredoc_tabs;
} Scan;
static void scan(const char *text, Verdict *v, int depth);
static void judge(Scan *s, char **w, size_t n);

static void judge_git(Verdict *v, char **w, size_t n) {
    size_t i = 0;
    while (i < n && *w[i] == '-')
        i += listed("-C -c --git-dir --work-tree --namespace", w[i]) ? 2 : 1;
    if (i >= n)
        return;
    const char *action = w[i];
    char **args = w + i + 1;
    size_t count = n - i - 1;
    const char *first = count ? args[0] : "";
    if (!strcmp(action, "push"))
        weigh(v, CH_RISK_ASK, "it sends commits to another computer");
    else if (listed("clean filter-branch filter-repo update-ref", action) ||
             (!strcmp(action, "reset") && has(args, count, "--hard")) ||
             (!strcmp(action, "restore") && !has(args, count, "--staged")) ||
             (!strcmp(action, "checkout") && has(args, count, "-- -f --force .")) ||
             (!strcmp(action, "switch") && has(args, count, "-f --force --discard-changes")) ||
             (!strcmp(action, "stash") && listed("drop clear", first)) ||
             (!strcmp(action, "branch") && has(args, count, "-D")) ||
             (!strcmp(action, "reflog") && listed("expire delete", first)))
        weigh(v, CH_RISK_ASK, LOST_WORK);
    else {
        bool reads = listed("status log diff show blame shortlog describe rev-parse rev-list ls-files ls-tree cat-file "
                            "merge-base name-rev for-each-ref grep whatchanged", action);
        if (!strcmp(action, "branch")) {
            reads = true;
            for (size_t k = 0; k < count; k++)
                reads &= listed("-a -r -v -vv -l --all --remotes --list --show-current --verbose", args[k]);
        } else if (!strcmp(action, "tag"))
            reads = !count || listed("-l --list", first);
        else if (!strcmp(action, "remote"))
            reads = !count || !strcmp(first, "-v");
        else if (!strcmp(action, "stash"))
            reads = listed("list show", first);
        else if (!strcmp(action, "config"))
            reads = listed("--get --get-all --list -l", first);
        for (size_t k = 0; k < count; k++)
            if (ch_str_has_prefix(args[k], "--output") || ch_str_has_prefix(args[k], "--ext-diff") ||
                ch_str_has_prefix(args[k], "-O"))
                reads = false;
        if (!reads)
            weigh(v, CH_RISK_CONFINED, NULL);
    }
}
static void judge_curl(Verdict *v, char **args, size_t n) {
    const char *sends = "it sends data to a server";
    for (size_t i = 0; i < n; i++) {
        const char *arg = args[i], *method = NULL;
        if (ch_str_has_prefix(arg, "--")) {
            if (ch_str_has_prefix(arg, "--data") || ch_str_has_prefix(arg, "--form") ||
                ch_str_has_prefix(arg, "--json") || ch_str_has_prefix(arg, "--upload-file"))
                weigh(v, CH_RISK_ASK, sends);
            else if (ch_str_has_prefix(arg, "--request"))
                method = arg[9] == '=' ? arg + 10 : i + 1 < n ? args[i + 1] : "";
        } else if (*arg == '-')
            for (const char *c = arg + 1; *c; c++) {
                if (*c == 'd' || *c == 'F' || *c == 'T')
                    weigh(v, CH_RISK_ASK, sends);
                else if (*c == 'X') {
                    method = c[1] ? c + 1 : i + 1 < n ? args[i + 1] : "";
                    break;
                }
            }
        if (method && !listed("GET HEAD", method))
            weigh(v, CH_RISK_ASK, sends);
    }
}
static void judge_find(Scan *s, char **args, size_t n) {
    Verdict *v = s->verdict;
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(args[i], "-delete"))
            weigh(v, CH_RISK_ASK, "it deletes every file it finds");
        else if (listed("-fprint -fprint0 -fprintf -fls", args[i]))
            weigh(v, CH_RISK_CONFINED, NULL);
        else if (listed("-exec -execdir -ok -okdir", args[i])) {
            weigh(v, CH_RISK_CONFINED, NULL);
            judge(s, args + i + 1, n - i - 1);
            return;
        }
    }
}
static bool assignment(const char *word) {
    if (!isalpha((unsigned char)*word) && *word != '_')
        return false;
    while (isalnum((unsigned char)*word) || *word == '_')
        word++;
    return *word == '=';
}
/* One simple command, once wrappers and keywords are out of the way. */
static void judge(Scan *s, char **w, size_t n) {
    Verdict *v = s->verdict;
    size_t i = 0;
    for (; i < n; i++) {
        if (assignment(w[i]))
            weigh(v, CH_RISK_CONFINED, NULL);
        else if (listed(WRAPPERS, base(w[i]))) {
            if (!strcmp(base(w[i]), "command") && i + 1 < n && listed("-v -V", w[i + 1]))
                return; /* only looks the name up */
            while (i + 1 < n && (*w[i + 1] == '-' || isdigit((unsigned char)*w[i + 1]) || !strcmp(w[i + 1], "{}")))
                i++;
        } else if (!listed(KEYWORDS, w[i]))
            break;
    }
    if (i == n)
        return;
    const char *program = base(w[i]);
    char **args = w + i + 1;
    size_t count = n - i - 1;
    if (listed(SHELLS, program) || listed(INTERPRETERS, program) || ch_str_has_prefix(program, "python3.")) {
        weigh(v, CH_RISK_CONFINED, NULL);
        if (s->fetched)
            weigh(v, CH_RISK_ASK, "it runs code downloaded from the network");
        if (listed(SHELLS, program))
            for (size_t k = 0; k < count && *args[k] == '-'; k++)
                if (args[k][1] != '-' && strchr(args[k], 'c')) {
                    if (k + 1 < count)
                        scan(args[k + 1], v, s->depth + 1);
                    break;
                }
        return;
    }
    if (!strcmp(program, "eval")) {
        ChBuf text;
        ch_buf_init(&text);
        for (size_t k = 0; k < count; k++)
            ch_buf_printf(&text, "%s ", args[k]);
        weigh(v, CH_RISK_CONFINED, NULL);
        scan(text.str, v, s->depth + 1);
        ch_buf_clear(&text);
        return;
    }
    for (size_t k = 0; k < CH_N_ELEMENTS(RISKY); k++)
        if (listed(RISKY[k].names, program) || (!strcmp(RISKY[k].names, "dd shred wipefs mkfs fdisk parted sgdisk") &&
                                                 ch_str_has_prefix(program, "mkfs."))) {
            weigh(v, CH_RISK_ASK, RISKY[k].why);
            return;
        }
    for (size_t k = 0; k < CH_N_ELEMENTS(PUBLISHING); k++)
        if (!strcmp(PUBLISHING[k].program, program) && has(args, count, PUBLISHING[k].actions)) {
            weigh(v, CH_RISK_ASK, "it publishes to other people");
            return;
        }
    if (!strcmp(program, "git"))
        judge_git(v, args, count);
    else if (!strcmp(program, "curl") || !strcmp(program, "wget")) {
        weigh(v, CH_RISK_CONFINED, NULL);
        s->fetched = true;
        if (!strcmp(program, "curl"))
            judge_curl(v, args, count);
        else
            for (size_t k = 0; k < count; k++)
                if (ch_str_has_prefix(args[k], "--post") || ch_str_has_prefix(args[k], "--method") ||
                    ch_str_has_prefix(args[k], "--body"))
                    weigh(v, CH_RISK_ASK, "it sends data to a server");
    } else if (!strcmp(program, "rm")) {
        weigh(v, CH_RISK_CONFINED, NULL);
        for (size_t k = 0; k < count; k++)
            if (!strcmp(args[k], "--recursive") || (*args[k] == '-' && args[k][1] != '-' && strpbrk(args[k], "rR")))
                weigh(v, CH_RISK_ASK, "it deletes whole folders");
    } else if (!strcmp(program, "rsync")) {
        weigh(v, CH_RISK_CONFINED, NULL);
        for (size_t k = 0; k < count; k++)
            if (*args[k] != '-' && strchr(args[k], ':'))
                weigh(v, CH_RISK_ASK, "it connects to another computer");
    } else if (!listed(READ_ONLY, program))
        weigh(v, CH_RISK_CONFINED, NULL);
    else if (!strcmp(program, "find"))
        judge_find(s, args, count);
    else if (!strcmp(program, "sort") && has(args, count, "-o --output"))
        weigh(v, CH_RISK_CONFINED, NULL);
}

static void end_word(Scan *s) {
    if (!s->in_word)
        return;
    if (s->target) {
        s->target = false;
        if (ch_path_protected(s->word.str))
            weigh(s->verdict, CH_RISK_ASK, "it writes to a file that controls how tools or git behave");
        if (!listed("/dev/null /dev/stdout /dev/stderr", s->word.str))
            s->writes = true;
    } else if (s->discard)
        s->discard = false;
    else
        ch_vec_push(&s->words, ch_strdup(s->word.str));
    ch_buf_truncate(&s->word, 0);
    s->in_word = false;
}
static void end_command(Scan *s, bool piped) {
    end_word(s);
    if (s->writes)
        weigh(s->verdict, CH_RISK_CONFINED, NULL);
    if (s->words.len)
        judge(s, (char **)s->words.data, s->words.len);
    ch_vec_clear(&s->words);
    s->writes = false;
    if (!piped)
        s->fetched = false;
}
/* "2" in "2>file" is the descriptor, not an argument. */
static void end_before_redirect(Scan *s) {
    bool digits = s->in_word;
    for (size_t i = 0; i < s->word.len; i++)
        digits &= isdigit((unsigned char)s->word.str[i]) != 0;
    if (digits) {
        ch_buf_truncate(&s->word, 0);
        s->in_word = false;
    } else
        end_word(s);
}
/* After "<<": the word that ends the text, with any quotes off. */
static const char *read_delimiter(Scan *s, const char *p) {
    s->heredoc_tabs = p[1] == '-';
    p += s->heredoc_tabs;
    while (p[1] == ' ' || p[1] == '\t')
        p++;
    ChBuf end;
    ch_buf_init(&end);
    while (p[1] && !strchr(" \t\n;&|<>()", p[1])) {
        p++;
        if (*p == '\\' && p[1])
            ch_buf_append_c(&end, *++p);
        else if (*p != '\'' && *p != '"')
            ch_buf_append_c(&end, *p);
    }
    /* What a shell is fed is what it will run, so that text is read like the rest. */
    bool to_shell = s->words.len && listed(SHELLS, base(s->words.data[0]));
    free(s->heredoc);
    s->heredoc = to_shell ? NULL : ch_strdup(end.str);
    ch_buf_clear(&end);
    return p;
}
/* From the line break before the text to the line break after its last line. */
static const char *skip_heredoc(const char *p, const char *delimiter, bool tabs) {
    size_t n = strlen(delimiter);
    for (const char *line = p + 1;;) {
        const char *start = line, *end = strchrnul(line, '\n');
        while (tabs && *start == '\t')
            start++;
        if ((size_t)(end - start) == n && !strncmp(start, delimiter, n))
            return *end ? end : end - 1;
        if (!*end)
            return end - 1;
        line = end + 1;
    }
}
static void scan(const char *text, Verdict *v, int depth) {
    if (depth > 4) {
        weigh(v, CH_RISK_ASK, UNREADABLE);
        return;
    }
    Scan s = {.verdict = v, .depth = depth};
    ch_vec_init(&s.words, free);
    ch_buf_init(&s.word);
    bool quoted = false, ticked = false, outer[8], broken = false;
    int nested = 0;
    for (const char *p = text; *p && !broken; p++) {
        char c = *p;
        bool opens = (c == '$' && p[1] == '(') || (!quoted && (c == '<' || c == '>') && p[1] == '(') || (c == '`' && !ticked);
        if (c == '\\' && p[1]) {
            if (*++p != '\n') {
                ch_buf_append_c(&s.word, *p);
                s.in_word = true;
            }
        } else if (c == '\'' && !quoted) {
            const char *end = strchr(p + 1, '\'');
            broken = !end;
            if (end) {
                ch_buf_append_len(&s.word, p + 1, (size_t)(end - p - 1));
                s.in_word = true;
                p = end;
            }
        } else if (c == '"') {
            quoted = !quoted;
            s.in_word = true;
        } else if (opens) {
            /* What a substitution runs is a command like any other. */
            end_command(&s, false);
            broken = nested == (int)CH_N_ELEMENTS(outer);
            if (!broken)
                outer[nested++] = quoted;
            quoted = false;
            ticked |= c == '`';
            p += c != '`';
        } else if (c == '`') {
            end_command(&s, false);
            ticked = false;
            if (nested)
                quoted = outer[--nested];
        } else if (quoted) {
            ch_buf_append_c(&s.word, c);
            s.in_word = true;
        } else if (c == ')') {
            end_command(&s, false);
            if (nested)
                quoted = outer[--nested];
        } else if (c == ' ' || c == '\t')
            end_word(&s);
        else if (c == '#' && !s.in_word)
            p = strchrnul(p, '\n') - 1;
        else if (c == '\n') {
            end_command(&s, false);
            if (s.heredoc) {
                p = skip_heredoc(p, s.heredoc, s.heredoc_tabs);
                ch_clear_pointer(&s.heredoc, free);
            }
        } else if (c == ';' || c == '(')
            end_command(&s, false);
        else if (c == '&' && p[1] == '>') {
            end_word(&s);
            p++;
            p += p[1] == '>';
            s.target = true;
        } else if (c == '&') {
            p += p[1] == '&';
            end_command(&s, false);
        } else if (c == '|') {
            bool either = p[1] == '|';
            p += either || p[1] == '&';
            end_command(&s, !either);
        } else if (c == '>') {
            end_before_redirect(&s);
            while (p[1] == '>' || p[1] == '|')
                p++;
            if (p[1] == '&') {
                p++;
                while (isdigit((unsigned char)p[1]) || p[1] == '-')
                    p++;
            } else
                s.target = true;
        } else if (c == '<') {
            end_before_redirect(&s);
            if (p[1] == '<' && p[2] == '<') {
                p += 2;
                s.discard = true;
            } else if (p[1] == '<')
                p = read_delimiter(&s, p + 1);
            else
                s.discard = true;
        } else {
            ch_buf_append_c(&s.word, c);
            s.in_word = true;
        }
    }
    end_command(&s, false);
    if (broken || quoted || nested)
        weigh(v, CH_RISK_ASK, UNREADABLE);
    ch_vec_clear(&s.words);
    ch_buf_clear(&s.word);
    free(s.heredoc);
}
ChRisk ch_command_risk(const char *command, const char **why) {
    Verdict v = {CH_RISK_NONE, NULL};
    scan(command, &v, 0);
    if (why)
        *why = v.why;
    return v.risk;
}
/* Files that decide what runs later, or what a tool may do. Inside a session's folders,
 * but writing one is a different thing from editing the project. */
bool ch_path_protected(const char *path) {
    bool found = false;
    char **parts = ch_strsplit(path, "/");
    for (size_t i = 0; parts[i] && !found; i++)
        found = listed(".git .husky .vscode .idea .claude .chela .ssh .gnupg .aws .bashrc .bash_profile .zshrc .profile "
                       ".gitconfig .npmrc .pypirc .netrc .env", parts[i]) ||
                ch_str_has_prefix(parts[i], ".env.");
    ch_strfreev(parts);
    return found;
}

bool ch_may_write(const ChRun *run, const char *path, const char **why) {
    *why = NULL;
    if (run->mode == CH_BYPASS || run->mode == CH_ACCEPT_EDITS)
        return true;
    if (run->mode != CH_AUTO)
        return false;
    if (!ch_path_protected(path))
        return true;
    *why = "this file controls how tools or git behave";
    return false;
}
bool ch_may_run(const ChRun *run, const char *command, bool confined, const char **why) {
    *why = NULL;
    if (run->mode == CH_BYPASS)
        return true;
    if (run->mode != CH_AUTO)
        return false;
    ChRisk risk = ch_command_risk(command, why);
    if (risk == CH_RISK_CONFINED && !confined) {
        *why = "it may change things and nothing here can confine it";
        return false;
    }
    return risk != CH_RISK_ASK;
}
/* Every file the harness names is in this session; it may name none, and then nobody knows. */
static bool places_allowed(const ChRun *run, json_object *tool, bool protect, const char **why) {
    json_object *locations = ch_json_get(tool, "locations"), *input = ch_json_get(tool, "rawInput");
    size_t count = json_object_is_type(locations, json_type_array) ? json_object_array_length(locations) : 0;
    const char *single = *ch_json_str(input, "file_path") ? ch_json_str(input, "file_path") : ch_json_str(input, "path");
    if (!count && !*single) {
        *why = "the harness did not say which file";
        return false;
    }
    for (size_t i = 0; i < CH_MAX(count, 1); i++) {
        const char *path = count ? ch_json_str(json_object_array_get_idx(locations, i), "path") : single;
        if (!ch_path_in_session(run, path)) {
            *why = "that file is outside this session's folders";
            return false;
        }
        if (protect && ch_path_protected(path)) {
            *why = "this file controls how tools or git behave";
            return false;
        }
    }
    return true;
}
/* Harnesses give a command as a string, or as the words of one. */
static char *harness_command(json_object *tool) {
    json_object *command = ch_json_get(ch_json_get(tool, "rawInput"), "command");
    if (json_object_is_type(command, json_type_string))
        return ch_strdup(json_object_get_string(command));
    if (!json_object_is_type(command, json_type_array))
        return NULL;
    ChBuf text;
    ch_buf_init(&text);
    for (size_t i = 0; i < json_object_array_length(command); i++) {
        char *quoted = ch_shell_quote(json_object_get_string(json_object_array_get_idx(command, i)));
        ch_buf_printf(&text, "%s%s", i ? " " : "", quoted);
        free(quoted);
    }
    return ch_buf_steal(&text);
}
/* A harness starts its own commands, so none of them are in Chela's sandbox: only what
 * changes nothing runs by itself. */
bool ch_may_harness(const ChRun *run, json_object *tool, const char **why) {
    *why = NULL;
    if (run->mode == CH_BYPASS)
        return true;
    if (run->mode != CH_AUTO && run->mode != CH_ACCEPT_EDITS)
        return false;
    const char *kind = ch_json_str(tool, "kind");
    if (!strcmp(kind, "edit"))
        return places_allowed(run, tool, run->mode == CH_AUTO, why);
    if (run->mode != CH_AUTO)
        return false;
    if (!strcmp(kind, "search") || !strcmp(kind, "think"))
        return true;
    if (!strcmp(kind, "read"))
        return places_allowed(run, tool, false, why);
    if (!strcmp(kind, "execute")) {
        char *command = harness_command(tool);
        bool allowed = command && ch_may_run(run, command, false, why);
        if (!command)
            *why = "the harness did not say which command";
        free(command);
        return allowed;
    }
    *why = "Chela cannot tell what this harness action does";
    return false;
}
