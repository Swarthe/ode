/*
 * Copyright (C) 2022 Emil Overbeck <emil.a.overbeck at gmail dot com>
 * Subject to the MIT License. See LICENSE.txt for more information.
 *
 */

#include <stdarg.h>
#include <string.h>

#include "ode.h"
#include "ode_alloc.h"

/*
 *                  Serial format
 *
 * Strings are enclosed in double quote '"' characters.
 *  - "string"
 * A Rust-inpired raw string format is used when needed.
 *  - #"str"ing"#
 *  - ##"str"#ing"##
 *
 * All integral objects (a name and value or only a name) are terminated by a
 * semicolon ';', and their fields are separated by a colon ':'.
 *  - "name";
 *  - "name":"value";
 *
 * A parent and its subordinates are separated by the pipe '|' character(s), the
 * number of which is the number of subordinates.
 *  - "parent"|"child";
 *  - "parent"|||"1";"2";"3";
 *
 */

#define OBJ_SPEC    '|'
#define OBJ_SEP     ';'
#define STR_SPEC    '#'
#define STR_SEP     '"'
#define FIELD_SEP   ':'

/* String information. */
#define SERIAL_START(serial, specs)     ((serial) + (specs) + 1)
#define AS_SERIAL_LEN(real_len, specs)  ((real_len) + 2 * (specs) + 2)

/* Subordinate object operations. */
#define LAST_SUB(obj)       ((obj)->sub + (obj)->nsub - 1)
#define ITER_SUB(obj, sb)   for (sb = obj->sub; sb <= LAST_SUB(obj); ++sb)

#define INIT(obj, parent)           \
    do {                            \
        (obj)->name_len  = 0;       \
        (obj)->name      = NULL;    \
        (obj)->value_len = 0;       \
        (obj)->value     = NULL;    \
                                    \
        (obj)->nsub = 0;            \
        (obj)->sur  = (parent);     \
        (obj)->sub  = NULL;         \
    } while (0)

#define EQ_MEM(a, b, n) (memcmp((a), (b), (n)) == 0)

struct ode_object {
    size_t  name_len, value_len;
    char   *name,    *value;

    size_t nsub;
    struct ode_object *sub;     /* Child(ren) */
    struct ode_object *sur;     /* Parent     */
};

/* Sets or replaces string in 'dest' and its size 'dest_len' to a copy of 'str'
   of size 'len'. This operation is atomic. Returns 1 on success, otherwise 0
   and sets errno. */
static int set_str(char **dest, size_t *dest_len, const char *str, size_t len)
{
    char *new;

    if (*dest)
        new = (len != *dest_len) ? ODE_REALLOC(*dest, len + 1) : *dest;
    else
        new = ODE_MALLOC(len + 1);

    if (!new) return 0;
    memcpy(new, str, len);
    new[len] = '\0';

    *dest     = new;
    *dest_len = len;
    return 1;
}

/* Extracts information from the string starting at 'serial' with 'end', and
   copies it to 'real_len' and 'spec_len'. Returns 1 on success, 0 on invalid
   string at 'serial'. */
static int info_serial(size_t *real_len, size_t *spec_len,
                       const char *serial, const char *end)
{
    size_t specs, i;
    const char *r_start, *r_end;    /* Delimiters of the quoted string */

    /* Require at least space for a pair of 'STR_SEP' */
    if (serial >= end) return 0;

    /* Handle opening 'STR_SPEC' */
    for (specs = 0; serial <= end && *serial != STR_SEP; ++serial) {
        if (*serial == STR_SPEC)
            ++specs;
        else
            return 0;
    }

    /* Handle opening 'STR_SEP' */
    if (serial >= end) return 0;
    r_start = serial++;

cont:
    /* Handle string ending */
    for (; serial <= end; ++serial) {
        if (*serial == STR_SEP) {
            r_end = serial;

            if (specs > 0) {
                i = 1;

                do {
                    ++serial;
                    if (serial > end)        return 0;
                    if (*serial != STR_SPEC) goto cont;
                } while (i++ < specs);
            }

            *real_len = r_end - r_start - 1;    /* -1 for 'STR_SEP' offset */
            *spec_len = specs;
            return 1;
        }
    }

    return 0;
}

/* Returns the numbers of 'STR_SPEC' of 'str' of 'len' in serial form. */
static size_t nspec(const char *str, size_t len)
{
    const char *term;
    size_t hi_specs, specs;

    for (term = str + len, hi_specs = 0; str < term; ++str) {
        if (*str == STR_SEP) {
            specs = 1, ++str;
            for (; str < term && *str == STR_SPEC; ++specs, ++str);
            if (specs > hi_specs) hi_specs = specs;
        }
    }

    return hi_specs;
}

/* Deserialises string from 'serial' of 'end' to 'dest' and copies its size
   to 'len'. Returns the new 'serial' position for writing on success, otherwise
   NULL and sets errno unless 'serial' is invalid. */
static const char *deserial_str(char **dest, size_t *len,
                                const char *serial, const char *end)
{
    size_t real_len, specs;
    char  *real;

    if (!info_serial(&real_len, &specs, serial, end)
        || !(real = ODE_MALLOC(real_len + 1)))
        return NULL;

    memcpy(real, SERIAL_START(serial, specs), real_len);
    real[real_len] = '\0';
    *dest = real;
    *len  = real_len;

    return serial + AS_SERIAL_LEN(real_len, specs);
}

/* Serialises 'str' of 'len' into 'dest'. Returns the new 'dest' position for
   writing. */
static char *serial_str(char *dest, const char *str, size_t len)
{
    size_t specs;
    char *start;

    start = dest;
    specs = nspec(str, len);

    if (specs != 0) memset(dest, STR_SPEC, specs);
    *(dest += specs) = STR_SEP;
    memcpy(++dest, str, len);
    *(dest += len)   = STR_SEP;
    if (specs != 0) memset(dest + 1, STR_SPEC, specs);

    return start + AS_SERIAL_LEN(len, specs);
}

/* Returns the size of 'obj' in serial form. */
static size_t size_as_serial(const ode_t *obj)
{
    const ode_t *sub;
    size_t ret;

    ret = AS_SERIAL_LEN(obj->name_len, nspec(obj->name, obj->name_len));

    if (obj->value) {
        /* +2 for 'OBJ_SEP' and preceding 'FIELD_SEP' */
        ret += 2 + AS_SERIAL_LEN(obj->value_len,
                                 nspec(obj->value, obj->value_len));
    } else if (obj->sub) {
        ret += obj->nsub;       /* For 'OBJ_SPEC' */
        ITER_SUB(obj, sub) ret += size_as_serial(sub);
    } else {
        ret += 1;       /* For 'FIELD_SEP' */
    }

    return ret;
}

/* Deserialises 'serial' with 'end' into 'dest'. Returns a non-NULL pointer on
   success, otherwise NULL and sets errno unless 'serial' is invalid. */
static char *mkdeserial(ode_t *dest, const char *serial, const char *end)
{
    ode_t  *sub;
    size_t  nsub;

    if (!(serial = deserial_str(&dest->name, &dest->name_len, serial, end))
        || serial > end)
        return NULL;

    switch (*serial++) {
    case FIELD_SEP:
        serial = deserial_str(&dest->value, &dest->value_len, serial, end);

        if (!serial || serial > end || *serial++ != OBJ_SEP)
            goto fail;

        break;

    case OBJ_SPEC:
        for (nsub = 1; serial <= end && *serial == OBJ_SPEC; ++serial, ++nsub);
        if (serial >= end) goto fail;

        if (!(sub = ODE_MALLOC((sizeof(*sub) * nsub))))
            goto fail;

        dest->sub  = sub;
        dest->nsub = nsub;

        /* Recursively deserialise into subordinates */
        ITER_SUB(dest, sub)  {
            INIT(sub, dest);

            if (!(serial = mkdeserial(sub, serial, end))) {
                free(dest->sub);
                goto fail;
            }
        }

        break;

    case OBJ_SEP : break;
    default      : goto fail;
    }

    return (char *) serial;

fail:
    free(dest->name);
    return NULL;
}

/* Serialises 'obj' into 'dest'. The return value should be ignored. */
static char *mkserial(char *dest, const ode_t *obj)
{
    const ode_t *sub;

    dest = serial_str(dest, obj->name, obj->name_len);

    if (obj->value) {
        *dest++ = FIELD_SEP;
        dest = serial_str(dest, obj->value, obj->value_len);
        *dest++ = OBJ_SEP;
    } else if (obj->sub) {
        memset(dest, OBJ_SPEC, obj->nsub);
        dest += obj->nsub;
        ITER_SUB(obj, sub) dest = mkserial(dest, sub);
    } else {
        *dest++ = OBJ_SEP;
    }

    return dest;
}

/* Compares C string 'a' and 'b' of size 'b_len'. */
static int eq_str(const char *a, const char *b, size_t b_len)
{
    const char *b_term;

    for (b_term = b + b_len; b < b_term; ++a, ++b)
        if (!*a || *a != *b) return 0;

    return *a ? 0 : 1;      /* If 'a' is longer than 'b' */
}

/* Recursively destroys 'obj'. The space it occupies & its parent are intact. */
static void destroy(ode_t *obj)
{
    ode_t *o;

    ODE_FREE(obj->name);

    if (obj->value) {
        ODE_FREE(obj->value);
    } else if (obj->sub) {
        ITER_SUB(obj, o) destroy(o);
        ODE_FREE(obj->sub);
    }
}

/* Corrects the structure of 'obj' after relocation of its subordinates. */
static void resur(ode_t *obj)
{
    ode_t *o, *p;

    ITER_SUB (obj, o) {
        if (o->sub) {
            ITER_SUB (o, p)
                p->sur = o;
        }
    }
}

ode_t *ode_create(const char *name, size_t len)
{
    ode_t *ret;

    if (!(ret = ODE_MALLOC(sizeof(*ret))))
        return NULL;

    INIT(ret, NULL);
    if (len == (size_t) -1) len = strlen(name);

    if (!set_str(&ret->name, &ret->name_len, name, len)) {
        ODE_FREE(ret);
        return NULL;
    }

    return ret;
}

ode_t *ode_deserial(const char *serial, size_t size)
{
    ode_t *ret;

    if (!(ret = ODE_MALLOC(sizeof(*ret))))
        return NULL;

    INIT(ret, NULL);

    if (!mkdeserial(ret, serial, serial + size - 1)) {
        free(ret);
        return NULL;
    }

    return ret;
}

char *ode_serial(const ode_t *obj, size_t *serial_size)
{
    char *ret;
    size_t ret_sz;

    ret_sz = size_as_serial(obj);

    if (!(ret = ODE_MALLOC(ret_sz)))
        return NULL;

    mkserial(ret, obj);
    *serial_size = ret_sz;
    return ret;
}

ode_t *ode_get1(const ode_t *from, const char *name, size_t len)
{
    const ode_t *o;

    if (!from->sub) return NULL;

    if (len == (size_t) -1) {
        ITER_SUB(from, o) {
            if (eq_str(name, o->name, o->name_len))
                return (ode_t *) o;
        }
    } else {
        ITER_SUB(from, o) {
            if (o->name_len == len && EQ_MEM(o->name, name, len))
                return (ode_t *) o;
        }
    }

    return NULL;
}

ode_t *ode_get(const ode_t *from, ...)
{
    va_list      ap;
    const char  *arg;
    const ode_t *o;

    va_start(ap, from);

next_arg:
    while ((arg = va_arg(ap, const char *))) {
        if (from->sub) {
            ITER_SUB(from, o) {
                if (eq_str(arg, o->name, o->name_len)) {
                    /* Iterate into match */
                    from = o;
                    goto next_arg;
                }
            }
        }

        /* No matches found or possible */
        from = NULL;
        break;
    }

    va_end(ap);
    return (ode_t *) from;      /* Is original 'from' if no arguments */
}

const char *ode_getstr(const ode_t *from, enum ode_type type)
{
    /* 'from->value' is NULL if 'from' has no value. */
    return (type == ODE_NAME) ? from->name : from->value;
}

size_t ode_getlen(const ode_t *from, enum ode_type type)
{
    if (type == ODE_NAME) {
        return from->name_len;
    } else {
        return from->value ? from->value_len : (size_t) -1;
    }
}

ode_t *ode_iter(const ode_t *obj, const ode_t *pos)
{
    if (pos && obj->sub) {
        if (pos >= obj->sub && pos < LAST_SUB(obj))
            return (ode_t *) pos + 1;
        else
            return NULL;
    } else {
        return obj->sub;    /* NULL if 'obj' has no children. */
    }
}

ode_t *ode_mod(ode_t *obj, enum ode_type type, const char *str, size_t len)
{

    /* Object may not have value and child */
    if (type == ODE_VALUE && obj->sub)
        return NULL;

    if (len == (size_t) -1) len = strlen(str);

    /* Prevent name duplication */
    if (type == ODE_NAME && obj->sur && ode_get1(obj->sur, str, len))
        return NULL;

    if (type == ODE_NAME)
        return set_str(&obj->name, &obj->name_len, str, len) ? obj : NULL;
    else
        return set_str(&obj->value, &obj->value_len, str, len) ? obj : NULL;
}

ode_t *ode_add(ode_t *to, const char *name, size_t len)
{
    ode_t *add, *new_sub;
    int moved = 0;

    if (to->value) return NULL;

    if (to->sub) {
        if (ode_get1(to, name, len)) return NULL;

        if (!(new_sub = ODE_REALLOC(to->sub, sizeof(*to->sub)
                                             * (to->nsub + 1))))
            return NULL;

        if (new_sub != to->sub) moved = 1;
        add = new_sub + to->nsub;       /* Last sub, unknown to 'to' */
    } else if (!(new_sub = add = ODE_MALLOC(sizeof(*add)))) {
        return NULL;
    }

    INIT(add, to);
    if (len == (size_t) -1) len = strlen(name);

    /* Reset on failure to set name for atomicity */
    if (!set_str(&add->name, &add->name_len, name, len)) {
        if (to->nsub == 0) {
            ODE_FREE(new_sub);
            to->sub = NULL;
        }

        return NULL;
    }

    to->sub = new_sub;
    if (moved) resur(to);       /* Does not affect 'add' (already correct). */
    ++to->nsub;
    return add;
}

int ode_del(ode_t *obj)
{
    ode_t *sur;
    ode_t *new_sub;     /* Sub of 'sur' after modification */
    ode_t  backup;      /* Copy of 'obj' for atomicity     */
    int moved = 0;

    if (!obj) return 0;

    /* Destroy 'obj' completely if it is a root object */
    if (!obj->sur) {
        destroy(obj);
        ODE_FREE(obj);
        return 1;
    }

    sur = obj->sur;

    if (sur->nsub > 1) {
        backup = *obj;

        /* The order of objects is meaningless; replace the object with the last
           one if needed before shrinking */
        if (obj != LAST_SUB(sur))
            *obj = *LAST_SUB(sur);

        new_sub = ODE_REALLOC(sur->sub, sizeof(*sur->sub) * sur->nsub - 1);

        /* Reset on failure to shrink */
        if (!new_sub) {
            if (obj != LAST_SUB(sur)) *obj = backup;
            return 0;
        }

        if (new_sub != sur->sub) moved = 1;
        destroy(&backup);       /* 'obj' can no longer be used */
    } else {
        destroy(obj);
        ODE_FREE(obj);
        new_sub = NULL;
    }

    sur->sub = new_sub;
    --sur->nsub;
    if (moved) resur(sur);
    return 1;
}

void ode_zero(ode_t *obj, void (*zero_fn)(void *s, size_t n))
{
    ode_t *o;

    zero_fn(obj->name, obj->name_len);
    zero_fn(&obj->name_len, sizeof(obj->name_len));

    if (obj->value) {
        zero_fn(obj->value, obj->value_len);
        zero_fn(&obj->value_len, sizeof(obj->value_len));
    } else if (obj->sub) {
        ITER_SUB(obj, o) ode_zero(o, zero_fn);
    }
}
