/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/* cJSON - Ultralightweight JSON parser in ANSI C. */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <float.h>

#include "cJSON.h"

/* define our own boolean type */
#ifdef true
#undef true
#endif
#define true ((cJSON_bool)1)

#ifdef false
#undef false
#endif
#define false ((cJSON_bool)0)

/* define isnan and isinf for ANSI C, if in C99 or above, isnan and isinf has been defined in math.h */
#ifndef isinf
#define isinf(d) (isnan((d - d)) && !isnan(d))
#endif
#ifndef isnan
#define isnan(d) (d != d)
#endif

typedef struct {
    const unsigned char *json;
    size_t position;
} error;
static error global_error = { NULL, 0 };

static unsigned char* global_ep = NULL;

static void *(*cJSON_malloc)(size_t sz) = malloc;
static void (*cJSON_free)(void *ptr) = free;

void cJSON_InitHooks(cJSON_Hooks* hooks)
{
    if (hooks == NULL)
    {
        cJSON_malloc = malloc;
        cJSON_free = free;
        return;
    }

    cJSON_malloc = (hooks->malloc_fn != NULL) ? hooks->malloc_fn : malloc;
    cJSON_free = (hooks->free_fn != NULL) ? hooks->free_fn : free;
}

/* Internal constructor. */
static cJSON *cJSON_New_Item(void)
{
    cJSON* node = (cJSON*)cJSON_malloc(sizeof(cJSON));
    if (node)
    {
        memset(node, '\0', sizeof(cJSON));
    }

    return node;
}

/* Delete a cJSON structure. */
void cJSON_Delete(cJSON *item)
{
    cJSON *next = NULL;
    while (item != NULL)
    {
        next = item->next;
        if (!(item->type & cJSON_IsReference) && (item->child != NULL))
        {
            cJSON_Delete(item->child);
        }
        if (!(item->type & cJSON_IsReference) && (item->valuestring != NULL))
        {
            cJSON_free(item->valuestring);
        }
        if (!(item->type & cJSON_StringIsConst) && (item->string != NULL))
        {
            cJSON_free(item->string);
        }
        cJSON_free(item);
        item = next;
    }
}

/* parse 4 digit hexadecimal number */
static unsigned parse_hex4(const unsigned char * const input)
{
    unsigned int h = 0;
    size_t i = 0;

    for (i = 0; i < 4; i++)
    {
        if ((input[i] >= '0') && (input[i] <= '9'))
        {
            h += (unsigned int) input[i] - '0';
        }
        else if ((input[i] >= 'A') && (input[i] <= 'F'))
        {
            h += (unsigned int) 10 + input[i] - 'A';
        }
        else if ((input[i] >= 'a') && (input[i] <= 'f'))
        {
            h += (unsigned int) 10 + input[i] - 'a';
        }
        else
        {
            return 0;
        }

        if (i < 3)
        {
            h = h << 4;
        }
    }

    return h;
}

/* Parse the input text into an unescaped cstring, and populate item. */
static const unsigned char *parse_string(cJSON * const item, const unsigned char *input)
{
    const unsigned char *input_pointer = input + 1;
    const unsigned char *input_end = input + 1;
    unsigned char *output_pointer = NULL;
    unsigned char *output = NULL;

    /* not a string! */
    if (*input != '\"')
    {
        return NULL;
    }

    {
        /* calculate approximate size of the output (overestimate) */
        size_t allocation_length = 0;
        size_t skipped_bytes = 0;
        while (((*input_end != '\"')) && (*input_end != '\0'))
        {
            /* is escape sequence */
            if (input_end[0] == '\\')
            {
                if (input_end[1] == '\0')
                {
                    /* prevent buffer overflow when last input is backslash */
                    return NULL;
                }
                skipped_bytes++;
                input_end++;
            }
            input_end++;
        }
        if (*input_end == '\0')
        {
            return NULL;
        }

        allocation_length = (size_t) (input_end - input) - skipped_bytes;
        output = (unsigned char*)cJSON_malloc(allocation_length + sizeof('\0'));
        if (output == NULL)
        {
            return NULL;
        }
    }

    output_pointer = output;
    while (input_pointer < input_end)
    {
        if (*input_pointer != '\\')
        {
            *output_pointer++ = *input_pointer++;
        }
        else
        {
            unsigned char sequence_length = 2;
            if ((input_end - input_pointer) < 1)
            {
                goto fail;
            }

            switch (input_pointer[1])
            {
                case 'b':
                    *output_pointer++ = '\b';
                    break;
                case 'f':
                    *output_pointer++ = '\f';
                    break;
                case 'n':
                    *output_pointer++ = '\n';
                    break;
                case 'r':
                    *output_pointer++ = '\r';
                    break;
                case 't':
                    *output_pointer++ = '\t';
                    break;
                case '\"':
                case '\\':
                case '/':
                    *output_pointer++ = input_pointer[1];
                    break;
                case 'u':
                    sequence_length = 6;
                    if ((input_end - input_pointer) < 6)
                    {
                        goto fail;
                    }
                    /* get codepoint */
                    {
                        unsigned int codepoint = parse_hex4(input_pointer + 2);
                        if (codepoint <= 0x7F)
                        {
                            *output_pointer++ = (unsigned char) codepoint;
                        }
                        else if (codepoint <= 0x7FF)
                        {
                            *output_pointer++ = (unsigned char)(0xC0 | ((codepoint >> 6) & 0x1F));
                            *output_pointer++ = (unsigned char)(0x80 | (codepoint & 0x3F));
                        }
                        else if (codepoint <= 0xFFFF)
                        {
                            *output_pointer++ = (unsigned char)(0xE0 | ((codepoint >> 12) & 0x0F));
                            *output_pointer++ = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
                            *output_pointer++ = (unsigned char)(0x80 | (codepoint & 0x3F));
                        }
                    }
                    break;
                default:
                    goto fail;
            }
            input_pointer += sequence_length;
        }
    }

    *output_pointer = '\0';

    item->type = cJSON_String;
    item->valuestring = (char*)output;

    return input_end + 1;

fail:
    if (output != NULL)
    {
        cJSON_free(output);
    }

    return NULL;
}

/* Render the cstring provided to an escaped version that can be printed. */
static char *print_string_ptr(const unsigned char * const input)
{
    const unsigned char *input_pointer = NULL;
    unsigned char *output = NULL;
    unsigned char *output_pointer = NULL;
    size_t output_length = 0;
    size_t escape_characters = 0;

    if (input == NULL)
    {
        return NULL;
    }

    for (input_pointer = input; *input_pointer; input_pointer++)
    {
        switch (*input_pointer)
        {
            case '\"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                escape_characters++;
                break;
            default:
                if (*input_pointer < 32)
                {
                    escape_characters += 5;
                }
                break;
        }
    }
    output_length = (size_t)(input_pointer - input) + escape_characters;

    output = (unsigned char*)cJSON_malloc(output_length + sizeof('\"') + sizeof('\"') + sizeof('\0'));
    if (output == NULL)
    {
        return NULL;
    }

    output_pointer = output;
    *output_pointer++ = '\"';
    for (input_pointer = input; *input_pointer; input_pointer++)
    {
        if ((*input_pointer > 31) && (*input_pointer != '\"') && (*input_pointer != '\\'))
        {
            *output_pointer++ = *input_pointer;
        }
        else
        {
            *output_pointer++ = '\\';
            switch (*input_pointer)
            {
                case '\\':
                    *output_pointer++ = '\\';
                    break;
                case '\"':
                    *output_pointer++ = '\"';
                    break;
                case '\b':
                    *output_pointer++ = 'b';
                    break;
                case '\f':
                    *output_pointer++ = 'f';
                    break;
                case '\n':
                    *output_pointer++ = 'n';
                    break;
                case '\r':
                    *output_pointer++ = 'r';
                    break;
                case '\t':
                    *output_pointer++ = 't';
                    break;
                default:
                    sprintf((char*)output_pointer, "u%04x", *input_pointer);
                    output_pointer += 5;
                    break;
            }
        }
    }
    *output_pointer++ = '\"';
    *output_pointer = '\0';

    return (char*)output;
}

/* Invote print_string_ptr (which is useful) on an item. */
static char *print_string(const cJSON * const item)
{
    return print_string_ptr((unsigned char*)item->valuestring);
}

/* Parser core - when encountering text, process appropriately. */
static const unsigned char *parse_value(cJSON * const item, const unsigned char *input);
static char *print_value(const cJSON * const item, int depth, cJSON_bool fmt);
static const unsigned char *parse_array(cJSON * const item, const unsigned char *input);
static char *print_array(const cJSON * const item, int depth, cJSON_bool fmt);
static const unsigned char *parse_object(cJSON * const item, const unsigned char *input);
static char *print_object(const cJSON * const item, int depth, cJSON_bool fmt);

/* Utility to skip whitespace and cr/lf */
static const unsigned char *skip_whitespace(const unsigned char *in)
{
    while (in && *in && ((unsigned char)*in <= 32))
    {
        in++;
    }

    return in;
}

/* Parse the input text to generate a number, and populate the result into item. */
static const unsigned char *parse_number(cJSON * const item, const unsigned char *input)
{
    double number = 0;
    unsigned char *after_end = NULL;

    if (input == NULL)
    {
        return NULL;
    }

    number = strtod((const char*)input, (char**)&after_end);
    if (input == after_end)
    {
        return NULL;
    }

    item->valuedouble = number;
    item->valueint = (int)number;
    item->type = cJSON_Number;

    return after_end;
}

/* Render the number nicely from the given item into a string. */
static char *print_number(const cJSON * const item)
{
    char *printed = NULL;
    double d = item->valuedouble;
    int length = 0;

    if ((fabs(floor(d) - d) <= DBL_EPSILON) && (fabs(d) < 1.0e60))
    {
        length = 21;
        printed = (char*)cJSON_malloc((size_t)length);
        if (printed)
        {
            sprintf(printed, "%.0f", d);
        }
    }
    else if ((fabs(d) < 1.0e-6) || (fabs(d) > 1.0e9))
    {
        length = 64;
        printed = (char*)cJSON_malloc((size_t)length);
        if (printed)
        {
            sprintf(printed, "%e", d);
        }
    }
    else
    {
        length = 64;
        printed = (char*)cJSON_malloc((size_t)length);
        if (printed)
        {
            sprintf(printed, "%f", d);
        }
    }

    return printed;
}

/* Parse the input text into an array, and populate the result. */
static const unsigned char *parse_array(cJSON * const item, const unsigned char *input)
{
    cJSON *child = NULL;
    cJSON *current_item = NULL;

    if (*input != '[')
    {
        return NULL;
    }

    item->type = cJSON_Array;
    input = skip_whitespace(input + 1);
    if (*input == ']')
    {
        return input + 1;
    }

    /* allocate first child */
    item->child = child = cJSON_New_Item();
    if (child == NULL)
    {
        return NULL;
    }

    /* parse the first value */
    input = skip_whitespace(parse_value(child, skip_whitespace(input)));
    if (input == NULL)
    {
        return NULL;
    }

    /* loop through the array */
    while (*input == ',')
    {
        cJSON *new_item = cJSON_New_Item();
        if (new_item == NULL)
        {
            return NULL;
        }

        /* add to array */
        child->next = new_item;
        new_item->prev = child;
        child = new_item;

        /* parse next value */
        input = skip_whitespace(parse_value(child, skip_whitespace(input + 1)));
        if (input == NULL)
        {
            return NULL;
        }
    }

    if (*input != ']')
    {
        return NULL;
    }

    return input + 1;
}

/* Render an array to text. */
static char *print_array(const cJSON * const item, int depth, cJSON_bool fmt)
{
    char **entries = NULL;
    char *out = NULL;
    char *ptr = NULL;
    char *ret = NULL;
    size_t len = 5;
    cJSON *child = item->child;
    size_t numentries = 0;
    size_t i = 0;
    cJSON_bool fail = false;

    /* Count array entries */
    while (child)
    {
        numentries++;
        child = child->next;
    }

    if (!numentries)
    {
        out = (char*)cJSON_malloc(3);
        if (out)
        {
            strcpy(out, "[]");
        }
        return out;
    }

    entries = (char**)cJSON_malloc(numentries * sizeof(char*));
    if (!entries)
    {
        return NULL;
    }
    memset(entries, '\0', numentries * sizeof(char*));

    /* Compose the output array. */
    child = item->child;
    while (child && !fail)
    {
        ret = print_value(child, depth + 1, fmt);
        entries[i++] = ret;
        if (ret)
        {
            len += strlen(ret) + 2 + (fmt ? 1 : 0);
        }
        else
        {
            fail = true;
        }
        child = child->next;
    }

    /* If failed, exit */
    if (fail)
    {
        for (i = 0; i < numentries; i++)
        {
            if (entries[i])
            {
                cJSON_free(entries[i]);
            }
        }
        cJSON_free(entries);
        return NULL;
    }

    /* Compose output string */
    out = (char*)cJSON_malloc(len);
    if (!out)
    {
        for (i = 0; i < numentries; i++)
        {
            if (entries[i])
            {
                cJSON_free(entries[i]);
            }
        }
        cJSON_free(entries);
        return NULL;
    }

    *out = '[';
    ptr = out + 1;
    *ptr = '\0';
    for (i = 0; i < numentries; i++)
    {
        size_t tmplen = strlen(entries[i]);
        memcpy(ptr, entries[i], tmplen);
        ptr += tmplen;
        if (i != (numentries - 1))
        {
            *ptr++ = ',';
            if (fmt)
            {
                *ptr++ = ' ';
            }
        }
        cJSON_free(entries[i]);
    }
    cJSON_free(entries);
    *ptr++ = ']';
    *ptr = '\0';

    return out;
}

/* Build an object from the text. */
static const unsigned char *parse_object(cJSON * const item, const unsigned char *input)
{
    cJSON *child = NULL;

    if (*input != '{')
    {
        return NULL;
    }

    item->type = cJSON_Object;
    input = skip_whitespace(input + 1);
    if (*input == '}')
    {
        return input + 1;
    }

    /* step back to the first string */
    item->child = child = cJSON_New_Item();
    if (child == NULL)
    {
        return NULL;
    }

    /* parse first key */
    input = skip_whitespace(parse_string(child, skip_whitespace(input)));
    if (input == NULL)
    {
        return NULL;
    }
    child->string = child->valuestring;
    child->valuestring = NULL;

    if (*input != ':')
    {
        return NULL;
    }
    /* parse value */
    input = skip_whitespace(parse_value(child, skip_whitespace(input + 1)));
    if (input == NULL)
    {
        return NULL;
    }

    /* loop through key-value pairs */
    while (*input == ',')
    {
        cJSON *new_item = cJSON_New_Item();
        if (new_item == NULL)
        {
            return NULL;
        }

        /* link to parent */
        child->next = new_item;
        new_item->prev = child;
        child = new_item;

        /* parse key */
        input = skip_whitespace(parse_string(child, skip_whitespace(input + 1)));
        if (input == NULL)
        {
            return NULL;
        }
        child->string = child->valuestring;
        child->valuestring = NULL;

        if (*input != ':')
        {
            return NULL;
        }
        /* parse value */
        input = skip_whitespace(parse_value(child, skip_whitespace(input + 1)));
        if (input == NULL)
        {
            return NULL;
        }
    }

    if (*input != '}')
    {
        return NULL;
    }

    return input + 1;
}

/* Render an object to text. */
static char *print_object(const cJSON * const item, int depth, cJSON_bool fmt)
{
    char **entries = NULL;
    char **names = NULL;
    char *out = NULL;
    char *ptr = NULL;
    char *ret = NULL;
    char *str = NULL;
    size_t len = 7;
    size_t i = 0;
    size_t j = 0;
    cJSON *child = item->child;
    size_t numentries = 0;
    cJSON_bool fail = false;

    /* Count object entries */
    while (child)
    {
        numentries++;
        child = child->next;
    }

    if (!numentries)
    {
        out = (char*)cJSON_malloc(fmt ? depth + 4 : 3);
        if (out)
        {
            strcpy(out, "{}");
        }
        return out;
    }

    entries = (char**)cJSON_malloc(numentries * sizeof(char*));
    if (!entries)
    {
        return NULL;
    }
    names = (char**)cJSON_malloc(numentries * sizeof(char*));
    if (!names)
    {
        cJSON_free(entries);
        return NULL;
    }
    memset(entries, '\0', numentries * sizeof(char*));
    memset(names, '\0', numentries * sizeof(char*));

    /* Collect entries */
    child = item->child;
    depth++;
    while (child && !fail)
    {
        names[i] = str = print_string_ptr((unsigned char*)child->string);
        entries[i++] = ret = print_value(child, depth, fmt);
        if (str && ret)
        {
            len += strlen(ret) + strlen(str) + 2 + (fmt ? 2 + depth : 0);
        }
        else
        {
            fail = true;
        }
        child = child->next;
    }

    /* Compose output string */
    if (!fail)
    {
        out = (char*)cJSON_malloc(len);
    }
    if (!out)
    {
        fail = true;
    }

    if (fail)
    {
        for (i = 0; i < numentries; i++)
        {
            if (names[i])
            {
                cJSON_free(names[i]);
            }
            if (entries[i])
            {
                cJSON_free(entries[i]);
            }
        }
        cJSON_free(names);
        cJSON_free(entries);
        return NULL;
    }

    *out = '{';
    ptr = out + 1;
    if (fmt)
    {
        *ptr++ = '\n';
    }
    *ptr = '\0';
    for (i = 0; i < numentries; i++)
    {
        if (fmt)
        {
            for (j = 0; j < depth; j++)
            {
                *ptr++ = '\t';
            }
        }
        size_t tmplen = strlen(names[i]);
        memcpy(ptr, names[i], tmplen);
        ptr += tmplen;
        *ptr++ = ':';
        if (fmt)
        {
            *ptr++ = '\t';
        }
        tmplen = strlen(entries[i]);
        memcpy(ptr, entries[i], tmplen);
        ptr += tmplen;
        if (i != (numentries - 1))
        {
            *ptr++ = ',';
        }
        if (fmt)
        {
            *ptr++ = '\n';
        }
        *ptr = '\0';
        cJSON_free(names[i]);
        cJSON_free(entries[i]);
    }
    cJSON_free(names);
    cJSON_free(entries);
    if (fmt)
    {
        for (i = 0; i < (depth - 1); i++)
        {
            *ptr++ = '\t';
        }
    }
    *ptr++ = '}';
    *ptr = '\0';

    return out;
}

/* Parser core - when encountering text, process appropriately. */
static const unsigned char *parse_value(cJSON * const item, const unsigned char *input)
{
    if (input == NULL)
    {
        return NULL;
    }

    /* parse the different types of values */
    if (!strncmp((const char*)input, "null", 4))
    {
        item->type = cJSON_NULL;
        return input + 4;
    }
    if (!strncmp((const char*)input, "false", 5))
    {
        item->type = cJSON_False;
        return input + 5;
    }
    if (!strncmp((const char*)input, "true", 4))
    {
        item->type = cJSON_True;
        item->valueint = 1;
        return input + 4;
    }
    if (*input == '\"')
    {
        return parse_string(item, input);
    }
    if ((*input == '-') || ((*input >= '0') && (*input <= '9')))
    {
        return parse_number(item, input);
    }
    if (*input == '[')
    {
        return parse_array(item, input);
    }
    if (*input == '{')
    {
        return parse_object(item, input);
    }

    return NULL;
}

/* Render a value to text. */
static char *print_value(const cJSON * const item, int depth, cJSON_bool fmt)
{
    char *out = NULL;

    if (item == NULL)
    {
        return NULL;
    }

    switch (item->type & 0xFF)
    {
        case cJSON_NULL:
            out = (char*)cJSON_malloc(5);
            if (out)
            {
                strcpy(out, "null");
            }
            break;
        case cJSON_False:
            out = (char*)cJSON_malloc(6);
            if (out)
            {
                strcpy(out, "false");
            }
            break;
        case cJSON_True:
            out = (char*)cJSON_malloc(5);
            if (out)
            {
                strcpy(out, "true");
            }
            break;
        case cJSON_Number:
            out = print_number(item);
            break;
        case cJSON_String:
            out = print_string(item);
            break;
        case cJSON_Array:
            out = print_array(item, depth, fmt);
            break;
        case cJSON_Object:
            out = print_object(item, depth, fmt);
            break;
        case cJSON_Raw:
        {
            size_t raw_length = 0;
            if (item->valuestring == NULL)
            {
                return NULL;
            }

            raw_length = strlen(item->valuestring) + sizeof('\0');
            out = (char*)cJSON_malloc(raw_length);
            if (out)
            {
                memcpy(out, item->valuestring, raw_length);
            }
            break;
        }
        default:
            break;
    }

    return out;
}

/* Parse a JSON string into a cJSON structure */
cJSON *cJSON_Parse(const char *value)
{
    return cJSON_ParseWithLength(value, value ? strlen(value) : 0);
}

cJSON *cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    cJSON *item = cJSON_New_Item();
    if (item == NULL)
    {
        return NULL;
    }

    if (!parse_value(item, skip_whitespace((const unsigned char*)value)))
    {
        cJSON_Delete(item);
        return NULL;
    }

    return item;
}

/* Render a cJSON item/entity/structure to text. */
char *cJSON_Print(const cJSON *item)
{
    return print_value(item, 0, true);
}

char *cJSON_PrintUnformatted(const cJSON *item)
{
    return print_value(item, 0, false);
}

/* Get array size */
int cJSON_GetArraySize(const cJSON *array)
{
    cJSON *child = NULL;
    size_t size = 0;

    if (array == NULL)
    {
        return 0;
    }

    child = array->child;

    while (child != NULL)
    {
        size++;
        child = child->next;
    }

    return (int)size;
}

/* Get array item by index */
cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    cJSON *current_child = NULL;

    if (array == NULL)
    {
        return NULL;
    }

    current_child = array->child;
    while ((current_child != NULL) && (index > 0))
    {
        index--;
        current_child = current_child->next;
    }

    return current_child;
}

/* Get object item */
static cJSON *get_object_item(const cJSON * const object, const char * const name, const cJSON_bool case_sensitive)
{
    cJSON *current_element = NULL;

    if ((object == NULL) || (name == NULL))
    {
        return NULL;
    }

    current_element = object->child;
    if (case_sensitive)
    {
        while ((current_element != NULL) && (current_element->string != NULL) && (strcmp(name, current_element->string) != 0))
        {
            current_element = current_element->next;
        }
    }
    else
    {
        while ((current_element != NULL) && (strcasecmp(name, current_element->string) != 0))
        {
            current_element = current_element->next;
        }
    }

    return current_element;
}

cJSON *cJSON_GetObjectItem(const cJSON * const object, const char * const string)
{
    return get_object_item(object, string, false);
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string)
{
    return get_object_item(object, string, true);
}

cJSON_bool cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItem(object, string) ? true : false;
}

/* Check if item is of given type */
cJSON_bool cJSON_IsInvalid(const cJSON * const item)
{
    return (item == NULL) || ((item->type & 0xFF) == cJSON_Invalid);
}

cJSON_bool cJSON_IsFalse(const cJSON * const item)
{
    return (item != NULL) && ((item->type & 0xFF) == cJSON_False);
}

cJSON_bool cJSON_IsTrue(const cJSON * const item)
{
    return (item != NULL) && ((item->type & 0xFF) == cJSON_True);
}

cJSON_bool cJSON_IsBool(const cJSON * const item)
{
    return (item != NULL) && (((item->type & 0xFF) == cJSON_True) || ((item->type & 0xFF) == cJSON_False));
}

cJSON_bool cJSON_IsNull(const cJSON * const item)
{
    return (item != NULL) && ((item->type & 0xFF) == cJSON_NULL);
}

cJSON_bool cJSON_IsNumber(const cJSON * const item)
{
    return (item != NULL) && ((item->type & 0xFF) == cJSON_Number);
}

cJSON_bool cJSON_IsString(const cJSON * const item)
{
    return (item != NULL) && ((item->type & 0xFF) == cJSON_String);
}

cJSON_bool cJSON_IsArray(const cJSON * const item)
{
    return (item != NULL) && ((item->type & 0xFF) == cJSON_Array);
}

cJSON_bool cJSON_IsObject(const cJSON * const item)
{
    return (item != NULL) && ((item->type & 0xFF) == cJSON_Object);
}

cJSON_bool cJSON_IsRaw(const cJSON * const item)
{
    return (item != NULL) && ((item->type & 0xFF) == cJSON_Raw);
}

/* Create basic items */
cJSON *cJSON_CreateNull(void)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_NULL;
    }
    return item;
}

cJSON *cJSON_CreateTrue(void)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_True;
    }
    return item;
}

cJSON *cJSON_CreateFalse(void)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_False;
    }
    return item;
}

cJSON *cJSON_CreateBool(cJSON_bool boolean)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = boolean ? cJSON_True : cJSON_False;
    }
    return item;
}

cJSON *cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_Number;
        item->valuedouble = num;
        item->valueint = (int)num;
    }
    return item;
}

cJSON *cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_String;
        item->valuestring = (char*)cJSON_malloc(strlen(string) + 1);
        if (item->valuestring)
        {
            strcpy(item->valuestring, string);
        }
        else
        {
            cJSON_Delete(item);
            return NULL;
        }
    }
    return item;
}

cJSON *cJSON_CreateArray(void)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_Array;
    }
    return item;
}

cJSON *cJSON_CreateObject(void)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_Object;
    }
    return item;
}

/* Add item to array/object */
static cJSON_bool add_item_to_array(cJSON *array, cJSON *item)
{
    cJSON *child = NULL;

    if ((item == NULL) || (array == NULL) || (array == item))
    {
        return false;
    }

    child = array->child;

    if (child == NULL)
    {
        array->child = item;
        item->prev = item;
        item->next = NULL;
    }
    else
    {
        /* find last child */
        if (child->prev)
        {
            child->prev->next = item;
            item->prev = child->prev;
            array->child->prev = item;
        }
    }

    return true;
}

cJSON_bool cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    return add_item_to_array(array, item);
}

cJSON_bool cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    char *new_key = NULL;
    int new_type = cJSON_Invalid;

    if ((object == NULL) || (string == NULL) || (item == NULL) || (object == item))
    {
        return false;
    }

    new_key = (char*)cJSON_malloc(strlen(string) + 1);
    if (new_key == NULL)
    {
        return false;
    }
    strcpy(new_key, string);

    new_type = item->type | cJSON_StringIsConst;

    if (item->string != NULL)
    {
        cJSON_free(item->string);
    }
    item->string = new_key;
    item->type = new_type;

    return add_item_to_array(object, item);
}
