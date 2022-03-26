/*
 * Copyright (C) 2022 Emil Overbeck <emil.a.overbeck at gmail.com>
 * Subject to the MIT License. See LICENSE.txt for more information.
 *
 */

#include <stdlib.h>

/* Allocator functions. May be set to alternate but conforming functions;
   include their header above. */
#define ODE_MALLOC  malloc
#define ODE_REALLOC realloc
#define ODE_FREE    free
