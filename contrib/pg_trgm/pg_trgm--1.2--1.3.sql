/* contrib/pg_trgm/pg_trgm--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_trgm UPDATE TO '1.3'" to load this file. \quit

CREATE FUNCTION set_substring_limit(float4)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION show_substring_limit()
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE;

CREATE FUNCTION substring_similarity(text,text)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION substring_similarity_op(text,text)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE;  -- stable because depends on trgm_substring_limit

CREATE FUNCTION substring_similarity_commutator_op(text,text)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE;  -- stable because depends on trgm_substring_limit

CREATE OPERATOR <% (
        LEFTARG = text,
        RIGHTARG = text,
        PROCEDURE = substring_similarity_op,
        COMMUTATOR = '%>',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OPERATOR %> (
        LEFTARG = text,
        RIGHTARG = text,
        PROCEDURE = substring_similarity_commutator_op,
        COMMUTATOR = '<%',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

ALTER OPERATOR FAMILY gist_trgm_ops USING gist ADD
        OPERATOR        7       %> (text, text);

ALTER OPERATOR FAMILY gin_trgm_ops USING gin ADD
        OPERATOR        7       %> (text, text);
