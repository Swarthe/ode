/*
 * Copyright (C) 2022 Emil Overbeck <emil.a.overbeck at gmail.com>
 * Subject to the MIT License. See LICENSE.txt for more information.
 *
 */

#include <stdarg.h>
#include <string.h>

#include "ode.h"
#include "ode_alloc.h"

#define ODE_ID      0x0de
#define HAS_VALUE   0x0
#define HAS_SUB     0x1
#define HAS_NONE    0x2

#define LAST_SUB(obj)       ((obj)->sub + (obj)->nsub - 1)
#define LOOP_SUB(obj, sb)   for (sb = obj->sub; sb <= LAST_SUB(obj); ++sb)

#define INIT(obj, sr)               \
    do {                            \
        (obj)->name_len  = 0;       \
        (obj)->name      = NULL;    \
        (obj)->value_len = 0;       \
        (obj)->value     = NULL;    \
                                    \
        (obj)->nsub = 0;            \
        (obj)->sur  = (sr);         \
        (obj)->sub  = NULL;         \
    } while (0)

#define WRITE(dest, val, size)              \
    do {                                    \
        memcpy((dest), (val), (size));      \
        (dest) += (size);                   \
    } while (0)

#define READ(dest, serial, remain, size)        \
    do {                                        \
        memcpy((dest), (serial), (size));       \
        (serial)   += (size);                   \
        (remain) -= (size);                     \
    } while (0)

#define EQ_MEM(a, b, n) (memcmp((a), (b), (n)) == 0)

struct ode_object {
    size_t  name_len, value_len;
    char   *name,    *value;

    size_t nsub;
    struct ode_object *sub;     /* Child(ren) */
    struct ode_object *sur;     /* Parent     */
};

/* Sets or replaces a string in 'obj' to a copy of 'str' of size 'len' depending
   on 'mode'. 'str' is treated as a C string if 'len' is '(size_t) -1'. This
   operation is atomic. Returns 1 on success, otherwise 0 and sets errno. */
static int set_str(ode_t *obj, enum ode_mode mode, const char *str, size_t len)
{
    char *target;       /* String to set for atomicity */

    if (len == (size_t) -1) len = strlen(str);
    target = (mode == ODE_NAME) ? obj->name : obj->value;

    /* Reallocate the string if it exists, otherwise create it */
    if (!(target = target ? ODE_REALLOC(target, len + 1) : ODE_MALLOC(len + 1)))
        return 0;

    memcpy(target, str, len);
    target[len] = '\0';

    if (mode == ODE_NAME) {
        obj->name      = target;
        obj->name_len  = len;
    } else {
        obj->value     = target;
        obj->value_len = len;
    }

    return 1;
}

/* Deserialises a string from 'serial' of size 'remain' and puts its size in
   'read_len'. Offsets 'serial' and 'remain'. Returns the string on success,
   otherwise NULL and sets errno unless 'serial' is invalid. */
static char *read_str(size_t *read_len,
                      const unsigned char **serial, size_t *remain)
{
    size_t  ret_sz;
    char   *ret;

    if (*remain < sizeof(ret_sz))        return NULL;
    READ(&ret_sz, *serial, *remain, sizeof(ret_sz));

    if (*remain < ret_sz)                return NULL;
    if (!(ret = ODE_MALLOC(ret_sz + 1))) return NULL;
    READ(ret, *serial, *remain, ret_sz);
    ret[ret_sz] = '\0';

    *read_len = ret_sz;
    return ret;
}

/* Returns the size of 'obj' in serial form. */
static size_t get_serial_size(const ode_t *obj)
{
    const ode_t *sub;
    size_t ret;

    ret = sizeof(obj->name_len) + obj->name_len
          + sizeof(unsigned char);      /* For subordinate/value identifier */

    if (obj->value) {
        ret += sizeof(obj->value_len) + obj->value_len;
    } else if (obj->sub) {
        ret += sizeof(obj->nsub);

        LOOP_SUB(obj, sub)
            ret += get_serial_size(sub);
    }

    return ret;
}

/* Deserialises 'serial' of size 'remain' into 'dest'. Returns the 'serial' read
   offset on success, otherwise NULL & sets errno unless 'serial' is invalid. */
static unsigned char *deserial(ode_t *dest, const unsigned char *serial,
                               size_t *remain)
{
    ode_t *sub;     /* Subordinate of 'obj' if found */

    dest->name = read_str(&dest->name_len, &serial, remain);
    if (!dest->name || *remain == 0) return NULL;

    switch (*serial++) {
    case HAS_VALUE:
        dest->value = read_str(&dest->value_len, &serial, remain);
        if (!dest->value) return NULL;
        break;

    case HAS_SUB:
        if (*remain < sizeof(dest->nsub)) return NULL;
        READ(&dest->nsub, serial, *remain, sizeof(dest->nsub));

        if (!(sub = dest->sub = ODE_MALLOC(sizeof(*sub) * dest->nsub)))
            return NULL;

        /* Recurse into subordinates */
        LOOP_SUB(dest, sub)  {
            INIT(sub, dest);

            if (!(serial = deserial(sub, serial, remain)))
                return NULL;
        }

        break;

    case HAS_NONE: break;
    default: return NULL;
    }

    return (unsigned char *) serial;    /* Acts as the offset */
}

/* Serialises 'obj' into 'dest' without bounds checking. Returns the 'dest'
   write offset. */
static unsigned char *serial(unsigned char *dest, const ode_t *obj)
{
    const ode_t *sub;

    WRITE(dest, &obj->name_len, sizeof(obj->name_len));
    WRITE(dest, obj->name, obj->name_len);

    /* Serialise value and return if no subordinate */
    if (obj->value) {
        *dest++ = HAS_VALUE;
        WRITE(dest, &obj->value_len, sizeof(obj->value_len));
        WRITE(dest, obj->value, obj->value_len);
    } else if (obj->sub) {
        *dest++ = HAS_SUB;
        WRITE(dest, &obj->nsub, sizeof(obj->nsub));
        LOOP_SUB(obj, sub) dest = serial(dest, sub);
    } else {
        *dest++ = HAS_NONE;
    }

    return dest;
}

/* Compares C string 'a' and 'b' of size 'b_len'. */
static int eq_str(const char *a, const char *b, size_t b_len)
{
    for (; b_len > 0; ++a, ++b, --b_len)
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
        LOOP_SUB(obj, o) destroy(o);
        ODE_FREE(obj->sub);
    }
}

/* Corrects the structure of 'obj' after relocation of its subordinates. */
static void resur(ode_t *obj)
{
    ode_t *o, *p;

    LOOP_SUB (obj, o) {
        if (o->sub) {
            LOOP_SUB (o, p)
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

    if (!set_str(ret, ODE_NAME, name, len)) {
        ODE_FREE(ret);
        return NULL;
    }

    return ret;
}

ode_t *ode_read(const unsigned char *serial, size_t size)
{
    ode_t *ret;

    if (size < sizeof(unsigned char)
        || --size, *serial++ != ODE_ID
        || !(ret = ODE_MALLOC(sizeof(*ret))))
        return NULL;

    INIT(ret, NULL);

    if (!deserial(ret, serial, &size)) {
        ODE_FREE(ret);
        return NULL;
    }

    return ret;
}

unsigned char *ode_write(const ode_t *obj, size_t *serial_size)
{
    unsigned char *ret;
    size_t ret_sz;

    ret_sz = get_serial_size(obj)
             + sizeof(unsigned char);   /* For 'ODE_ID' */

    if (!(ret = ODE_MALLOC(ret_sz))) return NULL;
    *ret = ODE_ID;
    serial(ret + 1, obj);
    *serial_size = ret_sz;

    return ret;
}

ode_t *ode_get1(const ode_t *from, const char *name, size_t len)
{
    const ode_t *o;

    if (!from->sub) return NULL;

    if (len == (size_t) -1) {
        LOOP_SUB(from, o) {
            if (eq_str(name, o->name, o->name_len))
                return (ode_t *) o;
        }
    } else {
        LOOP_SUB(from, o) {
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
            LOOP_SUB(from, o) {
                if (eq_str(arg, o->name, o->name_len)) {
                    /* Iterate into match */
                    from = o;
                    goto next_arg;
                }
            }
        }

        /* No matches found or possible */
        from = NULL;
        goto done;
    }

done:
    va_end(ap);
    return (ode_t *) from;      /* Is original 'from' if no arguments */
}

const char *ode_getstr(const ode_t *from, enum ode_mode mode)
{
    /* 'from->value' is NULL if 'from' has no value. */
    return (mode == ODE_NAME) ? from->name : from->value;
}

size_t ode_getlen(const ode_t *from, enum ode_mode mode)
{
    if (mode == ODE_NAME) {
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

ode_t *ode_mod(ode_t *obj, enum ode_mode mode, const char *str, size_t len)
{
    /* Object may not have value and child */
    if (mode == ODE_VALUE && obj->sub)
        return NULL;

    /* Prevent name duplication */
    if (obj->sub && mode == ODE_NAME && ode_get1(obj->sur, str, len))
        return NULL;

    return set_str(obj, mode, str, len) ? obj : NULL;
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

    /* Reset on failure to set name for atomicity */
    if (!set_str(add, ODE_NAME, name, len)) {
        if (to->nsub == 0) {
            ODE_FREE(new_sub);
            to->sub = NULL;
        }

        return NULL;
    }

    to->sub = new_sub;
    if (moved) resur(to);       /* Will not affect 'add' (already correct). */
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
        /* The order of objects is meaningless; replace the now empty object
           with the last one if needed before shrinking */
        if (obj != LAST_SUB(sur)) {
            backup = *obj;
            *obj   = *LAST_SUB(sur);
        }

        new_sub = ODE_REALLOC(sur->sub, sizeof(*sur->sub) * sur->nsub - 1);

        /* Reset on failure to shrink for atomicity */
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

void ode_zero(ode_t *obj, void (*zero_fn)(void *, size_t))
{
    ode_t *o;

    zero_fn(obj->name, obj->name_len);
    zero_fn(&obj->name_len, sizeof(obj->name_len));

    if (obj->value) {
        zero_fn(obj->value, obj->value_len);
        zero_fn(&obj->value_len, sizeof(obj->value_len));
    } else if (obj->sub) {
        LOOP_SUB(obj, o) ode_zero(o, zero_fn);
    }
}
