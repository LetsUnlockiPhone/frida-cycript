/* Cycript - Optimizing JavaScript Compiler/Runtime
 * Copyright (C) 2009-2015  Jay Freeman (saurik)
*/

/* GNU Affero General Public License, Version 3 {{{ */
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.

 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/
/* }}} */

#include "cycript.hpp"

#ifdef CY_EXECUTE
#include "JavaScript.hpp"
#endif

#include <cstdio>
#include <complex>
#include <fstream>
#include <sstream>

#ifdef HAVE_READLINE_H
#include <readline.h>
#else
#include <readline/readline.h>
#endif

#ifdef HAVE_HISTORY_H
#include <history.h>
#else
#include <readline/history.h>
#endif

#include <errno.h>
#include <getopt.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netdb.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>

#include <dlfcn.h>
#include <pwd.h>
#include <term.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#include "Driver.hpp"
#include "Error.hpp"
#include "Highlight.hpp"
#include "Syntax.hpp"

extern "C" int rl_display_fixed;
extern "C" int _rl_vis_botlin;
extern "C" int _rl_last_c_pos;
extern "C" int _rl_last_v_pos;

typedef std::complex<int> CYCursor;

static CYCursor current_;
static int width_;
static size_t point_;

unsigned CYDisplayWidth() {
    struct winsize info;
    if (ioctl(1, TIOCGWINSZ, &info) != -1)
        return info.ws_col;
    return tgetnum(const_cast<char *>("co"));
}

void CYDisplayOutput_(bool display, const char *&data) {
    for (;; ++data) {
        char next(*data);
        if (next == '\0' || next == CYIgnoreEnd)
            return;
        if (display)
            putchar(next);
    }
}

CYCursor CYDisplayOutput(bool display, int width, const char *data, ssize_t offset = 0) {
    CYCursor point(current_);

    for (;;) {
        if (offset-- == 0)
            point = current_;
        switch (char next = *data++) {
            case '\0':
                return point;
            break;

            case CYIgnoreStart:
                CYDisplayOutput_(display, data);
            case CYIgnoreEnd:
                ++offset;
            break;

            default:
                if (display)
                    putchar(next);
                current_ += CYCursor(0, 1);
                if (current_.imag() != width)
                    break;
                current_ = CYCursor(current_.real() + 1, 0);
                if (display)
                    putp(clr_eos);
            break;

            case '\n':
                current_ = CYCursor(current_.real() + 1, 4);
                if (display) {
                    putp(clr_eol);
                    putchar('\n');
                    putchar(' ');
                    putchar(' ');
                    putchar(' ');
                    putchar(' ');
                }
            break;

        }
    }
}

void CYDisplayMove_(char *negative, char *positive, int offset) {
    if (offset < 0)
        putp(tparm(negative, -offset));
    else if (offset > 0)
        putp(tparm(positive, offset));
}

void CYDisplayMove(CYCursor target) {
    CYCursor offset(target - current_);

    CYDisplayMove_(parm_up_cursor, parm_down_cursor, offset.real());

    if (char *parm = tparm(column_address, target.imag()))
        putp(parm);
    else
        CYDisplayMove_(parm_left_cursor, parm_right_cursor, offset.imag());

    current_ = target;
}

void CYDisplayUpdate() {
    current_ = CYCursor(_rl_last_v_pos, _rl_last_c_pos);

    const char *prompt(rl_display_prompt);

    std::ostringstream stream;
    CYLexerHighlight(rl_line_buffer, rl_end, stream, true);
    std::string string(stream.str());
    const char *buffer(string.c_str());

    int width(CYDisplayWidth());
    if (width_ != width) {
        current_ = CYCursor();
        CYDisplayOutput(false, width, prompt);
        current_ = CYDisplayOutput(false, width, buffer, point_);
    }

    CYDisplayMove(CYCursor());
    CYDisplayOutput(true, width, prompt);
    CYCursor target(CYDisplayOutput(true, width, stream.str().c_str(), rl_point));

    _rl_vis_botlin = current_.real();

    if (current_.imag() == 0)
        CYDisplayOutput(true, width, " ");
    putp(clr_eos);

    CYDisplayMove(target);
    fflush(stdout);

    _rl_last_v_pos = current_.real();
    _rl_last_c_pos = current_.imag();

    width_ = width;
    point_ = rl_point;
}

static volatile enum {
    Working,
    Parsing,
    Running,
    Sending,
    Waiting,
} mode_;

static jmp_buf ctrlc_;

static void sigint(int) {
    switch (mode_) {
        case Working:
            return;
        case Parsing:
            longjmp(ctrlc_, 1);
        case Running:
            CYCancel();
            return;
        case Sending:
            return;
        case Waiting:
            return;
    }
}

static bool bison_;
static bool timing_;
static bool strict_;
static bool pretty_;

void Setup(CYDriver &driver) {
    if (bison_)
        driver.debug_ = 1;
    if (strict_)
        driver.strict_ = true;
}

void Setup(CYOutput &out, CYDriver &driver, CYOptions &options, bool lower) {
    out.pretty_ = pretty_;
    if (lower)
        driver.Replace(options);
}

static CYUTF8String Run(CYPool &pool, int client, CYUTF8String code) {
    const char *json;
    uint32_t size;

    if (client == -1) {
        mode_ = Running;
#ifdef CY_EXECUTE
        json = CYExecute(CYGetJSContext(), pool, code);
#else
        json = NULL;
#endif
        mode_ = Working;
        if (json == NULL)
            size = 0;
        else
            size = strlen(json);
    } else {
        mode_ = Sending;
        size = code.size;
        _assert(CYSendAll(client, &size, sizeof(size)));
        _assert(CYSendAll(client, code.data, code.size));
        mode_ = Waiting;
        _assert(CYRecvAll(client, &size, sizeof(size)));
        if (size == _not(uint32_t))
            json = NULL;
        else {
            char *temp(new(pool) char[size + 1]);
            _assert(CYRecvAll(client, temp, size));
            temp[size] = '\0';
            json = temp;
        }
        mode_ = Working;
    }

    return CYUTF8String(json, size);
}

static CYUTF8String Run(CYPool &pool, int client, const std::string &code) {
    return Run(pool, client, CYUTF8String(code.c_str(), code.size()));
}

static std::ostream *out_;

static void Output(CYUTF8String json, std::ostream *out, bool expand = false) {
    const char *data(json.data);
    size_t size(json.size);

    if (data == NULL || out == NULL)
        return;

    if (!expand ||
        data[0] != '@' && data[0] != '"' && data[0] != '\'' ||
        data[0] == '@' && data[1] != '"' && data[1] != '\''
    )
        CYLexerHighlight(data, size, *out);
    else for (size_t i(0); i != size; ++i)
        if (data[i] != '\\')
            *out << data[i];
        else switch(data[++i]) {
            case '\0': goto done;
            case '\\': *out << '\\'; break;
            case '\'': *out << '\''; break;
            case '"': *out << '"'; break;
            case 'b': *out << '\b'; break;
            case 'f': *out << '\f'; break;
            case 'n': *out << '\n'; break;
            case 'r': *out << '\r'; break;
            case 't': *out << '\t'; break;
            case 'v': *out << '\v'; break;
            default: *out << '\\'; --i; break;
        }

  done:
    *out << std::endl;
}

int (*append_history$)(int, const char *);

static std::string command_;

static int client_;

static CYUTF8String Run(CYPool &pool, const std::string &code) {
    return Run(pool, client_, code);
}

static char **Complete(const char *word, int start, int end) {
    rl_attempted_completion_over = ~0;
    std::string line(rl_line_buffer, start);
    char **values(CYComplete(word, command_ + line, &Run));
    mode_ = Parsing;
    return values;
}

// need char *, not const char *
static char name_[] = "cycript";
static char break_[] = " \t\n\"\\'`@><=;|&{(" ")}" ".:[]";

class History {
  private:
    std::string histfile_;
    size_t histlines_;

  public:
    History(std::string histfile) :
        histfile_(histfile),
        histlines_(0)
    {
        read_history(histfile_.c_str());

        for (HIST_ENTRY *history((history_set_pos(0), current_history())); history; history = next_history())
            for (char *character(history->line); *character; ++character)
                if (*character == '\x01') *character = '\n';
    }

    ~History() {
        for (HIST_ENTRY *history((history_set_pos(0), current_history())); history; history = next_history())
            for (char *character(history->line); *character; ++character)
                if (*character == '\n') *character = '\x01';

        if (append_history$ != NULL) {
            int fd(_syscall(open(histfile_.c_str(), O_CREAT | O_WRONLY, 0600)));
            _syscall(close(fd));
            _assert((*append_history$)(histlines_, histfile_.c_str()) == 0);
        } else {
            _assert(write_history(histfile_.c_str()) == 0);
        }
    }

    void operator +=(std::string command) {
        add_history(command.c_str());
        ++histlines_;
    }
};

template <typename Type_>
static Type_ *CYmemrchr(Type_ *data, Type_ value, size_t size) {
    while (size != 0)
        if (data[--size] == value)
            return data + size;
    return NULL;
}

static int CYConsoleKeyReturn(int count, int key) {
    if (rl_point != rl_end) {
        if (memchr(rl_line_buffer, '\n', rl_end) == NULL)
            return rl_newline(count, key);

        char *before(CYmemrchr(rl_line_buffer, '\n', rl_point));
        if (before == NULL)
            before = rl_line_buffer;

        int space(before + 1 - rl_line_buffer);
        while (space != rl_point && rl_line_buffer[space] == ' ')
            ++space;

        int adjust(rl_line_buffer + space - 1 - before);
        if (space == rl_point && adjust != 0)
            rl_rubout(adjust, '\b');

        rl_insert(count, '\n');
        if (adjust != 0)
            rl_insert(adjust, ' ');

        return 0;
    }

    bool done(false);
    if (rl_line_buffer[0] == '?')
        done = true;
    else {
        std::string command(rl_line_buffer, rl_end);
        command += '\n';
        std::istringstream stream(command);

        size_t last(std::string::npos);
        for (size_t i(0); i != std::string::npos; i = command.find('\n', i + 1))
            ++last;

        CYPool pool;
        CYDriver driver(pool, stream);
        if (driver.Parse() || !driver.errors_.empty())
            for (CYDriver::Errors::const_iterator error(driver.errors_.begin()); error != driver.errors_.end(); ++error) {
                if (error->location_.begin.line != last + 1)
                    done = true;
                break;
            }
        else
            done = true;
    }

    if (done)
        return rl_newline(count, key);

    rl_insert(count, '\n');
    return 0;
}

static int CYConsoleKeyUp(int count, int key) {
    while (count-- != 0) {
        char *after(CYmemrchr(rl_line_buffer, '\n', rl_point));
        if (after == NULL) {
            if (int value = rl_get_previous_history(1, key))
                return value;
            continue;
        }

        char *before(CYmemrchr(rl_line_buffer, '\n', after - rl_line_buffer));
        if (before == NULL)
            before = rl_line_buffer - 1;

        ptrdiff_t offset(rl_line_buffer + rl_point - after);
        if (offset > after - before)
            rl_point = after - rl_line_buffer;
        else
            rl_point = before + offset - rl_line_buffer;
    }

    return 0;
}

static int CYConsoleKeyDown(int count, int key) {
    while (count-- != 0) {
        char *after(static_cast<char *>(memchr(rl_line_buffer + rl_point, '\n', rl_end - rl_point)));
        if (after == NULL) {
            int where(where_history());
            if (int value = rl_get_next_history(1, key))
                return value;
            if (where != where_history()) {
                char *first(static_cast<char *>(memchr(rl_line_buffer, '\n', rl_end)));
                if (first != NULL)
                    rl_point = first - 1 - rl_line_buffer;
            }
            continue;
        }

        char *before(CYmemrchr(rl_line_buffer, '\n', rl_point));
        if (before == NULL)
            before = rl_line_buffer - 1;

        char *next(static_cast<char *>(memchr(after + 1, '\n', rl_line_buffer + rl_end - after - 1)));
        if (next == NULL)
            next = rl_line_buffer + rl_end;

        ptrdiff_t offset(rl_line_buffer + rl_point - before);
        if (offset > next - after)
            rl_point = next - rl_line_buffer;
        else
            rl_point = after + offset - rl_line_buffer;
    }

    return 0;
}

static int CYConsoleLineBegin(int count, int key) {
    while (rl_point != 0 && rl_line_buffer[rl_point - 1] != '\n')
        --rl_point;
    return 0;
}

static int CYConsoleLineEnd(int count, int key) {
    while (rl_point != rl_end && rl_line_buffer[rl_point] != '\n')
        ++rl_point;
    return 0;
}

static int CYConsoleKeyBack(int count, int key) {
    while (count-- != 0) {
        char *before(CYmemrchr(rl_line_buffer, '\n', rl_point));
        if (before == NULL)
            return rl_rubout(count, key);

        int start(before + 1 - rl_line_buffer);
        if (start == rl_point) rubout: {
            rl_rubout(1, key);
            continue;
        }

        for (int i(start); i != rl_point; ++i)
            if (rl_line_buffer[i] != ' ')
                goto rubout;
        rl_rubout((rl_point - start) % 4 ?: 4, key);
    }

    return 0;
}

static int CYConsoleKeyTab(int count, int key) {
    char *before(CYmemrchr(rl_line_buffer, '\n', rl_point));
    if (before == NULL) complete:
        return rl_complete_internal(rl_completion_mode(&CYConsoleKeyTab));
    int start(before + 1 - rl_line_buffer);
    for (int i(start); i != rl_point; ++i)
        if (rl_line_buffer[i] != ' ')
            goto complete;
    return rl_insert(4 - (rl_point - start) % 4, ' ');
}

static void CYConsoleRemapBind(rl_command_func_t *from, rl_command_func_t *to) {
    char **keyseqs(rl_invoking_keyseqs(from));
    if (keyseqs == NULL)
        return;
    for (char **keyseq(keyseqs); *keyseq != NULL; ++keyseq) {
        rl_bind_keyseq(*keyseq, to);
        free(*keyseq);
    }
    free(keyseqs);
}

static void CYConsolePrepTerm(int meta) {
    rl_prep_terminal(meta);

    CYConsoleRemapBind(&rl_beg_of_line, &CYConsoleLineBegin);
    CYConsoleRemapBind(&rl_end_of_line, &CYConsoleLineEnd);
    CYConsoleRemapBind(&rl_rubout, &CYConsoleKeyBack);
}

static void Console(CYOptions &options) {
    std::string basedir;
    if (const char *home = getenv("HOME"))
        basedir = home;
    else {
        passwd *passwd;
        if (const char *username = getenv("LOGNAME"))
            passwd = getpwnam(username);
        else
            passwd = getpwuid(getuid());
        basedir = passwd->pw_dir;
    }

    basedir += "/.cycript";
    mkdir(basedir.c_str(), 0700);

    rl_initialize();
    rl_readline_name = name_;

    History history(basedir + "/history");

    bool bypass(false);
    bool debug(false);
    bool expand(false);
    bool lower(true);

    out_ = &std::cout;

    rl_completer_word_break_characters = break_;
    rl_attempted_completion_function = &Complete;

    rl_bind_key(TAB, &CYConsoleKeyTab);

    rl_redisplay_function = CYDisplayUpdate;
    rl_prep_term_function = CYConsolePrepTerm;

#if defined (__MSDOS__)
    rl_bind_keyseq("\033[0A", &CYConsoleKeyUp);
    rl_bind_keyseq("\033[0D", &CYConsoleKeyDown);
#endif
    rl_bind_keyseq("\033[A", &CYConsoleKeyUp);
    rl_bind_keyseq("\033[B", &CYConsoleKeyDown);
    rl_bind_keyseq("\033OA", &CYConsoleKeyUp);
    rl_bind_keyseq("\033OB", &CYConsoleKeyDown);
#if defined (__MINGW32__)
    rl_bind_keyseq("\340H", &CYConsoleKeyUp);
    rl_bind_keyseq("\340P", &CYConsoleKeyDown);
    rl_bind_keyseq("\\000H", &CYConsoleKeyUp);
    rl_bind_keyseq("\\000P", &CYConsoleKeyDown);
#endif

    struct sigaction action;
    sigemptyset(&action.sa_mask);
    action.sa_handler = &sigint;
    action.sa_flags = 0;
    sigaction(SIGINT, &action, NULL);

    for (;;) {
        if (setjmp(ctrlc_) != 0) {
            mode_ = Working;
            *out_ << std::endl;
            continue;
        }

        if (bypass) {
            rl_bind_key('\r', &rl_newline);
            rl_bind_key('\n', &rl_newline);
        } else {
            rl_bind_key('\r', &CYConsoleKeyReturn);
            rl_bind_key('\n', &CYConsoleKeyReturn);
        }

        mode_ = Parsing;
        char *line(readline("cy# "));
        mode_ = Working;

        if (line == NULL) {
            *out_ << std::endl;
            break;
        }

        std::string command(line);
        free(line);
        if (command.empty())
            continue;

        if (command[0] == '?') {
            std::string data(command.substr(1));
            if (data == "bypass") {
                bypass = !bypass;
                *out_ << "bypass == " << (bypass ? "true" : "false") << std::endl;
            } else if (data == "debug") {
                debug = !debug;
                *out_ << "debug == " << (debug ? "true" : "false") << std::endl;
            } else if (data == "destroy") {
                CYDestroyContext();
            } else if (data == "gc") {
                *out_ << "collecting... " << std::flush;
                CYGarbageCollect(CYGetJSContext());
                *out_ << "done." << std::endl;
            } else if (data == "exit") {
                return;
            } else if (data == "expand") {
                expand = !expand;
                *out_ << "expand == " << (expand ? "true" : "false") << std::endl;
            } else if (data == "lower") {
                lower = !lower;
                *out_ << "lower == " << (lower ? "true" : "false") << std::endl;
            }

            history += command;
            continue;
        }

        std::string code;
        if (bypass)
            code = command;
        else {
            std::istringstream stream(command);

            CYPool pool;
            CYDriver driver(pool, stream);
            Setup(driver);

            if (driver.Parse() || !driver.errors_.empty()) {
                for (CYDriver::Errors::const_iterator error(driver.errors_.begin()); error != driver.errors_.end(); ++error) {
                    CYPosition begin(error->location_.begin);
                    CYPosition end(error->location_.end);

                    /*if (begin.line != lines2.size()) {
                        std::cerr << "  | ";
                        std::cerr << lines2[begin.line - 1] << std::endl;
                    }*/

                    std::cerr << "....";
                    for (size_t i(0); i != begin.column; ++i)
                        std::cerr << '.';
                    if (begin.line != end.line || begin.column == end.column)
                        std::cerr << '^';
                    else for (size_t i(0), e(end.column - begin.column); i != e; ++i)
                        std::cerr << '^';
                    std::cerr << std::endl;

                    std::cerr << "  | ";
                    std::cerr << error->message_ << std::endl;

                    history += command;
                    break;
                }

                continue;
            }

            if (driver.script_ == NULL)
                continue;

            std::stringbuf str;
            CYOutput out(str, options);
            Setup(out, driver, options, lower);
            out << *driver.script_;
            code = str.str();
        }

        history += command;

        if (debug) {
            std::cout << "cy= ";
            CYLexerHighlight(code.c_str(), code.size(), std::cout);
            std::cout << std::endl;
        }

        CYPool pool;
        Output(Run(pool, client_, code), &std::cout, expand);
    }
}

void InjectLibrary(pid_t, int, const char *const []);

static uint64_t CYGetTime() {
#ifdef __APPLE__
    return mach_absolute_time();
#else
    struct timespec spec;
    clock_gettime(CLOCK_MONOTONIC, &spec);
    return spec.tv_sec * UINT64_C(1000000000) + spec.tv_nsec;
#endif
}

int Main(int argc, char * const argv[], char const * const envp[]) {
    bool tty(isatty(STDIN_FILENO));
    bool compile(false);
    bool target(false);
    CYOptions options;

    append_history$ = (int (*)(int, const char *)) (dlsym(RTLD_DEFAULT, "append_history"));

#ifdef CY_ATTACH
    pid_t pid(_not(pid_t));
#endif

    const char *host(NULL);
    const char *port(NULL);

    optind = 1;

    for (;;) {
        int option(getopt_long(argc, argv,
            "c"
            "g:"
            "n:"
#ifdef CY_ATTACH
            "p:"
#endif
            "r:"
            "s"
        , (const struct option[]) {
            {NULL, no_argument, NULL, 'c'},
            {NULL, required_argument, NULL, 'g'},
            {NULL, required_argument, NULL, 'n'},
#ifdef CY_ATTACH
            {NULL, required_argument, NULL, 'p'},
#endif
            {NULL, required_argument, NULL, 'r'},
            {NULL, no_argument, NULL, 's'},
        {0, 0, 0, 0}}, NULL));

        switch (option) {
            case -1:
                goto getopt;

            case ':':
            case '?':
                fprintf(stderr,
                    "usage: cycript [-c]"
#ifdef CY_ATTACH
                    " [-p <pid|name>]"
#endif
                    " [-r <host:port>]"
                    " [<script> [<arg>...]]\n"
                );
                return 1;

            target:
                if (!target)
                    target = true;
                else {
                    fprintf(stderr, "only one of -[c"
#ifdef CY_ATTACH
                    "p"
#endif
                    "r] may be used at a time\n");
                    return 1;
                }
            break;

            case 'c':
                compile = true;
            goto target;

            case 'g':
                if (false);
                else if (strcmp(optarg, "rename") == 0)
                    options.verbose_ = true;
                else if (strcmp(optarg, "bison") == 0)
                    bison_ = true;
                else if (strcmp(optarg, "timing") == 0)
                    timing_ = true;
                else {
                    fprintf(stderr, "invalid name for -g\n");
                    return 1;
                }
            break;

            case 'n':
                if (false);
                else if (strcmp(optarg, "minify") == 0)
                    pretty_ = true;
                else {
                    fprintf(stderr, "invalid name for -n\n");
                    return 1;
                }
            break;

#ifdef CY_ATTACH
            case 'p': {
                size_t size(strlen(optarg));
                char *end;

                pid = strtoul(optarg, &end, 0);
                if (optarg + size != end) {
                    // XXX: arg needs to be escaped in some horrendous way of doom
                    // XXX: this is a memory leak now because I just don't care enough
                    char *command;
                    int writ(asprintf(&command, "ps axc|sed -e '/^ *[0-9]/{s/^ *\\([0-9]*\\)\\( *[^ ]*\\)\\{3\\} *-*\\([^ ]*\\)/\\3 \\1/;/^%s /{s/^[^ ]* //;q;};};d'", optarg));
                    _assert(writ != -1);

                    if (FILE *pids = popen(command, "r")) {
                        char value[32];
                        size = 0;

                        for (;;) {
                            size_t read(fread(value + size, 1, sizeof(value) - size, pids));
                            if (read == 0)
                                break;
                            else {
                                size += read;
                                if (size == sizeof(value))
                                    goto fail;
                            }
                        }

                      size:
                        if (size == 0)
                            goto fail;
                        if (value[size - 1] == '\n') {
                            --size;
                            goto size;
                        }

                        value[size] = '\0';
                        size = strlen(value);
                        pid = strtoul(value, &end, 0);
                        if (value + size != end) fail:
                            pid = _not(pid_t);
                        _syscall(pclose(pids));
                    }

                    if (pid == _not(pid_t)) {
                        fprintf(stderr, "unable to find process `%s' using ps\n", optarg);
                        return 1;
                    }
                }
            } goto target;
#endif

            case 'r': {
                //size_t size(strlen(optarg));

                char *colon(strrchr(optarg, ':'));
                if (colon == NULL) {
                    fprintf(stderr, "missing colon in hostspec\n");
                    return 1;
                }

                /*char *end;
                port = strtoul(colon + 1, &end, 10);
                if (end != optarg + size) {
                    fprintf(stderr, "invalid port in hostspec\n");
                    return 1;
                }*/

                host = optarg;
                *colon = '\0';
                port = colon + 1;
            } goto target;

            case 's':
                strict_ = true;
            break;

            default:
                _assert(false);
        }
    }

  getopt:
    argc -= optind;
    argv += optind;

    const char *script;

#ifdef CY_ATTACH
    if (pid != _not(pid_t) && argc > 1) {
        fprintf(stderr, "-p cannot set argv\n");
        return 1;
    }
#endif

    if (argc == 0)
        script = NULL;
    else {
#ifdef CY_EXECUTE
        // XXX: const_cast?! wtf gcc :(
        CYSetArgs(argc - 1, const_cast<const char **>(argv + 1));
#endif
        script = argv[0];
        if (strcmp(script, "-") == 0)
            script = NULL;
    }

#ifdef CY_ATTACH
    if (pid == _not(pid_t))
        client_ = -1;
    else {
        struct Socket {
            int fd_;

            Socket(int fd) :
                fd_(fd)
            {
            }

            ~Socket() {
                close(fd_);
            }

            operator int() {
                return fd_;
            }
        } server(_syscall(socket(PF_UNIX, SOCK_STREAM, 0)));

        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;

        const char *tmp;
#if defined(__APPLE__) && (defined(__arm__) || defined(__arm64__))
        tmp = "/Library/Caches";
#else
        tmp = "/tmp";
#endif

        sprintf(address.sun_path, "%s/.s.cy.%u", tmp, getpid());
        unlink(address.sun_path);

        struct File {
            const char *path_;

            File(const char *path) :
                path_(path)
            {
            }

            ~File() {
                unlink(path_);
            }
        } file(address.sun_path);

        _syscall(bind(server, reinterpret_cast<sockaddr *>(&address), SUN_LEN(&address)));
        _syscall(chmod(address.sun_path, 0777));

        _syscall(listen(server, 1));
        const char *const argv[] = {address.sun_path, NULL};
        InjectLibrary(pid, 1, argv);
        client_ = _syscall(accept(server, NULL, NULL));
    }
#else
    client_ = -1;
#endif

    if (client_ == -1 && host != NULL && port != NULL) {
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = 0;
        hints.ai_flags = 0;

        struct addrinfo *infos;
        _syscall(getaddrinfo(host, port, &hints, &infos));

        _assert(infos != NULL); try {
            for (struct addrinfo *info(infos); info != NULL; info = info->ai_next) {
                int client(_syscall(socket(info->ai_family, info->ai_socktype, info->ai_protocol))); try {
                    _syscall(connect(client, info->ai_addr, info->ai_addrlen));
                    client_ = client;
                    break;
                } catch (...) {
                    _syscall(close(client));
                    throw;
                }
            }
        } catch (...) {
            freeaddrinfo(infos);
            throw;
        }
    }

    if (script == NULL && tty)
        Console(options);
    else {
        std::istream *stream;
        if (script == NULL) {
            stream = &std::cin;
            script = "<stdin>";
        } else {
            stream = new std::fstream(script, std::ios::in | std::ios::binary);
            _assert(!stream->fail());
        }

        if (timing_) {
            std::stringbuf buffer;
            stream->get(buffer, '\0');
            _assert(!stream->fail());

            double average(0);
            int samples(-50);
            uint64_t start(CYGetTime());

            for (;;) {
                stream = new std::istringstream(buffer.str());

                CYPool pool;
                CYDriver driver(pool, *stream, script);
                Setup(driver);

                uint64_t begin(CYGetTime());
                driver.Parse();
                uint64_t end(CYGetTime());

                delete stream;

                average += (end - begin - average) / ++samples;

                uint64_t now(CYGetTime());
                if (samples == 0)
                    average = 0;
                else if ((now - start) / 1000000000 >= 1)
                    std::cout << std::fixed << average << '\t' << (end - begin) << '\t' << samples << std::endl;
                else continue;

                start = now;
            }

            stream = new std::istringstream(buffer.str());
            std::cin.get();
        }

        CYPool pool;
        CYDriver driver(pool, *stream, script);
        Setup(driver);

        bool failed(driver.Parse());

        if (failed || !driver.errors_.empty()) {
            for (CYDriver::Errors::const_iterator i(driver.errors_.begin()); i != driver.errors_.end(); ++i)
                std::cerr << i->location_.begin << ": " << i->message_ << std::endl;
        } else if (driver.script_ != NULL) {
            std::stringbuf str;
            CYOutput out(str, options);
            Setup(out, driver, options, true);
            out << *driver.script_;
            std::string code(str.str());
            if (compile)
                std::cout << code;
            else {
                CYUTF8String json(Run(pool, client_, code));
                if (CYStartsWith(json, "throw ")) {
                    CYLexerHighlight(json.data, json.size, std::cerr);
                    std::cerr << std::endl;
                    return 1;
                }
            }
        }
    }

    return 0;
}

int main(int argc, char * const argv[], char const * const envp[]) {
    try {
        return Main(argc, argv, envp);
    } catch (const CYException &error) {
        CYPool pool;
        fprintf(stderr, "%s\n", error.PoolCString(pool));
        return 1;
    }
}
