/*
 * Copyright (C) 2022 Emil Overbeck <emil.a.overbeck at gmail dot com>
 * Subject to the MIT License. See LICENSE.txt for more information.
 *
 */

#ifndef ODE_H
#define ODE_H

#include <stddef.h>

/*
 * Base object type.
 *
 * Every successfully created object has a name. In a tree-like fashion, it may
 * have a value of arbitrary data or a set of subordinate objects, but not both
 * (though it can have neither, as an "empty" object). All subordinates have
 * unique names, unless 'ode_zero()' has been used on them or their parents.
 *
 * An object without a parent is considered a "root" object, and may be created
 * with 'ode_init()' and 'ode_read()'.
 *
 * An object (but not its parents) is "invalid" and must not be used in any way
 * if 'ode_del()' has been successfully applied to it. All object modifications
 * are atomic: failed changes are not applied.
 *
 */
typedef struct ode_object ode_t;

/* Object data specification. */
enum ode_type {
    ODE_NAME,
    ODE_VALUE
};

/*
 * Create and initialise a root object.
 *
 * Creates an empty object with name 'name', which is treated as a
 * null-terminated string if 'len' is '(size_t) -1'.
 *
 * Returns the object on success.
 * Returns NULL and sets errno on allocation failure.
 *
 * 'ode_del()' should be applied to the object after use.
 *
 */
ode_t *ode_create(const char *name, size_t len);

/*
 * Read a root object from serialised data.
 *
 * 'serial' of 'size' must be valid data generated by 'ode_write()'. The caller
 * is responsible for ensuring this, because only bounds checks and basic
 * verifications are made. Success may be apparent even if the resulting object
 * contains unexpected data.
 *
 * Returns the deserialised object on success.
 * Returns NULL and sets errno on allocation failure.
 * Returns NULL if 'serial' is detected to be invalid.
 *
 * 'ode_del()' should be applied to the object after use.
 *
 */
ode_t *ode_deserial(const char *serial, size_t size);

/*
 * Serialise an object into string form.
 *
 * Returns the serialised form of 'obj' and its children and puts its size in
 * 'serial_size', which must be a valid pointer. The data is encoded in the
 * format described in 'ode.c'.
 *
 * Returns serial data on success.
 * Returns NULL and sets errno on allocation failure.
 *
 * The pointer should be freed after use.
 *
 */
char *ode_serial(const ode_t *obj, size_t *serial_size);

/*
 * Find a directly subordinate object.
 *
 * Searched for an object directly subordinate to 'from' of name 'name', which
 * is treated as a null-terminated string if 'len' is '(size_t) -1'.
 *
 * Returns the found object if it exists.
 * Returns NULL if the object was not found or no subordinate exists.
 *
 */
ode_t *ode_get1(const ode_t *from, const char *name, size_t len);

/*
 * Find subordinate objects.
 *
 * Find an object starting from 'from', traversing its children of names
 * specified by null-terminated string arguments. The last argument must be
 * '(char *) NULL'.
 *
 * Returns the found object if it exists.
 * Returns 'from' if the second argument is '(char *) NULL'.
 * Returns NULL if the object or parents were not found or cannot exist.
 *
 */
ode_t *ode_get(const ode_t *from, ...);

/*
 * Get string data from an object.
 *
 * Returns the string specified by 'type' if it exists.
 * Returns NULL on use of the value mode on a value-less object.
 *
 * The returns string will always be null-terminated. It must not be modified;
 * use 'ode_mod()' instead.
 *
 */
const char *ode_getstr(const ode_t *from, enum ode_type type);

/*
 * Get data size from an object.
 *
 * Returns the size specified by 'type' if applicable.
 * Returns '(size_t) -1' on use of the value mode on a value-less object.
 *
 */
size_t ode_getlen(const ode_t *from, enum ode_type type);

/*
 * Iterate through the subordinates of an object.
 *
 * Pass 'pos' as NULL to obtain the first subordinate of 'obj'. This can be used
 * to check if 'obj' has any. Pass a subordinate of 'obj' as 'pos' to obtain the
 * next subordinate.
 *
 * Returns the subordinate of 'obj' after non-NULL 'pos' if it exists.
 * Returns NULL if 'obj' has no subordinates.
 * Returns NULL if 'pos' is the last subordinate or not subordinate to 'obj'.
 *
 */
ode_t *ode_iter(const ode_t *obj, const ode_t *pos);

/*
 * Modify or set object data.
 *
 * 'str' is treated as a null-terminated string if 'len' is '(size_t) -1'.
 * Illegal modifications are: attempting to set value on an object with
 * subordinate and setting the same name as that of another object of the same
 * rank. Pointers to the modified data are invalidated on success.
 *
 * Returns 'obj' on success.
 * Returns NULL and sets errno on modification failure.
 * Returns NULL on illegal modification attempt.
 *
 */
ode_t *ode_mod(ode_t *obj, enum ode_type type, const char *str, size_t len);

/*
 * Add a subordinate object.
 *
 * Adds an empty object with name 'name' to 'to', which is treated as a
 * null-terminated string if 'len' is '(size_t) -1'. Illegal additions are:
 * adding to an object with value and adding an object with the same name as
 * another of the same rank. Pointers to subordinates of 'to' are invalidated on
 * success.
 *
 * Returns the newly added object on success.
 * Returns NULL and sets errno on allocation failure.
 * Returns NULL on illegal addition attempt.
 *
 */
ode_t *ode_add(ode_t *to, const char *name, size_t len);

/*
 * Delete an object and its children.
 *
 * If 'obj' is a root object, it is completely deleted and freed. 'obj' and its
 * children (but not its parents if they exist) are invalidated on success, and
 * unchanged on failure. 'obj' may be NULL, in which case nothing happens.
 *
 * Returns 1 on success.
 * Returns 0 and sets errno on deletion failure.
 * Returns 0 if 'obj' is NULL.
 *
 * 'obj' must not be used after successful execution.
 *
 */
int ode_del(ode_t *obj);

/*
 * Securely zero the data of an object and its children.
 *
 * 'zero_fn' must be a 'bzero()' like function; it is used for all zeroing
 * operations and must not have side effects if its size parameter is 0.
 *
 * 'obj' is still valid and may be 'ode_free()'d after execution, but all data
 * and length info is lost.
 *
 */
void ode_zero(ode_t *obj, void (*zero_fn)(void *, size_t));

#endif /* ODE_H */
