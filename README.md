# Ordered Data Exchange

A hierarchical data management and serialisation library written in ANSI C,
with a focus on simplicity, security and efficiency.

## Installation

Include the files in `src/` in a C project and compile them together.

## Usage

Error checking is omitted for the sake of brevity.

### Root object initialisation

```c
ode_t *data;
```
We can create an empty object
```c
data = ode_create("root", -1);
```

Or read its contents from serialised data.
```c
data = ode_read(buffer, buffer_size);
```
### Storage & manipulation of data

Adding subordinate objects:
```c
ode_add(data, "test1", -1);
ode_add(ode_get1(data, "test1", -1), "test2", 5);
```

Storing and modifying data:
```c
ode_t *sub;

sub = ode_add(data, "emptyobj", -1);
ode_mod(sub, ODE_VALUE, "testval", -1);
ode_mod(sub, ODE_NAME, "objwithval", -1);
```

Removing data:
```c
sub = ode_get(data, "test1", "test2", (char *) NULL);
ode_zero(sub, &bzero);      /* Optional */
ode_del(sub);
```

### Data usage

We can obtain data directly
```c
puts(ode_getstr(data, ODE_NAME));
fwrite(ode_getstr(data, ODE_NAME), 1, ode_getlen(data, ODE_NAME), stdout);
```

Or by iterating through subordinates.
```c
ode_t *o;

for (o = ode_iter(data, NULL); o; o = ode_iter(data, o))
    puts(ode_getstr(o, ODE_NAME));
```

### Finalisation

Serialising data for future use:
```c
unsigned char *buffer;
size_t buffer_size

buffer = ode_write(data, &buffer_size);
```

Freeing all data:
```c
ode_del(data);
```

## License

This library is free software and subject to the MIT license. See `LICENSE.txt`
for more information.
