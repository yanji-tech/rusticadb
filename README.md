1. Install vendor libraries (WAMR):

```
$ make vendor
```

2. Install PostgreSQL for development:

```
$ make dev-pg
```

3. Build extension and reload the dev Postgres:

```
$ make reload DEV=1 -j $(nproc)
```

4. Access dev Postgres REPL:

```
$ psql -h /tmp postgres
postgres=# create extension "rustica-wamr" cascade;
postgres=# select octet_length(compile_wasm('\x0061736d010000000127085e77015e63000160017f00600164000060037f7f7f01640060016400017f60000164006000000217010873706563746573740a7072696e745f63686172000203060503040506070401000503010001060a016401004102fb07010b070a01065f737461727400050901000c01010a7c052301017f034020012000fb0f48044020002001fb0d001000200141016a21010c010b0b0b300201630001640023002000fb0b012203d14504402003d40f0b20012002fb0900002104230020002004fb0e0120040f0b1200200010014100410041011002100141000b0a0041014102410d10020b0700100410031a0b0b2001011d0a00480065006c006c006f002c00200077006f0072006c006400210020'::bytea));
```

5. Optionally, rebuild extension files:

```
$ make clean
```

Then repeat step 3.

6. Drop all vendor files:

```
$ make clean-vendor
```
