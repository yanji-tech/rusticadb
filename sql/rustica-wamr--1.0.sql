CREATE FUNCTION compile_wasm(bytea) RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
