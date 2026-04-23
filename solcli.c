#define _POSIX_C_SOURCE 200809L

#include <curl/curl.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define SOLCLI_VERSION "0.2.0"
#define DEFAULT_MODEL "gpt-4o-mini"
#define OPENAI_URL "https://api.openai.com/v1/chat/completions"
#define SOLANA_SKILLS_CONTEXT \
    "Your knowledge is grounded in the Solana Skills catalog, especially these official areas: " \
    "Common Errors & Solutions, Version Compatibility Matrix, IDL & Client Code Generation, " \
    "Kit and web3.js interoperability, Payments & Commerce, Curated Resources, Security Checklist, " \
    "and Testing Strategy. " \
    "You should also be comfortable with ecosystem tooling and protocols frequently referenced in the skills catalog, " \
    "including Metaplex, Helius, QuickNode, Jupiter, Orca, Raydium, Sanctum, Kamino, Switchboard, Pyth, Squads, " \
    "Surfpool, Pinocchio, and Solana Kit. "

typedef struct {
    char *data;
    size_t len;
} Buffer;

typedef struct {
    char *role;
    char *content;
} ChatMessage;

typedef struct {
    ChatMessage *items;
    size_t len;
    size_t cap;
} ChatHistory;

typedef struct {
    char *last_actionable_request;
    char *last_project_name;
    char *last_action_summary;
} AgentState;

typedef enum {
    PROJECT_ANCHOR,
    PROJECT_NATIVE
} ProjectType;

typedef struct {
    char *name;
    char *path;
    ProjectType type;
} SolanaProject;

typedef struct {
    SolanaProject *items;
    size_t len;
    size_t cap;
} ProjectList;

static void set_agent_state_value(char **slot, const char *value);
static char *read_input_line(const char *prompt);
static char *resolve_active_keypair(void);

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    Buffer *buffer = (Buffer *)userp;
    char *new_data = realloc(buffer->data, buffer->len + total + 1);

    if (new_data == NULL) {
        return 0;
    }

    buffer->data = new_data;
    memcpy(buffer->data + buffer->len, contents, total);
    buffer->len += total;
    buffer->data[buffer->len] = '\0';
    return total;
}

static void free_chat_history(ChatHistory *history) {
    if (history == NULL) {
        return;
    }

    for (size_t i = 0; i < history->len; ++i) {
        free(history->items[i].role);
        free(history->items[i].content);
    }

    free(history->items);
    history->items = NULL;
    history->len = 0;
    history->cap = 0;
}

static int add_chat_message(ChatHistory *history, const char *role, const char *content) {
    ChatMessage *new_items;

    if (history == NULL || role == NULL || content == NULL) {
        return -1;
    }

    if (history->len == history->cap) {
        size_t new_cap = history->cap == 0 ? 8 : history->cap * 2;
        new_items = realloc(history->items, new_cap * sizeof(ChatMessage));
        if (new_items == NULL) {
            return -1;
        }

        history->items = new_items;
        history->cap = new_cap;
    }

    history->items[history->len].role = strdup(role);
    history->items[history->len].content = strdup(content);
    if (history->items[history->len].role == NULL || history->items[history->len].content == NULL) {
        free(history->items[history->len].role);
        free(history->items[history->len].content);
        return -1;
    }

    ++history->len;

    if (history->len > 12) {
        free(history->items[0].role);
        free(history->items[0].content);
        memmove(history->items, history->items + 1, (history->len - 1) * sizeof(ChatMessage));
        --history->len;
    }

    return 0;
}

static int append_format(char **target, size_t *used, const char *fmt, ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *new_target;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (needed < 0) {
        va_end(args);
        return -1;
    }

    new_target = realloc(*target, *used + (size_t)needed + 1);
    if (new_target == NULL) {
        va_end(args);
        return -1;
    }

    *target = new_target;
    vsnprintf(*target + *used, (size_t)needed + 1, fmt, args);
    *used += (size_t)needed;
    va_end(args);
    return 0;
}

static char *json_escape(const char *input) {
    size_t len = strlen(input);
    size_t capacity = (len * 2) + 1;
    size_t used = 0;
    char *escaped = malloc(capacity);

    if (escaped == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)input[i];
        const char *replacement = NULL;

        switch (ch) {
            case '\\':
                replacement = "\\\\";
                break;
            case '"':
                replacement = "\\\"";
                break;
            case '\b':
                replacement = "\\b";
                break;
            case '\f':
                replacement = "\\f";
                break;
            case '\n':
                replacement = "\\n";
                break;
            case '\r':
                replacement = "\\r";
                break;
            case '\t':
                replacement = "\\t";
                break;
            default:
                break;
        }

        if (replacement != NULL) {
            size_t replacement_len = strlen(replacement);
            if (used + replacement_len + 1 > capacity) {
                capacity = (capacity * 2) + replacement_len + 1;
                escaped = realloc(escaped, capacity);
                if (escaped == NULL) {
                    return NULL;
                }
            }

            memcpy(escaped + used, replacement, replacement_len);
            used += replacement_len;
        } else if (ch < 0x20) {
            if (used + 7 > capacity) {
                capacity = (capacity * 2) + 7;
                escaped = realloc(escaped, capacity);
                if (escaped == NULL) {
                    return NULL;
                }
            }

            snprintf(escaped + used, 7, "\\u%04x", ch);
            used += 6;
        } else {
            if (used + 2 > capacity) {
                capacity = (capacity * 2) + 2;
                escaped = realloc(escaped, capacity);
                if (escaped == NULL) {
                    return NULL;
                }
            }

            escaped[used++] = (char)ch;
        }
    }

    escaped[used] = '\0';
    return escaped;
}

static char *decode_json_string(const char *start) {
    size_t capacity = 256;
    size_t used = 0;
    char *decoded;

    decoded = malloc(capacity);
    if (decoded == NULL) {
        return NULL;
    }

    while (*start != '\0') {
        char out = *start;

        if (*start == '"') {
            break;
        }

        if (*start == '\\') {
            ++start;
            if (*start == '\0') {
                free(decoded);
                return NULL;
            }

            switch (*start) {
                case '"':
                case '\\':
                case '/':
                    out = *start;
                    break;
                case 'b':
                    out = '\b';
                    break;
                case 'f':
                    out = '\f';
                    break;
                case 'n':
                    out = '\n';
                    break;
                case 'r':
                    out = '\r';
                    break;
                case 't':
                    out = '\t';
                    break;
                case 'u':
                    out = '?';
                    for (int i = 0; i < 4 && start[1] != '\0'; ++i) {
                        ++start;
                    }
                    break;
                default:
                    out = *start;
                    break;
            }
        }

        if (used + 2 > capacity) {
            capacity *= 2;
            decoded = realloc(decoded, capacity);
            if (decoded == NULL) {
                return NULL;
            }
        }

        decoded[used++] = out;
        ++start;
    }

    decoded[used] = '\0';
    return decoded;
}

static char *extract_json_string_after(const char *json, const char *anchor) {
    const char *start = strstr(json, anchor);

    if (start == NULL) {
        return NULL;
    }

    start += strlen(anchor);
    return decode_json_string(start);
}

static char *extract_json_field(const char *json, const char *field_name) {
    const char *cursor = json;
    size_t field_len = strlen(field_name);

    while ((cursor = strstr(cursor, field_name)) != NULL) {
        const char *value = cursor + field_len;

        while (*value != '\0' && isspace((unsigned char)*value)) {
            ++value;
        }

        if (*value != ':') {
            cursor += field_len;
            continue;
        }

        ++value;
        while (*value != '\0' && isspace((unsigned char)*value)) {
            ++value;
        }

        if (*value != '"') {
            cursor += field_len;
            continue;
        }

        return decode_json_string(value + 1);
    }

    return NULL;
}

static char *extract_message_content(const char *json) {
    const char *message = strstr(json, "\"message\"");
    char *content;

    if (message != NULL) {
        content = extract_json_field(message, "\"content\"");
        if (content != NULL) {
            return content;
        }
    }

    content = extract_json_field(json, "\"content\"");
    if (content != NULL) {
        return content;
    }

    return extract_json_field(json, "\"message\"");
}

static void trim_trailing_whitespace(char *text) {
    size_t len;

    if (text == NULL) {
        return;
    }

    len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
}

static char *run_command_capture(const char *command) {
    FILE *pipe;
    char chunk[256];
    char *output = NULL;
    size_t used = 0;
    int status;

    pipe = popen(command, "r");
    if (pipe == NULL) {
        return NULL;
    }

    while (fgets(chunk, sizeof(chunk), pipe) != NULL) {
        size_t chunk_len = strlen(chunk);
        char *new_output = realloc(output, used + chunk_len + 1);

        if (new_output == NULL) {
            free(output);
            pclose(pipe);
            return NULL;
        }

        output = new_output;
        memcpy(output + used, chunk, chunk_len);
        used += chunk_len;
        output[used] = '\0';
    }

    status = pclose(pipe);
    if (status == -1) {
        free(output);
        return NULL;
    }

    if (output == NULL) {
        return NULL;
    }

    trim_trailing_whitespace(output);
    if (output[0] == '\0') {
        free(output);
        return NULL;
    }

    return output;
}

static int run_shell_command_streaming(const char *label, const char *command) {
    FILE *pipe;
    char wrapped_command[8192];
    char line[512];
    int status;

    if ((size_t)snprintf(
            wrapped_command,
            sizeof(wrapped_command),
            "bash -lc '%s' 2>&1",
            command) >= sizeof(wrapped_command)) {
        fprintf(stderr, "Shell command is too long.\n");
        return 1;
    }

    printf("Agent shell action: %s\n", label);
    printf("Running shell command...\n\n");
    fflush(stdout);

    pipe = popen(wrapped_command, "r");
    if (pipe == NULL) {
        perror("Failed to open shell process");
        return 1;
    }

    while (fgets(line, sizeof(line), pipe) != NULL) {
        fputs(line, stdout);
        fflush(stdout);
    }

    status = pclose(pipe);
    if (status == -1) {
        perror("Failed to close shell process");
        return 1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "\nShell action failed.\n");
        return 1;
    }

    return 0;
}

static void print_version_item(const char *label, const char *value) {
    printf("  %-14s %s\n", label, value);
}

static void print_tool_version(const char *label, const char *command) {
    char *output = run_command_capture(command);

    if (output == NULL) {
        print_version_item(label, "not installed");
        return;
    }

    print_version_item(label, output);
    free(output);
}

static char *to_lower_copy(const char *input) {
    size_t len = strlen(input);
    char *copy = malloc(len + 1);

    if (copy == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < len; ++i) {
        copy[i] = (char)tolower((unsigned char)input[i]);
    }

    copy[len] = '\0';
    return copy;
}

static bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool is_directory_path(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *read_text_file(const char *path) {
    FILE *file = fopen(path, "rb");
    char *buffer;
    long size;

    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = malloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[size] = '\0';
    fclose(file);
    return buffer;
}

static int write_text_file(const char *path, const char *content) {
    FILE *file = fopen(path, "wb");
    size_t len;

    if (file == NULL) {
        return -1;
    }

    len = strlen(content);
    if (fwrite(content, 1, len, file) != len) {
        fclose(file);
        return -1;
    }

    fclose(file);
    return 0;
}

static int copy_file(const char *src_path, const char *dst_path) {
    FILE *src = fopen(src_path, "rb");
    FILE *dst;
    char buffer[8192];
    size_t n;

    if (src == NULL) {
        return -1;
    }

    dst = fopen(dst_path, "wb");
    if (dst == NULL) {
        fclose(src);
        return -1;
    }

    while ((n = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, n, dst) != n) {
            fclose(src);
            fclose(dst);
            return -1;
        }
    }

    if (ferror(src)) {
        fclose(src);
        fclose(dst);
        return -1;
    }

    fclose(src);
    if (fclose(dst) != 0) {
        return -1;
    }
    return 0;
}

static char *extract_quoted_name(const char *input) {
    const char *start = strchr(input, '"');
    const char *end;
    size_t len;
    char *value;

    if (start == NULL) {
        return NULL;
    }

    ++start;
    end = strchr(start, '"');
    if (end == NULL || end <= start) {
        return NULL;
    }

    len = (size_t)(end - start);
    value = malloc(len + 1);
    if (value == NULL) {
        return NULL;
    }

    memcpy(value, start, len);
    value[len] = '\0';
    return value;
}

static char *extract_last_name_token_before(const char *input, const char *marker) {
    const char *pos = strstr(input, marker);
    const char *end;
    const char *start;
    size_t len;
    char *value;

    if (pos == NULL || pos <= input) {
        return NULL;
    }

    end = pos;
    while (end > input && isspace((unsigned char)end[-1])) {
        --end;
    }

    start = end;
    while (start > input) {
        unsigned char ch = (unsigned char)start[-1];
        if (isalnum(ch) || ch == '-' || ch == '_') {
            --start;
            continue;
        }
        break;
    }

    if (start == end) {
        return NULL;
    }

    len = (size_t)(end - start);
    value = malloc(len + 1);
    if (value == NULL) {
        return NULL;
    }

    memcpy(value, start, len);
    value[len] = '\0';
    return value;
}

static char *extract_name_after_keyword(const char *input, const char *keyword) {
    const char *start = strstr(input, keyword);
    const char *end;
    size_t len;
    char *value;

    if (start == NULL) {
        return NULL;
    }

    start += strlen(keyword);
    while (*start != '\0' && isspace((unsigned char)*start)) {
        ++start;
    }

    end = start;
    while (*end != '\0') {
        unsigned char ch = (unsigned char)*end;
        if (isalnum(ch) || ch == '-' || ch == '_') {
            ++end;
            continue;
        }
        break;
    }

    if (end == start) {
        return NULL;
    }

    len = (size_t)(end - start);
    value = malloc(len + 1);
    if (value == NULL) {
        return NULL;
    }

    memcpy(value, start, len);
    value[len] = '\0';
    return value;
}

static char *sanitize_project_name(const char *input) {
    size_t len = strlen(input);
    char *name = malloc((len * 2) + 1);
    size_t used = 0;
    bool previous_dash = false;

    if (name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)input[i];

        if (isalnum(ch)) {
            name[used++] = (char)tolower(ch);
            previous_dash = false;
        } else if (ch == '-' || ch == '_' || isspace(ch)) {
            if (!previous_dash && used > 0) {
                name[used++] = '-';
                previous_dash = true;
            }
        }
    }

    while (used > 0 && name[used - 1] == '-') {
        --used;
    }

    if (used == 0) {
        strcpy(name, "solana-starter");
        return name;
    }

    name[used] = '\0';
    return name;
}

static char *make_unique_project_name(const char *base_name) {
    char *candidate = NULL;
    size_t used = 0;
    int suffix = 1;

    if (append_format(&candidate, &used, "%s", base_name) != 0) {
        return NULL;
    }

    while (path_exists(candidate)) {
        free(candidate);
        candidate = NULL;
        used = 0;
        ++suffix;
        if (append_format(&candidate, &used, "%s-%d", base_name, suffix) != 0) {
            return NULL;
        }
    }

    return candidate;
}

static char *resolve_project_name_from_request(const char *input) {
    char *quoted = extract_quoted_name(input);
    char *named = NULL;
    char *before_named = NULL;
    char *sanitized;
    char *unique;

    if (quoted == NULL) {
        named = extract_name_after_keyword(input, "named ");
    }

    if (quoted == NULL && named == NULL) {
        before_named = extract_last_name_token_before(input, " adinda");
    }

    if (quoted == NULL && named == NULL && before_named == NULL) {
        before_named = extract_last_name_token_before(input, " adında");
    }

    if (quoted != NULL) {
        sanitized = sanitize_project_name(quoted);
    } else if (named != NULL) {
        sanitized = sanitize_project_name(named);
    } else if (before_named != NULL) {
        sanitized = sanitize_project_name(before_named);
    } else {
        sanitized = sanitize_project_name("solana-starter");
    }

    free(quoted);
    free(named);
    free(before_named);

    if (sanitized == NULL) {
        return NULL;
    }

    unique = make_unique_project_name(sanitized);
    free(sanitized);
    return unique;
}

static bool refers_to_current_project(const char *input) {
    char *lower = to_lower_copy(input);
    bool result;

    if (lower == NULL) {
        return false;
    }

    result =
        strstr(lower, "bu proj") != NULL ||
        strstr(lower, "current project") != NULL ||
        strstr(lower, "this project") != NULL ||
        strstr(lower, "oluşturduğumuz proj") != NULL ||
        strstr(lower, "olusturdugumuz proj") != NULL ||
        strstr(lower, "guncel proje") != NULL ||
        strstr(lower, "güncel proje") != NULL;

    free(lower);
    return result;
}

static char *resolve_target_project_name_from_request(const char *input, const AgentState *state) {
    char *quoted = extract_quoted_name(input);
    char *named = NULL;
    char *before_to = NULL;
    char *sanitized;

    if (state != NULL && state->last_project_name != NULL && refers_to_current_project(input)) {
        return strdup(state->last_project_name);
    }

    if (quoted == NULL) {
        named = extract_name_after_keyword(input, "named ");
    }

    if (quoted == NULL && named == NULL) {
        before_to = extract_last_name_token_before(input, " projesine");
    }

    if (quoted == NULL && named == NULL && before_to == NULL) {
        before_to = extract_last_name_token_before(input, " projeye");
    }

    if (quoted == NULL && named == NULL && before_to == NULL) {
        before_to = extract_last_name_token_before(input, " project");
    }

    if (quoted != NULL) {
        sanitized = sanitize_project_name(quoted);
    } else if (named != NULL) {
        sanitized = sanitize_project_name(named);
    } else if (before_to != NULL) {
        sanitized = sanitize_project_name(before_to);
    } else {
        sanitized = NULL;
    }

    free(quoted);
    free(named);
    free(before_to);
    return sanitized;
}

static bool is_project_creation_request(const char *input) {
    char *lower = to_lower_copy(input);
    bool mentions_project;
    bool mentions_creation;
    bool mentions_turkish_creation;
    bool generic_project_creation;
    bool result;

    if (lower == NULL) {
        return false;
    }

    mentions_project =
        strstr(lower, "solana project") != NULL ||
        strstr(lower, "anchor project") != NULL ||
        strstr(lower, "solana proje") != NULL ||
        strstr(lower, "anchor proje") != NULL ||
        strstr(lower, "solana starter") != NULL ||
        strstr(lower, "anchor starter") != NULL;

    mentions_creation =
        strstr(lower, "create") != NULL ||
        strstr(lower, "scaffold") != NULL ||
        strstr(lower, "generate") != NULL ||
        strstr(lower, "bootstrap") != NULL ||
        strstr(lower, "make ") != NULL ||
        strstr(lower, "build me") != NULL ||
        strstr(lower, "start ") != NULL ||
        strstr(lower, "olustur") != NULL ||
        strstr(lower, "hazirla") != NULL ||
        strstr(lower, "kur") != NULL ||
        strstr(lower, "temel") != NULL ||
        strstr(lower, "basic") != NULL ||
        strstr(lower, "starter") != NULL;

    mentions_turkish_creation =
        strstr(input, "oluştur") != NULL ||
        strstr(input, "oluşturu") != NULL ||
        strstr(input, "projesi oluştur") != NULL ||
        strstr(input, "proje oluştur") != NULL;

    generic_project_creation =
        strstr(lower, "bir proje") != NULL ||
        strstr(lower, "new project") != NULL ||
        strstr(lower, "create project") != NULL ||
        strstr(lower, "create a project") != NULL ||
        strstr(lower, "create me a project") != NULL ||
        strstr(lower, "proje olustur") != NULL ||
        strstr(lower, "proje oluştur") != NULL;

    result = (mentions_project && (mentions_creation || mentions_turkish_creation)) || generic_project_creation;
    free(lower);
    return result;
}

static bool is_bank_contract_request(const char *input) {
    char *lower = to_lower_copy(input);
    bool mentions_project;
    bool mentions_contract;
    bool mentions_bank;
    bool mentions_write;
    bool result;

    if (lower == NULL) {
        return false;
    }

    mentions_project =
        strstr(lower, "projesine") != NULL ||
        strstr(lower, "projeye") != NULL ||
        strstr(lower, "bu proje") != NULL ||
        strstr(lower, "this project") != NULL ||
        strstr(lower, "current project") != NULL ||
        strstr(lower, "olusturdugumuz proje") != NULL ||
        strstr(lower, "oluşturduğumuz proje") != NULL ||
        strstr(lower, "project") != NULL ||
        strstr(lower, "workspace") != NULL;

    mentions_contract =
        strstr(lower, "kontrat") != NULL ||
        strstr(lower, "contract") != NULL ||
        strstr(lower, "smart contract") != NULL;

    mentions_bank =
        strstr(lower, "banka") != NULL ||
        strstr(lower, "bank") != NULL;

    mentions_write =
        strstr(lower, "yaz") != NULL ||
        strstr(lower, "write") != NULL ||
        strstr(lower, "ekle") != NULL ||
        strstr(lower, "implement") != NULL ||
        strstr(lower, "uygula") != NULL;

    result = mentions_project && mentions_contract && mentions_bank && mentions_write;
    free(lower);
    return result;
}

static char *extract_declare_id_value(const char *source) {
    const char *macro = strstr(source, "declare_id!(\"");
    const char *start;
    const char *end;
    size_t len;
    char *value;

    if (macro == NULL) {
        return NULL;
    }

    start = macro + strlen("declare_id!(\"");
    end = strchr(start, '"');
    if (end == NULL || end <= start) {
        return NULL;
    }

    len = (size_t)(end - start);
    value = malloc(len + 1);
    if (value == NULL) {
        return NULL;
    }

    memcpy(value, start, len);
    value[len] = '\0';
    return value;
}

static char *make_rust_identifier(const char *name) {
    size_t len = strlen(name);
    char *identifier = malloc(len + 2);
    size_t used = 0;

    if (identifier == NULL) {
        return NULL;
    }

    if (len == 0 || (!isalpha((unsigned char)name[0]) && name[0] != '_')) {
        identifier[used++] = '_';
    }

    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)name[i];

        if (isalnum(ch) || ch == '_') {
            identifier[used++] = (char)tolower(ch);
        } else if (ch == '-') {
            identifier[used++] = '_';
        }
    }

    identifier[used] = '\0';
    return identifier;
}

static int scaffold_basic_solana_project(const char *request, AgentState *state) {
    char *anchor_version;
    char *project_name;
    char *command = NULL;
    size_t used = 0;

    anchor_version = run_command_capture("bash -lc 'export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\"; anchor --version 2>/dev/null'");
    if (anchor_version == NULL) {
        fprintf(stderr, "Anchor CLI is not available.\n");
        fprintf(stderr, "Run `solcli download` first, then try again.\n");
        return 1;
    }
    free(anchor_version);

    project_name = resolve_project_name_from_request(request);
    if (project_name == NULL) {
        fprintf(stderr, "Failed to resolve project name.\n");
        return 1;
    }

    printf("Agent action detected: create a basic Solana project.\n");
    printf("Using project name: %s\n\n", project_name);

    if (append_format(
            &command,
            &used,
            "export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\" && "
            "anchor init \"%s\" --no-install --test-template rust && "
            "cd \"%s\" && anchor build",
            project_name,
            project_name) != 0) {
        free(project_name);
        fprintf(stderr, "Failed to build scaffold command.\n");
        return 1;
    }

    if (run_shell_command_streaming("create project scaffold", command) != 0) {
        free(project_name);
        free(command);
        fprintf(stderr, "\nAgent action failed while creating the project.\n");
        return 1;
    }
    free(command);

    printf("\nProject created successfully: ./%s\n", project_name);
    printf("Next steps:\n");
    printf("  cd %s\n", project_name);
    printf("  anchor test\n");
    if (state != NULL) {
        set_agent_state_value(&state->last_project_name, project_name);
        set_agent_state_value(&state->last_action_summary, "Created a basic Solana/Anchor project in the current workspace.");
    }
    free(project_name);
    return 0;
}

static int write_bank_contract_to_project(const char *request, AgentState *state) {
    char *project_name = resolve_target_project_name_from_request(request, state);
    char *module_name = NULL;
    char *lib_path = NULL;
    char *project_root = NULL;
    char *existing_source = NULL;
    char *declare_id = NULL;
    char *new_source = NULL;
    char *build_command = NULL;
    size_t used = 0;

    if (project_name == NULL) {
        fprintf(stderr, "Could not determine the target project name from the request.\n");
        return 1;
    }

    module_name = make_rust_identifier(project_name);
    if (module_name == NULL) {
        free(project_name);
        fprintf(stderr, "Failed to prepare the Rust module name.\n");
        return 1;
    }

    if (append_format(&project_root, &used, "%s", project_name) != 0) {
        free(project_name);
        free(module_name);
        fprintf(stderr, "Failed to prepare the project path.\n");
        return 1;
    }

    used = 0;
    if (append_format(&lib_path, &used, "%s/programs/%s/src/lib.rs", project_name, project_name) != 0) {
        free(project_name);
        free(module_name);
        free(project_root);
        fprintf(stderr, "Failed to prepare the contract path.\n");
        return 1;
    }

    if (!path_exists(project_root) || !path_exists(lib_path)) {
        fprintf(stderr, "Target project not found: %s\n", project_name);
        fprintf(stderr, "Create the project first, then run the agent action again.\n");
        free(project_name);
        free(module_name);
        free(project_root);
        free(lib_path);
        return 1;
    }

    existing_source = read_text_file(lib_path);
    if (existing_source == NULL) {
        fprintf(stderr, "Failed to read the existing contract file.\n");
        free(project_name);
        free(module_name);
        free(project_root);
        free(lib_path);
        return 1;
    }

    declare_id = extract_declare_id_value(existing_source);
    if (declare_id == NULL) {
        fprintf(stderr, "Failed to extract declare_id from the existing contract.\n");
        free(project_name);
        free(module_name);
        free(project_root);
        free(lib_path);
        free(existing_source);
        return 1;
    }

    used = 0;
    if (append_format(
            &new_source,
            &used,
            "use anchor_lang::prelude::*;\n"
            "\n"
            "declare_id!(\"%s\");\n"
            "\n"
            "#[program]\n"
            "pub mod %s {\n"
            "    use super::*;\n"
            "\n"
            "    pub fn initialize_bank(ctx: Context<InitializeBank>) -> Result<()> {\n"
            "        let bank = &mut ctx.accounts.bank;\n"
            "        bank.authority = ctx.accounts.authority.key();\n"
            "        bank.total_deposits = 0;\n"
            "        bank.bump = ctx.bumps.bank;\n"
            "        Ok(())\n"
            "    }\n"
            "\n"
            "    pub fn open_account(ctx: Context<OpenAccount>) -> Result<()> {\n"
            "        let account = &mut ctx.accounts.bank_account;\n"
            "        account.owner = ctx.accounts.owner.key();\n"
            "        account.balance = 0;\n"
            "        account.bump = ctx.bumps.bank_account;\n"
            "        Ok(())\n"
            "    }\n"
            "\n"
            "    pub fn deposit(ctx: Context<UpdateBankAccount>, amount: u64) -> Result<()> {\n"
            "        require!(amount > 0, BankError::InvalidAmount);\n"
            "\n"
            "        let bank = &mut ctx.accounts.bank;\n"
            "        let account = &mut ctx.accounts.bank_account;\n"
            "\n"
            "        account.balance = account\n"
            "            .balance\n"
            "            .checked_add(amount)\n"
            "            .ok_or(BankError::MathOverflow)?;\n"
            "        bank.total_deposits = bank\n"
            "            .total_deposits\n"
            "            .checked_add(amount)\n"
            "            .ok_or(BankError::MathOverflow)?;\n"
            "        Ok(())\n"
            "    }\n"
            "\n"
            "    pub fn withdraw(ctx: Context<UpdateBankAccount>, amount: u64) -> Result<()> {\n"
            "        require!(amount > 0, BankError::InvalidAmount);\n"
            "\n"
            "        let bank = &mut ctx.accounts.bank;\n"
            "        let account = &mut ctx.accounts.bank_account;\n"
            "\n"
            "        require!(account.balance >= amount, BankError::InsufficientFunds);\n"
            "\n"
            "        account.balance = account\n"
            "            .balance\n"
            "            .checked_sub(amount)\n"
            "            .ok_or(BankError::MathOverflow)?;\n"
            "        bank.total_deposits = bank\n"
            "            .total_deposits\n"
            "            .checked_sub(amount)\n"
            "            .ok_or(BankError::MathOverflow)?;\n"
            "        Ok(())\n"
            "    }\n"
            "}\n"
            "\n"
            "#[account]\n"
            "pub struct BankState {\n"
            "    pub authority: Pubkey,\n"
            "    pub total_deposits: u64,\n"
            "    pub bump: u8,\n"
            "}\n"
            "\n"
            "impl BankState {\n"
            "    pub const SPACE: usize = 8 + 32 + 8 + 1;\n"
            "}\n"
            "\n"
            "#[account]\n"
            "pub struct BankAccount {\n"
            "    pub owner: Pubkey,\n"
            "    pub balance: u64,\n"
            "    pub bump: u8,\n"
            "}\n"
            "\n"
            "impl BankAccount {\n"
            "    pub const SPACE: usize = 8 + 32 + 8 + 1;\n"
            "}\n"
            "\n"
            "#[derive(Accounts)]\n"
            "pub struct InitializeBank<'info> {\n"
            "    #[account(\n"
            "        init,\n"
            "        payer = authority,\n"
            "        space = BankState::SPACE,\n"
            "        seeds = [b\"bank-state\"],\n"
            "        bump\n"
            "    )]\n"
            "    pub bank: Account<'info, BankState>,\n"
            "    #[account(mut)]\n"
            "    pub authority: Signer<'info>,\n"
            "    pub system_program: Program<'info, System>,\n"
            "}\n"
            "\n"
            "#[derive(Accounts)]\n"
            "pub struct OpenAccount<'info> {\n"
            "    #[account(\n"
            "        mut,\n"
            "        seeds = [b\"bank-state\"],\n"
            "        bump = bank.bump,\n"
            "        has_one = authority\n"
            "    )]\n"
            "    pub bank: Account<'info, BankState>,\n"
            "    #[account(\n"
            "        init,\n"
            "        payer = owner,\n"
            "        space = BankAccount::SPACE,\n"
            "        seeds = [b\"bank-account\", owner.key().as_ref()],\n"
            "        bump\n"
            "    )]\n"
            "    pub bank_account: Account<'info, BankAccount>,\n"
            "    pub authority: Signer<'info>,\n"
            "    #[account(mut)]\n"
            "    pub owner: Signer<'info>,\n"
            "    pub system_program: Program<'info, System>,\n"
            "}\n"
            "\n"
            "#[derive(Accounts)]\n"
            "pub struct UpdateBankAccount<'info> {\n"
            "    #[account(\n"
            "        mut,\n"
            "        seeds = [b\"bank-state\"],\n"
            "        bump = bank.bump\n"
            "    )]\n"
            "    pub bank: Account<'info, BankState>,\n"
            "    #[account(\n"
            "        mut,\n"
            "        seeds = [b\"bank-account\", owner.key().as_ref()],\n"
            "        bump = bank_account.bump,\n"
            "        has_one = owner\n"
            "    )]\n"
            "    pub bank_account: Account<'info, BankAccount>,\n"
            "    pub owner: Signer<'info>,\n"
            "}\n"
            "\n"
            "#[error_code]\n"
            "pub enum BankError {\n"
            "    #[msg(\"The provided amount must be greater than zero.\")]\n"
            "    InvalidAmount,\n"
            "    #[msg(\"Insufficient funds in the bank account.\")]\n"
            "    InsufficientFunds,\n"
            "    #[msg(\"Arithmetic overflow or underflow occurred.\")]\n"
            "    MathOverflow,\n"
            "}\n",
            declare_id,
            module_name) != 0) {
        fprintf(stderr, "Failed to generate the bank contract source.\n");
        free(project_name);
        free(module_name);
        free(project_root);
        free(lib_path);
        free(existing_source);
        free(declare_id);
        free(new_source);
        return 1;
    }

    if (write_text_file(lib_path, new_source) != 0) {
        fprintf(stderr, "Failed to write the contract file.\n");
        free(project_name);
        free(module_name);
        free(project_root);
        free(lib_path);
        free(existing_source);
        free(declare_id);
        free(new_source);
        return 1;
    }

    printf("Agent action detected: write a bank-style contract into an existing project.\n");
    printf("Target project: %s\n", project_name);
    printf("Updated file: %s\n\n", lib_path);

    used = 0;
    if (append_format(
            &build_command,
            &used,
            "export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\" && "
            "cd \"%s\" && anchor build",
            project_root) != 0) {
        fprintf(stderr, "Failed to build the validation command.\n");
        free(project_name);
        free(module_name);
        free(project_root);
        free(lib_path);
        free(existing_source);
        free(declare_id);
        free(new_source);
        return 1;
    }

    if (run_shell_command_streaming("validate updated contract", build_command) != 0) {
        fprintf(stderr, "\nAgent action failed while validating the updated contract.\n");
        free(project_name);
        free(module_name);
        free(project_root);
        free(lib_path);
        free(existing_source);
        free(declare_id);
        free(new_source);
        free(build_command);
        return 1;
    }

    printf("\nContract update completed successfully.\n");
    printf("Next steps:\n");
    printf("  cd %s\n", project_name);
    printf("  anchor test\n");
    if (state != NULL) {
        set_agent_state_value(&state->last_project_name, project_name);
        set_agent_state_value(&state->last_action_summary, "Updated the target project with a bank-style Anchor contract and validated it with anchor build.");
    }

    free(project_name);
    free(module_name);
    free(project_root);
    free(lib_path);
    free(existing_source);
    free(declare_id);
    free(new_source);
    free(build_command);
    return 0;
}

static bool is_apply_follow_up_request(const char *input) {
    char *lower = to_lower_copy(input);
    bool result;

    if (lower == NULL) {
        return false;
    }

    result =
        strstr(lower, "apply it") != NULL ||
        strstr(lower, "do it") != NULL ||
        strstr(lower, "go ahead") != NULL ||
        strstr(lower, "continue") != NULL ||
        strstr(lower, "use that") != NULL ||
        strstr(lower, "evet") != NULL ||
        strstr(lower, "uygula") != NULL ||
        strstr(lower, "devam") != NULL ||
        strstr(lower, "bunu yap") != NULL;

    free(lower);
    return result;
}

static void set_last_actionable_request(AgentState *state, const char *input) {
    char *copy;

    if (state == NULL || input == NULL) {
        return;
    }

    copy = strdup(input);
    if (copy == NULL) {
        return;
    }

    free(state->last_actionable_request);
    state->last_actionable_request = copy;
}

static void clear_last_actionable_request(AgentState *state) {
    if (state == NULL) {
        return;
    }

    free(state->last_actionable_request);
    state->last_actionable_request = NULL;
}

static void set_agent_state_value(char **slot, const char *value) {
    char *copy;

    if (slot == NULL) {
        return;
    }

    if (value == NULL) {
        free(*slot);
        *slot = NULL;
        return;
    }

    copy = strdup(value);
    if (copy == NULL) {
        return;
    }

    free(*slot);
    *slot = copy;
}

static void clear_agent_state(AgentState *state) {
    if (state == NULL) {
        return;
    }

    clear_last_actionable_request(state);
    set_agent_state_value(&state->last_project_name, NULL);
    set_agent_state_value(&state->last_action_summary, NULL);
}

static int execute_agent_request(const char *input, AgentState *state) {
    if (is_bank_contract_request(input)) {
        return write_bank_contract_to_project(input, state);
    }

    if (is_project_creation_request(input)) {
        return scaffold_basic_solana_project(input, state);
    }

    return -1;
}

static bool is_action_summary_request(const char *input) {
    char *lower = to_lower_copy(input);
    bool result;

    if (lower == NULL) {
        return false;
    }

    result =
        strstr(lower, "which project") != NULL ||
        strstr(lower, "what project") != NULL ||
        strstr(lower, "what did you update") != NULL ||
        strstr(lower, "what did you create") != NULL ||
        strstr(lower, "hangi proj") != NULL ||
        strstr(lower, "neyi guncelledin") != NULL ||
        strstr(lower, "neyi güncelledin") != NULL ||
        strstr(lower, "ne olusturdun") != NULL ||
        strstr(lower, "ne olu") != NULL;

    free(lower);
    return result;
}

static int handle_agent_local_action(const char *input, ChatHistory *history, AgentState *state) {
    int direct_result = execute_agent_request(input, state);

    if (direct_result != -1) {
        set_last_actionable_request(state, input);
        if (direct_result == 0) {
            add_chat_message(history, "user", input);
            if (state != NULL && state->last_project_name != NULL && state->last_action_summary != NULL) {
                char *summary = NULL;
                size_t used = 0;
                if (append_format(&summary, &used, "%s Target project: %s.", state->last_action_summary, state->last_project_name) == 0) {
                    add_chat_message(history, "assistant", summary);
                }
                free(summary);
            } else {
                add_chat_message(history, "assistant", "Executed a local agent action in the current workspace.");
            }
            clear_last_actionable_request(state);
            return 0;
        }
        return 1;
    }

    if (state != NULL && state->last_project_name != NULL && is_action_summary_request(input)) {
        printf("Last updated project: %s\n", state->last_project_name);
        if (state->last_action_summary != NULL) {
            printf("%s\n", state->last_action_summary);
        }
        printf("\n");
        add_chat_message(history, "user", input);
        if (state->last_action_summary != NULL) {
            char *summary = NULL;
            size_t used = 0;
            if (append_format(&summary, &used, "Last updated project: %s. %s", state->last_project_name, state->last_action_summary) == 0) {
                add_chat_message(history, "assistant", summary);
            }
            free(summary);
        } else {
            add_chat_message(history, "assistant", state->last_project_name);
        }
        return 0;
    }

    if (state != NULL && state->last_actionable_request != NULL && is_apply_follow_up_request(input)) {
        if (execute_agent_request(state->last_actionable_request, state) == 0) {
            add_chat_message(history, "user", input);
            add_chat_message(history, "assistant", "Executed the previously requested local action in the current workspace.");
            clear_last_actionable_request(state);
            return 0;
        }
        return 1;
    }

    return -1;
}

static char *read_project_name_interactively(void) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    char *sanitized;
    char *unique;

    printf("Project name: ");
    fflush(stdout);

    len = getline(&line, &cap, stdin);
    if (len < 0) {
        free(line);
        return NULL;
    }

    while (len > 0 && isspace((unsigned char)line[len - 1])) {
        line[--len] = '\0';
    }

    if (len == 0) {
        free(line);
        fprintf(stderr, "Project name cannot be empty.\n");
        return NULL;
    }

    sanitized = sanitize_project_name(line);
    free(line);
    if (sanitized == NULL) {
        return NULL;
    }

    unique = make_unique_project_name(sanitized);
    free(sanitized);
    return unique;
}

static int scaffold_anchor_template(const char *name) {
    char *command = NULL;
    size_t used = 0;

    if (append_format(
            &command,
            &used,
            "export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\" && "
            "anchor init \"%s\" --no-install --test-template rust && "
            "cd \"%s\" && anchor build",
            name, name) != 0) {
        fprintf(stderr, "Failed to build scaffold command.\n");
        return 1;
    }

    if (run_shell_command_streaming("create anchor project", command) != 0) {
        free(command);
        return 1;
    }

    free(command);
    printf("\nAnchor project created: ./%s\n", name);
    printf("  cd %s\n  anchor test\n", name);
    return 0;
}

static int scaffold_native_template(const char *name) {
    char *rs_path = NULL;
    char *toml_path = NULL;
    char *src_dir = NULL;
    size_t used = 0;
    char *rust_id = make_rust_identifier(name);
    int ret = 1;

    if (rust_id == NULL) {
        return 1;
    }

    if (path_exists(name)) {
        fprintf(stderr, "Directory already exists: %s\n", name);
        free(rust_id);
        return 1;
    }

    if (append_format(&src_dir, &used, "%s/src", name) != 0) goto cleanup;

    {
        char mkdir_cmd[1024];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s/src\"", name);
        if (system(mkdir_cmd) != 0) {
            fprintf(stderr, "Failed to create project directories.\n");
            goto cleanup;
        }
    }

    used = 0;
    if (append_format(&toml_path, &used, "%s/Cargo.toml", name) != 0) goto cleanup;

    {
        char *toml_content = NULL;
        size_t tc = 0;
        if (append_format(
                &toml_content, &tc,
                "[package]\n"
                "name = \"%s\"\n"
                "version = \"0.1.0\"\n"
                "edition = \"2021\"\n"
                "\n"
                "[lib]\n"
                "crate-type = [\"cdylib\", \"lib\"]\n"
                "\n"
                "[dependencies]\n"
                "solana-program = \"2.1\"\n"
                "\n"
                "[dev-dependencies]\n"
                "solana-program-test = \"2.1\"\n"
                "solana-sdk = \"2.1\"\n"
                "tokio = { version = \"1\", features = [\"full\"] }\n",
                rust_id) != 0) {
            free(toml_content);
            goto cleanup;
        }
        if (write_text_file(toml_path, toml_content) != 0) {
            fprintf(stderr, "Failed to write Cargo.toml.\n");
            free(toml_content);
            goto cleanup;
        }
        free(toml_content);
    }

    used = 0;
    if (append_format(&rs_path, &used, "%s/src/lib.rs", name) != 0) goto cleanup;

    {
        char *rs_content = NULL;
        size_t rc = 0;
        if (append_format(
                &rs_content, &rc,
                "use solana_program::{\n"
                "    account_info::{next_account_info, AccountInfo},\n"
                "    entrypoint,\n"
                "    entrypoint::ProgramResult,\n"
                "    msg,\n"
                "    pubkey::Pubkey,\n"
                "};\n"
                "\n"
                "entrypoint!(process_instruction);\n"
                "\n"
                "pub fn process_instruction(\n"
                "    program_id: &Pubkey,\n"
                "    accounts: &[AccountInfo],\n"
                "    instruction_data: &[u8],\n"
                ") -> ProgramResult {\n"
                "    msg!(\"Hello from %s!\");\n"
                "    let _accounts = accounts;\n"
                "    let _ = (program_id, instruction_data);\n"
                "    Ok(())\n"
                "}\n",
                rust_id) != 0) {
            free(rs_content);
            goto cleanup;
        }
        if (write_text_file(rs_path, rs_content) != 0) {
            fprintf(stderr, "Failed to write src/lib.rs.\n");
            free(rs_content);
            goto cleanup;
        }
        free(rs_content);
    }

    printf("\nNative Solana program created: ./%s\n", name);
    printf("  cd %s\n  cargo build-sbf\n", name);
    ret = 0;

cleanup:
    free(rust_id);
    free(src_dir);
    free(toml_path);
    free(rs_path);
    return ret;
}

static int add_idl_build_dep(const char *toml_path, const char *extra_dep) {
    char *content = read_text_file(toml_path);
    const char *marker = "idl-build = [\"anchor-lang/idl-build\"]";
    char *pos;
    char *new_content = NULL;
    size_t used = 0;
    int ret;

    if (content == NULL) {
        return -1;
    }

    pos = strstr(content, marker);
    if (pos == NULL) {
        free(content);
        return 0;
    }

    {
        size_t prefix_len = (size_t)(pos - content);
        const char *suffix = pos + strlen(marker);

        if (append_format(&new_content, &used, "%.*s", (int)prefix_len, content) != 0 ||
            append_format(&new_content, &used,
                "idl-build = [\"anchor-lang/idl-build\", \"%s\"]", extra_dep) != 0 ||
            append_format(&new_content, &used, "%s", suffix) != 0) {
            free(content);
            free(new_content);
            return -1;
        }
    }

    ret = write_text_file(toml_path, new_content);
    free(content);
    free(new_content);
    return ret;
}

static int append_cargo_dep(const char *toml_path, const char *dep) {
    char *content = read_text_file(toml_path);
    char *new_content = NULL;
    size_t used = 0;
    int ret;

    if (content == NULL) {
        fprintf(stderr, "Failed to read: %s\n", toml_path);
        return -1;
    }

    if (append_format(&new_content, &used, "%s\n%s\n", content, dep) != 0) {
        free(content);
        return -1;
    }

    ret = write_text_file(toml_path, new_content);
    free(content);
    free(new_content);
    return ret;
}

static char *extract_dep_version(const char *content, const char *dep_name) {
    char marker[256];
    const char *start;
    const char *end;
    size_t len;
    char *version;

    if (snprintf(marker, sizeof(marker), "%s = \"", dep_name) >= (int)sizeof(marker)) {
        return NULL;
    }

    start = strstr(content, marker);
    if (start == NULL) {
        return NULL;
    }

    start += strlen(marker);
    end = strchr(start, '"');
    if (end == NULL || end <= start) {
        return NULL;
    }

    len = (size_t)(end - start);
    version = malloc(len + 1);
    if (version == NULL) {
        return NULL;
    }

    memcpy(version, start, len);
    version[len] = '\0';
    return version;
}

static int scaffold_token_template(const char *name) {
    char *command = NULL;
    char *cargo_path = NULL;
    char *lib_path = NULL;
    char *existing_toml = NULL;
    char *anchor_ver = NULL;
    char *existing_lib = NULL;
    char *declare_id = NULL;
    char *new_source = NULL;
    char *rust_id = NULL;
    char *build_command = NULL;
    char dep_line[256];
    size_t used = 0;
    int ret = 1;

    if (append_format(
            &command,
            &used,
            "export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\" && "
            "anchor init \"%s\" --no-install --test-template rust",
            name) != 0) {
        fprintf(stderr, "Failed to build init command.\n");
        goto cleanup;
    }

    if (run_shell_command_streaming("create token project scaffold", command) != 0) {
        goto cleanup;
    }

    used = 0;
    if (append_format(&cargo_path, &used, "%s/programs/%s/Cargo.toml", name, name) != 0) goto cleanup;
    used = 0;
    if (append_format(&lib_path, &used, "%s/programs/%s/src/lib.rs", name, name) != 0) goto cleanup;

    existing_toml = read_text_file(cargo_path);
    if (existing_toml == NULL) {
        fprintf(stderr, "Failed to read generated Cargo.toml.\n");
        goto cleanup;
    }

    anchor_ver = extract_dep_version(existing_toml, "anchor-lang");
    snprintf(dep_line, sizeof(dep_line),
        "anchor-spl = { version = \"%s\", features = [\"token\"] }",
        anchor_ver ? anchor_ver : "0.31.1");

    if (append_cargo_dep(cargo_path, dep_line) != 0) {
        fprintf(stderr, "Failed to add anchor-spl dependency.\n");
        goto cleanup;
    }

    if (add_idl_build_dep(cargo_path, "anchor-spl/idl-build") != 0) {
        fprintf(stderr, "Failed to update idl-build feature.\n");
        goto cleanup;
    }

    existing_lib = read_text_file(lib_path);
    if (existing_lib == NULL) {
        fprintf(stderr, "Failed to read generated lib.rs.\n");
        goto cleanup;
    }

    declare_id = extract_declare_id_value(existing_lib);
    if (declare_id == NULL) {
        fprintf(stderr, "Failed to extract declare_id.\n");
        goto cleanup;
    }

    rust_id = make_rust_identifier(name);
    if (rust_id == NULL) goto cleanup;

    used = 0;
    if (append_format(
            &new_source,
            &used,
            "use anchor_lang::prelude::*;\n"
            "use anchor_spl::token::{self, Mint, MintTo, Token, TokenAccount, Transfer};\n"
            "\n"
            "declare_id!(\"%s\");\n"
            "\n"
            "#[program]\n"
            "pub mod %s {\n"
            "    use super::*;\n"
            "\n"
            "    pub fn create_mint(ctx: Context<CreateMint>, decimals: u8) -> Result<()> {\n"
            "        msg!(\"Mint created with {} decimals\", decimals);\n"
            "        Ok(())\n"
            "    }\n"
            "\n"
            "    pub fn mint_tokens(ctx: Context<MintTokens>, amount: u64) -> Result<()> {\n"
            "        token::mint_to(\n"
            "            CpiContext::new(\n"
            "                ctx.accounts.token_program.to_account_info(),\n"
            "                MintTo {\n"
            "                    mint: ctx.accounts.mint.to_account_info(),\n"
            "                    to: ctx.accounts.destination.to_account_info(),\n"
            "                    authority: ctx.accounts.authority.to_account_info(),\n"
            "                },\n"
            "            ),\n"
            "            amount,\n"
            "        )?;\n"
            "        Ok(())\n"
            "    }\n"
            "\n"
            "    pub fn transfer_tokens(ctx: Context<TransferTokens>, amount: u64) -> Result<()> {\n"
            "        token::transfer(\n"
            "            CpiContext::new(\n"
            "                ctx.accounts.token_program.to_account_info(),\n"
            "                Transfer {\n"
            "                    from: ctx.accounts.from.to_account_info(),\n"
            "                    to: ctx.accounts.to.to_account_info(),\n"
            "                    authority: ctx.accounts.authority.to_account_info(),\n"
            "                },\n"
            "            ),\n"
            "            amount,\n"
            "        )?;\n"
            "        Ok(())\n"
            "    }\n"
            "}\n"
            "\n"
            "#[derive(Accounts)]\n"
            "#[instruction(decimals: u8)]\n"
            "pub struct CreateMint<'info> {\n"
            "    #[account(\n"
            "        init,\n"
            "        payer = authority,\n"
            "        mint::decimals = decimals,\n"
            "        mint::authority = authority,\n"
            "    )]\n"
            "    pub mint: Account<'info, Mint>,\n"
            "    #[account(mut)]\n"
            "    pub authority: Signer<'info>,\n"
            "    pub token_program: Program<'info, Token>,\n"
            "    pub system_program: Program<'info, System>,\n"
            "    pub rent: Sysvar<'info, Rent>,\n"
            "}\n"
            "\n"
            "#[derive(Accounts)]\n"
            "pub struct MintTokens<'info> {\n"
            "    #[account(mut)]\n"
            "    pub mint: Account<'info, Mint>,\n"
            "    #[account(mut)]\n"
            "    pub destination: Account<'info, TokenAccount>,\n"
            "    pub authority: Signer<'info>,\n"
            "    pub token_program: Program<'info, Token>,\n"
            "}\n"
            "\n"
            "#[derive(Accounts)]\n"
            "pub struct TransferTokens<'info> {\n"
            "    #[account(mut)]\n"
            "    pub from: Account<'info, TokenAccount>,\n"
            "    #[account(mut)]\n"
            "    pub to: Account<'info, TokenAccount>,\n"
            "    pub authority: Signer<'info>,\n"
            "    pub token_program: Program<'info, Token>,\n"
            "}\n",
            declare_id, rust_id) != 0) {
        goto cleanup;
    }

    if (write_text_file(lib_path, new_source) != 0) {
        fprintf(stderr, "Failed to write token lib.rs.\n");
        goto cleanup;
    }

    used = 0;
    if (append_format(
            &build_command,
            &used,
            "export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\" && "
            "cd \"%s\" && anchor build",
            name) != 0) goto cleanup;

    if (run_shell_command_streaming("build token project", build_command) != 0) {
        goto cleanup;
    }

    printf("\nSPL Token project created: ./%s\n", name);
    printf("  cd %s\n  anchor test\n", name);
    ret = 0;

cleanup:
    free(command);
    free(cargo_path);
    free(lib_path);
    free(existing_toml);
    free(anchor_ver);
    free(existing_lib);
    free(declare_id);
    free(new_source);
    free(rust_id);
    free(build_command);
    return ret;
}

static int scaffold_nft_template(const char *name) {
    char *command = NULL;
    char *cargo_path = NULL;
    char *lib_path = NULL;
    char *existing_toml = NULL;
    char *anchor_ver = NULL;
    char *existing_lib = NULL;
    char *declare_id = NULL;
    char *new_source = NULL;
    char *rust_id = NULL;
    char *build_command = NULL;
    char dep_line[256];
    size_t used = 0;
    int ret = 1;

    if (append_format(
            &command,
            &used,
            "export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\" && "
            "anchor init \"%s\" --no-install --test-template rust",
            name) != 0) {
        goto cleanup;
    }

    if (run_shell_command_streaming("create nft project scaffold", command) != 0) {
        goto cleanup;
    }

    used = 0;
    if (append_format(&cargo_path, &used, "%s/programs/%s/Cargo.toml", name, name) != 0) goto cleanup;
    used = 0;
    if (append_format(&lib_path, &used, "%s/programs/%s/src/lib.rs", name, name) != 0) goto cleanup;

    existing_toml = read_text_file(cargo_path);
    if (existing_toml == NULL) {
        fprintf(stderr, "Failed to read generated Cargo.toml.\n");
        goto cleanup;
    }

    anchor_ver = extract_dep_version(existing_toml, "anchor-lang");
    snprintf(dep_line, sizeof(dep_line),
        "anchor-spl = { version = \"%s\", features = [\"token\"] }",
        anchor_ver ? anchor_ver : "0.31.1");

    if (append_cargo_dep(cargo_path, dep_line) != 0) {
        fprintf(stderr, "Failed to add anchor-spl dependency.\n");
        goto cleanup;
    }

    if (add_idl_build_dep(cargo_path, "anchor-spl/idl-build") != 0) {
        fprintf(stderr, "Failed to update idl-build feature.\n");
        goto cleanup;
    }

    existing_lib = read_text_file(lib_path);
    if (existing_lib == NULL) {
        fprintf(stderr, "Failed to read generated lib.rs.\n");
        goto cleanup;
    }

    declare_id = extract_declare_id_value(existing_lib);
    if (declare_id == NULL) {
        fprintf(stderr, "Failed to extract declare_id.\n");
        goto cleanup;
    }

    rust_id = make_rust_identifier(name);
    if (rust_id == NULL) goto cleanup;

    used = 0;
    if (append_format(
            &new_source,
            &used,
            "use anchor_lang::prelude::*;\n"
            "use anchor_spl::token::{self, FreezeAccount, Mint, MintTo, Token, TokenAccount};\n"
            "\n"
            "declare_id!(\"%s\");\n"
            "\n"
            "#[program]\n"
            "pub mod %s {\n"
            "    use super::*;\n"
            "\n"
            "    /// Mints 1 token and freezes the mint so supply stays at exactly 1.\n"
            "    /// Attach on-chain metadata with: `solcli agent` or Metaplex CLI/SDK.\n"
            "    pub fn mint_nft(ctx: Context<MintNft>) -> Result<()> {\n"
            "        token::mint_to(\n"
            "            CpiContext::new(\n"
            "                ctx.accounts.token_program.to_account_info(),\n"
            "                MintTo {\n"
            "                    mint: ctx.accounts.mint.to_account_info(),\n"
            "                    to: ctx.accounts.token_account.to_account_info(),\n"
            "                    authority: ctx.accounts.authority.to_account_info(),\n"
            "                },\n"
            "            ),\n"
            "            1,\n"
            "        )?;\n"
            "\n"
            "        token::freeze_account(\n"
            "            CpiContext::new(\n"
            "                ctx.accounts.token_program.to_account_info(),\n"
            "                FreezeAccount {\n"
            "                    account: ctx.accounts.token_account.to_account_info(),\n"
            "                    mint: ctx.accounts.mint.to_account_info(),\n"
            "                    authority: ctx.accounts.authority.to_account_info(),\n"
            "                },\n"
            "            ),\n"
            "        )?;\n"
            "\n"
            "        Ok(())\n"
            "    }\n"
            "}\n"
            "\n"
            "#[derive(Accounts)]\n"
            "pub struct MintNft<'info> {\n"
            "    #[account(\n"
            "        init,\n"
            "        payer = authority,\n"
            "        mint::decimals = 0,\n"
            "        mint::authority = authority,\n"
            "        mint::freeze_authority = authority,\n"
            "    )]\n"
            "    pub mint: Account<'info, Mint>,\n"
            "    #[account(\n"
            "        init,\n"
            "        payer = authority,\n"
            "        token::mint = mint,\n"
            "        token::authority = authority,\n"
            "    )]\n"
            "    pub token_account: Account<'info, TokenAccount>,\n"
            "    #[account(mut)]\n"
            "    pub authority: Signer<'info>,\n"
            "    pub token_program: Program<'info, Token>,\n"
            "    pub system_program: Program<'info, System>,\n"
            "    pub rent: Sysvar<'info, Rent>,\n"
            "}\n",
            declare_id, rust_id) != 0) {
        goto cleanup;
    }

    if (write_text_file(lib_path, new_source) != 0) {
        fprintf(stderr, "Failed to write NFT lib.rs.\n");
        goto cleanup;
    }

    used = 0;
    if (append_format(
            &build_command,
            &used,
            "export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\" && "
            "cd \"%s\" && anchor build",
            name) != 0) goto cleanup;

    if (run_shell_command_streaming("build nft project", build_command) != 0) {
        goto cleanup;
    }

    printf("\nNFT project created: ./%s\n", name);
    printf("  cd %s\n  anchor test\n", name);
    ret = 0;

cleanup:
    free(command);
    free(cargo_path);
    free(lib_path);
    free(existing_toml);
    free(anchor_ver);
    free(existing_lib);
    free(declare_id);
    free(new_source);
    free(rust_id);
    free(build_command);
    return ret;
}

static int cmd_init(int argc, char **argv) {
    const char *template_type = "anchor";
    char *name;
    char *anchor_check;

    if (argc >= 3) {
        template_type = argv[2];
    }

    if (strcmp(template_type, "anchor") != 0 &&
        strcmp(template_type, "native") != 0 &&
        strcmp(template_type, "token") != 0 &&
        strcmp(template_type, "nft") != 0) {
        fprintf(stderr, "Unknown template: %s\n", template_type);
        fprintf(stderr, "Available templates: anchor, native, token, nft\n");
        return 1;
    }

    if (strcmp(template_type, "anchor") == 0 ||
        strcmp(template_type, "token") == 0 ||
        strcmp(template_type, "nft") == 0) {
        anchor_check = run_command_capture(
            "bash -lc 'export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\"; anchor --version 2>/dev/null'");
        if (anchor_check == NULL) {
            fprintf(stderr, "Anchor CLI is not available.\n");
            fprintf(stderr, "Run `solcli download` first, then try again.\n");
            return 1;
        }
        free(anchor_check);
    }

    printf("Initializing a new %s template.\n", template_type);
    name = read_project_name_interactively();
    if (name == NULL) {
        return 1;
    }

    printf("Creating project: %s\n\n", name);

    int ret;
    if (strcmp(template_type, "anchor") == 0) {
        ret = scaffold_anchor_template(name);
    } else if (strcmp(template_type, "native") == 0) {
        ret = scaffold_native_template(name);
    } else if (strcmp(template_type, "token") == 0) {
        ret = scaffold_token_template(name);
    } else {
        ret = scaffold_nft_template(name);
    }

    free(name);
    return ret;
}

static void print_usage(void) {
    puts("SolCLI");
    puts("A Solana-focused developer CLI.");
    puts("");
    puts("Usage:");
    puts("  solcli");
    puts("  solcli help");
    puts("  solcli init [anchor|native|token|nft]");
    puts("  solcli env [check|doctor|install|upgrade]");
    puts("  solcli wallet [new|import|address|balance|airdrop|send|list|active|assign|use|cluster]");
    puts("  solcli network [list|use]");
    puts("  solcli rpc [set|current]");
    puts("  solcli ping");
    puts("  solcli health");
    puts("  solcli build [--verbose]");
    puts("  solcli test [--watch]");
    puts("  solcli deploy [--devnet|--testnet|--mainnet]");
    puts("  solcli clean");
    puts("  solcli download");
    puts("  solcli agent");
    puts("  solcli agent \"task\"");
    puts("  solcli ask");
    puts("  solcli ask \"question\"");
    puts("  solcli version");
    puts("");
    puts("Commands:");
    puts("  help");
    puts("      Show this help screen.");
    puts("  init");
    puts("      Create a new project from a template (default: anchor).");
    puts("      Templates:");
    puts("        anchor   Anchor framework project (default)");
    puts("        native   Native Solana program without Anchor");
    puts("        token    Anchor project with SPL token boilerplate");
    puts("        nft      NFT mint + freeze (decimals=0, supply=1)");
    puts("  env");
    puts("      Manage the Solana development environment.");
    puts("      Subcommands:");
    puts("        check    Show tool status table (default)");
    puts("        doctor   Detailed diagnostics with PATH and version checks");
    puts("        install  Install missing tools (Rust, Solana, AVM, Anchor)");
    puts("        upgrade  Upgrade Rust, Solana CLI, and Anchor to latest");
    puts("  download");
    puts("      Alias for `solcli env install`.");
    puts("  wallet");
    puts("      Manage Solana wallets stored under ~/.config/solcli/wallets.");
    puts("      Subcommands:");
    puts("        new              Create a new keypair wallet");
    puts("        import           Import a JSON keypair or seed phrase");
    puts("        address          Show the active wallet address");
    puts("        balance          Show the active wallet balance");
    puts("        airdrop [amount] Request devnet/testnet SOL for the active wallet");
    puts("        send <to> <sol>  Send SOL from the active wallet");
    puts("        list             Show wallet profiles and the active keypair");
    puts("        active           Show the active wallet, keypair, address, and network");
    puts("        assign           Assign the active wallet to a selected project");
    puts("        use <name>       Switch active wallet profile");
    puts("        cluster [name]   Show or set devnet/testnet/mainnet");
    puts("  network");
    puts("      List or select devnet/testnet/mainnet RPC networks.");
    puts("  rpc");
    puts("      Set or show the current RPC URL.");
    puts("  ping");
    puts("      Check the current RPC endpoint latency with getHealth.");
    puts("  health");
    puts("      Check current RPC health and version.");
    puts("  build");
    puts("      Select a Solana project and run anchor build or cargo build-sbf.");
    puts("  test");
    puts("      Select a Solana project and run anchor test or cargo test.");
    puts("  deploy");
    puts("      Select a Solana project and deploy to devnet, testnet, or mainnet.");
    puts("  clean");
    puts("      Select a Solana project and clean build artifacts.");
    puts("  agent");
    puts("      Open an agentic Solana assistant for troubleshooting, planning, guided workflows, and local scaffold actions.");
    puts("  ask");
    puts("      Open a question-focused Solana Q&A interface.");
    puts("  ask \"question\"");
    puts("      Ask one focused Solana question and print the answer.");
    puts("  version");
    puts("      Show SolCLI and installed tool versions.");
    puts("");
    puts("Examples:");
    puts("  solcli init");
    puts("  solcli init anchor");
    puts("  solcli init native");
    puts("  solcli init token");
    puts("  solcli init nft");
    puts("  solcli env check");
    puts("  solcli env doctor");
    puts("  solcli env install");
    puts("  solcli env upgrade");
    puts("  solcli wallet new");
    puts("  solcli wallet import");
    puts("  solcli wallet address");
    puts("  solcli wallet balance");
    puts("  solcli wallet airdrop");
    puts("  solcli wallet send <recipient> <amount>");
    puts("  solcli wallet list");
    puts("  solcli wallet active");
    puts("  solcli wallet assign");
    puts("  solcli wallet cluster devnet");
    puts("  solcli network list");
    puts("  solcli network use devnet");
    puts("  solcli network use mainnet");
    puts("  solcli rpc set https://api.devnet.solana.com");
    puts("  solcli rpc current");
    puts("  solcli ping");
    puts("  solcli health");
    puts("  solcli build --verbose");
    puts("  solcli test --watch");
    puts("  solcli deploy --devnet");
    puts("  solcli deploy --testnet");
    puts("  solcli deploy --mainnet");
    puts("  solcli clean");
    puts("  solcli help");
    puts("  solcli download");
    puts("  solcli agent");
    puts("  solcli agent \"create a basic Solana project\"");
    puts("  solcli ask");
    puts("  solcli ask \"how does anchor account validation work?\"");
    puts("  solcli version");
}

static int show_versions(void) {
    puts("SolCLI version information:");
    print_version_item("solcli", SOLCLI_VERSION);
    print_tool_version(
        "rustc",
        "bash -lc 'export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\"; rustc --version 2>/dev/null'"
    );
    print_tool_version(
        "cargo",
        "bash -lc 'export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\"; cargo --version 2>/dev/null'"
    );
    print_tool_version(
        "solana",
        "bash -lc 'export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\"; solana --version 2>/dev/null'"
    );
    print_tool_version(
        "avm",
        "bash -lc 'export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\"; avm --version 2>/dev/null'"
    );
    print_tool_version(
        "anchor",
        "bash -lc 'export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\"; anchor --version 2>/dev/null'"
    );
    return 0;
}

static char *join_arguments(int start_index, int argc, char **argv) {
    char *joined = NULL;
    size_t used = 0;

    for (int i = start_index; i < argc; ++i) {
        if (append_format(&joined, &used, "%s%s", (i == start_index) ? "" : " ", argv[i]) != 0) {
            free(joined);
            return NULL;
        }
    }

    return joined;
}

static int run_step(const char *label, const char *command) {
    int status;

    printf("==> %s\n", label);
    status = system(command);
    if (status == -1) {
        perror("Command execution failed");
        return 1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        puts("Done.\n");
        return 0;
    }

    fprintf(stderr, "Step failed: %s\n", label);
    return 1;
}

static int download_tooling(void) {
    const char *steps[][2] = {
        {
            "Check / install Rust toolchain",
            "bash -lc 'command -v cargo >/dev/null 2>&1 || curl --proto \"=https\" --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y'"
        },
        {
            "Check / install Solana CLI",
            "bash -lc 'command -v solana >/dev/null 2>&1 || curl -sSfL https://release.anza.xyz/stable/install | bash'"
        },
        {
            "Check / install AVM",
            "bash -lc 'export PATH=\"$HOME/.cargo/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\"; command -v avm >/dev/null 2>&1 || cargo install --git https://github.com/coral-xyz/anchor avm --locked'"
        },
        {
            "Install / select Anchor version",
            "bash -lc 'export PATH=\"$HOME/.cargo/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\"; avm install latest && avm use latest; if ! anchor --version >/dev/null 2>&1; then cargo install --git https://github.com/coral-xyz/anchor anchor-cli --tag v0.31.1 --locked --force; fi'"
        }
    };

    puts("Installing Solana development tooling...");
    puts("This requires an internet connection and some system packages.\n");

    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        if (run_step(steps[i][0], steps[i][1]) != 0) {
            return 1;
        }
    }

    puts("Installation complete.");
    puts("You may need to open a new shell session.");
    return 0;
}

static const char * const ENV_TOOLS[] = { "rustc", "cargo", "solana", "avm", "anchor" };
#define ENV_TOOL_COUNT ((int)(sizeof(ENV_TOOLS) / sizeof(ENV_TOOLS[0])))
#define TOOL_PATH_CMD \
    "bash -lc 'export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:" \
    "$HOME/.local/share/solana/install/active_release/bin:$PATH\"; "

static char *check_tool_version(const char *bin_name) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        TOOL_PATH_CMD "%s --version 2>/dev/null'", bin_name);
    return run_command_capture(cmd);
}

static bool home_dir_exists(const char *subpath) {
    const char *home = getenv("HOME");
    char path[1024];

    if (home == NULL) {
        return false;
    }

    snprintf(path, sizeof(path), "%s/%s", home, subpath);
    return path_exists(path);
}

static int env_check(void) {
    int i;
    int missing = 0;
    char *ver;

    puts("Checking Solana development environment...\n");
    printf("  %-10s %-10s %s\n", "Tool", "Status", "Version");
    printf("  %-10s %-10s %s\n",
           "----------", "----------", "-------------------------------");

    for (i = 0; i < ENV_TOOL_COUNT; i++) {
        ver = check_tool_version(ENV_TOOLS[i]);
        if (ver != NULL) {
            printf("  %-10s ok         %s\n", ENV_TOOLS[i], ver);
            free(ver);
        } else {
            printf("  %-10s missing\n", ENV_TOOLS[i]);
            ++missing;
        }
    }

    puts("");
    if (missing == 0) {
        printf("All %d tools found.\n", ENV_TOOL_COUNT);
        puts("Run `solcli env doctor` for detailed diagnostics.");
    } else {
        printf("%d tool(s) missing. Run `solcli env install` to install them.\n", missing);
    }

    return missing > 0 ? 1 : 0;
}

static int env_doctor(void) {
    char *versions[ENV_TOOL_COUNT];
    int i;
    int missing = 0;
    bool cargo_bin;
    bool solana_bin;
    bool avm_bin;

    puts("SolCLI Environment Doctor");
    puts("==========================\n");

    puts("Tools:");
    for (i = 0; i < ENV_TOOL_COUNT; i++) {
        versions[i] = check_tool_version(ENV_TOOLS[i]);
        if (versions[i] != NULL) {
            printf("  [ok]      %-8s  %s\n", ENV_TOOLS[i], versions[i]);
        } else {
            printf("  [missing] %-8s  not installed\n", ENV_TOOLS[i]);
            ++missing;
        }
    }

    puts("\nPATH directories:");
    cargo_bin  = home_dir_exists(".cargo/bin");
    solana_bin = home_dir_exists(".local/share/solana/install/active_release/bin");
    avm_bin    = home_dir_exists(".avm/bin");

    printf("  %s  ~/.cargo/bin\n",
           cargo_bin  ? "[ok]     " : "[missing]");
    printf("  %s  ~/.local/share/solana/install/active_release/bin\n",
           solana_bin ? "[ok]     " : "[missing]");
    printf("  %s  ~/.avm/bin\n",
           avm_bin    ? "[ok]     " : "[missing]");

    if (versions[4] != NULL) {
        puts("\nVersion compatibility:");
        printf("  anchor  %s", versions[4]);
        if (versions[2] != NULL) {
            printf("\n  solana  %s", versions[2]);
        }
        puts("\n  Verify compatibility: https://www.anchor-lang.com/release-notes");
    }

    puts("\nRecommendations:");
    if (missing > 0) {
        printf("  %d tool(s) missing. Run: solcli env install\n", missing);
    } else if (!cargo_bin || !solana_bin || !avm_bin) {
        puts("  Some directories not found. Run: solcli env install");
        puts("  Then open a new shell or: source ~/.bashrc");
    } else {
        puts("  All checks passed.");
        puts("  Run `solcli env upgrade` to get the latest tool versions.");
    }

    for (i = 0; i < ENV_TOOL_COUNT; i++) {
        free(versions[i]);
    }

    return missing > 0 ? 1 : 0;
}

static int env_install(void) {
    return download_tooling();
}

static int env_upgrade(void) {
    const char *steps[][2] = {
        {
            "Update Rust toolchain",
            "bash -lc 'export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\"; rustup update'"
        },
        {
            "Update Solana CLI",
            "bash -lc 'export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\"; solana-install update'"
        },
        {
            "Update Anchor via AVM",
            "bash -lc 'export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\"; avm install latest && avm use latest'"
        }
    };

    puts("Upgrading Solana development toolchain...\n");

    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        if (run_step(steps[i][0], steps[i][1]) != 0) {
            return 1;
        }
    }

    puts("Upgrade complete.");
    puts("Run `solcli env doctor` to verify.");
    return 0;
}

static int cmd_env(int argc, char **argv) {
    const char *sub = (argc >= 3) ? argv[2] : "check";

    if (strcmp(sub, "check") == 0) {
        return env_check();
    }
    if (strcmp(sub, "doctor") == 0) {
        return env_doctor();
    }
    if (strcmp(sub, "install") == 0) {
        return env_install();
    }
    if (strcmp(sub, "upgrade") == 0) {
        return env_upgrade();
    }

    fprintf(stderr, "Unknown env subcommand: %s\n", sub);
    fprintf(stderr, "Available: check, doctor, install, upgrade\n");
    return 1;
}

/* ─── Build / test / deploy wrappers ─── */

static const char *project_type_label(ProjectType type) {
    return type == PROJECT_ANCHOR ? "Anchor" : "Native";
}

static void free_project_list(ProjectList *projects) {
    if (projects == NULL) return;
    for (size_t i = 0; i < projects->len; ++i) {
        free(projects->items[i].name);
        free(projects->items[i].path);
    }
    free(projects->items);
    projects->items = NULL;
    projects->len = 0;
    projects->cap = 0;
}

static int add_project(ProjectList *projects, const char *name, const char *path, ProjectType type) {
    SolanaProject *new_items;

    if (projects == NULL || name == NULL || path == NULL) return -1;

    if (projects->len == projects->cap) {
        size_t new_cap = projects->cap == 0 ? 8 : projects->cap * 2;
        new_items = realloc(projects->items, new_cap * sizeof(SolanaProject));
        if (new_items == NULL) return -1;
        projects->items = new_items;
        projects->cap = new_cap;
    }

    projects->items[projects->len].name = strdup(name);
    projects->items[projects->len].path = strdup(path);
    projects->items[projects->len].type = type;
    if (projects->items[projects->len].name == NULL || projects->items[projects->len].path == NULL) {
        free(projects->items[projects->len].name);
        free(projects->items[projects->len].path);
        return -1;
    }

    ++projects->len;
    return 0;
}

static int project_name_compare(const void *a, const void *b) {
    const SolanaProject *pa = (const SolanaProject *)a;
    const SolanaProject *pb = (const SolanaProject *)b;
    return strcmp(pa->name, pb->name);
}

static int detect_project_type(const char *path, ProjectType *type_out) {
    char *anchor_path = NULL;
    char *cargo_path = NULL;
    char *programs_path = NULL;
    size_t used = 0;
    int ret = 0;

    if (append_format(&anchor_path, &used, "%s/Anchor.toml", path) != 0) goto done;
    used = 0;
    if (append_format(&cargo_path, &used, "%s/Cargo.toml", path) != 0) goto done;
    used = 0;
    if (append_format(&programs_path, &used, "%s/programs", path) != 0) goto done;

    if (path_exists(anchor_path) && is_directory_path(programs_path)) {
        *type_out = PROJECT_ANCHOR;
        ret = 1;
    } else if (path_exists(cargo_path)) {
        *type_out = PROJECT_NATIVE;
        ret = 1;
    }

done:
    free(anchor_path);
    free(cargo_path);
    free(programs_path);
    return ret;
}

static int discover_solana_projects(ProjectList *projects) {
    DIR *dir = opendir(".");
    struct dirent *entry;
    ProjectType type;

    if (dir == NULL) {
        perror("Could not read current directory");
        return 1;
    }

    if (detect_project_type(".", &type)) {
        if (add_project(projects, ".", ".", type) != 0) {
            closedir(dir);
            return 1;
        }
    }

    while ((entry = readdir(dir)) != NULL) {
        char *path = NULL;
        size_t used = 0;

        if (entry->d_name[0] == '.') continue;
        if (append_format(&path, &used, "./%s", entry->d_name) != 0) {
            closedir(dir);
            return 1;
        }

        if (is_directory_path(path) && detect_project_type(path, &type)) {
            if (add_project(projects, entry->d_name, path, type) != 0) {
                free(path);
                closedir(dir);
                return 1;
            }
        }
        free(path);
    }

    closedir(dir);
    qsort(projects->items, projects->len, sizeof(SolanaProject), project_name_compare);
    return 0;
}

static void render_project_picker(const ProjectList *projects, size_t selected) {
    printf("\033[2J\033[H");
    puts("Select Solana project");
    puts("Use Up/Down arrows, Enter to select, q to cancel.\n");

    for (size_t i = 0; i < projects->len; ++i) {
        printf("%s %-24s %s\n",
               i == selected ? ">" : " ",
               projects->items[i].name,
               project_type_label(projects->items[i].type));
    }
    fflush(stdout);
}

static int choose_project_interactive(const ProjectList *projects) {
    struct termios old_term;
    struct termios raw_term;
    size_t selected = 0;
    int result = -1;

    if (projects->len == 0) return -1;
    if (!isatty(STDIN_FILENO)) {
        return 0;
    }

    if (tcgetattr(STDIN_FILENO, &old_term) != 0) {
        return 0;
    }

    raw_term = old_term;
    raw_term.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw_term.c_cc[VMIN] = 1;
    raw_term.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_term) != 0) {
        return 0;
    }

    render_project_picker(projects, selected);
    while (true) {
        int ch = getchar();
        if (ch == '\n' || ch == '\r') {
            result = (int)selected;
            break;
        }
        if (ch == 'q' || ch == 'Q' || ch == 27) {
            if (ch == 27) {
                int next = getchar();
                if (next == '[') {
                    int arrow = getchar();
                    if (arrow == 'A') {
                        selected = (selected == 0) ? projects->len - 1 : selected - 1;
                        render_project_picker(projects, selected);
                        continue;
                    }
                    if (arrow == 'B') {
                        selected = (selected + 1) % projects->len;
                        render_project_picker(projects, selected);
                        continue;
                    }
                }
            }
            result = -1;
            break;
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    printf("\033[2J\033[H");
    return result;
}

static SolanaProject *select_solana_project(ProjectList *projects) {
    int selected;

    if (discover_solana_projects(projects) != 0) {
        return NULL;
    }

    if (projects->len == 0) {
        fprintf(stderr, "No Solana projects found in the current directory.\n");
        fprintf(stderr, "Expected Anchor.toml or Cargo.toml in ./ or direct child directories.\n");
        return NULL;
    }

    selected = choose_project_interactive(projects);
    if (selected < 0) {
        puts("Cancelled.");
        return NULL;
    }

    printf("Selected project: %s (%s)\n",
           projects->items[selected].path,
           project_type_label(projects->items[selected].type));
    return &projects->items[selected];
}

static bool has_arg(int argc, char **argv, const char *needle) {
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], needle) == 0) return true;
    }
    return false;
}

static const char *deploy_cluster_from_args(int argc, char **argv) {
    const char *cluster = NULL;

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--devnet") == 0) {
            cluster = "devnet";
        } else if (strcmp(argv[i], "--testnet") == 0) {
            cluster = "testnet";
        } else if (strcmp(argv[i], "--mainnet") == 0 || strcmp(argv[i], "--mainnet-beta") == 0) {
            cluster = "mainnet-beta";
        } else if (strncmp(argv[i], "--", 2) == 0) {
            fprintf(stderr, "Unknown deploy option: %s\n", argv[i]);
            fprintf(stderr, "Usage: solcli deploy [--devnet|--testnet|--mainnet]\n");
            return NULL;
        }
    }

    return cluster != NULL ? cluster : "devnet";
}

static int run_project_command(const char *label, const SolanaProject *project, const char *command) {
    char *wrapped = NULL;
    size_t used = 0;
    int ret;

    if (append_format(
            &wrapped,
            &used,
            "export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH\" && "
            "cd \"%s\" && %s",
            project->path,
            command) != 0) {
        fprintf(stderr, "Failed to build command.\n");
        return 1;
    }

    ret = run_shell_command_streaming(label, wrapped);
    free(wrapped);
    return ret;
}

static char *project_wallet_path(const SolanaProject *project) {
    char *path = NULL;
    size_t used = 0;

    if (project == NULL) return NULL;
    if (append_format(&path, &used, "%s/.solcli/wallet", project->path) != 0) {
        free(path);
        return NULL;
    }
    return path;
}

static char *read_project_wallet(const SolanaProject *project) {
    char *path = project_wallet_path(project);
    char *wallet;

    if (path == NULL) return NULL;
    wallet = read_text_file(path);
    free(path);
    if (wallet != NULL) {
        trim_trailing_whitespace(wallet);
        if (wallet[0] == '\0' || !path_exists(wallet)) {
            free(wallet);
            wallet = NULL;
        }
    }
    return wallet;
}

static int write_project_wallet_file(const SolanaProject *project, const char *wallet) {
    char *dir = NULL;
    char *path = NULL;
    size_t used = 0;
    int ret;

    if (project == NULL || wallet == NULL) return -1;
    if (append_format(&dir, &used, "%s/.solcli", project->path) != 0) return -1;
    if (mkdir(dir, 0700) != 0 && !path_exists(dir)) {
        free(dir);
        return -1;
    }
    free(dir);

    path = project_wallet_path(project);
    if (path == NULL) return -1;
    ret = write_text_file(path, wallet);
    free(path);
    return ret;
}

static char *toml_escape_string(const char *input) {
    return json_escape(input);
}

static int update_anchor_wallet_config(const SolanaProject *project, const char *wallet) {
    char *anchor_path = NULL;
    char *source = NULL;
    char *wallet_escaped = NULL;
    char *updated = NULL;
    size_t used = 0;
    size_t path_used = 0;
    bool in_provider = false;
    bool provider_seen = false;
    bool wallet_written = false;
    const char *cursor;
    int ret = 1;

    if (append_format(&anchor_path, &path_used, "%s/Anchor.toml", project->path) != 0) goto done;
    source = read_text_file(anchor_path);
    if (source == NULL) goto done;
    wallet_escaped = toml_escape_string(wallet);
    if (wallet_escaped == NULL) goto done;

    cursor = source;
    while (*cursor != '\0') {
        const char *line_start = cursor;
        const char *line_end = strchr(cursor, '\n');
        size_t line_len = line_end == NULL ? strlen(cursor) : (size_t)(line_end - cursor);
        char *line = malloc(line_len + 1);
        char *trimmed;

        if (line == NULL) goto done;
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';
        trimmed = line;
        while (*trimmed != '\0' && isspace((unsigned char)*trimmed)) ++trimmed;

        if (trimmed[0] == '[') {
            if (in_provider && !wallet_written) {
                if (append_format(&updated, &used, "wallet = \"%s\"\n", wallet_escaped) != 0) {
                    free(line);
                    goto done;
                }
                wallet_written = true;
            }
            in_provider = strcmp(trimmed, "[provider]") == 0;
            if (in_provider) provider_seen = true;
        }

        if (in_provider && strncmp(trimmed, "wallet", 6) == 0) {
            const char *after = trimmed + 6;
            while (*after != '\0' && isspace((unsigned char)*after)) ++after;
            if (*after == '=') {
                if (append_format(&updated, &used, "wallet = \"%s\"\n", wallet_escaped) != 0) {
                    free(line);
                    goto done;
                }
                wallet_written = true;
                free(line);
                cursor = line_end == NULL ? line_start + line_len : line_end + 1;
                continue;
            }
        }

        if (append_format(&updated, &used, "%s%s", line, line_end == NULL ? "" : "\n") != 0) {
            free(line);
            goto done;
        }
        free(line);
        cursor = line_end == NULL ? line_start + line_len : line_end + 1;
    }

    if (provider_seen && !wallet_written) {
        if (append_format(&updated, &used, "wallet = \"%s\"\n", wallet_escaped) != 0) goto done;
    } else if (!provider_seen) {
        if (used > 0 && updated[used - 1] != '\n') {
            if (append_format(&updated, &used, "\n") != 0) goto done;
        }
        if (append_format(&updated, &used, "\n[provider]\ncluster = \"localnet\"\nwallet = \"%s\"\n", wallet_escaped) != 0) goto done;
    }

    if (write_text_file(anchor_path, updated) != 0) goto done;
    ret = 0;

done:
    free(anchor_path);
    free(source);
    free(wallet_escaped);
    free(updated);
    return ret;
}

static int cmd_wallet_assign(void) {
    ProjectList projects = {0};
    SolanaProject *project;
    char *wallet = resolve_active_keypair();
    int ret = 1;

    if (wallet == NULL) {
        return 1;
    }

    project = select_solana_project(&projects);
    if (project == NULL) goto done;

    if (write_project_wallet_file(project, wallet) != 0) {
        fprintf(stderr, "Failed to write project wallet file.\n");
        goto done;
    }

    if (project->type == PROJECT_ANCHOR && update_anchor_wallet_config(project, wallet) != 0) {
        fprintf(stderr, "Failed to update Anchor.toml wallet.\n");
        goto done;
    }

    printf("Assigned wallet to project: %s\n", project->path);
    printf("Wallet: %s\n", wallet);
    if (project->type == PROJECT_ANCHOR) {
        puts("Updated: Anchor.toml [provider].wallet");
    }
    puts("Updated: .solcli/wallet");
    ret = 0;

done:
    free(wallet);
    free_project_list(&projects);
    return ret;
}

static int cmd_build(int argc, char **argv) {
    ProjectList projects = {0};
    SolanaProject *project = select_solana_project(&projects);
    char *command = NULL;
    size_t used = 0;
    int ret = 1;

    if (project == NULL) goto done;

    if (project->type == PROJECT_ANCHOR) {
        if (append_format(&command, &used, "anchor build%s", has_arg(argc, argv, "--verbose") ? " -- --verbose" : "") != 0) goto done;
    } else {
        if (append_format(&command, &used, "cargo build-sbf%s", has_arg(argc, argv, "--verbose") ? " --verbose" : "") != 0) goto done;
    }

    ret = run_project_command("build project", project, command);

done:
    free(command);
    free_project_list(&projects);
    return ret;
}

static int cmd_test(int argc, char **argv) {
    ProjectList projects = {0};
    SolanaProject *project = select_solana_project(&projects);
    bool watch = has_arg(argc, argv, "--watch");
    const char *command;
    int ret = 1;

    if (project == NULL) goto done;

    if (watch) {
        command = project->type == PROJECT_ANCHOR
            ? "while true; do clear; anchor test; inotifywait -r -e modify,create,delete programs tests migrations Anchor.toml Cargo.toml 2>/dev/null || sleep 2; done"
            : "while true; do clear; cargo test; inotifywait -r -e modify,create,delete src tests Cargo.toml 2>/dev/null || sleep 2; done";
    } else {
        command = project->type == PROJECT_ANCHOR ? "anchor test" : "cargo test";
    }

    ret = run_project_command(watch ? "watch tests" : "test project", project, command);

done:
    free_project_list(&projects);
    return ret;
}

static int cmd_deploy(int argc, char **argv) {
    ProjectList projects = {0};
    SolanaProject *project = NULL;
    const char *cluster = deploy_cluster_from_args(argc, argv);
    char *wallet = NULL;
    char *command = NULL;
    size_t used = 0;
    int ret = 1;

    if (cluster == NULL) goto done;

    project = select_solana_project(&projects);
    if (project == NULL) goto done;

    if (strcmp(cluster, "mainnet-beta") == 0) {
        char *confirm;
        puts("WARNING: You are about to deploy to mainnet.");
        confirm = read_input_line("Type 'yes' to confirm: ");
        if (confirm == NULL || strcmp(confirm, "yes") != 0) {
            puts("Cancelled.");
            free(confirm);
            ret = 0;
            goto done;
        }
        free(confirm);
    }

    wallet = read_project_wallet(project);
    if (wallet == NULL) {
        wallet = resolve_active_keypair();
    }

    if (project->type == PROJECT_ANCHOR) {
        if (wallet != NULL) {
            if (append_format(&command, &used, "ANCHOR_WALLET=\"%s\" anchor deploy --provider.cluster %s", wallet, cluster) != 0) goto done;
        } else {
            if (append_format(&command, &used, "anchor deploy --provider.cluster %s", cluster) != 0) goto done;
        }
    } else {
        if (wallet != NULL) {
            if (append_format(&command, &used, "cargo build-sbf && solana program deploy target/deploy/*.so --url %s --keypair \"%s\"", cluster, wallet) != 0) goto done;
        } else {
            if (append_format(&command, &used, "cargo build-sbf && solana program deploy target/deploy/*.so --url %s", cluster) != 0) goto done;
        }
    }

    ret = run_project_command("deploy project", project, command);

done:
    free(wallet);
    free(command);
    free_project_list(&projects);
    return ret;
}

static int cmd_clean(void) {
    ProjectList projects = {0};
    SolanaProject *project = select_solana_project(&projects);
    const char *command;
    int ret = 1;

    if (project == NULL) goto done;

    command = project->type == PROJECT_ANCHOR ? "anchor clean" : "cargo clean";
    ret = run_project_command("clean project", project, command);

done:
    free_project_list(&projects);
    return ret;
}

/* ─── Wallet management ─── */

#define WALLET_ENV \
    "export PATH=\"$HOME/.cargo/bin:$HOME/.avm/bin:" \
    "$HOME/.local/share/solana/install/active_release/bin:$PATH\""

static char *solcli_path(const char *subpath) {
    const char *home = getenv("HOME");
    char *path = NULL;
    size_t used = 0;
    if (home == NULL) return NULL;
    if (append_format(&path, &used,
            "%s/.config/solcli/%s", home, subpath) != 0) return NULL;
    return path;
}

static void ensure_wallets_dir(void) {
    const char *home = getenv("HOME");
    char config_dir[1024];
    char solcli_dir[1024];
    char *wallets_dir;
    if (home == NULL) return;
    if ((size_t)snprintf(config_dir, sizeof(config_dir), "%s/.config", home) >= sizeof(config_dir) ||
        (size_t)snprintf(solcli_dir, sizeof(solcli_dir), "%s/.config/solcli", home) >= sizeof(solcli_dir)) {
        fprintf(stderr, "Warning: HOME path is too long for wallet directory.\n");
        return;
    }
    (void)mkdir(home, 0700);
    (void)mkdir(config_dir, 0700);
    (void)mkdir(solcli_dir, 0700);
    wallets_dir = solcli_path("wallets");
    if (wallets_dir == NULL) return;
    if (mkdir(wallets_dir, 0700) != 0 && !path_exists(wallets_dir)) {
        fprintf(stderr, "Warning: could not create wallet directory: %s\n", wallets_dir);
    }
    free(wallets_dir);
}

static char *wallet_keypair_path(const char *name) {
    const char *home = getenv("HOME");
    char *path = NULL;
    size_t used = 0;
    if (home == NULL) return NULL;
    if (append_format(&path, &used,
            "%s/.config/solcli/wallets/%s.json", home, name) != 0) return NULL;
    return path;
}

static char *read_active_wallet(void) {
    char *p = solcli_path("active_wallet");
    char *name;
    if (p == NULL) return NULL;
    name = read_text_file(p);
    free(p);
    if (name != NULL) {
        trim_trailing_whitespace(name);
        if (name[0] == '\0') { free(name); name = NULL; }
    }
    return name;
}

static int write_active_wallet(const char *name) {
    char *p = solcli_path("active_wallet");
    int ret;
    ensure_wallets_dir();
    if (p == NULL) return -1;
    ret = write_text_file(p, name);
    free(p);
    return ret;
}

static char *read_wallet_cluster(void) {
    char *p = solcli_path("cluster");
    char *c;
    if (p == NULL) return strdup("devnet");
    c = read_text_file(p);
    free(p);
    if (c == NULL) return strdup("devnet");
    trim_trailing_whitespace(c);
    if (c[0] == '\0') { free(c); return strdup("devnet"); }
    return c;
}

static int write_wallet_cluster(const char *cluster) {
    char *p = solcli_path("cluster");
    int ret;
    ensure_wallets_dir();
    if (p == NULL) return -1;
    ret = write_text_file(p, cluster);
    free(p);
    return ret;
}

static const char *network_url_for_cluster(const char *cluster) {
    if (cluster == NULL || strcmp(cluster, "devnet") == 0) {
        return "https://api.devnet.solana.com";
    }
    if (strcmp(cluster, "testnet") == 0) {
        return "https://api.testnet.solana.com";
    }
    if (strcmp(cluster, "mainnet") == 0 || strcmp(cluster, "mainnet-beta") == 0) {
        return "https://api.mainnet-beta.solana.com";
    }
    return cluster;
}

static const char *normalize_cluster_name(const char *input) {
    if (input == NULL) return NULL;
    if (strcmp(input, "devnet") == 0) return "devnet";
    if (strcmp(input, "testnet") == 0) return "testnet";
    if (strcmp(input, "mainnet") == 0 || strcmp(input, "mainnet-beta") == 0) return "mainnet-beta";
    return NULL;
}

static char *read_custom_rpc_url(void) {
    char *p = solcli_path("rpc_url");
    char *url;
    if (p == NULL) return NULL;
    url = read_text_file(p);
    free(p);
    if (url != NULL) {
        trim_trailing_whitespace(url);
        if (url[0] == '\0') { free(url); url = NULL; }
    }
    return url;
}

static int write_custom_rpc_url(const char *url) {
    char *p = solcli_path("rpc_url");
    int ret;
    ensure_wallets_dir();
    if (p == NULL) return -1;
    ret = write_text_file(p, url);
    free(p);
    return ret;
}

static void clear_custom_rpc_url(void) {
    char *p = solcli_path("rpc_url");
    if (p != NULL) {
        (void)unlink(p);
        free(p);
    }
}

static char *current_rpc_url(void) {
    char *custom = read_custom_rpc_url();
    char *cluster;
    const char *url;

    if (custom != NULL) {
        return custom;
    }

    cluster = read_wallet_cluster();
    url = network_url_for_cluster(cluster);
    custom = strdup(url);
    free(cluster);
    return custom;
}

static double elapsed_ms(struct timespec start, struct timespec end) {
    time_t sec = end.tv_sec - start.tv_sec;
    long nsec = end.tv_nsec - start.tv_nsec;
    return ((double)sec * 1000.0) + ((double)nsec / 1000000.0);
}

static int rpc_request(const char *url, const char *method, char **response_out, long *status_out, double *latency_out) {
    CURL *curl;
    CURLcode result;
    struct curl_slist *headers = NULL;
    Buffer response = {0};
    char *payload = NULL;
    size_t used = 0;
    struct timespec start;
    struct timespec end;
    int ret = 1;

    if (response_out != NULL) *response_out = NULL;
    if (status_out != NULL) *status_out = 0;
    if (latency_out != NULL) *latency_out = 0.0;

    if (append_format(
            &payload,
            &used,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\"}",
            method) != 0) {
        return 1;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        free(payload);
        return 1;
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)used);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SolCLI/0.2");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    result = curl_easy_perform(curl);
    (void)clock_gettime(CLOCK_MONOTONIC, &end);

    if (latency_out != NULL) {
        *latency_out = elapsed_ms(start, end);
    }

    if (result == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status_out != NULL) *status_out = status;
        if (response_out != NULL) {
            *response_out = response.data;
            response.data = NULL;
        }
        ret = (status >= 200 && status < 300) ? 0 : 1;
    } else {
        fprintf(stderr, "RPC request failed: %s\n", curl_easy_strerror(result));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(payload);
    free(response.data);
    return ret;
}

static char *read_input_line(const char *prompt) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    if (prompt != NULL) { printf("%s", prompt); fflush(stdout); }
    len = getline(&line, &cap, stdin);
    if (len < 0) { free(line); return NULL; }
    while (len > 0 && isspace((unsigned char)line[len - 1])) line[--len] = '\0';
    return line;
}

static char *sanitize_wallet_name(const char *input) {
    size_t len = strlen(input);
    char *name = malloc(len + 1);
    size_t out = 0, i;
    if (name == NULL) return NULL;
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)input[i];
        if (isalnum(ch) || ch == '-' || ch == '_') name[out++] = (char)tolower(ch);
    }
    if (out == 0) { strcpy(name, "default"); return name; }
    name[out] = '\0';
    return name;
}

static char *resolve_active_keypair(void) {
    char *name = read_active_wallet();
    char *kp;
    if (name == NULL) {
        fprintf(stderr, "No active wallet. Run: solcli wallet new\n");
        return NULL;
    }
    kp = wallet_keypair_path(name);
    free(name);
    if (kp == NULL || !path_exists(kp)) {
        fprintf(stderr, "Active wallet keypair not found: %s\n", kp ? kp : "(unknown)");
        free(kp);
        return NULL;
    }
    return kp;
}

static int wallet_new(void) {
    char *raw, *name, *kp, *command = NULL, *existing;
    size_t used = 0;
    int ret = 1;

    ensure_wallets_dir();

    raw = read_input_line("Wallet name (Enter for 'default'): ");
    if (raw == NULL) return 1;
    name = (raw[0] == '\0') ? strdup("default") : sanitize_wallet_name(raw);
    free(raw);
    if (name == NULL) return 1;

    kp = wallet_keypair_path(name);
    if (kp == NULL) { free(name); return 1; }

    if (path_exists(kp)) {
        fprintf(stderr, "Wallet '%s' already exists. Choose a different name or delete:\n  %s\n", name, kp);
        free(name); free(kp);
        return 1;
    }

    if (append_format(&command, &used,
            WALLET_ENV " && solana-keygen new --outfile \"%s\" --no-passphrase",
            kp) != 0) {
        free(name); free(kp);
        return 1;
    }

    printf("Generating wallet '%s'...\n\n", name);
    if (run_shell_command_streaming("generate keypair", command) != 0) goto done;

    existing = read_active_wallet();
    if (existing == NULL) {
        write_active_wallet(name);
        printf("\nActive wallet set to: %s\n", name);
    }
    free(existing);

    printf("\nWallet '%s' created.\n", name);
    printf("  Keypair: %s\n", kp);
    printf("  Network: devnet  (change with: solcli wallet cluster)\n");
    ret = 0;

done:
    free(name); free(kp); free(command);
    return ret;
}

static int wallet_import(void) {
    char *raw, *name, *kp;
    char *choice;
    int ret = 1;

    ensure_wallets_dir();

    raw = read_input_line("Wallet name (Enter for 'default'): ");
    if (raw == NULL) return 1;
    name = (raw[0] == '\0') ? strdup("default") : sanitize_wallet_name(raw);
    free(raw);
    if (name == NULL) return 1;

    kp = wallet_keypair_path(name);
    if (kp == NULL) { free(name); return 1; }

    if (path_exists(kp)) {
        fprintf(stderr, "Wallet '%s' already exists.\n", name);
        free(name); free(kp);
        return 1;
    }

    puts("Import from:");
    puts("  [1] JSON keypair file (default)");
    puts("  [2] Seed phrase (BIP39 mnemonic)");
    choice = read_input_line("Choice [1]: ");
    if (choice == NULL) { free(name); free(kp); return 1; }

    if (strcmp(choice, "2") == 0) {
        free(choice);
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
            "bash -lc '" WALLET_ENV " && solana-keygen recover --outfile \"%s\" --force'",
            kp);
        puts("\nFollow the prompts to enter your seed phrase...\n");
        if (system(cmd) != 0) goto done;
    } else {
        free(choice);
        char *src = read_input_line("Path to JSON keypair file: ");
        if (src == NULL) { free(name); free(kp); return 1; }

        if (!path_exists(src)) {
            fprintf(stderr, "File not found: %s\n", src);
            free(src); free(name); free(kp);
            return 1;
        }

        int copy_result = copy_file(src, kp);
        free(src);
        if (copy_result != 0) {
            fprintf(stderr, "Failed to copy keypair file.\n");
            free(name); free(kp);
            return 1;
        }
    }

    {
        char *existing = read_active_wallet();
        if (existing == NULL) {
            write_active_wallet(name);
            printf("Active wallet set to: %s\n", name);
        }
        free(existing);
    }

    printf("\nWallet '%s' imported.\n  Keypair: %s\n", name, kp);
    ret = 0;

done:
    free(name); free(kp);
    return ret;
}

static int wallet_list(void) {
    char *wallets_dir = solcli_path("wallets");
    char *active = read_active_wallet();
    char *cluster = read_wallet_cluster();
    char *active_keypair = NULL;
    char cmd[2048];
    FILE *pipe;
    char line[1024];

    if (wallets_dir == NULL) { free(active); free(cluster); return 1; }

    printf("Wallets  (network: %s)\n", cluster);
    printf("─────────────────────────────────\n");
    if (active != NULL) {
        active_keypair = wallet_keypair_path(active);
        if (active_keypair != NULL) {
            printf("Active keypair: %s\n\n", active_keypair);
        }
    }

    if (!path_exists(wallets_dir)) {
        puts("  (none)  Run: solcli wallet new");
        free(wallets_dir); free(active); free(cluster); free(active_keypair);
        return 0;
    }

    snprintf(cmd, sizeof(cmd),
        "ls \"%s\"/*.json 2>/dev/null | sort", wallets_dir);
    pipe = popen(cmd, "r");
    if (pipe != NULL) {
        int found = 0;
        while (fgets(line, sizeof(line), pipe) != NULL) {
            char *slash = strrchr(line, '/');
            char *dot;
            if (slash == NULL) continue;
            ++slash;
            dot = strstr(slash, ".json");
            if (dot != NULL) *dot = '\0';
            trim_trailing_whitespace(slash);
            if (active != NULL && strcmp(slash, active) == 0) {
                printf("  * %-20s (active)\n", slash);
            } else {
                printf("    %-20s\n", slash);
            }
            ++found;
        }
        pclose(pipe);
        if (found == 0) puts("  (none)  Run: solcli wallet new");
    }

    puts("\nCommands:");
    puts("  solcli wallet active        Show active wallet details");
    puts("  solcli wallet use <name>    Switch active wallet");
    puts("  solcli wallet cluster       Show / set network");

    free(wallets_dir); free(active); free(cluster); free(active_keypair);
    return 0;
}

static int wallet_active(void) {
    char *kp = resolve_active_keypair();
    char *active = read_active_wallet();
    char *cluster = read_wallet_cluster();
    char *cmd = NULL;
    size_t used = 0;
    int ret = 1;

    if (kp == NULL) {
        free(active);
        free(cluster);
        return 1;
    }

    if (active != NULL) {
        printf("Wallet:  %s\n", active);
    }
    printf("Keypair: %s\n", kp);
    printf("Network: %s\n", cluster);

    if (append_format(&cmd, &used,
            "bash -lc '" WALLET_ENV " && solana-keygen pubkey \"%s\"'", kp) != 0) {
        goto done;
    }

    {
        char *addr = run_command_capture(cmd);
        if (addr != NULL) {
            if (active != NULL) printf("Wallet:  %s\n", active);
            printf("Address: %s\n", addr);
            free(addr);
            ret = 0;
        } else {
            fprintf(stderr, "Could not read keypair. File may be corrupt.\n");
        }
    }

done:
    free(kp);
    free(active);
    free(cluster);
    free(cmd);
    return ret;
}

static int wallet_use(int argc, char **argv) {
    char *name, *kp;

    if (argc >= 4) {
        name = sanitize_wallet_name(argv[3]);
    } else {
        char *raw = read_input_line("Wallet name: ");
        if (raw == NULL) return 1;
        name = (raw[0] == '\0') ? strdup("default") : sanitize_wallet_name(raw);
        free(raw);
    }
    if (name == NULL) return 1;

    kp = wallet_keypair_path(name);
    if (kp == NULL || !path_exists(kp)) {
        fprintf(stderr, "Wallet '%s' not found.\n", name);
        fprintf(stderr, "Run `solcli wallet list` to see available wallets.\n");
        free(name); free(kp);
        return 1;
    }

    write_active_wallet(name);
    printf("Active wallet: %s\n  Keypair: %s\n", name, kp);
    free(name); free(kp);
    return 0;
}

static int wallet_address(void) {
    char *kp = resolve_active_keypair();
    char *active = read_active_wallet();
    char *cmd = NULL;
    size_t used = 0;
    int ret = 1;

    if (kp == NULL) { free(active); return 1; }

    if (append_format(&cmd, &used,
            "bash -lc '" WALLET_ENV " && solana-keygen pubkey \"%s\"'", kp) != 0)
        goto done;

    {
        char *addr = run_command_capture(cmd);
        if (addr != NULL) {
            if (active != NULL) printf("Wallet:  %s\n", active);
            printf("Address: %s\n", addr);
            free(addr);
            ret = 0;
        } else {
            fprintf(stderr, "Could not read keypair. File may be corrupt.\n");
        }
    }

done:
    free(kp); free(active); free(cmd);
    return ret;
}

static int wallet_balance(void) {
    char *kp = resolve_active_keypair();
    char *cluster = read_wallet_cluster();
    char *active = read_active_wallet();
    char *cmd = NULL;
    size_t used = 0;
    int ret = 1;

    if (kp == NULL) { free(cluster); free(active); return 1; }

    {
        char *pubcmd = NULL;
        size_t pu = 0;
        if (append_format(&pubcmd, &pu,
                "bash -lc '" WALLET_ENV " && solana-keygen pubkey \"%s\"'", kp) == 0) {
            char *addr = run_command_capture(pubcmd);
            if (active != NULL) printf("Wallet:  %s\n", active);
            if (addr != NULL) { printf("Address: %s\n", addr); free(addr); }
        }
        free(pubcmd);
    }

    printf("Network: %s\n", cluster);
    printf("Balance: ");
    fflush(stdout);

    if (append_format(&cmd, &used,
            "bash -lc '" WALLET_ENV " && solana balance"
            " --keypair \"%s\" --url %s'",
            kp, cluster) != 0) goto done;

    {
        char *bal = run_command_capture(cmd);
        if (bal != NULL) { printf("%s\n", bal); free(bal); ret = 0; }
        else fprintf(stderr, "(failed to fetch balance)\n");
    }

done:
    free(kp); free(cluster); free(active); free(cmd);
    return ret;
}

static int wallet_airdrop(int argc, char **argv) {
    char *kp = resolve_active_keypair();
    char *cluster = read_wallet_cluster();
    const char *amount;
    char *amount_input = NULL;
    char *cmd = NULL;
    size_t used = 0;
    int ret = 1;

    if (kp == NULL) { free(cluster); return 1; }

    if (strcmp(cluster, "mainnet-beta") == 0) {
        fprintf(stderr, "Airdrop is not available on mainnet.\n");
        fprintf(stderr, "Switch to devnet: solcli wallet cluster devnet\n");
        free(kp); free(cluster);
        return 1;
    }

    if (argc >= 4) {
        amount = argv[3];
    } else {
        amount_input = read_input_line("Amount in SOL (Enter for 1): ");
        if (amount_input == NULL) { free(kp); free(cluster); return 1; }
        amount = (amount_input[0] == '\0') ? "1" : amount_input;
    }

    printf("Requesting %s SOL airdrop on %s...\n\n", amount, cluster);

    if (append_format(&cmd, &used,
            WALLET_ENV " && solana airdrop %s"
            " --keypair \"%s\" --url %s",
            amount, kp, cluster) != 0) goto done;

    if (run_shell_command_streaming("airdrop", cmd) == 0) ret = 0;

done:
    free(kp); free(cluster); free(amount_input); free(cmd);
    return ret;
}

static int wallet_send(int argc, char **argv) {
    char *kp = resolve_active_keypair();
    char *cluster = read_wallet_cluster();
    char *recipient = NULL;
    char *amount_str = NULL;
    char *cmd = NULL;
    size_t used = 0;
    int ret = 1;

    if (kp == NULL) { free(cluster); return 1; }

    recipient = (argc >= 4) ? strdup(argv[3]) : read_input_line("Recipient address: ");
    if (recipient == NULL || recipient[0] == '\0') {
        fprintf(stderr, "Recipient address is required.\n");
        free(recipient); free(kp); free(cluster);
        return 1;
    }

    amount_str = (argc >= 5) ? strdup(argv[4]) : read_input_line("Amount in SOL: ");
    if (amount_str == NULL || amount_str[0] == '\0') {
        fprintf(stderr, "Amount is required.\n");
        free(recipient); free(amount_str); free(kp); free(cluster);
        return 1;
    }

    if (strcmp(cluster, "mainnet-beta") == 0) {
        char *confirm;
        printf("WARNING: You are about to send %s SOL on mainnet to:\n  %s\n",
               amount_str, recipient);
        confirm = read_input_line("Type 'yes' to confirm: ");
        if (confirm == NULL || strcmp(confirm, "yes") != 0) {
            puts("Cancelled.");
            free(confirm); free(recipient); free(amount_str); free(kp); free(cluster);
            return 0;
        }
        free(confirm);
    }

    printf("Sending %s SOL to %s on %s...\n\n", amount_str, recipient, cluster);

    if (append_format(&cmd, &used,
            WALLET_ENV " && solana transfer %s %s"
            " --keypair \"%s\" --url %s --allow-unfunded-recipient",
            recipient, amount_str, kp, cluster) != 0) goto done;

    if (run_shell_command_streaming("transfer SOL", cmd) == 0) ret = 0;

done:
    free(recipient); free(amount_str); free(kp); free(cluster); free(cmd);
    return ret;
}

static int wallet_cluster(int argc, char **argv) {
    if (argc >= 4) {
        const char *input = argv[3];
        const char *normalized = normalize_cluster_name(input);

        if (normalized == NULL) {
            fprintf(stderr, "Unknown cluster: %s\n", input);
            fprintf(stderr, "Use: devnet, testnet, mainnet\n");
            return 1;
        }

        write_wallet_cluster(normalized);
        clear_custom_rpc_url();
        printf("Network set to: %s\n", normalized);
        return 0;
    }

    {
        char *c = read_wallet_cluster();
        printf("Current network: %s\n", c);
        puts("Change with: solcli wallet cluster [devnet|testnet|mainnet]");
        free(c);
    }
    return 0;
}

static int network_list(void) {
    char *cluster = read_wallet_cluster();
    char *custom = read_custom_rpc_url();
    const char *items[][2] = {
        { "devnet", "https://api.devnet.solana.com" },
        { "testnet", "https://api.testnet.solana.com" },
        { "mainnet-beta", "https://api.mainnet-beta.solana.com" }
    };

    puts("Networks");
    puts("────────────────────────────────────────────────────");
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        const char *mark = (cluster != NULL && strcmp(cluster, items[i][0]) == 0) ? "*" : " ";
        printf("  %s %-12s %s\n", mark, items[i][0], items[i][1]);
    }

    if (custom != NULL) {
        printf("\nCustom RPC: %s\n", custom);
        puts("Active RPC source: custom URL");
    } else {
        puts("\nActive RPC source: selected network");
    }

    free(cluster);
    free(custom);
    return 0;
}

static int network_use(int argc, char **argv) {
    const char *normalized;

    if (argc < 4) {
        fprintf(stderr, "Usage: solcli network use [devnet|testnet|mainnet]\n");
        return 1;
    }

    normalized = normalize_cluster_name(argv[3]);
    if (normalized == NULL) {
        fprintf(stderr, "Unknown network: %s\n", argv[3]);
        fprintf(stderr, "Use: devnet, testnet, mainnet\n");
        return 1;
    }

    if (write_wallet_cluster(normalized) != 0) {
        fprintf(stderr, "Failed to save network selection.\n");
        return 1;
    }
    clear_custom_rpc_url();

    printf("Network set to: %s\n", normalized);
    printf("RPC URL: %s\n", network_url_for_cluster(normalized));
    return 0;
}

static int cmd_network(int argc, char **argv) {
    const char *sub = (argc >= 3) ? argv[2] : "list";

    if (strcmp(sub, "list") == 0) return network_list();
    if (strcmp(sub, "use") == 0) return network_use(argc, argv);

    fprintf(stderr, "Unknown network subcommand: %s\n", sub);
    fprintf(stderr, "Available: list, use\n");
    return 1;
}

static int rpc_current(void) {
    char *cluster = read_wallet_cluster();
    char *custom = read_custom_rpc_url();
    char *url = current_rpc_url();

    if (url == NULL) {
        fprintf(stderr, "Could not resolve current RPC URL.\n");
        free(cluster);
        free(custom);
        return 1;
    }

    printf("Network: %s\n", cluster != NULL ? cluster : "devnet");
    printf("RPC URL: %s\n", url);
    printf("Source:  %s\n", custom != NULL ? "custom" : "network");

    free(cluster);
    free(custom);
    free(url);
    return 0;
}

static int rpc_set(int argc, char **argv) {
    const char *url;

    if (argc < 4) {
        fprintf(stderr, "Usage: solcli rpc set <url>\n");
        return 1;
    }

    url = argv[3];
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        fprintf(stderr, "RPC URL must start with http:// or https://\n");
        return 1;
    }

    if (write_custom_rpc_url(url) != 0) {
        fprintf(stderr, "Failed to save RPC URL.\n");
        return 1;
    }

    printf("Custom RPC URL set: %s\n", url);
    return 0;
}

static int cmd_rpc(int argc, char **argv) {
    const char *sub = (argc >= 3) ? argv[2] : "current";

    if (strcmp(sub, "current") == 0) return rpc_current();
    if (strcmp(sub, "set") == 0) return rpc_set(argc, argv);

    fprintf(stderr, "Unknown rpc subcommand: %s\n", sub);
    fprintf(stderr, "Available: set, current\n");
    return 1;
}

static int cmd_ping(void) {
    char *url = current_rpc_url();
    char *response = NULL;
    long status = 0;
    double latency = 0.0;
    int ret;

    if (url == NULL) {
        fprintf(stderr, "Could not resolve current RPC URL.\n");
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    ret = rpc_request(url, "getHealth", &response, &status, &latency);
    curl_global_cleanup();

    printf("RPC URL: %s\n", url);
    printf("HTTP:    %ld\n", status);
    printf("Latency: %.0f ms\n", latency);

    if (ret == 0 && response != NULL && strstr(response, "\"result\":\"ok\"") != NULL) {
        puts("Status:  ok");
        ret = 0;
    } else {
        puts("Status:  failed");
        if (response != NULL) {
            printf("Response: %s\n", response);
        }
        ret = 1;
    }

    free(url);
    free(response);
    return ret;
}

static int cmd_health(void) {
    char *url = current_rpc_url();
    char *health_response = NULL;
    char *version_response = NULL;
    char *version = NULL;
    long health_status = 0;
    long version_status = 0;
    double health_latency = 0.0;
    double version_latency = 0.0;
    int health_ok;
    int version_ok;
    bool health_result_ok;

    if (url == NULL) {
        fprintf(stderr, "Could not resolve current RPC URL.\n");
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    health_ok = rpc_request(url, "getHealth", &health_response, &health_status, &health_latency);
    version_ok = rpc_request(url, "getVersion", &version_response, &version_status, &version_latency);
    curl_global_cleanup();

    puts("RPC health");
    puts("────────────────────────────────────────────────────");
    printf("RPC URL: %s\n", url);
    health_result_ok = health_ok == 0 && health_response != NULL && strstr(health_response, "\"result\":\"ok\"") != NULL;

    printf("Health:  %s (HTTP %ld, %.0f ms)\n",
           health_result_ok ? "ok" : "failed",
           health_status,
           health_latency);
    printf("Version: %s (HTTP %ld, %.0f ms)\n",
           version_ok == 0 ? "reachable" : "failed",
           version_status,
           version_latency);

    if (version_response != NULL) {
        version = extract_json_field(version_response, "\"solana-core\"");
        if (version != NULL) {
            printf("Core:    %s\n", version);
        }
    }

    if (health_response != NULL && health_ok != 0) {
        printf("Health response: %s\n", health_response);
    }
    if (version_response != NULL && version_ok != 0) {
        printf("Version response: %s\n", version_response);
    }

    free(url);
    free(health_response);
    free(version_response);
    free(version);
    return (health_result_ok && version_ok == 0) ? 0 : 1;
}

static int cmd_wallet(int argc, char **argv) {
    const char *sub = (argc >= 3) ? argv[2] : NULL;

    if (sub == NULL)                      return wallet_address();
    if (strcmp(sub, "new") == 0)          return wallet_new();
    if (strcmp(sub, "import") == 0)       return wallet_import();
    if (strcmp(sub, "list") == 0)         return wallet_list();
    if (strcmp(sub, "active") == 0)       return wallet_active();
    if (strcmp(sub, "assign") == 0)       return cmd_wallet_assign();
    if (strcmp(sub, "use") == 0)          return wallet_use(argc, argv);
    if (strcmp(sub, "address") == 0)      return wallet_address();
    if (strcmp(sub, "balance") == 0)      return wallet_balance();
    if (strcmp(sub, "airdrop") == 0)      return wallet_airdrop(argc, argv);
    if (strcmp(sub, "send") == 0)         return wallet_send(argc, argv);
    if (strcmp(sub, "cluster") == 0)      return wallet_cluster(argc, argv);

    fprintf(stderr, "Unknown wallet subcommand: %s\n", sub);
    fprintf(stderr, "Available: new, import, list, active, assign, use, address, balance, airdrop, send, cluster\n");
    return 1;
}

static int call_openai(
    const char *api_key,
    const char *system_prompt,
    const ChatHistory *history,
    const char *question,
    char **answer_out
) {
    const char *model = getenv("OPENAI_MODEL");
    CURL *curl;
    CURLcode result;
    Buffer response = {0};
    struct curl_slist *headers = NULL;
    char *escaped_question = NULL;
    char *escaped_system = NULL;
    char *payload = NULL;
    size_t payload_len = 0;
    long status_code = 0;

    if (model == NULL || *model == '\0') {
        model = DEFAULT_MODEL;
    }

    escaped_question = json_escape(question);
    escaped_system = json_escape(system_prompt);
    if (escaped_question == NULL || escaped_system == NULL) {
        fprintf(stderr, "Out of memory while preparing JSON.\n");
        free(escaped_question);
        free(escaped_system);
        return 1;
    }

    if (append_format(
            &payload,
            &payload_len,
            "{"
            "\"model\":\"%s\","
            "\"messages\":["
            "{\"role\":\"system\",\"content\":\"%s\"}",
            model,
            escaped_system) != 0) {
        fprintf(stderr, "Failed to build request body.\n");
        free(escaped_question);
        free(escaped_system);
        free(payload);
        return 1;
    }

    if (history != NULL) {
        for (size_t i = 0; i < history->len; ++i) {
            char *escaped_content = json_escape(history->items[i].content);

            if (escaped_content == NULL) {
                fprintf(stderr, "Out of memory while preparing message history.\n");
                free(escaped_question);
                free(escaped_system);
                free(payload);
                return 1;
            }

            if (append_format(
                    &payload,
                    &payload_len,
                    ",{\"role\":\"%s\",\"content\":\"%s\"}",
                    history->items[i].role,
                    escaped_content) != 0) {
                fprintf(stderr, "Failed to append message history.\n");
                free(escaped_content);
                free(escaped_question);
                free(escaped_system);
                free(payload);
                return 1;
            }

            free(escaped_content);
        }
    }

    if (append_format(
            &payload,
            &payload_len,
            ",{\"role\":\"user\",\"content\":\"%s\"}]"
            "}",
            escaped_question) != 0) {
        fprintf(stderr, "Failed to finalize request body.\n");
        free(escaped_question);
        free(escaped_system);
        free(payload);
        return 1;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        fprintf(stderr, "Failed to initialize libcurl.\n");
        free(escaped_question);
        free(escaped_system);
        free(payload);
        return 1;
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");
    {
        char auth_header[1024];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
        headers = curl_slist_append(headers, auth_header);
    }

    curl_easy_setopt(curl, CURLOPT_URL, OPENAI_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload_len);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SolCLI/0.1");

    result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        fprintf(stderr, "OpenAI request failed: %s\n", curl_easy_strerror(result));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(escaped_question);
        free(escaped_system);
        free(payload);
        free(response.data);
        return 1;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    if (status_code < 200 || status_code >= 300) {
        char *error_message = extract_json_string_after(response.data ? response.data : "", "\"message\":\"");
        fprintf(stderr, "OpenAI HTTP error: %ld\n", status_code);
        if (error_message != NULL) {
            fprintf(stderr, "Detail: %s\n", error_message);
        }
        free(error_message);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(escaped_question);
        free(escaped_system);
        free(payload);
        free(response.data);
        return 1;
    }

    *answer_out = extract_message_content(response.data ? response.data : "");
    if (*answer_out == NULL) {
        fprintf(stderr, "Failed to parse API response.\n");
        if (response.data != NULL) {
            fprintf(stderr, "Raw response: %s\n", response.data);
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(escaped_question);
        free(escaped_system);
        free(payload);
        free(response.data);
        return 1;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(escaped_question);
    free(escaped_system);
    free(payload);
    free(response.data);
    return 0;
}

static const char *get_agent_system_prompt(void) {
    return
        "You are the expert Solana assistant inside SolCLI. "
        "Only help with Solana, Anchor, SPL, wallets, RPC, test validator, program deployment, "
        "IDLs, account structures, and related development workflows. "
        "You operate in agent mode: be diagnostic, proactive, and workflow-oriented. "
        SOLANA_SKILLS_CONTEXT
        "When the user asks about build failures, version conflicts, GLIBC errors, or Anchor issues, prioritize diagnosis using "
        "version compatibility and common error patterns, and ask for `solcli version` output when versions matter. "
        "When the user asks about security, include signer checks, account validation, authority constraints, and deployment review points. "
        "When the user asks about testing, prefer a practical pyramid: fast local tests first, then isolated instruction checks, then realistic integration tests. "
        "When useful, suggest the most relevant official skill area by name so the user knows which Solana topic they are dealing with. "
        "Answer in English. Be concise, technically correct, and actionable. "
        "Prefer this output style: "
        "1) direct answer, "
        "2) concrete commands or code steps when relevant, "
        "3) pitfalls or version notes if they matter, "
        "4) one short follow-up question only if required to proceed.";
}

static const char *get_ask_system_prompt(void) {
    return
        "You are the SolCLI ask assistant. "
        "Only answer questions about Solana, Anchor, SPL, wallets, RPC, test validator, program deployment, "
        "IDLs, account structures, and related development workflows. "
        "You operate in ask mode: be question-focused, direct, and educational. "
        SOLANA_SKILLS_CONTEXT
        "Do not behave like a general agent unless the question clearly requires troubleshooting structure. "
        "Default to answering the exact question first. "
        "When the user asks about version conflicts or toolchain issues, mention `solcli version` as the fastest way to inspect installed versions. "
        "When relevant, mention the official skill area that best matches the question, such as Version Compatibility Matrix, Security Checklist, or Testing Strategy. "
        "Answer in English. Keep the response concise, technically correct, and easy to scan. "
        "Prefer this output style: "
        "1) direct answer, "
        "2) short command or code examples if useful, "
        "3) one short caution or note if it matters. "
        "Avoid unnecessary follow-up questions.";
}

static int run_chat_mode(
    int argc,
    char **argv,
    int argument_start_index,
    const char *system_prompt,
    const char *welcome_line_1,
    const char *welcome_line_2,
    const char *prompt_label,
    int (*local_action_handler)(const char *input, ChatHistory *history, AgentState *state)
) {
    char *answer = NULL;
    char *question = NULL;
    const char *api_key;
    ChatHistory history = {0};
    AgentState agent_state = {0};

    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (argc > argument_start_index) {
        question = join_arguments(argument_start_index, argc, argv);
        if (question == NULL) {
            fprintf(stderr, "Failed to prepare question text.\n");
            curl_global_cleanup();
            return 1;
        }

        if (local_action_handler != NULL) {
            int action_result = local_action_handler(question, &history, &agent_state);
            if (action_result != -1) {
                free(question);
                clear_agent_state(&agent_state);
                free_chat_history(&history);
                curl_global_cleanup();
                return action_result;
            }
        }

        api_key = getenv("OPENAI_API_KEY");
        if (api_key == NULL || *api_key == '\0') {
            fprintf(stderr, "OPENAI_API_KEY is not set.\n");
            fprintf(stderr, "Example: export OPENAI_API_KEY=\"your-key\"\n");
            free(question);
            curl_global_cleanup();
            return 1;
        }

        if (call_openai(api_key, system_prompt, &history, question, &answer) != 0) {
            free(question);
            clear_agent_state(&agent_state);
            free_chat_history(&history);
            curl_global_cleanup();
            return 1;
        }

        printf("\n%s\n", answer);
        add_chat_message(&history, "user", question);
        add_chat_message(&history, "assistant", answer);
        free(question);
        free(answer);
        clear_agent_state(&agent_state);
        free_chat_history(&history);
        curl_global_cleanup();
        return 0;
    }

    puts(welcome_line_1);
    puts(welcome_line_2);
    puts("Type `exit` or `quit` to leave.\n");

    while (true) {
        char *line = NULL;
        size_t line_cap = 0;
        ssize_t read_len;

        printf("%s", prompt_label);
        fflush(stdout);

        read_len = getline(&line, &line_cap, stdin);
        if (read_len < 0) {
            free(line);
            putchar('\n');
            break;
        }

        while (read_len > 0 && isspace((unsigned char)line[read_len - 1])) {
            line[--read_len] = '\0';
        }

        if (read_len == 0) {
            free(line);
            continue;
        }

        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            free(line);
            break;
        }

        if (local_action_handler != NULL) {
            int action_result = local_action_handler(line, &history, &agent_state);
            if (action_result != -1) {
                free(line);
                if (action_result != 0) {
                    clear_agent_state(&agent_state);
                    free_chat_history(&history);
                    curl_global_cleanup();
                    return action_result;
                }
                continue;
            }
        }

        api_key = getenv("OPENAI_API_KEY");
        if (api_key == NULL || *api_key == '\0') {
            fprintf(stderr, "OPENAI_API_KEY is not set.\n");
            fprintf(stderr, "Example: export OPENAI_API_KEY=\"your-key\"\n");
            free(line);
            curl_global_cleanup();
            return 1;
        }

        if (call_openai(api_key, system_prompt, &history, line, &answer) == 0) {
            printf("\n%s\n\n", answer);
            add_chat_message(&history, "user", line);
            add_chat_message(&history, "assistant", answer);
            free(answer);
            answer = NULL;
        }

        free(line);
    }

    clear_agent_state(&agent_state);
    free_chat_history(&history);
    curl_global_cleanup();
    return 0;
}

static int agent_loop(int argc, char **argv) {
    return run_chat_mode(
        argc,
        argv,
        2,
        get_agent_system_prompt(),
        "Welcome to SolCLI agent mode.",
        "Use this for troubleshooting, planning, guided workflows, local actions, and session-aware Solana problem-solving.",
        "solcli-agent> ",
        handle_agent_local_action
    );
}

static int ask_loop(int argc, char **argv) {
    return run_chat_mode(
        argc,
        argv,
        2,
        get_ask_system_prompt(),
        "Welcome to SolCLI ask mode.",
        "Use this for focused Solana Q&A, short explanations, and direct technical answers.",
        "solcli-ask> ",
        NULL
    );
}

int main(int argc, char **argv) {
    if (argc == 1) {
        print_usage();
        return 0;
    }

    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage();
        return 0;
    }

    if (strcmp(argv[1], "init") == 0) {
        return cmd_init(argc, argv);
    }

    if (strcmp(argv[1], "env") == 0) {
        return cmd_env(argc, argv);
    }

    if (strcmp(argv[1], "wallet") == 0) {
        return cmd_wallet(argc, argv);
    }

    if (strcmp(argv[1], "network") == 0) {
        return cmd_network(argc, argv);
    }

    if (strcmp(argv[1], "rpc") == 0) {
        return cmd_rpc(argc, argv);
    }

    if (strcmp(argv[1], "ping") == 0) {
        return cmd_ping();
    }

    if (strcmp(argv[1], "health") == 0) {
        return cmd_health();
    }

    if (strcmp(argv[1], "build") == 0) {
        return cmd_build(argc, argv);
    }

    if (strcmp(argv[1], "test") == 0) {
        return cmd_test(argc, argv);
    }

    if (strcmp(argv[1], "deploy") == 0) {
        return cmd_deploy(argc, argv);
    }

    if (strcmp(argv[1], "clean") == 0) {
        return cmd_clean();
    }

    if (strcmp(argv[1], "download") == 0) {
        return env_install();
    }

    if (strcmp(argv[1], "agent") == 0) {
        return agent_loop(argc, argv);
    }

    if (strcmp(argv[1], "ask") == 0) {
        return ask_loop(argc, argv);
    }

    if (strcmp(argv[1], "version") == 0) {
        return show_versions();
    }

    fprintf(stderr, "Unknown command: %s\n\n", argv[1]);
    print_usage();
    return 1;
}

static int solcli_program_show(const char *program_id) {
    char command[256];
    snprintf(command, sizeof(command), "solcli program show %s", program_id);
    return run_shell_command_streaming("Show Program Info", command);
}

static int solcli_program_logs(const char *program_id) {
    char command[256];
    snprintf(command, sizeof(command), "solcli program logs %s", program_id);
    return run_shell_command_streaming("Show Program Logs", command);
}

static int solcli_program_accounts(const char *program_id) {
    char command[256];
    snprintf(command, sizeof(command), "solcli program accounts %s", program_id);
    return run_shell_command_streaming("Show Program Accounts", command);
}

static int solcli_program_idl(const char *program_id) {
    char command[256];
    snprintf(command, sizeof(command), "solcli program idl %s", program_id);
    return run_shell_command_streaming("Show Program IDL", command);
}

static void print_wallet_address_with_prefix(const char *address) {
    if (address == NULL) {
        printf("No active wallet address found.\n");
        return;
    }

    printf("Active wallet address: %s\n", address);

    // Check if the address starts with '0x'
    if (strncmp(address, "0x", 2) != 0) {
        char prefixed_address[256];
        snprintf(prefixed_address, sizeof(prefixed_address), "0x%s", address);
        printf("Address with 0x prefix: %s\n", prefixed_address);
    }
}

// Debugging: Add a test call to print_wallet_address_with_prefix to verify its functionality
char *test_address = "CcthDLqqj8ATBvqaEHRYRdFJGuCCidLU8N7PRVas5mB2";
printf("\n--- Debugging Wallet Address Function ---\n");
print_wallet_address_with_prefix(test_address);
printf("\n--- End Debugging ---\n");
