/* contrib/hunspell_nl_nl/hunspell_nl_nl--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION hunspell_nl_nl" to load this file. \quit

CREATE TEXT SEARCH DICTIONARY public.dutch_hunspell (
    TEMPLATE = ispell,
    DictFile = nl_nl,
    AffFile = nl_nl,
    StopWords = dutch
);

CREATE TEXT SEARCH CONFIGURATION public.dutch (
    COPY = pg_catalog.simple
);

ALTER TEXT SEARCH CONFIGURATION public.dutch
    ALTER MAPPING FOR asciiword, asciihword, hword_asciipart,
        word, hword, hword_part
    WITH public.dutch_hunspell, pg_catalog.dutch_stem;
