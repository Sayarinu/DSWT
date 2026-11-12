-- Record each identity's game, version, language and revision (protocol/README.md,
-- "Game variants"). A variant never changes. Identities enrolled before this
-- migration came from released clients, which send legacy code 1: US English Platinum.
CREATE FUNCTION dswt.valid_variant(code bigint) RETURNS boolean
LANGUAGE sql IMMUTABLE SET search_path = pg_catalog AS $$
    SELECT code = 1 OR (code BETWEEN 0 AND 4294967295
        AND ((code >> 24)::integer, ((code >> 16) & 255)::integer)
            IN ((1, 10), (1, 11), (2, 12), (3, 7), (3, 8))
        AND ((code >> 8) & 255)::integer IN (1, 2, 3, 4, 5, 7, 8))
$$;
CREATE TABLE dswt.identity_variants (
    save_id dswt.id PRIMARY KEY REFERENCES dswt.save_identities,
    variant bigint NOT NULL CHECK (dswt.valid_variant(variant))
);
CREATE TRIGGER immutable_row BEFORE UPDATE OR DELETE ON dswt.identity_variants
    FOR EACH ROW EXECUTE FUNCTION dswt.reject_change();
CREATE TRIGGER retain_table BEFORE TRUNCATE ON dswt.identity_variants
    FOR EACH STATEMENT EXECUTE FUNCTION dswt.reject_change();
CREATE TRIGGER audit_rows AFTER INSERT ON dswt.identity_variants
    FOR EACH ROW EXECUTE FUNCTION dswt.audit_row('save_id');
INSERT INTO dswt.identity_variants SELECT save_id, 1 FROM dswt.save_identities;

-- Every new identity commits together with its variant.
CREATE FUNCTION dswt.check_identity_variant() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM dswt.identity_variants WHERE save_id = NEW.save_id) THEN
        RAISE EXCEPTION 'identity has no variant' USING ERRCODE = '23514';
    END IF;
    RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER identity_variant AFTER INSERT ON dswt.save_identities
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION dswt.check_identity_variant();
REVOKE ALL ON dswt.identity_variants FROM PUBLIC;
REVOKE ALL ON FUNCTION dswt.valid_variant(bigint), dswt.check_identity_variant() FROM PUBLIC;

CREATE OR REPLACE FUNCTION dswt.verify_replay() RETURNS bigint LANGUAGE plpgsql
SET search_path = pg_catalog SET timezone = 'UTC' SET bytea_output = 'hex' AS $$
DECLARE
    item record;
    previous jsonb;
    projection_name text;
    different boolean;
    row_count bigint := 0;
    allowed_tables text[] := ARRAY['save_identities', 'payloads', 'offers', 'ownership',
        'matches', 'receipts', 'operations', 'trade_events', 'acknowledgements', 'matcher_attempts',
        'identity_variants'];
BEGIN
    CREATE TEMP TABLE wt_replay_rows (
        table_name name, row_key jsonb, row_data jsonb, PRIMARY KEY (table_name, row_key)
    ) ON COMMIT DROP;
    FOR item IN SELECT * FROM dswt.audit_journal ORDER BY sequence LOOP
        IF item.version <> 1 OR NOT item.table_name = ANY(allowed_tables) THEN
            RAISE EXCEPTION 'unsupported replay event' USING ERRCODE = '23514';
        END IF;
        SELECT row_data INTO previous FROM pg_temp.wt_replay_rows
            WHERE wt_replay_rows.table_name = item.table_name AND row_key = item.row_key;
        IF previous IS DISTINCT FROM item.before_image THEN
            RAISE EXCEPTION 'replay preimage mismatch' USING ERRCODE = '23514';
        END IF;
        IF (item.before_image IS NULL AND dswt.valid_initial(item.table_name, item.after_image) IS NOT TRUE)
            OR (item.before_image IS NOT NULL AND dswt.valid_transition(
                item.table_name, item.before_image, item.after_image) IS NOT TRUE) THEN
            RAISE EXCEPTION 'invalid replay transition' USING ERRCODE = '23514';
        END IF;
        INSERT INTO pg_temp.wt_replay_rows VALUES (item.table_name, item.row_key, item.after_image)
            ON CONFLICT (table_name, row_key) DO UPDATE SET row_data = EXCLUDED.row_data;
        row_count := row_count + 1;
    END LOOP;
    FOREACH projection_name IN ARRAY allowed_tables LOOP
        EXECUTE format('SELECT EXISTS ((SELECT to_jsonb(t) FROM dswt.%I t EXCEPT ALL SELECT row_data FROM pg_temp.wt_replay_rows WHERE table_name = $1) UNION ALL (SELECT row_data FROM pg_temp.wt_replay_rows WHERE table_name = $1 EXCEPT ALL SELECT to_jsonb(t) FROM dswt.%I t))', projection_name, projection_name)
            INTO different USING projection_name;
        IF different THEN
            RAISE EXCEPTION 'replay projection mismatch' USING ERRCODE = '23514';
        END IF;
    END LOOP;
    IF EXISTS (SELECT 1 FROM dswt.save_identities i
        LEFT JOIN (SELECT save_id, count(*) AS events, max(sequence) AS last_event FROM dswt.trade_events GROUP BY save_id) e USING (save_id)
        LEFT JOIN (SELECT save_id, count(*) AS operations, max(counter) AS last_counter FROM dswt.operations GROUP BY save_id) o USING (save_id)
        WHERE i.event_highwater = 0 OR i.event_highwater <> coalesce(e.events, 0)
            OR i.event_highwater <> coalesce(e.last_event, 0)
            OR i.next_counter <> coalesce(o.operations, 0) + 1
            OR i.next_counter <> coalesce(o.last_counter, 0) + 1) THEN
        RAISE EXCEPTION 'replay checkpoint history is incomplete' USING ERRCODE = '23514';
    END IF;
    IF EXISTS (SELECT 1 FROM dswt.save_identities i
        WHERE NOT EXISTS (SELECT 1 FROM dswt.identity_variants v WHERE v.save_id = i.save_id)) THEN
        RAISE EXCEPTION 'replay identity variant is missing' USING ERRCODE = '23514';
    END IF;
    PERFORM dswt.verify_foreign_keys();
    PERFORM dswt.validate_offer(trade_id) FROM dswt.offers;
    PERFORM dswt.validate_event(e) FROM dswt.trade_events e;
    PERFORM dswt.validate_match(m) FROM dswt.matches m;
    PERFORM dswt.validate_acknowledgement(a) FROM dswt.acknowledgements a;
    DROP TABLE pg_temp.wt_replay_rows;
    RETURN row_count;
END $$;
