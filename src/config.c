#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utils.h"

#define _atoi atoi

/// https://github.com/Teklad/tconfig > https://github.com/gimli2/tconfig
#include "config.h"

static ini_entry_s* _ini_entry_create(ini_section_s* section, const char* key, const char* value) {
    if ((section->size % 10) == 0) {
        section->entry =
                (ini_entry_s*)realloc(section->entry, (10 + section->size) * sizeof(ini_entry_s));
    }
    ini_entry_s* entry = &section->entry[section->size++];
    entry->key = (char*)malloc((strlen(key) + 1) * sizeof(char));
    entry->value = (char*)malloc((strlen(value) + 1) * sizeof(char));
    debug_printf("key: %s = value: %s\n", key, value);
    strcpy(entry->key, key);
    strcpy(entry->value, value);
    return entry;
}

static ini_section_s* _ini_section_create(ini_table_s* table, const char* section_name) {
    if ((table->size % 10) == 0) {
        table->section =
                (ini_section_s*)realloc(table->section, (10 + table->size) * sizeof(ini_section_s));
    }
    ini_section_s* section = &table->section[table->size++];
    section->size = 0;
    section->name = (char*)malloc((strlen(section_name) + 1) * sizeof(char));
    strcpy(section->name, section_name);
    section->entry = (ini_entry_s*)malloc(10 * sizeof(ini_entry_s));
    return section;
}

// Ctn: make this non-static
ini_section_s* _ini_section_find(ini_table_s* table, const char* name) {
    for (int i = 0; i < table->size; i++) {
        if (strcmp(table->section[i].name, name) == 0) {
            return &table->section[i];
        }
    }
    return NULL;
}

static ini_entry_s* _ini_entry_find(ini_section_s* section, const char* key) {
    for (int i = 0; i < section->size; i++) {
        if (strcmp(section->entry[i].key, key) == 0) {
            return &section->entry[i];
        }
    }
    return NULL;
}

static ini_entry_s* _ini_entry_get(ini_table_s* table, const char* section_name, const char* key) {
    ini_section_s* section = _ini_section_find(table, section_name);
    if (section == NULL) {
        return NULL;
    }

    ini_entry_s* entry = _ini_entry_find(section, key);
    if (entry == NULL) {
        return NULL;
    }
    return entry;
}

ini_table_s* ini_table_create(void) {
    ini_table_s* table = (ini_table_s*)malloc(sizeof(ini_table_s));
    if (table == NULL) return NULL;
    table->size = 0;
    table->section = (ini_section_s*)malloc(10 * sizeof(ini_section_s));
    if (table->section == NULL) {
        free(table);
        return NULL;
    }
    return table;
}

void ini_table_destroy(ini_table_s* table) {
    for (int i = 0; i < table->size; i++) {
        ini_section_s* section = &table->section[i];
        for (int q = 0; q < section->size; q++) {
            ini_entry_s* entry = &section->entry[q];
            free(entry->key);
            free(entry->value);
        }
        free(section->entry);
        free(section->name);
    }
    free(table->section);
    free(table);
}

bool ini_table_read_from_file(ini_table_s* table, const char* file) {
    FILE* f = fopen(file, "r");
    if (f == NULL) return false;

    // Skip the UTF-8 byte order mark some Windows editors add
    if (!(fgetc(f) == 0xEF && fgetc(f) == 0xBB && fgetc(f) == 0xBF)) rewind(f);

    enum { Section, Key, Value, Comment } state = Section;
    int c;
    int position = 0;
    int spaces = 0;
    int buffer_size = 128 * sizeof(char);
    char* buf = (char*)malloc(buffer_size);
    char* value = NULL;
    bool eof_seen = false;

    ini_section_s* current_section = NULL;
    if (buf == NULL) {
        fclose(f);
        return false;
    }
    memset(buf, '\0', buffer_size);

    while (1) {
        c = fgetc(f);
        if (c == EOF) {
            // EOF ends the last line like a newline would, then stops the loop
            if (eof_seen) break;
            eof_seen = true;
        }

        if (c == '\r') continue;
        // Room for the pending spaces, this character and the terminator
        if (position + spaces + 2 > buffer_size) {
            int new_size = position + spaces + 2 + 128 * (int) sizeof(char);
            size_t value_offset = value == NULL ? 0 : value - buf;
            char* new_buf = (char*)realloc(buf, new_size);
            if (new_buf == NULL) {
                free(buf);
                fclose(f);
                return false;
            }
            buf = new_buf;
            memset(buf + position, '\0', new_size - position);
            buffer_size = new_size;

            if (value != NULL) value = buf + value_offset;
        }
        switch (c) {
            case ' ':
                switch (state) {
                    case Value:
                        if (value[0] != '\0') spaces++;
                        break;
                    default:
                        if (buf[0] != '\0') spaces++;
                        break;
                }
                break;
            case ';':
                while (c != EOF && c != '\n') {
                    c = fgetc(f);
                }
                if (c == EOF) eof_seen = true;
                // fallthrough
            case '\n':
                // fallthrough
            case EOF:
                if (state == Value) {
                    if (current_section == NULL) {
                        current_section = _ini_section_create(table, "");
                    }
                    _ini_entry_create(current_section, buf, value);
                    value = NULL;
                } else if (strlen(buf) > 1 && position && state == Key) {
                    if (current_section == NULL) {
                        current_section = _ini_section_create(table, "");
                    }
                    _ini_entry_create(current_section, buf, "");
                } else if (state == Comment) {
                    if (current_section == NULL) {
                        current_section = _ini_section_create(table, "");
                    }
                    _ini_entry_create(current_section, buf, "");
                } else if (state == Section) {
                    debug_printf("Section `%s' missing `]' operator.", buf);
                } else if (state == Key && position) {
                    debug_printf("Key `%s' missing `=' operator.", buf);
                }
                memset(buf, '\0', buffer_size);
                state = Key;
                position = 0;
                spaces = 0;
                break;
            case '[':
                if (state != Value) {
                    // A section header: drop anything before it on this line
                    memset(buf, '\0', buffer_size);
                    position = 0;
                    spaces = 0;
                    state = Section;
                    break;
                }
                goto append;
            case ']':
                if (state != Section) goto append;
                current_section = _ini_section_create(table, buf);
                memset(buf, '\0', buffer_size);
                position = 0;
                spaces = 0;
                state = Key;
                break;
            case '=':
                if (state == Key) {
                    state = Value;
                    buf[position++] = '\0';
                    value = buf + position;
                    spaces = 0;
                    continue;
                }
                goto append;
            default:
            append:
                for (; spaces > 0; spaces--) buf[position++] = ' ';
                buf[position++] = c;
                break;
        }
    }
    free(buf);
    fclose(f);
    return true;
}

bool ini_table_write_to_file(ini_table_s* table, const char* file) {
    FILE* f = fopen(file, "w+");
    if (f == NULL) return false;
    for (int i = 0; i < table->size; i++) {
        ini_section_s* section = &table->section[i];
        fprintf(f, i > 0 ? "\n[%s]\n" : "[%s]\n", section->name);
        for (int q = 0; q < section->size; q++) {
            ini_entry_s* entry = &section->entry[q];
            if (entry->key[0] == ';') {
                fprintf(f, "%s\n", entry->key);
            } else {
                fprintf(f, "%s = %s\n", entry->key, entry->value);
            }
        }
    }
    if (fflush(f) == 0) fsync(fileno(f));
    fclose(f);
    return true;
}

void ini_table_create_entry(ini_table_s* table, const char* section_name, const char* key,
                            const char* value) {
    ini_section_s* section = _ini_section_find(table, section_name);
    if (section == NULL) {
        section = _ini_section_create(table, section_name);
    }
    ini_entry_s* entry = _ini_entry_find(section, key);
    if (entry == NULL) {
        entry = _ini_entry_create(section, key, value);
    } else {
        free(entry->value);
        entry->value = (char*)malloc((strlen(value) + 1) * sizeof(char));
        strcpy(entry->value, value);
    }
}

bool ini_table_check_entry(ini_table_s* table, const char* section_name, const char* key) {
    return (_ini_entry_get(table, section_name, key) != NULL);
}

const char* ini_table_get_entry(ini_table_s* table, const char* section_name, const char* key) {
    ini_entry_s* entry = _ini_entry_get(table, section_name, key);
    if (entry == NULL) {
        return NULL;
    }
    return entry->value;
}

bool ini_table_get_entry_as_int(ini_table_s* table, const char* section_name, const char* key,
                                int* value) {
    const char* val = ini_table_get_entry(table, section_name, key);
    if (val == NULL) {
        return false;
    }
    *value = _atoi(val);
    return true;
}

bool ini_table_get_entry_as_bool(ini_table_s* table, const char* section_name, const char* key,
                                 bool* value) {
    const char* val = ini_table_get_entry(table, section_name, key);
    if (val == NULL) {
        return false;
    }
    if (strcasecmp(val, "on") == 0 || strcasecmp(val, "true") == 0 || strcasecmp(val, "1") == 0) {
        *value = true;
    } else {
        *value = false;
    }
    return true;
}

bool ini_parse_user_id(const char* text, int32_t* value) {
    const char* digits;
    const char* p;
    unsigned int base = 10;
    unsigned long long parsed = 0;

    while (*text == ' ' || *text == '\t') text++;
    digits = text;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        digits = text + 2;
    } else {
        for (p = text; *p; p++) {
            if (isxdigit((unsigned char)*p) && !isdigit((unsigned char)*p)) {
                base = 16;
                break;
            }
        }
    }

    for (p = digits; isxdigit((unsigned char)*p); p++) {
        unsigned int digit = isdigit((unsigned char)*p) ? (unsigned int)(*p - '0')
                                                        : (unsigned int)(tolower((unsigned char)*p) - 'a' + 10);
        if (digit >= base) return false;
        parsed = parsed * base + digit;
        if (parsed > 0xFFFFFFFFull) return false;
    }
    while (*p == ' ' || *p == '\t') p++;
    // 0 and 0xFFFFFFFF (-1) are not user ids
    if (p == digits || *p != '\0' || parsed == 0 || parsed == 0xFFFFFFFFull) return false;
    *value = (int32_t)(uint32_t)parsed;
    return true;
}

bool ini_table_get_entry_as_user_id(ini_table_s* table, const char* section_name, const char* key,
                                    int32_t* value) {
    const char* val = ini_table_get_entry(table, section_name, key);
    if (val == NULL) {
        return false;
    }
    return ini_parse_user_id(val, value);
}
