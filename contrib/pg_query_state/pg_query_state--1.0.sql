-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_query_state" to load this file. \quit

CREATE FUNCTION pg_query_state(pid 		integer
							 , verbose	boolean = FALSE
							 , costs 	boolean = FALSE
							 , timing 	boolean = FALSE
							 , buffers 	boolean = FALSE
							 , triggers	boolean = FALSE
						     , format	text = 'text')
	RETURNS TABLE (query_text text, plan text)
	AS 'MODULE_PATHNAME'
	LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION executor_step(pid integer) RETURNS VOID
	AS 'MODULE_PATHNAME'
	LANGUAGE C VOLATILE;

CREATE FUNCTION executor_continue(pid integer) RETURNS VOID
	AS 'MODULE_PATHNAME'
	LANGUAGE C VOLATILE;
