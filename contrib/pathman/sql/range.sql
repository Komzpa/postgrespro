/*
 * Creates RANGE partitions for specified relation
 */
CREATE OR REPLACE FUNCTION create_range_partitions(
    v_relation TEXT
    , v_attribute TEXT
    , v_start_value ANYELEMENT
    , v_interval INTERVAL
    , v_premake INTEGER)
RETURNS VOID AS
$$
DECLARE
    v_relid     INTEGER;
    v_value     TEXT;
    i INTEGER;
BEGIN
    SELECT relfilenode INTO v_relid
    FROM pg_class WHERE relname = v_relation;

    IF EXISTS (SELECT * FROM pg_pathman_rels WHERE relname = v_relation) THEN
        RAISE EXCEPTION 'Reltion "%" has already been partitioned', v_relation;
    END IF;

    EXECUTE format('DROP SEQUENCE IF EXISTS %s_seq', v_relation);
    EXECUTE format('CREATE SEQUENCE %s_seq START 1', v_relation);

    INSERT INTO pg_pathman_rels (relname, attname, parttype)
    VALUES (v_relation, v_attribute, 2);

    /* create first partition */
    FOR i IN 1..v_premake+1
    LOOP
        EXECUTE format('SELECT create_single_range_partition($1, $2, $3::%s);', pg_typeof(v_start_value))
        USING v_relation, v_start_value, v_start_value + v_interval;

        v_start_value := v_start_value + v_interval;
    END LOOP;

    /* Create triggers */
    PERFORM create_range_insert_trigger(v_relation, v_attribute);
    -- PERFORM create_hash_update_trigger(relation, attribute, partitions_count);
    /* Notify backend about changes */
    PERFORM pg_pathman_on_create_partitions(v_relid);
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION create_range_partitions(
    v_relation TEXT
    , v_attribute TEXT
    , v_start_value ANYELEMENT
    , v_interval ANYELEMENT
    , v_premake INTEGER)
RETURNS VOID AS
$$
DECLARE
    v_relid     INTEGER;
    v_value     TEXT;
    i INTEGER;
BEGIN
    SELECT relfilenode INTO v_relid
    FROM pg_class WHERE relname = v_relation;

    IF EXISTS (SELECT * FROM pg_pathman_rels WHERE relname = v_relation) THEN
        RAISE EXCEPTION 'Reltion "%" has already been partitioned', v_relation;
    END IF;

    EXECUTE format('DROP SEQUENCE IF EXISTS %s_seq', v_relation);
    EXECUTE format('CREATE SEQUENCE %s_seq START 1', v_relation);

    INSERT INTO pg_pathman_rels (relname, attname, parttype)
    VALUES (v_relation, v_attribute, 2);

    /* create first partition */
    FOR i IN 1..v_premake+1
    LOOP
        PERFORM create_single_range_partition(v_relation
                                              , v_start_value
                                              , v_start_value + v_interval);
        v_start_value := v_start_value + v_interval;
    END LOOP;

    /* Create triggers */
    PERFORM create_range_insert_trigger(v_relation, v_attribute);
    -- PERFORM create_hash_update_trigger(relation, attribute, partitions_count);
    /* Notify backend about changes */
    PERFORM pg_pathman_on_create_partitions(v_relid);
END
$$ LANGUAGE plpgsql;

/*
 * Create additional partitions for existing RANGE partitioning
 */
-- CREATE OR REPLACE FUNCTION append_range_partitions(
--     v_relation TEXT
--     , v_interval TEXT
--     , v_premake INTEGER)
-- RETURNS VOID AS
-- $$
-- DECLARE
--     v_attribute TEXT;
--     v_type TEXT;
--     v_dt TIMESTAMP;
--     v_num DOUBLE PRECISION;
--     v_relid INTEGER;
-- BEGIN
--     /* get an attribute name config */
--     v_attribute := attname FROM pg_pathman_rels WHERE relname = v_relation;
--     RAISE NOTICE 'v_attribute = %', v_attribute;

--     /* get relation oid */
--     v_relid := relfilenode FROM pg_class WHERE relname = v_relation;

--     /* get range type: time or numeral */
--     SELECT max(max_dt), max(max_num) INTO v_dt, v_num
--     FROM pg_pathman_range_rels WHERE parent = v_relation;
--     IF NOT v_dt IS NULL THEN
--         v_type := 'time';
--     ELSIF NOT v_num IS NULL THEN
--         v_type := 'num';
--     END IF;

--     /* create partitions */
--     PERFORM append_range_partitions_internal(v_relation, v_interval, v_premake);

--     /* recreate triggers */
--     PERFORM drop_range_triggers(v_relation);
--     PERFORM create_range_insert_trigger(v_relation, v_attribute, v_type);

--     PERFORM pg_pathman_on_update_partitions(v_relid);
-- END
-- $$ LANGUAGE plpgsql;

/*
 * Create additional partitions for existing RANGE partitioning
 * (function for internal use)
 */
-- CREATE OR REPLACE FUNCTION append_range_partitions_internal(
--     v_relation TEXT
--     , v_interval TEXT
--     , v_premake INTEGER)
-- RETURNS VOID AS
-- $$
-- DECLARE
--     v_part_timestamp TIMESTAMP;
--     v_part_num DOUBLE PRECISION;
--     v_type TEXT;
--     i INTEGER;
-- BEGIN
--     SELECT max(max_dt), max(max_num)
--     INTO v_part_timestamp, v_part_num
--     FROM pg_pathman_range_rels
--     WHERE parent = v_relation;

--     /* Create partitions and update pg_pathman configuration */
--     if NOT v_part_timestamp IS NULL THEN
--         FOR i IN 0..v_premake-1
--         LOOP
--             PERFORM create_single_range_partition(v_relation
--                                                   , 'time'
--                                                   , v_part_timestamp::TEXT
--                                                   , v_interval);
--             v_part_timestamp := v_part_timestamp + v_interval::INTERVAL;
--         END LOOP;
--     ELSIF NOT v_part_num IS NULL THEN
--         /* Numerical range partitioning */
--         FOR i IN 0..v_premake-1
--         LOOP
--             PERFORM create_single_range_partition(v_relation
--                                                   , 'num'
--                                                   , v_part_num::TEXT
--                                                   , v_interval);
--             v_part_num := v_part_num + v_interval::DOUBLE PRECISION;
--         END LOOP;
--     END IF;
-- END
-- $$ LANGUAGE plpgsql;

/*
 * Creates range condition. Utility function.
 */
CREATE OR REPLACE FUNCTION get_range_condition(
    p_attname TEXT
    , p_start_value ANYELEMENT
    , p_end_value ANYELEMENT)
RETURNS TEXT AS
$$
DECLARE
    v_type REGTYPE;
    v_sql  TEXT;
BEGIN
    /* determine the type of values */
    v_type := pg_typeof(p_start_value);

    /* we cannot use placeholders in DDL queries, so we are using format(...) */
    IF v_type IN ('date'::regtype, 'timestamp'::regtype, 'timestamptz'::regtype) THEN
        v_sql := '%s >= ''%s'' AND %s < ''%s''';
    ELSE
        v_sql := '%s >= %s AND %s < %s';
    END IF;

    v_sql := format(v_sql
                    , p_attname
                    , p_start_value
                    , p_attname
                    , p_end_value);
    RETURN v_sql;
END
$$
LANGUAGE plpgsql;

/*
 * Creates new RANGE partition. Returns partition name
 */
CREATE OR REPLACE FUNCTION create_single_range_partition(
    p_parent_relname TEXT
    , p_start_value  ANYELEMENT
    , p_end_value    ANYELEMENT)
RETURNS TEXT AS
$$
DECLARE
    v_child_relname TEXT;
    v_attname TEXT;

    v_part_num INT;
    v_sql TEXT;
    -- v_type TEXT;
    v_cond TEXT;
BEGIN
    v_attname := attname FROM pg_pathman_rels
                 WHERE relname = p_parent_relname;

    /* get next value from sequence */
    v_part_num := nextval(format('%s_seq', p_parent_relname));
    v_child_relname := format('%s_%s', p_parent_relname, v_part_num);

    /* Skip existing partitions */
    IF EXISTS (SELECT * FROM pg_tables WHERE tablename = v_child_relname) THEN
        RAISE WARNING 'Relation % already exists, skipping...', v_child_relname;
        RETURN NULL;
    END IF;

    EXECUTE format('CREATE TABLE %s (LIKE %s INCLUDING ALL)'
                   , v_child_relname
                   , p_parent_relname);

    EXECUTE format('ALTER TABLE %s INHERIT %s'
                   , v_child_relname
                   , p_parent_relname);

    v_cond := get_range_condition(v_attname, p_start_value, p_end_value);
    v_sql := format('ALTER TABLE %s ADD CHECK (%s)'
                  , v_child_relname
                  , v_cond);

    EXECUTE v_sql;
    RETURN v_child_relname;
END
$$ LANGUAGE plpgsql;


/*
 * Split RANGE partition
 */
CREATE OR REPLACE FUNCTION split_range_partition(
    p_partition TEXT
    , p_value ANYELEMENT
    , OUT p_range ANYARRAY)
RETURNS ANYARRAY AS
$$
DECLARE
    v_parent_relid OID;
    v_child_relid OID := p_partition::regclass::oid;
    v_attname TEXT;
    v_cond TEXT;
    v_new_partition TEXT;
    v_part_type INTEGER;
BEGIN
    v_parent_relid := inhparent
                      FROM pg_inherits
                      WHERE inhrelid = v_child_relid;

    SELECT attname, parttype INTO v_attname, v_part_type
    FROM pg_pathman_rels
    WHERE relname = v_parent_relid::regclass::text;

    /* Check if this is RANGE partition */
    IF v_part_type != 2 THEN
        RAISE EXCEPTION 'Specified partition isn''t RANGE partition';
    END IF;

    /* Get partition values range */
    p_range := get_partition_range(v_parent_relid, v_child_relid, 0);
    IF p_range IS NULL THEN
        RAISE EXCEPTION 'Could not find specified partition';
    END IF;

    /* Check if value fit into the range */
    IF p_range[1] > p_value OR p_range[2] <= p_value
    THEN
        RAISE EXCEPTION 'Specified value does not fit into the range [%, %)',
            p_range[1], p_range[2];
    END IF;

    /* Create new partition */
    RAISE NOTICE 'Creating new partition...';
    v_new_partition := create_single_range_partition(v_parent_relid::regclass::text,
                                                     p_value,
                                                     p_range[2]);

    /* Copy data */
    RAISE NOTICE 'Copying data to new partition...';
    v_cond := get_range_condition(v_attname, p_value, p_range[2]);
    EXECUTE format('
                WITH part_data AS (
                    DELETE FROM %s WHERE %s RETURNING *)
                INSERT INTO %s SELECT * FROM part_data'
                , p_partition
                , v_cond
                , v_new_partition);

    /* Alter original partition */
    RAISE NOTICE 'Altering original partition...';
    v_cond := get_range_condition(v_attname, p_range[1], p_value);
    EXECUTE format('ALTER TABLE %s DROP CONSTRAINT %s_%s_check'
                   , p_partition
                   , p_partition
                   , v_attname);
    EXECUTE format('ALTER TABLE %s ADD CHECK (%s)'
                   , p_partition
                   , v_cond);

    /* Tell backend to reload configuration */
    PERFORM pg_pathman_on_update_partitions(v_parent_relid::INTEGER);

    RAISE NOTICE 'Done!';
END
$$
LANGUAGE plpgsql;


/*
 * Merge RANGE partitions
 * 
 * Note: we had to have at least one argument of type 
 */
    -- , OUT p_range1 ANYARRAY
CREATE OR REPLACE FUNCTION merge_range_partitions(
    p_partition1 TEXT
    , p_partition2 TEXT)
RETURNS VOID AS
$$
DECLARE
    v_parent_relid1 OID;
    v_parent_relid2 OID;
    v_part1_relid OID := p_partition1::regclass::oid;
    v_part2_relid OID := p_partition2::regclass::oid;
    v_attname TEXT;
    v_part_type INTEGER;
    v_atttype TEXT;
BEGIN
    IF v_part1_relid = v_part2_relid THEN
        RAISE EXCEPTION 'Cannot merge partition with itself';
    END IF;

    v_parent_relid1 := inhparent FROM pg_inherits WHERE inhrelid = v_part1_relid;
    v_parent_relid2 := inhparent FROM pg_inherits WHERE inhrelid = v_part2_relid;

    IF v_parent_relid1 != v_parent_relid2 THEN
        RAISE EXCEPTION 'Cannot merge partitions having different parents';
    END IF;

    SELECT attname, parttype INTO v_attname, v_part_type
    FROM pg_pathman_rels
    WHERE relname = v_parent_relid1::regclass::text;

    /* Check if this is RANGE partition */
    IF v_part_type != 2 THEN
        RAISE EXCEPTION 'Specified partitions aren''t RANGE partitions';
    END IF;

    v_atttype := get_attribute_type_name(v_parent_relid1::regclass::text, v_attname);

    EXECUTE format('SELECT merge_range_partitions_internal($1, $2 , $3, NULL::%s)', v_atttype)
    USING v_parent_relid1, v_part1_relid , v_part2_relid;

    /* Tell backend to reload configuration */
    PERFORM pg_pathman_on_update_partitions(v_parent_relid1::INTEGER);

    RAISE NOTICE 'Done!';
END
$$
LANGUAGE plpgsql;


/*
 * Merge two partitions. All data will be copied to the first one. Second
 * partition will be destroyed.
 *
 * Notes: dummy field is used to pass the element type to the function
 * (it is neccessary because of pseudo-types used in function)
 */
CREATE OR REPLACE FUNCTION merge_range_partitions_internal(
    p_parent_relid OID
    , p_part1_relid OID
    , p_part2_relid OID
    , dummy ANYELEMENT
    , OUT p_range ANYARRAY)
RETURNS ANYARRAY AS
$$
DECLARE
    v_attname TEXT;
    v_cond TEXT;
BEGIN
    SELECT attname INTO v_attname FROM pg_pathman_rels
    WHERE relname = p_parent_relid::regclass::text;

    /*
     * Get ranges
     * first and second elements of array are MIN and MAX of partition1
     * third and forth elements are MIN and MAX of partition2
     */
    p_range := get_partition_range(p_parent_relid, p_part1_relid, 0) ||
               get_partition_range(p_parent_relid, p_part2_relid, 0);

    /* Check if ranges are adjacent */
    IF p_range[1] != p_range[4] AND p_range[2] != p_range[3] THEN
        RAISE EXCEPTION 'Merge failed. Partitions must be adjacent';
    END IF;

    /* Extend first partition */
    v_cond := get_range_condition(v_attname
                                  , least(p_range[1], p_range[3])
                                  , greatest(p_range[2], p_range[4]));

    /* Alter first partition */
    RAISE NOTICE 'Altering first partition...';
    EXECUTE format('ALTER TABLE %s DROP CONSTRAINT %s_%s_check'
                   , p_part1_relid::regclass::text
                   , p_part1_relid::regclass::text
                   , v_attname);
    EXECUTE format('ALTER TABLE %s ADD CHECK (%s)'
                   , p_part1_relid::regclass::text
                   , v_cond);

    /* Copy data from second partition to the first one */
    RAISE NOTICE 'Copying data...';
    EXECUTE format('WITH part_data AS (DELETE FROM %s RETURNING *)
                    INSERT INTO %s SELECT * FROM part_data'
                   , p_part2_relid::regclass::text
                   , p_part1_relid::regclass::text);

    /* Remove second partition */
    RAISE NOTICE 'Dropping second partition...';
    EXECUTE format('DROP TABLE %s', p_part2_relid::regclass::text);
END
$$ LANGUAGE plpgsql;


/*
 * Append new partition
 */
CREATE OR REPLACE FUNCTION append_partition(p_relation TEXT)
RETURNS VOID AS
$$
DECLARE
    v_attname TEXT;
    v_atttype TEXT;
BEGIN
    v_attname := attname FROM pg_pathman_rels WHERE relname = p_relation;
    v_atttype := get_attribute_type_name(p_relation, v_attname);
    EXECUTE format('SELECT append_partition_internal($1, NULL::%s)', v_atttype)
    USING p_relation;
END
$$
LANGUAGE plpgsql;


CREATE OR REPLACE FUNCTION append_partition_internal(
    p_relation TEXT
    , dummy ANYELEMENT
    , OUT p_range ANYARRAY)
RETURNS ANYARRAY AS
$$
BEGIN
    p_range := get_range_by_idx(p_relation::regclass::oid, -1, 0);
    RAISE NOTICE 'Appending new partition...';
    PERFORM create_single_range_partition(p_relation
                                          , p_range[2]
                                          , p_range[2] + (p_range[2] - p_range[1]));

    /* Tell backend to reload configuration */
    PERFORM pg_pathman_on_update_partitions(p_relation::regclass::integer);
    RAISE NOTICE 'Done!';
END
$$
LANGUAGE plpgsql;


/*
 * Append new partition
 */
CREATE OR REPLACE FUNCTION prepend_partition(p_relation TEXT)
RETURNS VOID AS
$$
DECLARE
    v_attname TEXT;
    v_atttype TEXT;
BEGIN
    v_attname := attname FROM pg_pathman_rels WHERE relname = p_relation;
    v_atttype := get_attribute_type_name(p_relation, v_attname);
    EXECUTE format('SELECT prepend_partition_internal($1, NULL::%s)', v_atttype)
    USING p_relation;
END
$$
LANGUAGE plpgsql;


CREATE OR REPLACE FUNCTION prepend_partition_internal(
    p_relation TEXT
    , dummy ANYELEMENT
    , OUT p_range ANYARRAY)
RETURNS ANYARRAY AS
$$
BEGIN
    p_range := get_range_by_idx(p_relation::regclass::oid, 0, 0);
    RAISE NOTICE 'Prepending new partition...';
    PERFORM create_single_range_partition(p_relation
                                          , p_range[1] - (p_range[2] - p_range[1])
                                          , p_range[1]);

    /* Tell backend to reload configuration */
    PERFORM pg_pathman_on_update_partitions(p_relation::regclass::integer);
    RAISE NOTICE 'Done!';
END
$$
LANGUAGE plpgsql;


/*
 * Creates range partitioning insert trigger
 */
CREATE OR REPLACE FUNCTION create_range_insert_trigger(
    v_relation    TEXT
    , v_attname   TEXT)
RETURNS VOID AS
$$
DECLARE
    v_func TEXT := '
        CREATE OR REPLACE FUNCTION %s_range_insert_trigger_func()
        RETURNS TRIGGER
        AS $body$
        DECLARE
            v_part_relid OID;
        BEGIN
            IF TG_OP = ''INSERT'' THEN
                v_part_relid := find_range_partition(TG_RELID, NEW.%s);
                IF NOT v_part_relid IS NULL THEN
                    EXECUTE format(''INSERT INTO %%s SELECT $1.*'', v_part_relid::regclass)
                    USING NEW;
                ELSE
                    RAISE EXCEPTION ''ERROR: Cannot determine approprite partition'';
                END IF;
            END IF;
            RETURN NULL;
        END
        $body$ LANGUAGE plpgsql;';
    v_trigger TEXT := '
        CREATE TRIGGER %s_insert_trigger
        BEFORE INSERT ON %1$s
        FOR EACH ROW EXECUTE PROCEDURE %1$s_range_insert_trigger_func();';
BEGIN
    v_func := format(v_func, v_relation, v_attname);
    v_trigger := format(v_trigger, v_relation);

    EXECUTE v_func;
    EXECUTE v_trigger;
    RETURN;
END
$$ LANGUAGE plpgsql;


/*
 * Drop partitions
 */
CREATE OR REPLACE FUNCTION drop_range_partitions(IN relation TEXT)
RETURNS VOID AS
$$
DECLARE
    v_relid INTEGER;
    v_rec   RECORD;
BEGIN
    /* Drop trigger first */
    PERFORM drop_range_triggers(relation);

    v_relid := relfilenode FROM pg_class WHERE relname = relation;

    FOR v_rec IN (SELECT inhrelid::regclass AS tbl FROM pg_inherits WHERE inhparent = v_relid)
    LOOP
        EXECUTE format('DROP TABLE %s', v_rec.tbl);
    END LOOP;

    DELETE FROM pg_pathman_rels WHERE relname = relation;
    -- DELETE FROM pg_pathman_range_rels WHERE parent = relation;

    /* Notify backend about changes */
    PERFORM pg_pathman_on_remove_partitions(v_relid);
END
$$ LANGUAGE plpgsql;


/*
 * Drop trigger
 */
CREATE OR REPLACE FUNCTION drop_range_triggers(IN relation TEXT)
RETURNS VOID AS
$$
BEGIN
    EXECUTE format('DROP TRIGGER IF EXISTS %s_insert_trigger ON %1$s CASCADE', relation);
END
$$ LANGUAGE plpgsql;
